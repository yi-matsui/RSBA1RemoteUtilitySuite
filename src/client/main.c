/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * client_bridge エントリポイント（操作端末側）。
 * サーバに接続して認証し、CI-V（操作ソフトの仮想 COM 等）とオーディオを中継する。
 * 設定の優先順位: 既定値 < 設定ファイル（--config、既定 config.client.json）< 環境変数
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
#include "client_app.h"
#include "client_config.h"
#include "json.h"
#include "rs_error.h"
#include "rs_log.h"
#include "rs_shutdown.h"

#define MODULE "main"

static volatile sig_atomic_t g_stop;

static void usage(void)
{
    printf("usage: client_bridge [--config FILE] [options]\n"
           "  settings come from FILE (default: ./config.client.json if present); options override it.\n"
           "  password: auth.password in FILE, or environment variable RSBA_PASSWORD (takes precedence)\n"
           "  --config FILE              configuration file (JSON)\n"
           "  --check-config             validate the configuration and exit\n"
           "  --server HOST              server address or host name\n"
           "  --user NAME                account name\n"
           "  --port N                   control port; CI-V = N+1, audio = N+2 (default: 50001)\n"
           "  --bind ADDR                local source address (default: ::)\n"
           "  --no-reconnect             do not reconnect automatically\n"
           "  --civ-device NAME          local serial port for the control software (e.g. COM10)\n"
           "  --civ-baud N               baud rate (default: 19200)\n"
           "  --audio-backend B          auto|winmm|alsa|null|none (default: auto)\n"
           "  --audio-capture NAME       capture device (substring match)\n"
           "  --audio-playback NAME      playback device\n"
           "  --audio-codec C            pcm (default) | opus (not built yet)\n"
           "  --jitter-min MS / --jitter-max MS   adaptive jitter buffer range (default: 20 / 80)\n"
           "  --list-audio               list audio devices and exit\n"
           "  --log-level LEVEL          trace|debug|info|warn|error (default: info)\n"
           "  --log-file PATH            also write the log to PATH (rotated)\n"
           "  --help\n");
}

static void print_audio(const char *backend, int is_capture, const char *name, void *user)
{
    (void)user;
    printf("%-6s %-8s %s\n", backend, is_capture ? "capture" : "playback", name);
}

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
    client_app_config_t cfg;
    rs_log_config_t lcfg;
    json_value_t *doc = NULL;
    rs_log_t *log = NULL;
    const char *config_path;
    const char *env_password;
    uint32_t port = 0;
    int check_only = 0;
    int rc = 2;
    int i;

    client_app_config_default(&cfg);
    rs_log_config_default(&lcfg);

    /* 1. 設定ファイル */
    config_path = find_config_arg(argc, argv);
    if (config_path == NULL && file_exists(CLIENT_CONFIG_DEFAULT_PATH))
        config_path = CLIENT_CONFIG_DEFAULT_PATH;
    if (config_path != NULL) {
        int warnings = 0;
        if (cfg_load_file(config_path, &doc) != 0)
            return 2;
        if (client_config_apply(&cfg, &lcfg, doc, config_path, &warnings) != 0) {
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
        } else if (strcmp(a, "--no-reconnect") == 0) {
            cfg.auto_reconnect = 0;
            continue;
        } else if (strcmp(a, "--check-config") == 0) {
            check_only = 1;
            continue;
        } else if (strcmp(a, "--list-audio") == 0) {
            audio_dev_list(print_audio, NULL);
            rc = 0;
            goto out;
        } else if (next == NULL) {
            fprintf(stderr, "unknown or incomplete option: %s\n", a);
            goto out;
        }

        i++;
        if (strcmp(a, "--config") == 0) {
            /* 読み込み済み */
        } else if (strcmp(a, "--server") == 0) {
            cfg.server_host = next;
        } else if (strcmp(a, "--user") == 0) {
            cfg.username = next;
        } else if (strcmp(a, "--port") == 0) {
            bad = !parse_u32(next, 1, 65533, &port);
        } else if (strcmp(a, "--bind") == 0) {
            cfg.bind_addr = next;
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

    /* 4. 最終検証 */
    if (client_config_validate(&cfg) != 0) {
        RS_LOG_ERROR(MODULE, "configuration is invalid; not starting (see --help)");
        goto out;
    }
    if (check_only) {
        RS_LOG_INFO(MODULE, "configuration OK");
        rc = 0;
        goto out;
    }

    /* 5. 設定どおりのロガーに切り替える */
    if (lcfg.file_path != NULL) {
        int lrc = rs_log_open(&log, &lcfg);
        if (lrc != RS_OK) {
            RS_LOG_ERROR(MODULE, "cannot open log file %s: %s", lcfg.file_path, rs_strerror(lrc));
            goto out;
        }
        rs_log_set_default(log);
    }

    RS_LOG_INFO(MODULE, "client_bridge %s starting", RSBA_VERSION);
    rs_shutdown_install(&g_stop);
    rc = client_app_run(&cfg, &g_stop);

out:
    rs_log_close(log);
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
        fprintf(stderr, "client_bridge: failed to open log: %s\n", rs_strerror(rc));
        return 1;
    }
    rs_log_set_default(log);
    rc = run(argc, argv);
    rs_shutdown_done();
    rs_log_close(log);
    return rc;
}
