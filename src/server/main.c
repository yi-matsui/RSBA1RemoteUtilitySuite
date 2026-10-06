/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * server_bridge エントリポイント（シャック側 / IC-9100 接続）。
 *   (既定)                      制御・CI-V・オーディオを中継する
 *   --check-config              設定を検証して終了
 *   --list-usb / --list-audio   デバイス一覧
 *   --usb-reset VID:PID [...]   指定デバイスを手動でハードウェアリセット
 * 設定の優先順位: 既定値 < 設定ファイル（--config、既定 config.server.json）< 環境変数
 * RSBA_PASSWORD < コマンドライン引数。パスワードはコマンドライン（プロセス一覧に露出する）では受け付けない。
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "audio_dev.h"
#include "cfg_load.h"
#include "json.h"
#include "rs_error.h"
#include "rs_log.h"
#include "rs_shutdown.h"
#include "server_app.h"
#include "server_config.h"
#include "usb_device.h"
#include "usb_monitor.h"

#define MODULE "main"

static volatile sig_atomic_t g_stop;

static void usage(void)
{
    printf("usage: server_bridge [--config FILE] [options]\n"
           "  settings come from FILE (default: ./config.server.json if present); options override it.\n"
           "  password: auth.password in FILE, or environment variable RSBA_PASSWORD (takes precedence)\n"
           "      --config FILE                configuration file (JSON)\n"
           "      --check-config               validate the configuration and exit\n"
           "      --user NAME                  account name\n"
           "  network:\n"
           "      --listen ADDR                listen address (default: ::)\n"
           "      --port N                     control port; CI-V = N+1, audio = N+2 (default: 50001)\n"
           "      --iterations N               PBKDF2 iterations (default: 100000)\n"
           "  CI-V:\n"
           "      --civ-device NAME            radio serial port (COM3, /dev/ttyUSB0); omit to disable\n"
           "      --civ-baud N                 baud rate (default: 19200)\n"
           "  audio (48 kHz / 16-bit / mono):\n"
           "      --audio-backend B            auto|winmm|alsa|null|none (default: auto)\n"
           "      --audio-capture NAME         capture device (substring match; default device if omitted)\n"
           "      --audio-playback NAME        playback device\n"
           "      --audio-codec C              pcm (default) | opus (not built yet)\n"
           "      --jitter-min MS / --jitter-max MS   adaptive jitter buffer range (default: 20 / 80)\n"
           "  USB watch / reset:\n"
           "      --usb-watch VID:PID          watch the radio's USB device and reset it automatically\n"
           "      --usb-reset VID:PID          hardware-reset the matching USB device once and exit\n"
           "      --serial SERIAL / --device-id ID / --method auto|usbdevfs|authorized|devnode\n"
           "      --reset-parent               reset the parent hub instead of the device\n"
           "      --usb-log PATH               dedicated reset log (default: usb_reset.log)\n"
           "  other:\n"
           "      --list-usb / --list-audio    list devices and exit\n"
           "      --log-level LEVEL            trace|debug|info|warn|error (default: info)\n"
           "      --log-file PATH              also write the log to PATH (rotated)\n"
           "      --help\n");
}

/* ---- 一覧・手動リセット -------------------------------------------------- */

static int print_usb(const usb_device_t *dev, void *user)
{
    (void)user;
    printf("%04X:%04X  %-9s  serial=%-20s  %s\n", dev->vid, dev->pid,
           dev->ready ? "ready" : "NOT-READY", dev->serial[0] ? dev->serial : "-", dev->id);
    return 0;
}

static void print_audio(const char *backend, int is_capture, const char *name, void *user)
{
    (void)user;
    printf("%-6s %-8s %s\n", backend, is_capture ? "capture" : "playback", name);
}

static int cmd_usb_reset(const usb_monitor_config_t *base, const char *log_path)
{
    usb_monitor_config_t cfg = *base;
    rs_log_config_t lcfg;
    rs_log_t *reset_log = NULL;
    usb_monitor_t *mon = NULL;
    usb_reset_result_t res;
    int rc;

    memset(&res, 0, sizeof(res));
    rs_log_config_default(&lcfg);
    lcfg.file_path = log_path;
    rc = rs_log_open(&reset_log, &lcfg);
    if (rc != RS_OK) {
        RS_LOG_ERROR(MODULE, "cannot open USB reset log %s: %s", log_path, rs_strerror(rc));
        return 1;
    }
    cfg.log = reset_log;
    cfg.min_interval_ms = 0; /* 手動実行はレート制限しない */
    rc = usb_monitor_create(&mon, &cfg, NULL);
    if (rc == RS_OK)
        rc = usb_monitor_reset(mon, USB_TRIGGER_MANUAL, &res);
    if (rc == RS_OK)
        RS_LOG_INFO(MODULE, "USB reset succeeded (%s, recovery %u ms)",
                    usb_reset_method_name(res.method), (unsigned)res.recovery_ms);
    else
        RS_LOG_ERROR(MODULE, "USB reset failed: %s", rs_strerror(rc));
    usb_monitor_destroy(mon);
    rs_log_close(reset_log);
    return rc == RS_OK ? 0 : 1;
}

/* ---- 引数解析 ------------------------------------------------------------ */

static int parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < min || v > max)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

/* 引数から --config の値を探す（他の引数より先に設定ファイルを読むため） */
static const char *find_config_arg(int argc, char **argv)
{
    int i;
    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--config") == 0)
            return argv[i + 1];
    return NULL;
}

static int file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return 0;
    fclose(fp);
    return 1;
}

static int run(int argc, char **argv)
{
    server_app_config_t cfg;
    rs_log_config_t lcfg;
    usb_monitor_config_t *u = &cfg.usb;
    json_value_t *doc = NULL;
    rs_log_t *log = NULL;
    const char *config_path;
    const char *env_password;
    uint32_t port = 0;
    int list_usb = 0;
    int list_audio = 0;
    int usb_reset = 0;
    int check_only = 0;
    int rc = 2;
    int i;

    server_app_config_default(&cfg);
    rs_log_config_default(&lcfg);

    /* 1. 設定ファイル（--config 指定時は必須、未指定ならカレントの既定ファイルがあれば読む） */
    config_path = find_config_arg(argc, argv);
    if (config_path == NULL && file_exists(SERVER_CONFIG_DEFAULT_PATH))
        config_path = SERVER_CONFIG_DEFAULT_PATH;
    if (config_path != NULL) {
        int warnings = 0;
        if (cfg_load_file(config_path, &doc) != 0)
            return 2;
        if (server_config_apply(&cfg, &lcfg, doc, config_path, &warnings) != 0) {
            RS_LOG_ERROR(MODULE, "%s has errors; not starting", config_path);
            goto out;
        }
        cfg_check_permissions(config_path);
        RS_LOG_INFO(MODULE, "loaded %s (%d warning(s))", config_path, warnings);
    }

    /* 2. 環境変数のパスワードは設定ファイルより優先 */
    env_password = getenv("RSBA_PASSWORD");
    if (env_password != NULL && env_password[0] != '\0')
        cfg.password = env_password;

    /* 3. コマンドライン引数は最優先 */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        int bad = 0;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            rc = 0;
            goto out;
        } else if (strcmp(a, "--list-usb") == 0) {
            list_usb = 1;
            continue;
        } else if (strcmp(a, "--list-audio") == 0) {
            list_audio = 1;
            continue;
        } else if (strcmp(a, "--check-config") == 0) {
            check_only = 1;
            continue;
        } else if (strcmp(a, "--reset-parent") == 0) {
            u->reset_parent = 1;
            continue;
        } else if (next == NULL) {
            fprintf(stderr, "unknown or incomplete option: %s\n", a);
            goto out;
        }

        i++;
        if (strcmp(a, "--config") == 0) {
            /* 読み込み済み */
        } else if (strcmp(a, "--user") == 0) {
            cfg.username = next;
        } else if (strcmp(a, "--listen") == 0) {
            cfg.listen_addr = next;
        } else if (strcmp(a, "--port") == 0) {
            bad = !parse_u32(next, 1, 65533, &port);
        } else if (strcmp(a, "--iterations") == 0) {
            bad = !parse_u32(next, 1, 10000000, &cfg.pbkdf2_iterations);
        } else if (strcmp(a, "--civ-device") == 0) {
            cfg.civ_device = next;
        } else if (strcmp(a, "--civ-baud") == 0) {
            bad = !parse_u32(next, 1200, 115200, &cfg.civ_baud);
        } else if (strcmp(a, "--audio-backend") == 0) {
            if (strcmp(next, "none") == 0) {
                cfg.audio_enabled = 0;
            } else {
                cfg.audio_enabled = 1;
                cfg.audio_dev.backend = next;
            }
        } else if (strcmp(a, "--audio-capture") == 0) {
            cfg.audio_dev.capture = cfg_device_name(next);
        } else if (strcmp(a, "--audio-playback") == 0) {
            cfg.audio_dev.playback = cfg_device_name(next);
        } else if (strcmp(a, "--audio-codec") == 0) {
            bad = audio_codec_from_string(next, &cfg.audio_link.codec) != RS_OK;
        } else if (strcmp(a, "--jitter-min") == 0) {
            bad = !parse_u32(next, 10, 1000, &cfg.audio_link.jitter_min_ms);
        } else if (strcmp(a, "--jitter-max") == 0) {
            bad = !parse_u32(next, 10, 1000, &cfg.audio_link.jitter_max_ms);
        } else if (strcmp(a, "--usb-watch") == 0) {
            bad = usb_parse_vid_pid(next, &u->match.vid, &u->match.pid) != RS_OK;
            cfg.usb_watch = 1;
        } else if (strcmp(a, "--usb-reset") == 0) {
            bad = usb_parse_vid_pid(next, &u->match.vid, &u->match.pid) != RS_OK;
            usb_reset = 1;
        } else if (strcmp(a, "--serial") == 0) {
            u->match.serial = next;
        } else if (strcmp(a, "--device-id") == 0) {
            u->match.device_id = next;
        } else if (strcmp(a, "--method") == 0) {
            bad = usb_reset_method_from_string(next, &u->method) != RS_OK;
        } else if (strcmp(a, "--usb-log") == 0) {
            cfg.usb_log_path = next;
        } else if (strcmp(a, "--log-level") == 0) {
            bad = rs_log_level_from_string(next, &lcfg.level) != RS_OK;
        } else if (strcmp(a, "--log-file") == 0) {
            lcfg.file_path = next;
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            goto out;
        }
        if (bad) {
            fprintf(stderr, "invalid value for %s: %s\n", a, next);
            goto out;
        }
    }
    if (port != 0) {
        cfg.ctl_port = (uint16_t)port;
        cfg.civ_port = (uint16_t)(port + 1);
        cfg.audio_port = (uint16_t)(port + 2);
    }
    rs_log_set_level(rs_log_get_default(), lcfg.level);

    /* 4. 保守コマンド（認証設定は不要） */
    if (list_usb) {
        rc = usb_enumerate(usb_backend_platform(), print_usb, NULL) == RS_OK ? 0 : 1;
        goto out;
    }
    if (list_audio) {
        audio_dev_list(print_audio, NULL);
        rc = 0;
        goto out;
    }
    if (usb_reset) {
        rc = cmd_usb_reset(u, cfg.usb_log_path);
        goto out;
    }

    /* 5. 最終検証 */
    if (server_config_validate(&cfg) != 0) {
        RS_LOG_ERROR(MODULE, "configuration is invalid; not starting");
        goto out;
    }
    if (check_only) {
        RS_LOG_INFO(MODULE, "configuration OK");
        rc = 0;
        goto out;
    }

    /* 6. 設定どおりのロガーに切り替える（ファイル出力・ローテーション） */
    if (lcfg.file_path != NULL) {
        int lrc = rs_log_open(&log, &lcfg);
        if (lrc != RS_OK) {
            RS_LOG_ERROR(MODULE, "cannot open log file %s: %s", lcfg.file_path, rs_strerror(lrc));
            goto out;
        }
        rs_log_set_default(log);
    }

    RS_LOG_INFO(MODULE, "server_bridge %s starting", RSBA_VERSION);
    rs_shutdown_install(&g_stop);
    rc = server_app_run(&cfg, &g_stop);

out:
    if (log != NULL) {
        rs_log_close(log); /* 既定ロガーの設定も解除される */
    }
    json_free(doc);
    return rc;
}

int main(int argc, char **argv)
{
    rs_log_config_t cfg;
    rs_log_t *log = NULL;
    int rc;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); /* デバイス名などの UTF-8 出力を正しく表示する */
#endif
    rs_log_config_default(&cfg);
    rc = rs_log_open(&log, &cfg);
    if (rc != RS_OK) {
        fprintf(stderr, "server_bridge: failed to open log: %s\n", rs_strerror(rc));
        return 1;
    }
    rs_log_set_default(log);
    rc = run(argc, argv);
    rs_shutdown_done();
    rs_log_close(log);
    return rc;
}
