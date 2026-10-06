/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * usb_monitor のテスト。モックバックエンドでリセット・復帰・エスカレーション・
 * レート制限・専用ログ出力を検証する（実デバイスには触れない）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs_error.h"
#include "rs_log.h"
#include "rs_time.h"
#include "test_util.h"
#include "usb_device.h"
#include "usb_monitor.h"

#define LOG_PATH "test_usb_monitor.log"

/* ---- モックバックエンド -------------------------------------------------- */

typedef struct mock {
    usb_device_t       dev;               /* 監視対象 (10C4:EA60, "1-1.2") */
    usb_device_t       other;             /* 無関係なデバイス */
    usb_device_t       parent;            /* 親ハブ ("1-1") */
    int                present;
    int                parent_supported;
    int                reset_calls;
    usb_reset_method_t methods[8];
    char               last_target[USB_DEVICE_ID_MAX];
    int                reset_rc[8];       /* 呼び出し毎の戻り値 */
    int                recover_on_call;   /* この回のリセット後に復帰（0 = 復帰しない） */
    int                polls_until_ready; /* 復帰までに not ready を返す列挙回数 */
    int                recovering;
    int                polls_remaining;
} mock_t;

static int mock_enumerate(void *ctx, usb_enum_cb cb, void *user)
{
    mock_t *m = ctx;

    if (cb(&m->other, user) != 0)
        return RS_OK;
    if (m->present) {
        if (m->recovering) {
            if (m->polls_remaining > 0) {
                m->polls_remaining--;
            } else {
                m->dev.ready = 1;
                m->recovering = 0;
            }
        }
        cb(&m->dev, user);
    }
    return RS_OK;
}

static int mock_get_parent(void *ctx, const usb_device_t *dev, usb_device_t *parent)
{
    mock_t *m = ctx;

    (void)dev;
    if (!m->parent_supported)
        return RS_ERR_UNSUPPORTED;
    *parent = m->parent;
    return RS_OK;
}

static int mock_reset(void *ctx, const usb_device_t *target, usb_reset_method_t method)
{
    mock_t *m = ctx;
    int idx = m->reset_calls++;

    if (idx < 8)
        m->methods[idx] = method;
    strcpy(m->last_target, target->id);
    if (idx < 8 && m->reset_rc[idx] != RS_OK) {
        usb_set_last_os_error(5);
        return m->reset_rc[idx];
    }
    m->dev.ready = 0;
    m->present = 1;
    if (m->recover_on_call == idx + 1) {
        m->recovering = 1;
        m->polls_remaining = m->polls_until_ready;
    }
    return RS_OK;
}

static const usb_reset_method_t k_mock_auto[] = { USB_RESET_USBDEVFS, USB_RESET_AUTHORIZED };

static void mock_init(mock_t *m, usb_backend_t *be)
{
    memset(m, 0, sizeof(*m));
    strcpy(m->dev.id, "1-1.2");
    m->dev.vid = 0x10C4;
    m->dev.pid = 0xEA60;
    m->dev.ready = 1;
    strcpy(m->other.id, "1-3");
    m->other.vid = 0x046D;
    m->other.pid = 0xC52B;
    m->other.ready = 1;
    strcpy(m->parent.id, "1-1");
    m->parent.vid = 0x0424;
    m->parent.pid = 0x2514;
    m->parent.ready = 1;
    m->present = 1;
    m->parent_supported = 1;
    m->recover_on_call = 1;
    m->polls_until_ready = 2;

    memset(be, 0, sizeof(*be));
    be->name = "mock";
    be->ctx = m;
    be->enumerate = mock_enumerate;
    be->get_parent = mock_get_parent;
    be->reset = mock_reset;
    be->auto_methods = k_mock_auto;
    be->n_auto_methods = 2;
}

/* ---- ヘルパ -------------------------------------------------------------- */

static rs_log_t *open_log(void)
{
    rs_log_config_t cfg;
    rs_log_t *log = NULL;

    remove(LOG_PATH);
    rs_log_config_default(&cfg);
    cfg.file_path = LOG_PATH;
    cfg.to_stderr = 0;
    cfg.level = RS_LOG_LEVEL_TRACE;
    CHECK_RC(rs_log_open(&log, &cfg), RS_OK);
    return log;
}

/* ログを閉じて内容を返す（呼び出し側で free） */
static char *close_and_read_log(rs_log_t *log)
{
    FILE *fp;
    char *buf;
    long size;

    rs_log_close(log);
    fp = fopen(LOG_PATH, "rb");
    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)size + 1);
    if (buf != NULL) {
        size = (long)fread(buf, 1, (size_t)size, fp);
        buf[size] = '\0';
    }
    fclose(fp);
    return buf;
}

static int count_occurrences(const char *text, const char *needle)
{
    int n = 0;
    const char *p = text;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

static void test_config(usb_monitor_config_t *cfg, rs_log_t *log)
{
    usb_monitor_config_default(cfg);
    cfg->match.vid = 0x10C4;
    cfg->match.pid = 0xEA60;
    cfg->recovery_timeout_ms = 200;
    cfg->poll_interval_ms = 5;
    cfg->log = log;
}

/* ---- テスト -------------------------------------------------------------- */

static void test_detection_counters(void)
{
    mock_t m;
    usb_backend_t be;
    usb_monitor_config_t cfg;
    usb_monitor_t *mon = NULL;

    mock_init(&m, &be);
    test_config(&cfg, NULL);
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);

    CHECK_RC(usb_monitor_note_io_timeout(mon), RS_OK);
    CHECK_RC(usb_monitor_note_io_timeout(mon), RS_OK);
    CHECK_RC(usb_monitor_note_io_timeout(mon), USB_MONITOR_RESET_REQUIRED);
    CHECK(usb_monitor_pending_trigger(mon) == USB_TRIGGER_SERIAL_TIMEOUT);

    usb_monitor_note_io_ok(mon);
    CHECK_RC(usb_monitor_note_io_timeout(mon), RS_OK);

    CHECK_RC(usb_monitor_note_rx_garbage(mon, 100), RS_OK);
    CHECK_RC(usb_monitor_note_rx_garbage(mon, 4096), USB_MONITOR_RESET_REQUIRED);
    CHECK(usb_monitor_pending_trigger(mon) == USB_TRIGGER_RX_GARBAGE);

    CHECK(m.reset_calls == 0);
    usb_monitor_destroy(mon);
}

static void test_reset_success_and_rate_limit(void)
{
    mock_t m;
    usb_backend_t be;
    usb_monitor_config_t cfg;
    usb_monitor_t *mon = NULL;
    usb_reset_result_t res;
    rs_log_t *log = open_log();
    char *text;

    mock_init(&m, &be);
    test_config(&cfg, log);
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);

    usb_monitor_note_io_timeout(mon);
    usb_monitor_note_io_timeout(mon);
    CHECK_RC(usb_monitor_note_io_timeout(mon), USB_MONITOR_RESET_REQUIRED);

    CHECK_RC(usb_monitor_reset(mon, usb_monitor_pending_trigger(mon), &res), RS_OK);
    CHECK(m.reset_calls == 1);
    CHECK(m.methods[0] == USB_RESET_USBDEVFS);
    CHECK(strcmp(m.last_target, "1-1.2") == 0);
    CHECK(res.status == RS_OK);
    CHECK(res.attempts == 1);
    CHECK(res.method == USB_RESET_USBDEVFS);
    CHECK(res.trigger == USB_TRIGGER_SERIAL_TIMEOUT);
    CHECK(strcmp(res.device_id, "1-1.2") == 0);
    CHECK(strcmp(res.target_id, "1-1.2") == 0);
    CHECK(res.recovery_ms <= res.total_ms);

    /* カウンタはリセット後にクリアされる */
    CHECK_RC(usb_monitor_note_io_timeout(mon), RS_OK);

    /* 最小間隔内の再実行は行わない */
    CHECK_RC(usb_monitor_reset(mon, USB_TRIGGER_MANUAL, &res), RS_ERR_BUSY);
    CHECK(m.reset_calls == 1);

    usb_monitor_destroy(mon);
    text = close_and_read_log(log);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(strstr(text, "event=reset_start trigger=serial_timeout device=1-1.2 target=1-1.2 "
                           "method=auto detail=consecutive_timeouts=3") != NULL);
        CHECK(strstr(text, "event=reset_attempt trigger=serial_timeout device=1-1.2 target=1-1.2 "
                           "method=usbdevfs attempt=1 result=success") != NULL);
        CHECK(strstr(text, "event=reset_done trigger=serial_timeout device=1-1.2 target=1-1.2 "
                           "result=success") != NULL);
        CHECK(strstr(text, "recovery_ms=") != NULL);
        CHECK(strstr(text, "event=reset_skipped trigger=manual device=1-1.2 reason=rate_limited") != NULL);
        free(text);
    }
}

static void test_escalation(void)
{
    mock_t m;
    usb_backend_t be;
    usb_monitor_config_t cfg;
    usb_monitor_t *mon = NULL;
    usb_reset_result_t res;
    rs_log_t *log = open_log();
    char *text;

    mock_init(&m, &be);
    m.recover_on_call = 2; /* 1 つ目の方式では復帰しない */
    test_config(&cfg, log);
    cfg.recovery_timeout_ms = 50;
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);

    CHECK_RC(usb_monitor_reset(mon, USB_TRIGGER_DEVICE_LOST, &res), RS_OK);
    CHECK(m.reset_calls == 2);
    CHECK(m.methods[0] == USB_RESET_USBDEVFS);
    CHECK(m.methods[1] == USB_RESET_AUTHORIZED);
    CHECK(res.attempts == 2);
    CHECK(res.method == USB_RESET_AUTHORIZED);
    CHECK(res.total_ms >= 50);

    usb_monitor_destroy(mon);
    text = close_and_read_log(log);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(strstr(text, "method=usbdevfs attempt=1 result=failure status=\"timed out\"") != NULL);
        CHECK(strstr(text, "method=authorized attempt=2 result=success") != NULL);
        CHECK(strstr(text, "result=success status=\"success\" method=authorized attempts=2") != NULL);
        free(text);
    }
}

static void test_never_recovers(void)
{
    mock_t m;
    usb_backend_t be;
    usb_monitor_config_t cfg;
    usb_monitor_t *mon = NULL;
    usb_reset_result_t res;
    rs_log_t *log = open_log();
    char *text;

    mock_init(&m, &be);
    m.recover_on_call = 0;
    test_config(&cfg, log);
    cfg.recovery_timeout_ms = 30;
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);

    CHECK_RC(usb_monitor_reset(mon, USB_TRIGGER_MANUAL, &res), RS_ERR_TIMEOUT);
    CHECK(res.attempts == 2);
    CHECK(res.recovery_ms >= 30);

    usb_monitor_destroy(mon);
    text = close_and_read_log(log);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(count_occurrences(text, "event=reset_attempt") == 2);
        CHECK(strstr(text, "event=reset_done trigger=manual device=1-1.2 target=1-1.2 "
                           "result=failure status=\"timed out\"") != NULL);
        CHECK(strstr(text, " ERROR [usb_reset] event=reset_done") != NULL);
        free(text);
    }
}

static void test_backend_error(void)
{
    mock_t m;
    usb_backend_t be;
    usb_monitor_config_t cfg;
    usb_monitor_t *mon = NULL;
    usb_reset_result_t res;
    rs_log_t *log = open_log();
    char *text;

    mock_init(&m, &be);
    m.reset_rc[0] = RS_ERR_PERMISSION;
    m.reset_rc[1] = RS_ERR_PERMISSION;
    test_config(&cfg, log);
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);

    CHECK_RC(usb_monitor_reset(mon, USB_TRIGGER_MANUAL, &res), RS_ERR_PERMISSION);
    CHECK(m.reset_calls == 2);

    usb_monitor_destroy(mon);
    text = close_and_read_log(log);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(strstr(text, "status=\"permission denied\" recovery_ms=") != NULL);
        CHECK(strstr(text, "os_error=5") != NULL);
        free(text);
    }
}

static void test_explicit_method_and_parent(void)
{
    mock_t m;
    usb_backend_t be;
    usb_monitor_config_t cfg;
    usb_monitor_t *mon = NULL;
    usb_reset_result_t res;

    /* 方式指定時はその方式のみ */
    mock_init(&m, &be);
    test_config(&cfg, NULL);
    cfg.method = USB_RESET_AUTHORIZED;
    cfg.min_interval_ms = 0;
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);
    CHECK_RC(usb_monitor_reset(mon, USB_TRIGGER_MANUAL, &res), RS_OK);
    CHECK(m.reset_calls == 1);
    CHECK(m.methods[0] == USB_RESET_AUTHORIZED);
    usb_monitor_destroy(mon);

    /* 親ハブのリセット */
    mock_init(&m, &be);
    test_config(&cfg, NULL);
    cfg.reset_parent = 1;
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);
    CHECK_RC(usb_monitor_reset(mon, USB_TRIGGER_MANUAL, &res), RS_OK);
    CHECK(strcmp(m.last_target, "1-1") == 0);
    CHECK(strcmp(res.device_id, "1-1.2") == 0);
    CHECK(strcmp(res.target_id, "1-1") == 0);
    usb_monitor_destroy(mon);

    /* 親がルートハブ等で許可されない場合はリセットしない */
    mock_init(&m, &be);
    m.parent_supported = 0;
    test_config(&cfg, NULL);
    cfg.reset_parent = 1;
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);
    CHECK_RC(usb_monitor_reset(mon, USB_TRIGGER_MANUAL, &res), RS_ERR_UNSUPPORTED);
    CHECK(m.reset_calls == 0);
    usb_monitor_destroy(mon);
}

static void test_device_absent(void)
{
    mock_t m;
    usb_backend_t be;
    usb_monitor_config_t cfg;
    usb_monitor_t *mon = NULL;
    usb_reset_result_t res;

    /* 一度も見えていないデバイスはリセットできない */
    mock_init(&m, &be);
    m.present = 0;
    test_config(&cfg, NULL);
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);
    CHECK_RC(usb_monitor_reset(mon, USB_TRIGGER_MANUAL, &res), RS_ERR_NOT_FOUND);
    CHECK(m.reset_calls == 0);
    usb_monitor_destroy(mon);

    /* 消失前に見えていれば、最後の ID でリセットを試みる */
    mock_init(&m, &be);
    test_config(&cfg, NULL);
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);
    CHECK_RC(usb_monitor_poll(mon), RS_OK);
    m.present = 0;
    CHECK_RC(usb_monitor_reset(mon, USB_TRIGGER_DEVICE_LOST, &res), RS_OK);
    CHECK(m.reset_calls == 1);
    CHECK(strcmp(res.device_id, "1-1.2") == 0);
    usb_monitor_destroy(mon);
}

static void test_poll(void)
{
    mock_t m;
    usb_backend_t be;
    usb_monitor_config_t cfg;
    usb_monitor_t *mon = NULL;

    mock_init(&m, &be);
    test_config(&cfg, NULL);
    cfg.recovery_timeout_ms = 40;
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_OK);

    /* 起動時に不在でも要求しない */
    m.present = 0;
    CHECK_RC(usb_monitor_poll(mon), RS_OK);

    /* 使用可能 → 消失で 1 回だけ要求 */
    m.present = 1;
    CHECK_RC(usb_monitor_poll(mon), RS_OK);
    m.present = 0;
    CHECK_RC(usb_monitor_poll(mon), USB_MONITOR_RESET_REQUIRED);
    CHECK(usb_monitor_pending_trigger(mon) == USB_TRIGGER_DEVICE_LOST);
    CHECK_RC(usb_monitor_poll(mon), RS_OK);

    /* 存在するが使用不可の状態が recovery_timeout_ms 続いたら要求 */
    m.present = 1;
    m.dev.ready = 0;
    CHECK_RC(usb_monitor_poll(mon), RS_OK);
    rs_time_sleep_ms(60);
    CHECK_RC(usb_monitor_poll(mon), USB_MONITOR_RESET_REQUIRED);

    usb_monitor_destroy(mon);
}

static void test_parsing(void)
{
    uint16_t vid = 0;
    uint16_t pid = 0;
    usb_reset_method_t method = USB_RESET_AUTO;
    usb_monitor_config_t cfg;
    usb_monitor_t *mon = NULL;
    mock_t m;
    usb_backend_t be;

    CHECK_RC(usb_parse_vid_pid("10c4:EA60", &vid, &pid), RS_OK);
    CHECK(vid == 0x10C4 && pid == 0xEA60);
    CHECK_RC(usb_parse_vid_pid("zz:1", &vid, &pid), RS_ERR_INVALID_ARG);
    CHECK_RC(usb_parse_vid_pid("10C4", &vid, &pid), RS_ERR_INVALID_ARG);
    CHECK_RC(usb_parse_vid_pid("10C45:1", &vid, &pid), RS_ERR_INVALID_ARG);

    CHECK_RC(usb_reset_method_from_string("devnode", &method), RS_OK);
    CHECK(method == USB_RESET_DEVNODE);
    CHECK_RC(usb_reset_method_from_string("reboot", &method), RS_ERR_INVALID_ARG);
    CHECK(strcmp(usb_reset_method_name(USB_RESET_USBDEVFS), "usbdevfs") == 0);
    CHECK(strcmp(usb_reset_trigger_name(USB_TRIGGER_RX_GARBAGE), "rx_garbage") == 0);

    /* 検索条件が空の設定は拒否 */
    mock_init(&m, &be);
    usb_monitor_config_default(&cfg);
    CHECK_RC(usb_monitor_create(&mon, &cfg, &be), RS_ERR_INVALID_ARG);
    CHECK(mon == NULL);
}

int main(void)
{
    test_detection_counters();
    test_reset_success_and_rate_limit();
    test_escalation();
    test_never_recovers();
    test_backend_error();
    test_explicit_method_and_parent();
    test_device_absent();
    test_poll();
    test_parsing();
    remove(LOG_PATH);
    return TEST_RESULT();
}
