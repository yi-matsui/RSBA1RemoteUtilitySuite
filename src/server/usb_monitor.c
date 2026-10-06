/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "usb_monitor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs_error.h"
#include "rs_time.h"

#define LOG_MODULE "usb_reset"

struct usb_monitor {
    usb_monitor_config_t cfg;
    char                 serial[USB_SERIAL_MAX];
    char                 device_id[USB_DEVICE_ID_MAX];
    const usb_backend_t *backend;

    int                  consecutive_timeouts;
    usb_reset_trigger_t  pending_trigger;
    char                 trigger_detail[64];

    int                  was_ready;          /* 直近の確認で使用可能だった */
    uint64_t             not_ready_since;    /* 存在するが使用不可になった時刻（0 = 該当なし） */

    usb_device_t         last_dev;           /* 最後に見つかった監視対象 */
    int                  have_last_dev;
    usb_device_t         last_parent;        /* 最後に取得できた親ハブ */
    int                  have_last_parent;

    uint64_t             last_reset_ms;
    int                  have_reset;
};

static const char *const k_trigger_names[] = {
    "serial_timeout", "device_lost", "rx_garbage", "manual"
};

const char *usb_reset_trigger_name(usb_reset_trigger_t trigger)
{
    if ((int)trigger < 0 || (size_t)trigger >= sizeof(k_trigger_names) / sizeof(k_trigger_names[0]))
        return "?";
    return k_trigger_names[trigger];
}

void usb_monitor_config_default(usb_monitor_config_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->method = USB_RESET_AUTO;
    cfg->reset_parent = 0;
    cfg->serial_timeout_threshold = 3;
    cfg->rx_garbage_threshold = 4096;
    cfg->recovery_timeout_ms = 15000;
    cfg->poll_interval_ms = 250;
    cfg->min_interval_ms = 60000;
    cfg->log = NULL;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

int usb_monitor_create(usb_monitor_t **out, const usb_monitor_config_t *cfg,
                       const usb_backend_t *backend)
{
    usb_monitor_t *mon;
    const usb_match_t *m;

    if (out == NULL || cfg == NULL)
        return RS_ERR_INVALID_ARG;
    *out = NULL;

    m = &cfg->match;
    if (m->vid == 0 && m->pid == 0 && (m->serial == NULL || m->serial[0] == '\0') &&
        (m->device_id == NULL || m->device_id[0] == '\0'))
        return RS_ERR_INVALID_ARG;
    if (cfg->poll_interval_ms == 0)
        return RS_ERR_INVALID_ARG;

    if (backend == NULL)
        backend = usb_backend_platform();
    if (backend->enumerate == NULL || backend->reset == NULL)
        return RS_ERR_INVALID_ARG;

    mon = calloc(1, sizeof(*mon));
    if (mon == NULL)
        return RS_ERR_NO_MEMORY;

    mon->cfg = *cfg;
    copy_str(mon->serial, sizeof(mon->serial), m->serial);
    copy_str(mon->device_id, sizeof(mon->device_id), m->device_id);
    mon->cfg.match.serial = mon->serial;
    mon->cfg.match.device_id = mon->device_id;
    mon->backend = backend;

    *out = mon;
    return RS_OK;
}

void usb_monitor_destroy(usb_monitor_t *mon)
{
    free(mon);
}

usb_reset_trigger_t usb_monitor_pending_trigger(const usb_monitor_t *mon)
{
    return mon != NULL ? mon->pending_trigger : USB_TRIGGER_MANUAL;
}

static int request_reset(usb_monitor_t *mon, usb_reset_trigger_t trigger, const char *detail)
{
    mon->pending_trigger = trigger;
    copy_str(mon->trigger_detail, sizeof(mon->trigger_detail), detail);
    return USB_MONITOR_RESET_REQUIRED;
}

static int rate_limit_ok(const usb_monitor_t *mon, uint64_t now)
{
    return !mon->have_reset || now - mon->last_reset_ms >= mon->cfg.min_interval_ms;
}

void usb_monitor_note_io_ok(usb_monitor_t *mon)
{
    if (mon == NULL)
        return;
    mon->consecutive_timeouts = 0;
}

int usb_monitor_note_io_timeout(usb_monitor_t *mon)
{
    char detail[64];

    if (mon == NULL)
        return RS_ERR_INVALID_ARG;
    mon->consecutive_timeouts++;
    if (mon->cfg.serial_timeout_threshold <= 0 ||
        mon->consecutive_timeouts < mon->cfg.serial_timeout_threshold)
        return RS_OK;
    snprintf(detail, sizeof(detail), "consecutive_timeouts=%d", mon->consecutive_timeouts);
    return request_reset(mon, USB_TRIGGER_SERIAL_TIMEOUT, detail);
}

int usb_monitor_note_rx_garbage(usb_monitor_t *mon, size_t pending_bytes)
{
    char detail[64];

    if (mon == NULL)
        return RS_ERR_INVALID_ARG;
    if (mon->cfg.rx_garbage_threshold == 0 || pending_bytes < mon->cfg.rx_garbage_threshold)
        return RS_OK;
    snprintf(detail, sizeof(detail), "pending_bytes=%lu", (unsigned long)pending_bytes);
    return request_reset(mon, USB_TRIGGER_RX_GARBAGE, detail);
}

static void cache_parent(usb_monitor_t *mon, const usb_device_t *dev)
{
    usb_device_t parent;

    if (!mon->cfg.reset_parent || mon->backend->get_parent == NULL)
        return;
    if (mon->backend->get_parent(mon->backend->ctx, dev, &parent) == RS_OK) {
        mon->last_parent = parent;
        mon->have_last_parent = 1;
    }
}

int usb_monitor_poll(usb_monitor_t *mon)
{
    usb_device_t dev;
    uint64_t now;
    int rc;

    if (mon == NULL)
        return RS_ERR_INVALID_ARG;

    now = rs_time_monotonic_ms();
    rc = usb_find(mon->backend, &mon->cfg.match, &dev);

    if (rc == RS_ERR_NOT_FOUND) {
        mon->not_ready_since = 0;
        /* 使用可能状態からの消失時のみ 1 回要求する。無線機の電源断等で
         * 不在が続く場合に、リセットを繰り返し試みないため */
        if (mon->was_ready) {
            mon->was_ready = 0;
            return request_reset(mon, USB_TRIGGER_DEVICE_LOST, "state=absent");
        }
        return RS_OK;
    }
    if (rc != RS_OK)
        return rc;

    mon->last_dev = dev;
    mon->have_last_dev = 1;

    if (dev.ready) {
        if (!mon->have_last_parent)
            cache_parent(mon, &dev);
        mon->was_ready = 1;
        mon->not_ready_since = 0;
        return RS_OK;
    }

    /* 列挙はされているがドライバが起動していない（Windows のコード 43 等）。
     * 正常な再列挙中の一時状態と区別するため recovery_timeout_ms 継続した場合に要求し、
     * 以後は最小間隔ごとに再要求する */
    mon->was_ready = 0;
    if (mon->not_ready_since == 0)
        mon->not_ready_since = now;
    if (now - mon->not_ready_since >= mon->cfg.recovery_timeout_ms && rate_limit_ok(mon, now))
        return request_reset(mon, USB_TRIGGER_DEVICE_LOST, "state=not_ready");
    return RS_OK;
}

/* 監視対象が使用可能になるまで待つ。elapsed は start からの経過時間 */
static int wait_recovery(usb_monitor_t *mon, uint64_t start, uint32_t *elapsed)
{
    uint64_t deadline = start + mon->cfg.recovery_timeout_ms;

    for (;;) {
        usb_device_t dev;
        uint64_t now;

        if (usb_find(mon->backend, &mon->cfg.match, &dev) == RS_OK && dev.ready) {
            mon->last_dev = dev;
            mon->have_last_dev = 1;
            *elapsed = (uint32_t)(rs_time_monotonic_ms() - start);
            return RS_OK;
        }
        now = rs_time_monotonic_ms();
        if (now >= deadline) {
            *elapsed = (uint32_t)(now - start);
            return RS_ERR_TIMEOUT;
        }
        rs_time_sleep_ms((uint32_t)(deadline - now < mon->cfg.poll_interval_ms
                                        ? deadline - now : mon->cfg.poll_interval_ms));
    }
}

static void log_done(const usb_monitor_t *mon, const usb_reset_result_t *r)
{
    rs_log_write(mon->cfg.log, r->status == RS_OK ? RS_LOG_LEVEL_INFO : RS_LOG_LEVEL_ERROR,
                 LOG_MODULE,
                 "event=reset_done trigger=%s device=%s target=%s result=%s status=\"%s\" "
                 "method=%s attempts=%d recovery_ms=%u total_ms=%u",
                 usb_reset_trigger_name(r->trigger),
                 r->device_id[0] ? r->device_id : "-", r->target_id[0] ? r->target_id : "-",
                 r->status == RS_OK ? "success" : "failure", rs_strerror(r->status),
                 usb_reset_method_name(r->method), r->attempts,
                 (unsigned)r->recovery_ms, (unsigned)r->total_ms);
}

static void finish(usb_monitor_t *mon, usb_reset_result_t *r, usb_reset_result_t *out)
{
    mon->consecutive_timeouts = 0;
    mon->not_ready_since = 0;
    mon->was_ready = (r->status == RS_OK);
    log_done(mon, r);
    if (out != NULL)
        *out = *r;
}

int usb_monitor_reset(usb_monitor_t *mon, usb_reset_trigger_t trigger, usb_reset_result_t *result)
{
    usb_reset_result_t r;
    usb_device_t dev;
    usb_device_t target;
    const usb_reset_method_t *methods;
    size_t n_methods;
    size_t i;
    uint64_t t_begin;
    const char *detail;
    int rc;

    if (mon == NULL)
        return RS_ERR_INVALID_ARG;

    memset(&r, 0, sizeof(r));
    r.trigger = trigger;
    r.method = mon->cfg.method;
    r.status = RS_ERR_SYSTEM;
    detail = (trigger == mon->pending_trigger && mon->trigger_detail[0] != '\0')
                 ? mon->trigger_detail : "-";

    t_begin = rs_time_monotonic_ms();

    /* 監視対象の特定。消失している場合は最後に見えた情報を使う */
    rc = usb_find(mon->backend, &mon->cfg.match, &dev);
    if (rc == RS_OK) {
        mon->last_dev = dev;
        mon->have_last_dev = 1;
    } else if (rc == RS_ERR_NOT_FOUND && mon->have_last_dev) {
        dev = mon->last_dev;
    } else {
        r.status = rc;
        finish(mon, &r, result);
        return rc;
    }
    copy_str(r.device_id, sizeof(r.device_id), dev.id);

    if (!rate_limit_ok(mon, t_begin)) {
        uint64_t wait = mon->cfg.min_interval_ms - (t_begin - mon->last_reset_ms);
        rs_log_write(mon->cfg.log, RS_LOG_LEVEL_WARN, LOG_MODULE,
                     "event=reset_skipped trigger=%s device=%s reason=rate_limited "
                     "next_allowed_in_ms=%u detail=%s",
                     usb_reset_trigger_name(trigger), r.device_id, (unsigned)wait, detail);
        r.status = RS_ERR_BUSY;
        if (result != NULL)
            *result = r;
        return RS_ERR_BUSY;
    }

    /* リセット対象の決定 */
    target = dev;
    if (mon->cfg.reset_parent) {
        usb_device_t parent;

        memset(&parent, 0, sizeof(parent));
        rc = mon->backend->get_parent != NULL
                 ? mon->backend->get_parent(mon->backend->ctx, &dev, &parent)
                 : RS_ERR_UNSUPPORTED;
        if (rc == RS_OK) {
            mon->last_parent = parent;
            mon->have_last_parent = 1;
            target = parent;
        } else if (mon->have_last_parent) {
            target = mon->last_parent;
        } else {
            r.status = rc;
            finish(mon, &r, result);
            return rc;
        }
    }
    copy_str(r.target_id, sizeof(r.target_id), target.id);

    if (mon->cfg.method == USB_RESET_AUTO) {
        methods = mon->backend->auto_methods;
        n_methods = mon->backend->n_auto_methods;
    } else {
        methods = &mon->cfg.method;
        n_methods = 1;
    }
    if (methods == NULL || n_methods == 0) {
        r.status = RS_ERR_UNSUPPORTED;
        finish(mon, &r, result);
        return r.status;
    }

    mon->last_reset_ms = t_begin;
    mon->have_reset = 1;

    rs_log_write(mon->cfg.log, RS_LOG_LEVEL_WARN, LOG_MODULE,
                 "event=reset_start trigger=%s device=%s target=%s method=%s detail=%s",
                 usb_reset_trigger_name(trigger), r.device_id, r.target_id,
                 usb_reset_method_name(mon->cfg.method), detail);

    for (i = 0; i < n_methods; i++) {
        uint64_t t0 = rs_time_monotonic_ms();
        int os_err = 0;

        r.method = methods[i];
        r.attempts++;
        usb_set_last_os_error(0);
        rc = mon->backend->reset(mon->backend->ctx, &target, methods[i]);
        if (rc == RS_OK) {
            rc = wait_recovery(mon, t0, &r.recovery_ms);
        } else {
            os_err = usb_last_os_error();
            r.recovery_ms = (uint32_t)(rs_time_monotonic_ms() - t0);
        }
        r.status = rc;

        rs_log_write(mon->cfg.log, rc == RS_OK ? RS_LOG_LEVEL_INFO : RS_LOG_LEVEL_ERROR,
                     LOG_MODULE,
                     "event=reset_attempt trigger=%s device=%s target=%s method=%s attempt=%d "
                     "result=%s status=\"%s\" recovery_ms=%u os_error=%d",
                     usb_reset_trigger_name(trigger), r.device_id, r.target_id,
                     usb_reset_method_name(methods[i]), r.attempts,
                     rc == RS_OK ? "success" : "failure", rs_strerror(rc),
                     (unsigned)r.recovery_ms, os_err);
        if (rc == RS_OK)
            break;
    }

    r.total_ms = (uint32_t)(rs_time_monotonic_ms() - t_begin);
    finish(mon, &r, result);
    return r.status;
}
