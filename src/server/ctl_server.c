/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "ctl_server.h"

#include <stdlib.h>
#include <string.h>

#include "ban_list.h"
#include "rs_crypto.h"
#include "rs_error.h"

#define LOG_MODULE  "ctl"
#define PENDING_MAX 16

typedef struct pending {
    int        used;
    net_addr_t peer;
    uint8_t    cn[CTL_NONCE_LEN];
    uint8_t    sn[CTL_NONCE_LEN];
    int        user_ok;
    uint64_t   created;
} pending_t;

struct ctl_server {
    ctl_server_config_t cfg;
    ctl_server_hooks_t  hooks;
    char                username[CTL_USERNAME_MAX + 1];
    uint8_t             salt[CTL_SALT_LEN];
    uint8_t             key[CTL_KEY_LEN];         /* PBKDF2(password, salt) */
    uint8_t             decoy_secret[CTL_KEY_LEN]; /* 未知ユーザ用の偽ソルト生成鍵 */
    ban_list_t          bans;
    pending_t           pending[PENDING_MAX];

    /* セッション */
    int                 active;
    uint32_t            sid;
    net_addr_t          peer;
    uint8_t             k_c2s[CTL_KEY_LEN];
    uint8_t             k_s2c[CTL_KEY_LEN];
    uint32_t            tx_seq;
    ctl_replay_t        rx;
    uint64_t            last_rx;

    ctl_server_stats_t  stats;
};

static const char *const k_end_names[] = { "client_disconnect", "timeout", "replaced", "shutdown" };

const char *ctl_end_reason_name(ctl_end_reason_t reason)
{
    if ((int)reason < 0 || (size_t)reason >= sizeof(k_end_names) / sizeof(k_end_names[0]))
        return "?";
    return k_end_names[reason];
}

void ctl_server_config_default(ctl_server_config_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->pbkdf2_iterations = 100000;
    cfg->keepalive_interval_ms = 1000;
    cfg->keepalive_timeout_ms = 5000;
    cfg->challenge_timeout_ms = 5000;
    cfg->max_failures = 5;
    cfg->failure_window_ms = 300000;
    cfg->ban_ms = 900000;
}

static const char *fmt_addr(const net_addr_t *a, char *buf)
{
    if (net_addr_format(a, buf, NET_ADDR_STRLEN) != RS_OK)
        strcpy(buf, "?");
    return buf;
}

int ctl_server_create(ctl_server_t **out, const ctl_server_config_t *cfg,
                      const ctl_server_hooks_t *hooks)
{
    ctl_server_t *srv;
    ban_list_config_t bcfg;
    size_t ulen;
    int rc;

    if (out == NULL || cfg == NULL || hooks == NULL || hooks->send == NULL)
        return RS_ERR_INVALID_ARG;
    *out = NULL;
    if (cfg->username == NULL || cfg->password == NULL || cfg->password[0] == '\0' ||
        cfg->pbkdf2_iterations == 0 || cfg->keepalive_timeout_ms == 0)
        return RS_ERR_INVALID_ARG;
    ulen = strlen(cfg->username);
    if (ulen == 0 || ulen > CTL_USERNAME_MAX)
        return RS_ERR_INVALID_ARG;

    srv = calloc(1, sizeof(*srv));
    if (srv == NULL)
        return RS_ERR_NO_MEMORY;

    srv->cfg = *cfg;
    srv->cfg.password = NULL; /* 鍵導出後は参照しない */
    memcpy(srv->username, cfg->username, ulen + 1);
    srv->cfg.username = srv->username;
    srv->hooks = *hooks;

    rc = rs_crypto_random(srv->salt, sizeof(srv->salt));
    if (rc == RS_OK)
        rc = rs_crypto_random(srv->decoy_secret, sizeof(srv->decoy_secret));
    if (rc == RS_OK)
        rc = ctl_derive_password_key(cfg->password, srv->salt, cfg->pbkdf2_iterations, srv->key);
    if (rc != RS_OK) {
        rs_crypto_wipe(srv, sizeof(*srv));
        free(srv);
        return rc;
    }

    bcfg.max_failures = cfg->max_failures;
    bcfg.failure_window_ms = cfg->failure_window_ms;
    bcfg.ban_ms = cfg->ban_ms;
    ban_list_init(&srv->bans, &bcfg);

    *out = srv;
    return RS_OK;
}

/* ---- 送信 ---------------------------------------------------------------- */

static void send_packet(ctl_server_t *srv, const net_addr_t *to, uint8_t type, uint32_t sid,
                        uint32_t seq, const void *payload, uint16_t payload_len,
                        const uint8_t *key)
{
    uint8_t buf[CTL_MAX_PACKET_LEN];
    ctl_header_t h;
    size_t len;

    h.type = type;
    h.payload_len = payload_len;
    h.session_id = sid;
    h.seq = seq;
    if (ctl_encode(buf, sizeof(buf), &h, payload, key, &len) == RS_OK)
        srv->hooks.send(srv->hooks.user, to, buf, len);
}

static void send_session_packet(ctl_server_t *srv, uint8_t type, const void *payload,
                                uint16_t payload_len)
{
    send_packet(srv, &srv->peer, type, srv->sid, ++srv->tx_seq, payload, payload_len, srv->k_s2c);
}

/* ---- セッション終了 ------------------------------------------------------ */

static void end_session(ctl_server_t *srv, ctl_end_reason_t reason)
{
    char abuf[NET_ADDR_STRLEN];
    uint32_t sid = srv->sid;

    if (!srv->active)
        return;

    /* 最優先: 送信状態のまま放置しない */
    if (srv->hooks.ptt_release != NULL)
        srv->hooks.ptt_release(srv->hooks.user);

    if (reason != CTL_END_CLIENT_DISCONNECT) {
        uint8_t r = reason == CTL_END_TIMEOUT    ? CTL_DISC_TIMEOUT
                  : reason == CTL_END_REPLACED   ? CTL_DISC_REPLACED
                                                 : CTL_DISC_SHUTDOWN;
        send_session_packet(srv, CTL_DISCONNECT, &r, 1);
    }

    rs_log_write(srv->cfg.log, RS_LOG_LEVEL_INFO, LOG_MODULE,
                 "session end: id=%08x peer=%s reason=%s", (unsigned)sid,
                 fmt_addr(&srv->peer, abuf), ctl_end_reason_name(reason));

    srv->active = 0;
    srv->sid = 0;
    srv->tx_seq = 0;
    rs_crypto_wipe(srv->k_c2s, sizeof(srv->k_c2s));
    rs_crypto_wipe(srv->k_s2c, sizeof(srv->k_s2c));
    ctl_replay_init(&srv->rx);
    srv->stats.sessions_ended++;

    if (srv->hooks.session_end != NULL)
        srv->hooks.session_end(srv->hooks.user, sid, reason);
}

void ctl_server_shutdown(ctl_server_t *srv)
{
    if (srv != NULL)
        end_session(srv, CTL_END_SHUTDOWN);
}

void ctl_server_destroy(ctl_server_t *srv)
{
    if (srv == NULL)
        return;
    end_session(srv, CTL_END_SHUTDOWN);
    rs_crypto_wipe(srv, sizeof(*srv));
    free(srv);
}

/* ---- ハンドシェイク ------------------------------------------------------ */

static pending_t *pending_slot(ctl_server_t *srv, const net_addr_t *from)
{
    pending_t *oldest = NULL;
    size_t i;

    for (i = 0; i < PENDING_MAX; i++) {
        if (srv->pending[i].used && net_addr_equal(&srv->pending[i].peer, from))
            return &srv->pending[i];
    }
    for (i = 0; i < PENDING_MAX; i++) {
        pending_t *p = &srv->pending[i];
        if (!p->used)
            return p;
        if (oldest == NULL || p->created < oldest->created)
            oldest = p;
    }
    return oldest;
}

static void handle_hello(ctl_server_t *srv, const net_addr_t *from, const uint8_t *p,
                         uint64_t now)
{
    char user[CTL_USERNAME_MAX + 1];
    uint8_t payload[CTL_CHALLENGE_PAYLOAD_LEN];
    uint8_t salt[CTL_SALT_LEN];
    pending_t *pd;
    uint8_t ulen = p[CTL_NONCE_LEN];

    if (ulen == 0 || ulen > CTL_USERNAME_MAX) {
        srv->stats.rx_malformed++;
        return;
    }
    memcpy(user, p + CTL_NONCE_LEN + 1, ulen);
    user[ulen] = '\0';

    pd = pending_slot(srv, from);
    memset(pd, 0, sizeof(*pd));
    if (rs_crypto_random(pd->sn, sizeof(pd->sn)) != RS_OK) {
        rs_log_write(srv->cfg.log, RS_LOG_LEVEL_ERROR, LOG_MODULE, "random generation failed");
        return;
    }
    pd->used = 1;
    pd->peer = *from;
    memcpy(pd->cn, p, CTL_NONCE_LEN);
    pd->user_ok = strcmp(user, srv->username) == 0;
    pd->created = now;

    /* 未知ユーザにも決定的な偽ソルトで応答し、ユーザ名の有無を外部から判別させない */
    if (pd->user_ok) {
        memcpy(salt, srv->salt, sizeof(salt));
    } else {
        uint8_t h[32];
        rs_hmac_sha256(srv->decoy_secret, sizeof(srv->decoy_secret), user, ulen, h);
        memcpy(salt, h, sizeof(salt));
    }

    memcpy(payload, pd->sn, CTL_NONCE_LEN);
    memcpy(payload + CTL_NONCE_LEN, pd->cn, CTL_NONCE_LEN);
    memcpy(payload + CTL_NONCE_LEN * 2, salt, CTL_SALT_LEN);
    ctl_put_u32(payload + CTL_NONCE_LEN * 2 + CTL_SALT_LEN, srv->cfg.pbkdf2_iterations);
    send_packet(srv, from, CTL_CHALLENGE, 0, 0, payload, sizeof(payload), NULL);
}

static void send_auth_fail(ctl_server_t *srv, const net_addr_t *to, uint8_t reason)
{
    send_packet(srv, to, CTL_AUTH_FAIL, 0, 0, &reason, 1, NULL);
}

static void handle_auth(ctl_server_t *srv, const net_addr_t *from, const uint8_t *p,
                        uint64_t now)
{
    char abuf[NET_ADDR_STRLEN];
    const uint8_t *cn = p;
    const uint8_t *sn = p + CTL_NONCE_LEN;
    const uint8_t *proof = p + CTL_NONCE_LEN * 2;
    uint8_t expect[CTL_PROOF_LEN];
    uint8_t payload[CTL_AUTH_OK_PAYLOAD_LEN];
    pending_t *pd = NULL;
    pending_t found;
    size_t i;
    int ok;

    for (i = 0; i < PENDING_MAX; i++) {
        pending_t *c = &srv->pending[i];
        if (c->used && net_addr_equal(&c->peer, from) &&
            memcmp(c->cn, cn, CTL_NONCE_LEN) == 0 && memcmp(c->sn, sn, CTL_NONCE_LEN) == 0) {
            pd = c;
            break;
        }
    }
    if (pd == NULL || now - pd->created > srv->cfg.challenge_timeout_ms) {
        /* 再送・遅延・リプレイされた AUTH。失敗回数には数えない */
        if (pd != NULL)
            memset(pd, 0, sizeof(*pd));
        srv->stats.rx_stale_challenge++;
        send_auth_fail(srv, from, CTL_FAIL_PROTOCOL);
        return;
    }

    /* チャレンジは 1 回限り */
    found = *pd;
    memset(pd, 0, sizeof(*pd));

    ok = 0;
    if (found.user_ok) {
        ctl_client_proof(srv->key, cn, sn, srv->username, expect);
        ok = rs_crypto_equal(expect, proof, CTL_PROOF_LEN);
    }

    if (!ok) {
        srv->stats.auth_fail++;
        rs_log_write(srv->cfg.log, RS_LOG_LEVEL_WARN, LOG_MODULE,
                     "authentication failed: peer=%s", fmt_addr(from, abuf));
        if (ban_list_record_failure(&srv->bans, from, now)) {
            srv->stats.bans++;
            rs_log_write(srv->cfg.log, RS_LOG_LEVEL_WARN, LOG_MODULE,
                         "address banned: peer=%s duration_ms=%u", fmt_addr(from, abuf),
                         (unsigned)srv->cfg.ban_ms);
        }
        send_auth_fail(srv, from, CTL_FAIL_CREDENTIALS);
        return;
    }

    ban_list_record_success(&srv->bans, from);

    if (srv->active)
        end_session(srv, CTL_END_REPLACED);

    do {
        if (rs_crypto_random(&srv->sid, sizeof(srv->sid)) != RS_OK) {
            rs_log_write(srv->cfg.log, RS_LOG_LEVEL_ERROR, LOG_MODULE, "random generation failed");
            srv->sid = 0;
            return;
        }
    } while (srv->sid == 0);

    ctl_session_keys(srv->key, cn, sn, srv->sid, srv->k_c2s, srv->k_s2c);
    srv->active = 1;
    srv->peer = *from;
    srv->tx_seq = 0;
    ctl_replay_init(&srv->rx);
    srv->last_rx = now;
    srv->stats.auth_ok++;

    ctl_server_proof(srv->key, cn, sn, srv->sid, payload);
    ctl_put_u32(payload + CTL_PROOF_LEN, srv->cfg.keepalive_interval_ms);
    ctl_put_u32(payload + CTL_PROOF_LEN + 4, srv->cfg.keepalive_timeout_ms);
    send_session_packet(srv, CTL_AUTH_OK, payload, sizeof(payload));

    rs_log_write(srv->cfg.log, RS_LOG_LEVEL_INFO, LOG_MODULE,
                 "session start: id=%08x peer=%s user=%s", (unsigned)srv->sid,
                 fmt_addr(from, abuf), srv->username);
    if (srv->hooks.session_start != NULL)
        srv->hooks.session_start(srv->hooks.user, srv->sid, &srv->peer);
}

/* ---- セッションパケット -------------------------------------------------- */

static void handle_session(ctl_server_t *srv, const net_addr_t *from, const ctl_header_t *h,
                           const uint8_t *data, size_t len, const uint8_t *p, uint64_t now)
{
    char abuf[NET_ADDR_STRLEN];

    if (!srv->active || h->session_id != srv->sid) {
        srv->stats.rx_unknown_session++;
        return;
    }
    if (!ctl_replay_check(&srv->rx, h->seq)) {
        srv->stats.rx_replay++;
        return;
    }
    if (!ctl_verify_tag(data, len, srv->k_c2s)) {
        srv->stats.rx_bad_tag++;
        return;
    }
    ctl_replay_update(&srv->rx, h->seq);
    srv->last_rx = now;

    /* 正当なタグを持つパケットの送信元が変わった場合は追従する（NAT 再割当・回線切替） */
    if (!net_addr_equal(&srv->peer, from)) {
        rs_log_write(srv->cfg.log, RS_LOG_LEVEL_INFO, LOG_MODULE,
                     "session %08x peer moved to %s", (unsigned)srv->sid, fmt_addr(from, abuf));
        srv->peer = *from;
    }

    if (h->type == CTL_PING)
        send_session_packet(srv, CTL_PONG, p, CTL_PING_PAYLOAD_LEN);
    else if (h->type == CTL_DISCONNECT)
        end_session(srv, CTL_END_CLIENT_DISCONNECT);
}

void ctl_server_handle_packet(ctl_server_t *srv, const net_addr_t *from,
                              const uint8_t *data, size_t len, uint64_t now_ms)
{
    ctl_header_t h;
    const uint8_t *p;

    if (srv == NULL || from == NULL || data == NULL)
        return;

    if (ban_list_banned_ms(&srv->bans, from, now_ms) > 0) {
        srv->stats.rx_banned++;
        return;
    }
    if (ctl_decode(data, len, &h, &p) != RS_OK) {
        srv->stats.rx_malformed++;
        return;
    }

    switch (h.type) {
    case CTL_HELLO:
        handle_hello(srv, from, p, now_ms);
        break;
    case CTL_AUTH:
        handle_auth(srv, from, p, now_ms);
        break;
    case CTL_PING:
    case CTL_DISCONNECT:
        handle_session(srv, from, &h, data, len, p, now_ms);
        break;
    default:
        srv->stats.rx_malformed++; /* サーバ→クライアント方向の種別 */
        break;
    }
}

void ctl_server_tick(ctl_server_t *srv, uint64_t now_ms)
{
    size_t i;

    if (srv == NULL)
        return;

    if (srv->active && now_ms - srv->last_rx >= srv->cfg.keepalive_timeout_ms)
        end_session(srv, CTL_END_TIMEOUT);

    for (i = 0; i < PENDING_MAX; i++) {
        pending_t *pd = &srv->pending[i];
        if (pd->used && now_ms - pd->created > srv->cfg.challenge_timeout_ms)
            memset(pd, 0, sizeof(*pd));
    }
}

ctl_server_state_t ctl_server_state(const ctl_server_t *srv)
{
    return srv != NULL && srv->active ? CTL_SERVER_ACTIVE : CTL_SERVER_WAITING;
}

uint32_t ctl_server_session_id(const ctl_server_t *srv)
{
    return srv != NULL ? srv->sid : 0;
}

int ctl_server_session_keys(const ctl_server_t *srv, uint8_t k_c2s[CTL_KEY_LEN],
                            uint8_t k_s2c[CTL_KEY_LEN])
{
    if (srv == NULL || k_c2s == NULL || k_s2c == NULL)
        return RS_ERR_INVALID_ARG;
    if (!srv->active)
        return RS_ERR_NOT_FOUND;
    memcpy(k_c2s, srv->k_c2s, CTL_KEY_LEN);
    memcpy(k_s2c, srv->k_s2c, CTL_KEY_LEN);
    return RS_OK;
}

uint64_t ctl_server_banned_ms(const ctl_server_t *srv, const net_addr_t *addr, uint64_t now_ms)
{
    return srv != NULL ? ban_list_banned_ms(&srv->bans, addr, now_ms) : 0;
}

const ctl_server_stats_t *ctl_server_stats(const ctl_server_t *srv)
{
    return srv != NULL ? &srv->stats : NULL;
}
