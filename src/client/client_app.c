/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "client_app.h"

#include <string.h>

#include "civ_link.h"
#include "ctl_client.h"
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

#define MODULE              "client"
#define LOOP_WAIT_MS        2
#define MAX_PACKETS_PER_SOCK 32
#define BIND_INTERVAL_MS    1000
#define SERIAL_RETRY_MS     2000
#define AUDIO_RETRY_MS      5000
#define STATS_LOG_MS        10000

typedef struct app {
    const client_app_config_t *cfg;
    net_udp_t      ctl_sock;
    net_udp_t      civ_sock;
    net_udp_t      aud_sock;
    net_addr_t     civ_addr;    /* サーバの CI-V ポート */
    net_addr_t     aud_addr;    /* サーバのオーディオポート */
    ctl_client_t  *ctl;
    civ_link_t     civ;
    audio_link_t  *aud;

    serial_port_t  serial;
    uint64_t       serial_retry_at;
    int            serial_warned;
    audio_dev_t    audio;
    uint64_t       audio_retry_at;
    int            audio_warned;

    uint64_t       bind_at;
    uint64_t       stats_at;
    int            fatal;       /* 再接続しない理由で切断された */
} app_t;

void client_app_config_default(client_app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->ctl_port = 50001;
    cfg->civ_port = 50002;
    cfg->audio_port = 50003;
    cfg->auto_reconnect = 1;
    cfg->handshake_timeout_ms = 3000;
    cfg->reconnect_interval_ms = 3000;
    cfg->civ_baud = 19200;
    cfg->audio_enabled = 1;
    audio_dev_config_default(&cfg->audio_dev);
    audio_link_config_default(&cfg->audio_link);
}

/* ---- デバイス管理 -------------------------------------------------------- */

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
            RS_LOG_WARN(MODULE, "cannot open audio device (backend=%s): %s; retrying",
                        app->cfg->audio_dev.backend, rs_strerror(rc));
        app->audio_warned = 1;
        app->audio_retry_at = now + AUDIO_RETRY_MS;
        return;
    }
    app->audio_warned = 0;
    audio_link_attach(app->aud, &app->audio);
    RS_LOG_INFO(MODULE, "audio device opened (backend=%s)", app->cfg->audio_dev.backend);
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
    return net_udp_send(&app->civ_sock, pkt, len, &app->civ_addr);
}

static int aud_send(void *user, const void *pkt, size_t len)
{
    app_t *app = user;
    return net_udp_send(&app->aud_sock, pkt, len, &app->aud_addr);
}

static void send_binds(app_t *app)
{
    civ_link_send_bind(&app->civ);
    if (app->aud != NULL)
        audio_link_send_bind(app->aud);
}

static void hook_connected(void *user, uint32_t sid)
{
    app_t *app = user;
    uint8_t c2s[CTL_KEY_LEN];
    uint8_t s2c[CTL_KEY_LEN];

    if (ctl_client_session_keys(app->ctl, c2s, s2c) != RS_OK)
        return;
    civ_link_start(&app->civ, DCH_ROLE_CLIENT, sid, c2s, s2c);
    if (app->aud != NULL)
        audio_link_start(app->aud, DCH_ROLE_CLIENT, sid, c2s, s2c);
    rs_crypto_wipe(c2s, sizeof(c2s));
    rs_crypto_wipe(s2c, sizeof(s2c));
    /* サーバにデータチャネルの送信元アドレスを登録する */
    send_binds(app);
    app->bind_at = rs_time_monotonic_ms() + BIND_INTERVAL_MS;
}

static void hook_disconnected(void *user, ctl_client_end_t reason)
{
    app_t *app = user;

    civ_link_stop(&app->civ);
    if (app->aud != NULL)
        audio_link_stop(app->aud);
    if (reason == CTL_CLIENT_END_AUTH_FAILED || reason == CTL_CLIENT_END_SERVER_PROOF ||
        reason == CTL_CLIENT_END_REPLACED)
        app->fatal = 1;
}

/* ---- 受信 ---------------------------------------------------------------- */

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
            ctl_client_handle_packet(app->ctl, &from, buf, len, now);
        } else if (which == 1) {
            if (!net_addr_equal(&from, &app->civ_addr))
                continue;
            if (civ_link_handle_packet(&app->civ, buf, len, &type) == RS_ERR_IO)
                serial_close_lost(app, "write error");
        } else if (app->aud != NULL) {
            if (net_addr_equal(&from, &app->aud_addr))
                audio_link_handle_packet(app->aud, buf, len, now, &type);
        }
    }
}

static void periodic(app_t *app, uint64_t now)
{
    serial_try_open(app, now);
    audio_try_open(app, now);

    if (ctl_client_state(app->ctl) == CTL_CLIENT_CONNECTED && now >= app->bind_at) {
        app->bind_at = now + BIND_INTERVAL_MS;
        send_binds(app);
    }
    if (now >= app->stats_at) {
        app->stats_at = now + STATS_LOG_MS;
        if (ctl_client_state(app->ctl) == CTL_CLIENT_CONNECTED) {
            audio_link_stats_t as;
            jbuf_stats_t js;
            memset(&as, 0, sizeof(as));
            memset(&js, 0, sizeof(js));
            if (app->aud != NULL)
                audio_link_get_stats(app->aud, &as, &js);
            RS_LOG_DEBUG(MODULE, "rtt=%ums | civ to_net=%u from_net=%u | audio sent=%u recv=%u "
                         "jitter=%ums burst=%ums target=%ums level=%ums underruns=%llu concealed=%llu",
                         (unsigned)ctl_client_last_rtt_ms(app->ctl),
                         app->civ.stats.frames_to_net, app->civ.stats.frames_from_net,
                         as.frames_sent, as.frames_received, js.jitter_ms, js.extra_ms, js.target_ms,
                         js.level_ms, (unsigned long long)js.underruns,
                         (unsigned long long)js.frames_concealed);
        }
    }
}

/* ---- 実行 ---------------------------------------------------------------- */

int client_app_run(const client_app_config_t *cfg, volatile sig_atomic_t *stop)
{
    app_t app;
    ctl_client_config_t ccfg;
    ctl_client_hooks_t hooks;
    net_udp_t *socks[3];
    char abuf[NET_ADDR_STRLEN];
    int result = 2;
    int rc;

    memset(&app, 0, sizeof(app));
    app.cfg = cfg;
    app.ctl_sock.fd = app.civ_sock.fd = app.aud_sock.fd = (net_os_socket_t)-1;

    if (net_init() != RS_OK)
        return 2;

    ctl_client_config_default(&ccfg);
    rc = net_addr_resolve(&ccfg.server, cfg->server_host, cfg->ctl_port);
    if (rc == RS_OK) {
        app.civ_addr = ccfg.server;
        app.civ_addr.sa.sin6_port = htons(cfg->civ_port);
        app.aud_addr = ccfg.server;
        app.aud_addr.sa.sin6_port = htons(cfg->audio_port);
    } else {
        RS_LOG_ERROR(MODULE, "cannot resolve %s: %s", cfg->server_host, rs_strerror(rc));
        goto out;
    }

    {
        net_addr_t local;
        rc = net_addr_resolve(&local, cfg->bind_addr, 0);
        if (rc == RS_OK)
            rc = net_udp_open(&app.ctl_sock, &local, NULL);
        if (rc == RS_OK)
            rc = net_udp_open(&app.civ_sock, &local, NULL);
        if (rc == RS_OK)
            rc = net_udp_open(&app.aud_sock, &local, NULL);
        if (rc != RS_OK) {
            RS_LOG_ERROR(MODULE, "socket open failed (bind %s): %s",
                         cfg->bind_addr != NULL ? cfg->bind_addr : "::", rs_strerror(rc));
            goto out;
        }
    }

    civ_link_init(&app.civ, civ_send, &app);
    if (cfg->audio_enabled) {
        rc = audio_link_create(&app.aud, &cfg->audio_link, aud_send, &app);
        if (rc != RS_OK) {
            RS_LOG_ERROR(MODULE, "audio init failed (codec=%s): %s",
                         audio_codec_name(cfg->audio_link.codec), rs_strerror(rc));
            goto out;
        }
    }

    ccfg.username = cfg->username;
    ccfg.password = cfg->password;
    ccfg.auto_reconnect = cfg->auto_reconnect;
    ccfg.handshake_timeout_ms = cfg->handshake_timeout_ms;
    ccfg.reconnect_interval_ms = cfg->reconnect_interval_ms;
    memset(&hooks, 0, sizeof(hooks));
    hooks.user = &app;
    hooks.send = ctl_send;
    hooks.connected = hook_connected;
    hooks.disconnected = hook_disconnected;
    rc = ctl_client_create(&app.ctl, &ccfg, &hooks);
    if (rc != RS_OK) {
        RS_LOG_ERROR(MODULE, "control init failed: %s", rs_strerror(rc));
        goto out;
    }

    serial_try_open(&app, rs_time_monotonic_ms());
    audio_try_open(&app, rs_time_monotonic_ms());
    net_addr_format(&ccfg.server, abuf, sizeof(abuf));
    RS_LOG_INFO(MODULE, "connecting to %s as %s", abuf, cfg->username);
    ctl_client_connect(app.ctl, rs_time_monotonic_ms());

#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    socks[0] = &app.ctl_sock;
    socks[1] = &app.civ_sock;
    socks[2] = &app.aud_sock;
    result = 0;
    while (!*stop && !app.fatal) {
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
        ctl_client_tick(app.ctl, now);
        if (!cfg->auto_reconnect && ctl_client_state(app.ctl) == CTL_CLIENT_IDLE)
            break;
        periodic(&app, now);
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    if (app.fatal)
        result = 1;

out:
    if (app.ctl != NULL) {
        ctl_client_disconnect(app.ctl, rs_time_monotonic_ms());
        ctl_client_destroy(app.ctl);
    }
    if (app.aud != NULL) {
        audio_link_attach(app.aud, NULL);
        audio_link_destroy(app.aud);
    }
    audio_dev_close(&app.audio);
    civ_link_stop(&app.civ);
    civ_link_attach(&app.civ, NULL);
    serial_close(&app.serial);
    net_udp_close(&app.ctl_sock);
    net_udp_close(&app.civ_sock);
    net_udp_close(&app.aud_sock);
    net_cleanup();
    return result;
}
