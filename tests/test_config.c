/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 設定ローダのテスト: リポジトリの設定例ファイルがそのまま読めること、欠落項目のフォールバック、
 * 型・範囲エラー、未知キーの警告、起動前検証。
 * （エラー・警告はログにも出力されるため、このテストの標準エラー出力には意図したメッセージが並ぶ）
 */
#include <string.h>

#include "cfg_load.h"
#include "client_config.h"
#include "json.h"
#include "rs_error.h"
#include "server_config.h"
#include "test_util.h"

#ifndef RSBA_SOURCE_DIR
#define RSBA_SOURCE_DIR "."
#endif

typedef struct srv_case {
    server_app_config_t cfg;
    rs_log_config_t     log;
    json_value_t       *doc;
    int                 errors;
    int                 warnings;
} srv_case_t;

static void srv_apply(srv_case_t *c, const char *text)
{
    json_error_t err;

    server_app_config_default(&c->cfg);
    rs_log_config_default(&c->log);
    c->doc = NULL;
    c->errors = -1;
    c->warnings = -1;
    if (json_parse(text, strlen(text), &c->doc, &err) != RS_OK) {
        fprintf(stderr, "  JSON error in test input: %s\n", err.message);
        return;
    }
    c->errors = server_config_apply(&c->cfg, &c->log, c->doc, "test", &c->warnings);
}

typedef struct cli_case {
    client_app_config_t cfg;
    rs_log_config_t     log;
    json_value_t       *doc;
    int                 errors;
    int                 warnings;
} cli_case_t;

static void cli_apply(cli_case_t *c, const char *text)
{
    client_app_config_default(&c->cfg);
    rs_log_config_default(&c->log);
    c->doc = NULL;
    c->errors = -1;
    if (json_parse(text, strlen(text), &c->doc, NULL) == RS_OK)
        c->errors = client_config_apply(&c->cfg, &c->log, c->doc, "test", &c->warnings);
}

/* ---- リポジトリの設定例 -------------------------------------------------- */

static void test_example_files(void)
{
    srv_case_t s;
    cli_case_t c;
    int warnings = -1;

    rs_log_config_default(&s.log);
    server_app_config_default(&s.cfg);
    s.doc = NULL;
    CHECK(cfg_load_file(RSBA_SOURCE_DIR "/config.server.example.json", &s.doc) == 0);
    CHECK(server_config_apply(&s.cfg, &s.log, s.doc, "config.server.example.json", &warnings) == 0);
    CHECK(warnings == 0);
    CHECK(s.cfg.listen_addr != NULL && strcmp(s.cfg.listen_addr, "::") == 0);
    CHECK(s.cfg.ctl_port == 50001 && s.cfg.civ_port == 50002 && s.cfg.audio_port == 50003);
    CHECK(strcmp(s.cfg.username, "operator") == 0);
    CHECK(s.cfg.pbkdf2_iterations == 100000);
    CHECK(s.cfg.max_failures == 5 && s.cfg.failure_window_ms == 300000 && s.cfg.ban_ms == 900000);
    CHECK(s.cfg.keepalive_interval_ms == 1000 && s.cfg.keepalive_timeout_ms == 5000);
    CHECK(s.cfg.radio_addr == 0x7C && s.cfg.ctrl_addr == 0xE0);
    CHECK(s.cfg.civ_device != NULL && strcmp(s.cfg.civ_device, "/dev/ttyUSB0") == 0);
    CHECK(s.cfg.civ_baud == 19200);
    CHECK(s.cfg.audio_enabled == 1 && strcmp(s.cfg.audio_dev.backend, "auto") == 0);
    CHECK(s.cfg.audio_dev.capture != NULL &&
          strcmp(s.cfg.audio_dev.capture, "plughw:CARD=CODEC,DEV=0") == 0);
    CHECK(s.cfg.audio_link.codec == AUDIO_CODEC_PCM16);
    CHECK(s.cfg.audio_link.frame_samples == 480);
    CHECK(s.cfg.audio_link.jitter_min_ms == 20 && s.cfg.audio_link.jitter_max_ms == 80);
    CHECK(s.cfg.usb_watch == 1 && s.cfg.usb.match.vid == 0x10C4 && s.cfg.usb.match.pid == 0xEA60);
    CHECK(s.cfg.usb.match.serial == NULL && s.cfg.usb.reset_parent == 0);
    CHECK(s.cfg.usb.min_interval_ms == 60000 && s.cfg.usb.rx_garbage_threshold == 4096);
    CHECK(s.log.file_path != NULL && strcmp(s.log.file_path, "/var/log/rsba/server_bridge.log") == 0);
    CHECK(strcmp(s.cfg.usb_log_path, "/var/log/rsba/usb_reset.log") == 0);
    /* 設定例のパスワード CHANGE_ME のままでは起動しない */
    CHECK(server_config_validate(&s.cfg) == 1);
    s.cfg.password = "a-real-secret";
    CHECK(server_config_validate(&s.cfg) == 0);
    json_free(s.doc);

    rs_log_config_default(&c.log);
    client_app_config_default(&c.cfg);
    c.doc = NULL;
    warnings = -1;
    CHECK(cfg_load_file(RSBA_SOURCE_DIR "/config.client.example.json", &c.doc) == 0);
    CHECK(client_config_apply(&c.cfg, &c.log, c.doc, "config.client.example.json", &warnings) == 0);
    CHECK(warnings == 0);
    CHECK(strcmp(c.cfg.server_host, "2001:db8::10") == 0);
    CHECK(c.cfg.bind_addr != NULL && strcmp(c.cfg.bind_addr, "::") == 0);
    CHECK(c.cfg.civ_device != NULL && strcmp(c.cfg.civ_device, "COM10") == 0);
    CHECK(c.cfg.audio_dev.capture == NULL && c.cfg.audio_dev.playback == NULL); /* "default" */
    CHECK(c.cfg.auto_reconnect == 1 && c.cfg.reconnect_interval_ms == 3000);
    CHECK(c.cfg.handshake_timeout_ms == 3000);
    CHECK(c.log.file_path != NULL && strcmp(c.log.file_path, "client_bridge.log") == 0);
    CHECK(client_config_validate(&c.cfg) == 1);
    json_free(c.doc);

    CHECK(cfg_load_file("no-such-config.json", &s.doc) != 0);
}

/* ---- フォールバック ------------------------------------------------------ */

static void test_fallbacks(void)
{
    srv_case_t s;
    cli_case_t c;

    /* 空の設定は既定値のまま（エラーなし） */
    srv_apply(&s, "{}");
    CHECK(s.errors == 0 && s.warnings == 0);
    CHECK(s.cfg.ctl_port == 50001 && s.cfg.civ_baud == 19200 && s.cfg.radio_addr == 0x7C);
    CHECK(s.cfg.civ_device == NULL && s.cfg.usb_watch == 0 && s.cfg.audio_enabled == 1);
    CHECK(s.cfg.audio_link.jitter_min_ms == 20 && s.cfg.audio_link.jitter_max_ms == 80);
    CHECK(s.log.file_path == NULL && s.log.level == RS_LOG_LEVEL_INFO);
    CHECK(strcmp(s.cfg.usb_log_path, "usb_reset.log") == 0);
    json_free(s.doc);

    /* 一部だけ指定・null は未指定扱い・空文字列のデバイスは無効化 */
    srv_apply(&s, "{\"listen\": {\"control_port\": 51001}, \"radio\": {\"serial_device\": \"\","
                  " \"controller_address\": null}, \"audio\": {\"backend\": \"none\"}}");
    CHECK(s.errors == 0);
    CHECK(s.cfg.ctl_port == 51001 && s.cfg.civ_port == 50002);
    CHECK(s.cfg.civ_device == NULL && s.cfg.ctrl_addr == 0xE0);
    CHECK(s.cfg.audio_enabled == 0);
    json_free(s.doc);

    cli_apply(&c, "{\"server\": {\"host\": \"192.0.2.1\"}, \"auth\": {\"username\": \"u\"}}");
    CHECK(c.errors == 0);
    CHECK(c.cfg.ctl_port == 50001 && c.cfg.civ_device == NULL && c.cfg.bind_addr == NULL);
    c.cfg.password = "longenough";
    CHECK(client_config_validate(&c.cfg) == 0);
    json_free(c.doc);
}

/* ---- 型・範囲・値のエラー、未知キーの警告 -------------------------------- */

static void test_errors_and_warnings(void)
{
    static const struct {
        const char *json;
        int errors;
        int warnings;
    } cases[] = {
        { "[]", 1, 0 },                                                      /* ルートが配列 */
        { "{\"listen\": {\"control_port\": \"50001\"}}", 1, 0 },              /* 型違い */
        { "{\"listen\": {\"control_port\": 70000}}", 1, 0 },                  /* 範囲外 */
        { "{\"listen\": {\"control_port\": 50001.5}}", 1, 0 },                /* 小数 */
        { "{\"auth\": []}", 1, 0 },                                         /* セクションがオブジェクトでない */
        { "{\"auth\": {\"usrname\": \"op\"}}", 0, 1 },                        /* 書き間違い → 警告 */
        { "{\"extra\": 1, \"listen\": {\"adress\": \"::\"}}", 0, 2 },
        { "{\"radio\": {\"civ_address\": \"0x1FF\"}}", 1, 0 },
        { "{\"radio\": {\"civ_address\": \"seven\"}}", 1, 0 },
        { "{\"radio\": {\"civ_address\": 124}}", 0, 0 },
        { "{\"radio\": {\"serial_baud\": 300}}", 1, 0 },
        { "{\"radio\": {\"model\": \"IC-7300\"}}", 0, 1 },
        { "{\"audio\": {\"sample_rate\": 44100}}", 1, 0 },
        { "{\"audio\": {\"frame_ms\": 20}}", 1, 0 },
        { "{\"audio\": {\"channels\": 2}}", 1, 0 },
        { "{\"audio\": {\"codec\": \"mp3\"}}", 1, 0 },
        { "{\"audio\": {\"backend\": \"pulse\"}}", 1, 0 },
        { "{\"audio\": {\"jitter_min_ms\": 90, \"jitter_max_ms\": 80}}", 1, 0 },
        { "{\"usb_reset\": {\"vid_pid\": \"zz:1\"}}", 1, 0 },
        { "{\"usb_reset\": {\"method\": \"magic\"}}", 1, 0 },
        { "{\"usb_reset\": {\"enabled\": \"yes\"}}", 1, 0 },
        { "{\"logging\": {\"level\": \"verbose\"}}", 1, 0 },
        { "{\"logging\": {\"max_backups\": -1}}", 1, 0 },
        { "{\"listen\": {\"control_port\": \"x\"}, \"audio\": {\"frame_ms\": 20}}", 2, 0 },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        srv_case_t s;
        srv_apply(&s, cases[i].json);
        if (s.errors != cases[i].errors || s.warnings != cases[i].warnings) {
            fprintf(stderr, "  case %u: %s -> errors=%d warnings=%d (expected %d/%d)\n",
                    (unsigned)i, cases[i].json, s.errors, s.warnings, cases[i].errors,
                    cases[i].warnings);
            g_test_failures++;
        }
        json_free(s.doc);
    }
}

/* ---- 起動前検証（上書き後の相互関係） ------------------------------------ */

static void test_validation(void)
{
    srv_case_t s;
    cli_case_t c;

    srv_apply(&s, "{\"auth\": {\"username\": \"op\", \"password\": \"good-password\"}}");
    CHECK(s.errors == 0);
    CHECK(server_config_validate(&s.cfg) == 0);

    s.cfg.username = NULL;
    CHECK(server_config_validate(&s.cfg) == 1);
    s.cfg.username = "this-user-name-is-longer-than-32-characters";
    CHECK(server_config_validate(&s.cfg) == 1);
    s.cfg.username = "op";

    s.cfg.civ_port = s.cfg.ctl_port;
    CHECK(server_config_validate(&s.cfg) == 1);
    s.cfg.civ_port = 50002;

    s.cfg.keepalive_timeout_ms = 1500; /* 周期 1000 の 2 倍未満 */
    CHECK(server_config_validate(&s.cfg) == 1);
    s.cfg.keepalive_timeout_ms = 5000;

    s.cfg.usb_watch = 1;               /* VID:PID なしの USB 監視 */
    CHECK(server_config_validate(&s.cfg) == 1);
    s.cfg.usb.match.vid = 0x10C4;
    s.cfg.usb.match.pid = 0xEA60;
    CHECK(server_config_validate(&s.cfg) == 0);

    s.cfg.audio_link.jitter_min_ms = 100; /* 引数での上書きで min > max */
    CHECK(server_config_validate(&s.cfg) == 1);
    json_free(s.doc);

    cli_apply(&c, "{}");
    CHECK(client_config_validate(&c.cfg) == 3); /* host・username・password がない */
    c.cfg.server_host = "h";
    c.cfg.username = "u";
    c.cfg.password = "x";
    CHECK(client_config_validate(&c.cfg) == 0);
    {
        char longpw[300];
        memset(longpw, 'p', sizeof(longpw) - 1);
        longpw[sizeof(longpw) - 1] = '\0';
        c.cfg.password = longpw;
        CHECK(client_config_validate(&c.cfg) == 1);
    }
    json_free(c.doc);
}

int main(void)
{
    test_example_files();
    test_fallbacks();
    test_errors_and_warnings();
    test_validation();
    return TEST_RESULT();
}
