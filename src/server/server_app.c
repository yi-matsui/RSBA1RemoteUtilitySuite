/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "server_app.h"

#include <string.h>

#include "civ.h"
#include "civ_link.h"
#include "ctl_server.h"
#include "net_socket.h"
#include "rs_crypto.h"
#include "rs_error.h"
#include "rs_log.h"
#include "rs_time.h"
#include "serial_port.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define MODULE              "server"
#define LOOP_WAIT_MS        2
#define MAX_PACKETS_PER_SOCK 32
#define SERIAL_RETRY_MS     2000
#define AUDIO_RETRY_MS      5000
#define USB_POLL_MS         1000
#define STATS_LOG_MS        10000

typedef struct app {
    const server_app_config_t *cfg;
    net_udp_t       ctl_sock;
    net_udp_t       civ_sock;
    net_udp_t       aud_sock;
    ctl_server_t   *ctl;
    civ_link_t      civ;
    audio_link_t   *aud;

    serial_port_t   serial;
    uint64_t        serial_retry_at;
    int             serial_warned;
    audio_dev_t     audio;
    uint64_t        audio_retry_at;
    int             audio_warned;

    net_addr_t      civ_peer;
    int             have_civ_peer;
    net_addr_t      aud_peer;
    int             have_aud_peer;

    usb_monitor_t  *usb;
    rs_log_t       *usb_log;
    uint64_t        usb_poll_at;
    uint64_t        last_civ_frames;

    uint64_t        stats_at;
} app_t;

void server_app_config_default(server_app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->pbkdf2_iterations = 100000;
    cfg->ctl_port = 50001;
    cfg->civ_port = 50002;
    cfg->audio_port = 50003;
    {
        ctl_server_config_t d;
        ctl_server_config_default(&d);
        cfg->keepalive_interval_ms = d.keepalive_interval_ms;
        cfg->keepalive_timeout_ms = d.keepalive_timeout_ms;
        cfg->challenge_timeout_ms = d.challenge_timeout_ms;
        cfg->max_failures = d.max_failures;
        cfg->failure_window_ms = d.failure_window_ms;
        cfg->ban_ms = d.ban_ms;
    }
    cfg->civ_baud = 19200;
    cfg->radio_addr = CIV_ADDR_IC9100;
    cfg->ctrl_addr = CIV_ADDR_CONTROLLER;
    cfg->audio_enabled = 1;
    audio_dev_config_default(&cfg->audio_dev);
    audio_link_config_default(&cfg->audio_link);
    usb_monitor_config_default(&cfg->usb);
    cfg->usb_log_path = "usb_reset.log";
}

static const char *fmt(const net_addr_t *a, char *buf)
{
    if (net_addr_format(a, buf, NET_ADDR_STRLEN) != RS_OK)
        strcpy(buf, "?");
    return buf;
}

/* ---- デバイス管理 -------------------------------------------------------- */

static void ptt_off(app_t *app, const char *why)
{
    uint8_t frame[16];
    size_t n = civ_build_ptt_off(frame, sizeof(frame), app->cfg->radio_addr, app->cfg->ctrl_addr);
    int rc = civ_link_write_raw(&app->civ, frame, n);

    if (rc == RS_OK)
        RS_LOG_WARN(MODULE, "PTT release sent to radio (%s)", why);
    else
        RS_LOG_ERROR(MODULE, "PTT release could not be sent (%s): %s", why, rs_strerror(rc));
}

static void serial_close_lost(app_t *app, const char *why)
{
    if (!serial_is_open(&app->serial))
        return;
    RS_LOG_ERROR(MODULE, "CI-V port %s closed: %s", app->cfg->civ_device, why);
    civ_link_attach(&app->civ, NULL);
    serial_close(&app->serial);
    app->serial_retry_at = rs_time_monotonic_ms() + SERIAL_RETRY_MS;
}

static void serial_try_open(app_t *app, uint64_t now)
{
    int rc;

    if (app->cfg->civ_device == NULL || serial_is_open(&app->serial) || now < app->serial_retry_at)
        return;
    rc = serial_open(&app->serial, app->cfg->civ_device, app->cfg->civ_baud);
    if (rc != RS_OK) {
        if (!app->serial_warned)
            RS_LOG_WARN(MODULE, "cannot open CI-V port %s: %s (os_error=%d); retrying",
                        app->cfg->civ_device, rs_strerror(rc), serial_last_os_error());
        app->serial_warned = 1;
        app->serial_retry_at = now + SERIAL_RETRY_MS;
        return;
    }
    app->serial_warned = 0;
    civ_link_attach(&app->civ, &app->serial);
    RS_LOG_INFO(MODULE, "CI-V port %s opened (%u baud)", app->cfg->civ_device,
                (unsigned)app->cfg->civ_baud);
    /* 起動・再接続直後は必ず受信状態にする */
    ptt_off(app, "port opened");
}

static void audio_close_lost(app_t *app, const char *why)
{
    if (!audio_dev_is_open(&app->audio))
        return;
    RS_LOG_ERROR(MODULE, "audio device closed: %s", why);
    audio_link_attach(app->aud, NULL);
    audio_dev_close(&app->audio);
    app->audio_retry_at = rs_time_monotonic_ms() + AUDIO_RETRY_MS;
}

static void audio_try_open(app_t *app, uint64_t now)
{
    int rc;

    if (!app->cfg->audio_enabled || audio_dev_is_open(&app->audio) || now < app->audio_retry_at)
        return;
    rc = audio_dev_open(&app->audio, &app->cfg->audio_dev);
    if (rc != RS_OK) {
        if (!app->audio_warned)
            RS_LOG_WARN(MODULE, "cannot open audio device (backend=%s capture=%s playback=%s): %s;"
                        " retrying", app->cfg->audio_dev.backend,
                        app->cfg->audio_dev.capture ? app->cfg->audio_dev.capture : "default",
                        app->cfg->audio_dev.playback ? app->cfg->audio_dev.playback : "default",
                        rs_strerror(rc));
        app->audio_warned = 1;
        app->audio_retry_at = now + AUDIO_RETRY_MS;
        return;
    }
    app->audio_warned = 0;
    audio_link_attach(app->aud, &app->audio);
    RS_LOG_INFO(MODULE, "audio device opened (backend=%s)", app->cfg->audio_dev.backend);
}

static void usb_reset_now(app_t *app)
{
    usb_reset_result_t res;
    int rc;

    /* リセット前にハンドルを閉じる（Windows では開いたままだと無効化が拒否される） */
    ptt_off(app, "before USB reset");
    serial_close_lost(app, "USB reset");
    audio_close_lost(app, "USB reset");

    rc = usb_monitor_reset(app->usb, usb_monitor_pending_trigger(app->usb), &res);
    if (rc == RS_OK)
        RS_LOG_INFO(MODULE, "USB reset completed in %u ms", (unsigned)res.total_ms);
    else
        RS_LOG_ERROR(MODULE, "USB reset failed: %s", rs_strerror(rc));

    app->serial_retry_at = 0;
    app->audio_retry_at = 0;
    serial_try_open(app, rs_time_monotonic_ms());
    audio_try_open(app, rs_time_monotonic_ms());
}

/* ---- フック -------------------------------------------------------------- */

static int ctl_send(void *user, const net_addr_t *to, const void *buf, size_t len)
{
    app_t *app = user;
    return net_udp_send(&app->ctl_sock, buf, len, to);
}

static int civ_send(void *user, const void *pkt, size_t len)
{
    app_t *app = user;
    if (!app->have_civ_peer)
        return RS_ERR_NOT_FOUND;
    return net_udp_send(&app->civ_sock, pkt, len, &app->civ_peer);
}

static int aud_send(void *user, const void *pkt, size_t len)
{
    app_t *app = user;
    if (!app->have_aud_peer)
        return RS_ERR_NOT_FOUND;
    return net_udp_send(&app->aud_sock, pkt, len, &app->aud_peer);
}

static void hook_ptt_release(void *user)
{
    ptt_off(user, "session end");
}

static void hook_session_start(void *user, uint32_t sid, const net_addr_t *peer)
{
    app_t *app = user;
    uint8_t c2s[CTL_KEY_LEN];
    uint8_t s2c[CTL_KEY_LEN];

    (void)peer;
    if (ctl_server_session_keys(app->ctl, c2s, s2c) != RS_OK)
        return;
    app->have_civ_peer = 0;
    app->have_aud_peer = 0;
    civ_link_start(&app->civ, DCH_ROLE_SERVER, sid, c2s, s2c);
    if (app->aud != NULL)
        audio_link_start(app->aud, DCH_ROLE_SERVER, sid, c2s, s2c);
    rs_crypto_wipe(c2s, sizeof(c2s));
    rs_crypto_wipe(s2c, sizeof(s2c));
}

static void hook_session_end(void *user, uint32_t sid, ctl_end_reason_t reason)
{
    app_t *app = user;

    (void)sid;
    (void)reason;
    civ_link_stop(&app->civ);
    if (app->aud != NULL)
        audio_link_stop(app->aud);
    app->have_civ_peer = 0;
    app->have_aud_peer = 0;
}

/* ---- 受信 ---------------------------------------------------------------- */

static void learn_peer(net_addr_t *peer, int *have, const net_addr_t *from, const char *what)
{
    char abuf[NET_ADDR_STRLEN];

    if (*have && net_addr_equal(peer, from))
        return;
    *peer = *from;
    *have = 1;
    RS_LOG_INFO(MODULE, "%s peer: %s", what, fmt(from, abuf));
}

static void drain(app_t *app, int which, uint64_t now)
{
    net_udp_t *sock = which == 0 ? &app->ctl_sock : which == 1 ? &app->civ_sock : &app->aud_sock;
    int i;

    for (i = 0; i < MAX_PACKETS_PER_SOCK; i++) {
        uint8_t buf[NET_MAX_UDP_PAYLOAD];
        net_addr_t from;
        size_t len = 0;
        uint8_t type = 0;
        int rc = net_udp_recv(sock, buf, sizeof(buf), &len, &from, 0);

        if (rc != RS_OK)
            break;
        if (which == 0) {
            ctl_server_handle_packet(app->ctl, &from, buf, len, now);
        } else if (which == 1) {
            rc = civ_link_handle_packet(&app->civ, buf, len, &type);
            if (rc == RS_OK || rc == RS_ERR_TIMEOUT || rc == RS_ERR_IO)
                learn_peer(&app->civ_peer, &app->have_civ_peer, &from, "CI-V");
            if (rc == RS_ERR_TIMEOUT && app->usb != NULL &&
                usb_monitor_note_io_timeout(app->usb) == USB_MONITOR_RESET_REQUIRED)
                usb_reset_now(app);
            else if (rc == RS_ERR_IO)
                serial_close_lost(app, "write error");
        } else if (app->aud != NULL) {
            if (audio_link_handle_packet(app->aud, buf, len, now, &type) == RS_OK)
                learn_peer(&app->aud_peer, &app->have_aud_peer, &from, "audio");
        }
    }
}

/* ---- 周期処理 ------------------------------------------------------------ */

static void periodic(app_t *app, uint64_t now)
{
    serial_try_open(app, now);
    audio_try_open(app, now);

    if (app->usb != NULL && now >= app->usb_poll_at) {
        app->usb_poll_at = now + USB_POLL_MS;
        if (app->civ.framer.frames != app->last_civ_frames) {
            app->last_civ_frames = app->civ.framer.frames;
            usb_monitor_note_io_ok(app->usb);
        }
        if (usb_monitor_note_rx_garbage(app->usb, app->civ.framer.garbage_pending) ==
                USB_MONITOR_RESET_REQUIRED ||
            usb_monitor_poll(app->usb) == USB_MONITOR_RESET_REQUIRED)
            usb_reset_now(app);
    }

    if (now >= app->stats_at) {
        app->stats_at = now + STATS_LOG_MS;
        if (ctl_server_state(app->ctl) == CTL_SERVER_ACTIVE) {
            audio_link_stats_t as;
            jbuf_stats_t js;
            memset(&as, 0, sizeof(as));
            memset(&js, 0, sizeof(js));
            if (app->aud != NULL)
                audio_link_get_stats(app->aud, &as, &js);
            RS_LOG_DEBUG(MODULE, "civ to_net=%u from_net=%u | audio sent=%u recv=%u "
                         "jitter=%ums burst=%ums target=%ums level=%ums underruns=%llu concealed=%llu",
                         app->civ.stats.frames_to_net, app->civ.stats.frames_from_net,
                         as.frames_sent, as.frames_received, js.jitter_ms, js.extra_ms, js.target_ms,
                         js.level_ms, (unsigned long long)js.underruns,
                         (unsigned long long)js.frames_concealed);
        }
    }
}

/* ---- 実行 ---------------------------------------------------------------- */

static int open_sock(net_udp_t *s, const char *addr, uint16_t port, const char *what)
{
    net_addr_t a;
    int rc = net_addr_resolve(&a, addr, port);

    if (rc == RS_OK)
        rc = net_udp_open(s, &a, NULL);
    if (rc != RS_OK) {
        RS_LOG_ERROR(MODULE, "cannot listen on %s port %u (%s): %s (os_error=%d)",
                     addr != NULL ? addr : "::", (unsigned)port, what, rs_strerror(rc),
                     net_last_os_error());
        return rc;
    }
    return RS_OK;
}

int server_app_run(const server_app_config_t *cfg, volatile sig_atomic_t *stop)
{
    app_t app;
    ctl_server_config_t scfg;
    ctl_server_hooks_t hooks;
    net_udp_t *socks[3];
    int rc;

    memset(&app, 0, sizeof(app));
    app.cfg = cfg;
    /* 無効値で初期化しておき、途中で失敗しても後始末の close を安全にする */
    app.ctl_sock.fd = app.civ_sock.fd = app.aud_sock.fd = (net_os_socket_t)-1;

    rc = net_init();
    if (rc != RS_OK)
        return 1;
    rc = open_sock(&app.ctl_sock, cfg->listen_addr, cfg->ctl_port, "control");
    if (rc == RS_OK)
        rc = open_sock(&app.civ_sock, cfg->listen_addr, cfg->civ_port, "CI-V");
    if (rc == RS_OK)
        rc = open_sock(&app.aud_sock, cfg->listen_addr, cfg->audio_port, "audio");
    if (rc != RS_OK)
        goto out;

    civ_link_init(&app.civ, civ_send, &app);
    if (cfg->audio_enabled) {
        rc = audio_link_create(&app.aud, &cfg->audio_link, aud_send, &app);
        if (rc != RS_OK) {
            RS_LOG_ERROR(MODULE, "audio init failed (codec=%s): %s",
                         audio_codec_name(cfg->audio_link.codec), rs_strerror(rc));
            goto out;
        }
    }

    if (cfg->usb_watch) {
        rs_log_config_t lcfg;
        usb_monitor_config_t ucfg = cfg->usb;
        rs_log_config_default(&lcfg);
        lcfg.file_path = cfg->usb_log_path;
        lcfg.to_stderr = 0;
        rc = rs_log_open(&app.usb_log, &lcfg);
        if (rc == RS_OK) {
            ucfg.log = app.usb_log;
            rc = usb_monitor_create(&app.usb, &ucfg, NULL);
        }
        if (rc != RS_OK) {
            RS_LOG_ERROR(MODULE, "USB watch init failed: %s", rs_strerror(rc));
            goto out;
        }
        RS_LOG_INFO(MODULE, "USB watch enabled for %04X:%04X (log: %s)", cfg->usb.match.vid,
                    cfg->usb.match.pid, cfg->usb_log_path);
    }

    ctl_server_config_default(&scfg);
    scfg.username = cfg->username;
    scfg.password = cfg->password;
    scfg.pbkdf2_iterations = cfg->pbkdf2_iterations;
    scfg.keepalive_interval_ms = cfg->keepalive_interval_ms;
    scfg.keepalive_timeout_ms = cfg->keepalive_timeout_ms;
    scfg.challenge_timeout_ms = cfg->challenge_timeout_ms;
    scfg.max_failures = cfg->max_failures;
    scfg.failure_window_ms = cfg->failure_window_ms;
    scfg.ban_ms = cfg->ban_ms;
    memset(&hooks, 0, sizeof(hooks));
    hooks.user = &app;
    hooks.send = ctl_send;
    hooks.ptt_release = hook_ptt_release;
    hooks.session_start = hook_session_start;
    hooks.session_end = hook_session_end;
    RS_LOG_INFO(MODULE, "deriving key (PBKDF2, %u iterations)...",
                (unsigned)cfg->pbkdf2_iterations);
    rc = ctl_server_create(&app.ctl, &scfg, &hooks);
    if (rc != RS_OK) {
        RS_LOG_ERROR(MODULE, "control init failed: %s", rs_strerror(rc));
        goto out;
    }

    serial_try_open(&app, rs_time_monotonic_ms());
    audio_try_open(&app, rs_time_monotonic_ms());
    RS_LOG_INFO(MODULE, "listening on ports %u/%u/%u (control/CI-V/audio), user=%s",
                (unsigned)cfg->ctl_port, (unsigned)cfg->civ_port, (unsigned)cfg->audio_port,
                cfg->username);

#ifdef _WIN32
    timeBeginPeriod(1); /* 待機の分解能を 1ms にして音声・CI-V の遅延を抑える */
#endif
    socks[0] = &app.ctl_sock;
    socks[1] = &app.civ_sock;
    socks[2] = &app.aud_sock;
    while (!*stop) {
        int ready[3];
        uint64_t now;
        int i;

        rc = net_udp_wait(socks, 3, LOOP_WAIT_MS, ready);
        now = rs_time_monotonic_ms();
        if (rc == RS_OK)
            for (i = 0; i < 3; i++)
                if (ready[i])
                    drain(&app, i, now);

        if (civ_link_poll(&app.civ) == RS_ERR_IO)
            serial_close_lost(&app, "read error");
        if (app.aud != NULL && audio_link_poll(app.aud) == RS_ERR_IO)
            audio_close_lost(&app, "I/O error");
        ctl_server_tick(app.ctl, now);
        periodic(&app, now);
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    RS_LOG_INFO(MODULE, "shutting down");
    rc = RS_OK;

out:
    /* シリアルを閉じる前に制御を破棄し、PTT 解除をシリアルへ送る */
    ctl_server_destroy(app.ctl);
    if (app.aud != NULL) {
        audio_link_attach(app.aud, NULL);
        audio_link_destroy(app.aud);
    }
    audio_dev_close(&app.audio);
    civ_link_attach(&app.civ, NULL);
    serial_close(&app.serial);
    usb_monitor_destroy(app.usb);
    rs_log_close(app.usb_log);
    net_udp_close(&app.ctl_sock);
    net_udp_close(&app.civ_sock);
    net_udp_close(&app.aud_sock);
    net_cleanup();
    return rc == RS_OK ? 0 : 1;
}
