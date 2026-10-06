/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "ctl_client.h"

#include <stdlib.h>
#include <string.h>

#include "rs_crypto.h"
#include "rs_error.h"

#define LOG_MODULE "ctl"
#define PASSWORD_MAX 256

/* サーバ通知値の許容範囲（改ざんはタグで防ぐが、設定ミスへの防御） */
#define KEEPALIVE_INTERVAL_MIN 100
#define KEEPALIVE_INTERVAL_MAX 60000

struct ctl_client {
    ctl_client_config_t cfg;
    ctl_client_hooks_t  hooks;
    char                username[CTL_USERNAME_MAX + 1];
    char                password[PASSWORD_MAX + 1];

    ctl_client_state_t  state;
    uint64_t            state_since;
    uint8_t             cn[CTL_NONCE_LEN];
    uint8_t             sn[CTL_NONCE_LEN];

    /* PBKDF2 の結果をソルト・反復回数ごとにキャッシュ（再接続時の再計算を避ける） */
    int                 have_key;
    uint8_t             key_salt[CTL_SALT_LEN];
    uint32_t            key_iterations;
    uint8_t             key[CTL_KEY_LEN];

    uint32_t            sid;
    uint8_t             k_c2s[CTL_KEY_LEN];
    uint8_t             k_s2c[CTL_KEY_LEN];
    uint32_t            tx_seq;
    ctl_replay_t        rx;
    uint64_t            last_rx;
    uint64_t            last_ping;
    uint32_t            keepalive_interval_ms;
    uint32_t            keepalive_timeout_ms;
    uint32_t            last_rtt_ms;

    int                 reconnect_pending;
    uint64_t            reconnect_at;
};

static const char *const k_end_names[] = {
    "user", "server", "replaced", "timeout", "handshake_timeout", "auth_failed", "server_proof"
};
static const char *const k_state_names[] = { "idle", "hello_sent", "auth_sent", "connected" };

const char *ctl_client_end_name(ctl_client_end_t reason)
{
    if ((int)reason < 0 || (size_t)reason >= sizeof(k_end_names) / sizeof(k_end_names[0]))
        return "?";
    return k_end_names[reason];
}

const char *ctl_client_state_name(ctl_client_state_t state)
{
    if ((int)state < 0 || (size_t)state >= sizeof(k_state_names) / sizeof(k_state_names[0]))
        return "?";
    return k_state_names[state];
}

void ctl_client_config_default(ctl_client_config_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->server.sa.sin6_family = AF_INET6;
    cfg->handshake_timeout_ms = 3000;
    cfg->auto_reconnect = 1;
    cfg->reconnect_interval_ms = 3000;
    cfg->max_pbkdf2_iterations = 1000000;
}

int ctl_client_create(ctl_client_t **out, const ctl_client_config_t *cfg,
                      const ctl_client_hooks_t *hooks)
{
    ctl_client_t *cli;
    size_t ulen;
    size_t plen;

    if (out == NULL || cfg == NULL || hooks == NULL || hooks->send == NULL)
        return RS_ERR_INVALID_ARG;
    *out = NULL;
    if (cfg->username == NULL || cfg->password == NULL)
        return RS_ERR_INVALID_ARG;
    ulen = strlen(cfg->username);
    plen = strlen(cfg->password);
    if (ulen == 0 || ulen > CTL_USERNAME_MAX || plen == 0 || plen > PASSWORD_MAX ||
        cfg->handshake_timeout_ms == 0)
        return RS_ERR_INVALID_ARG;

    cli = calloc(1, sizeof(*cli));
    if (cli == NULL)
        return RS_ERR_NO_MEMORY;
    cli->cfg = *cfg;
    cli->hooks = *hooks;
    memcpy(cli->username, cfg->username, ulen + 1);
    memcpy(cli->password, cfg->password, plen + 1);
    cli->cfg.username = cli->username;
    cli->cfg.password = NULL;
    *out = cli;
    return RS_OK;
}

static void send_packet(ctl_client_t *cli, uint8_t type, uint32_t sid, uint32_t seq,
                        const void *payload, uint16_t payload_len, const uint8_t *key)
{
    uint8_t buf[CTL_MAX_PACKET_LEN];
    ctl_header_t h;
    size_t len;

    h.type = type;
    h.payload_len = payload_len;
    h.session_id = sid;
    h.seq = seq;
    if (ctl_encode(buf, sizeof(buf), &h, payload, key, &len) == RS_OK)
        cli->hooks.send(cli->hooks.user, &cli->cfg.server, buf, len);
}

static void send_session_packet(ctl_client_t *cli, uint8_t type, const void *payload,
                                uint16_t payload_len)
{
    send_packet(cli, type, cli->sid, ++cli->tx_seq, payload, payload_len, cli->k_c2s);
}

static void wipe_session(ctl_client_t *cli)
{
    cli->sid = 0;
    cli->tx_seq = 0;
    rs_crypto_wipe(cli->k_c2s, sizeof(cli->k_c2s));
    rs_crypto_wipe(cli->k_s2c, sizeof(cli->k_s2c));
    rs_crypto_wipe(cli->cn, sizeof(cli->cn));
    rs_crypto_wipe(cli->sn, sizeof(cli->sn));
    ctl_replay_init(&cli->rx);
}

/* IDLE に戻り、必要なら再接続を予約して通知する */
static void teardown(ctl_client_t *cli, ctl_client_end_t reason, uint64_t now)
{
    int reconnect = cli->cfg.auto_reconnect &&
                    (reason == CTL_CLIENT_END_SERVER || reason == CTL_CLIENT_END_TIMEOUT ||
                     reason == CTL_CLIENT_END_HANDSHAKE_TIMEOUT);

    wipe_session(cli);
    cli->state = CTL_CLIENT_IDLE;
    cli->state_since = now;
    cli->reconnect_pending = reconnect;
    cli->reconnect_at = now + cli->cfg.reconnect_interval_ms;

    rs_log_write(cli->cfg.log,
                 reason == CTL_CLIENT_END_USER ? RS_LOG_LEVEL_INFO : RS_LOG_LEVEL_WARN,
                 LOG_MODULE, "disconnected: reason=%s%s", ctl_client_end_name(reason),
                 reconnect ? " (reconnect scheduled)" : "");
    if (cli->hooks.disconnected != NULL)
        cli->hooks.disconnected(cli->hooks.user, reason);
}

static int send_hello(ctl_client_t *cli, uint64_t now)
{
    uint8_t payload[CTL_HELLO_PAYLOAD_LEN];
    size_t ulen = strlen(cli->username);

    if (rs_crypto_random(cli->cn, sizeof(cli->cn)) != RS_OK)
        return RS_ERR_SYSTEM;

    memset(payload, 0, sizeof(payload));
    memcpy(payload, cli->cn, CTL_NONCE_LEN);
    payload[CTL_NONCE_LEN] = (uint8_t)ulen;
    memcpy(payload + CTL_NONCE_LEN + 1, cli->username, ulen);
    send_packet(cli, CTL_HELLO, 0, 0, payload, sizeof(payload), NULL);

    cli->state = CTL_CLIENT_HELLO_SENT;
    cli->state_since = now;
    cli->reconnect_pending = 0;
    return RS_OK;
}

int ctl_client_connect(ctl_client_t *cli, uint64_t now_ms)
{
    if (cli == NULL)
        return RS_ERR_INVALID_ARG;
    if (cli->state != CTL_CLIENT_IDLE)
        return RS_ERR_BUSY;
    return send_hello(cli, now_ms);
}

void ctl_client_disconnect(ctl_client_t *cli, uint64_t now_ms)
{
    if (cli == NULL)
        return;
    if (cli->state == CTL_CLIENT_CONNECTED) {
        uint8_t r = CTL_DISC_NORMAL;
        send_session_packet(cli, CTL_DISCONNECT, &r, 1);
    }
    if (cli->state != CTL_CLIENT_IDLE) {
        teardown(cli, CTL_CLIENT_END_USER, now_ms);
    }
    cli->reconnect_pending = 0;
}

void ctl_client_destroy(ctl_client_t *cli)
{
    if (cli == NULL)
        return;
    if (cli->state == CTL_CLIENT_CONNECTED) {
        uint8_t r = CTL_DISC_SHUTDOWN;
        send_session_packet(cli, CTL_DISCONNECT, &r, 1);
    }
    rs_crypto_wipe(cli, sizeof(*cli));
    free(cli);
}

/* ---- 受信 ---------------------------------------------------------------- */

static void handle_challenge(ctl_client_t *cli, const uint8_t *p)
{
    const uint8_t *sn = p;
    const uint8_t *cn_echo = p + CTL_NONCE_LEN;
    const uint8_t *salt = p + CTL_NONCE_LEN * 2;
    uint32_t iterations = ctl_get_u32(p + CTL_NONCE_LEN * 2 + CTL_SALT_LEN);
    uint8_t payload[CTL_AUTH_PAYLOAD_LEN];

    if (cli->state != CTL_CLIENT_HELLO_SENT || memcmp(cn_echo, cli->cn, CTL_NONCE_LEN) != 0)
        return; /* 古い・無関係なチャレンジ */
    if (iterations == 0 || iterations > cli->cfg.max_pbkdf2_iterations) {
        rs_log_write(cli->cfg.log, RS_LOG_LEVEL_WARN, LOG_MODULE,
                     "challenge rejected: iterations=%u", (unsigned)iterations);
        return;
    }

    if (!cli->have_key || cli->key_iterations != iterations ||
        memcmp(cli->key_salt, salt, CTL_SALT_LEN) != 0) {
        if (ctl_derive_password_key(cli->password, salt, iterations, cli->key) != RS_OK)
            return;
        memcpy(cli->key_salt, salt, CTL_SALT_LEN);
        cli->key_iterations = iterations;
        cli->have_key = 1;
    }

    memcpy(cli->sn, sn, CTL_NONCE_LEN);
    memcpy(payload, cli->cn, CTL_NONCE_LEN);
    memcpy(payload + CTL_NONCE_LEN, cli->sn, CTL_NONCE_LEN);
    ctl_client_proof(cli->key, cli->cn, cli->sn, cli->username, payload + CTL_NONCE_LEN * 2);
    send_packet(cli, CTL_AUTH, 0, 0, payload, sizeof(payload), NULL);
    cli->state = CTL_CLIENT_AUTH_SENT;
}

static void handle_auth_ok(ctl_client_t *cli, const ctl_header_t *h, const uint8_t *data,
                           size_t len, const uint8_t *p, uint64_t now)
{
    uint8_t k_c2s[CTL_KEY_LEN];
    uint8_t k_s2c[CTL_KEY_LEN];
    uint8_t expect[CTL_PROOF_LEN];
    uint32_t interval;
    uint32_t timeout;

    if (cli->state != CTL_CLIENT_AUTH_SENT || h->session_id == 0)
        return;

    ctl_session_keys(cli->key, cli->cn, cli->sn, h->session_id, k_c2s, k_s2c);
    if (!ctl_verify_tag(data, len, k_s2c)) {
        /* 偽造パケットの可能性。ハンドシェイクタイムアウトに任せる */
        rs_crypto_wipe(k_c2s, sizeof(k_c2s));
        rs_crypto_wipe(k_s2c, sizeof(k_s2c));
        return;
    }
    ctl_server_proof(cli->key, cli->cn, cli->sn, h->session_id, expect);
    if (!rs_crypto_equal(expect, p, CTL_PROOF_LEN)) {
        rs_crypto_wipe(k_c2s, sizeof(k_c2s));
        rs_crypto_wipe(k_s2c, sizeof(k_s2c));
        teardown(cli, CTL_CLIENT_END_SERVER_PROOF, now);
        return;
    }

    interval = ctl_get_u32(p + CTL_PROOF_LEN);
    timeout = ctl_get_u32(p + CTL_PROOF_LEN + 4);
    if (interval < KEEPALIVE_INTERVAL_MIN)
        interval = KEEPALIVE_INTERVAL_MIN;
    if (interval > KEEPALIVE_INTERVAL_MAX)
        interval = KEEPALIVE_INTERVAL_MAX;
    if (timeout < interval * 2)
        timeout = interval * 2;

    cli->sid = h->session_id;
    memcpy(cli->k_c2s, k_c2s, CTL_KEY_LEN);
    memcpy(cli->k_s2c, k_s2c, CTL_KEY_LEN);
    rs_crypto_wipe(k_c2s, sizeof(k_c2s));
    rs_crypto_wipe(k_s2c, sizeof(k_s2c));
    cli->tx_seq = 0;
    ctl_replay_init(&cli->rx);
    ctl_replay_update(&cli->rx, h->seq);
    cli->keepalive_interval_ms = interval;
    cli->keepalive_timeout_ms = timeout;
    cli->last_rx = now;
    cli->last_ping = now;
    cli->state = CTL_CLIENT_CONNECTED;
    cli->state_since = now;

    rs_log_write(cli->cfg.log, RS_LOG_LEVEL_INFO, LOG_MODULE,
                 "connected: session=%08x keepalive=%ums timeout=%ums", (unsigned)cli->sid,
                 (unsigned)interval, (unsigned)timeout);
    if (cli->hooks.connected != NULL)
        cli->hooks.connected(cli->hooks.user, cli->sid);
}

static void handle_session(ctl_client_t *cli, const ctl_header_t *h, const uint8_t *data,
                           size_t len, const uint8_t *p, uint64_t now)
{
    if (cli->state != CTL_CLIENT_CONNECTED || h->session_id != cli->sid)
        return;
    if (!ctl_replay_check(&cli->rx, h->seq) || !ctl_verify_tag(data, len, cli->k_s2c))
        return;
    ctl_replay_update(&cli->rx, h->seq);
    cli->last_rx = now;

    if (h->type == CTL_PONG) {
        uint64_t sent = ctl_get_u64(p);
        if (sent <= now)
            cli->last_rtt_ms = (uint32_t)(now - sent);
    } else if (h->type == CTL_DISCONNECT) {
        teardown(cli, p[0] == CTL_DISC_REPLACED ? CTL_CLIENT_END_REPLACED : CTL_CLIENT_END_SERVER,
                 now);
    }
}

void ctl_client_handle_packet(ctl_client_t *cli, const net_addr_t *from,
                              const uint8_t *data, size_t len, uint64_t now_ms)
{
    ctl_header_t h;
    const uint8_t *p;

    if (cli == NULL || from == NULL || data == NULL)
        return;
    if (!net_addr_equal(from, &cli->cfg.server))
        return;
    if (ctl_decode(data, len, &h, &p) != RS_OK)
        return;

    switch (h.type) {
    case CTL_CHALLENGE:
        handle_challenge(cli, p);
        break;
    case CTL_AUTH_OK:
        handle_auth_ok(cli, &h, data, len, p, now_ms);
        break;
    case CTL_AUTH_FAIL:
        if (cli->state == CTL_CLIENT_HELLO_SENT || cli->state == CTL_CLIENT_AUTH_SENT) {
            if (p[0] == CTL_FAIL_PROTOCOL)
                teardown(cli, CTL_CLIENT_END_HANDSHAKE_TIMEOUT, now_ms); /* 再試行可能 */
            else
                teardown(cli, CTL_CLIENT_END_AUTH_FAILED, now_ms);
        }
        break;
    case CTL_PONG:
    case CTL_DISCONNECT:
        handle_session(cli, &h, data, len, p, now_ms);
        break;
    default:
        break;
    }
}

void ctl_client_tick(ctl_client_t *cli, uint64_t now_ms)
{
    if (cli == NULL)
        return;

    switch (cli->state) {
    case CTL_CLIENT_IDLE:
        if (cli->reconnect_pending && now_ms >= cli->reconnect_at)
            send_hello(cli, now_ms);
        break;
    case CTL_CLIENT_HELLO_SENT:
    case CTL_CLIENT_AUTH_SENT:
        if (now_ms - cli->state_since >= cli->cfg.handshake_timeout_ms)
            teardown(cli, CTL_CLIENT_END_HANDSHAKE_TIMEOUT, now_ms);
        break;
    case CTL_CLIENT_CONNECTED:
        if (now_ms - cli->last_rx >= cli->keepalive_timeout_ms) {
            teardown(cli, CTL_CLIENT_END_TIMEOUT, now_ms);
        } else if (now_ms - cli->last_ping >= cli->keepalive_interval_ms) {
            uint8_t ts[CTL_PING_PAYLOAD_LEN];
            ctl_put_u64(ts, now_ms);
            send_session_packet(cli, CTL_PING, ts, sizeof(ts));
            cli->last_ping = now_ms;
        }
        break;
    }
}

ctl_client_state_t ctl_client_state(const ctl_client_t *cli)
{
    return cli != NULL ? cli->state : CTL_CLIENT_IDLE;
}

uint32_t ctl_client_session_id(const ctl_client_t *cli)
{
    return cli != NULL ? cli->sid : 0;
}

uint32_t ctl_client_last_rtt_ms(const ctl_client_t *cli)
{
    return cli != NULL ? cli->last_rtt_ms : 0;
}

int ctl_client_session_keys(const ctl_client_t *cli, uint8_t k_c2s[CTL_KEY_LEN],
                            uint8_t k_s2c[CTL_KEY_LEN])
{
    if (cli == NULL || k_c2s == NULL || k_s2c == NULL)
        return RS_ERR_INVALID_ARG;
    if (cli->state != CTL_CLIENT_CONNECTED)
        return RS_ERR_NOT_FOUND;
    memcpy(k_c2s, cli->k_c2s, CTL_KEY_LEN);
    memcpy(k_s2c, cli->k_s2c, CTL_KEY_LEN);
    return RS_OK;
}
