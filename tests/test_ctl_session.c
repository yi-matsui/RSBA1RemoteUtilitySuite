/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * ctl_server と ctl_client を仮想ネットワーク（メモリ上のキュー）と仮想時計で接続し、
 * 認証・Keepalive・切断ライフサイクル・防御機能を検証する。
 */
#include <stdlib.h>
#include <string.h>

#include "ctl_client.h"
#include "ctl_server.h"
#include "net_socket.h"
#include "rs_error.h"
#include "test_util.h"

#define PASSWORD   "correct horse battery staple"
#define USERNAME   "operator"
#define ITERATIONS 1000
#define QMAX       128
#define STEP_MS    100

/* ---- 仮想ネットワーク ---------------------------------------------------- */

typedef struct pkt {
    net_addr_t from;
    net_addr_t to;
    uint8_t    data[CTL_MAX_PACKET_LEN];
    size_t     len;
} pkt_t;

static pkt_t      g_q[QMAX];
static int        g_qn;
static uint64_t   g_now = 1000000;
static net_addr_t g_server_addr;
static int        g_drop_to_server;
static int        g_drop_to_client;
static int        g_password_leaked;
static pkt_t      g_last_by_type[16];
static int        g_sent_by_type[16];

static int contains(const uint8_t *hay, size_t hlen, const void *needle, size_t nlen)
{
    size_t i;
    for (i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return 1;
    return 0;
}

static void net_send(const net_addr_t *from, const net_addr_t *to, const void *buf, size_t len)
{
    const uint8_t *b = buf;
    pkt_t *p;

    if (contains(b, len, PASSWORD, strlen(PASSWORD)))
        g_password_leaked = 1;
    if (len > 5 && b[5] < 16) {
        pkt_t *l = &g_last_by_type[b[5]];
        l->from = *from;
        l->to = *to;
        memcpy(l->data, buf, len);
        l->len = len;
        g_sent_by_type[b[5]]++;
    }

    if (net_addr_equal(to, &g_server_addr) ? g_drop_to_server : g_drop_to_client)
        return;
    if (g_qn >= QMAX)
        return;
    p = &g_q[g_qn++];
    p->from = *from;
    p->to = *to;
    memcpy(p->data, buf, len);
    p->len = len;
}

/* ---- サーバ側フック ------------------------------------------------------ */

typedef struct server_obs {
    int              ptt_releases;
    int              starts;
    int              ends;
    ctl_end_reason_t last_end;
} server_obs_t;

static int srv_send(void *user, const net_addr_t *to, const void *buf, size_t len)
{
    (void)user;
    net_send(&g_server_addr, to, buf, len);
    return RS_OK;
}

static void srv_ptt(void *user)
{
    ((server_obs_t *)user)->ptt_releases++;
}

static void srv_start(void *user, uint32_t sid, const net_addr_t *peer)
{
    (void)sid;
    (void)peer;
    ((server_obs_t *)user)->starts++;
}

static void srv_end(void *user, uint32_t sid, ctl_end_reason_t reason)
{
    server_obs_t *o = user;
    (void)sid;
    o->ends++;
    o->last_end = reason;
}

/* ---- クライアント側フック ------------------------------------------------ */

typedef struct client_ep {
    net_addr_t       addr;
    ctl_client_t    *cli;
    int              connects;
    int              disconnects;
    ctl_client_end_t last_end;
} client_ep_t;

static int cli_send(void *user, const net_addr_t *to, const void *buf, size_t len)
{
    client_ep_t *ep = user;
    net_send(&ep->addr, to, buf, len);
    return RS_OK;
}

static void cli_connected(void *user, uint32_t sid)
{
    (void)sid;
    ((client_ep_t *)user)->connects++;
}

static void cli_disconnected(void *user, ctl_client_end_t reason)
{
    client_ep_t *ep = user;
    ep->disconnects++;
    ep->last_end = reason;
}

/* ---- 環境 ---------------------------------------------------------------- */

typedef struct env {
    ctl_server_t *srv;
    server_obs_t  obs;
    client_ep_t   c[2];
    int           nclients;
} env_t;

static void reset_net(void)
{
    g_qn = 0;
    g_drop_to_server = 0;
    g_drop_to_client = 0;
    memset(g_last_by_type, 0, sizeof(g_last_by_type));
    memset(g_sent_by_type, 0, sizeof(g_sent_by_type));
}

static void pump(env_t *e)
{
    int guard = 0;

    while (g_qn > 0 && guard++ < 1000) {
        pkt_t p = g_q[0];
        int i;

        memmove(g_q, g_q + 1, sizeof(pkt_t) * (size_t)(g_qn - 1));
        g_qn--;
        if (net_addr_equal(&p.to, &g_server_addr)) {
            ctl_server_handle_packet(e->srv, &p.from, p.data, p.len, g_now);
        } else {
            for (i = 0; i < e->nclients; i++)
                if (e->c[i].cli != NULL && net_addr_equal(&p.to, &e->c[i].addr))
                    ctl_client_handle_packet(e->c[i].cli, &p.from, p.data, p.len, g_now);
        }
    }
}

static void advance(env_t *e, uint32_t ms)
{
    uint32_t t;
    int i;

    for (t = 0; t < ms; t += STEP_MS) {
        g_now += STEP_MS;
        ctl_server_tick(e->srv, g_now);
        for (i = 0; i < e->nclients; i++)
            if (e->c[i].cli != NULL)
                ctl_client_tick(e->c[i].cli, g_now);
        pump(e);
    }
}

static void add_client(env_t *e, const char *addr, uint16_t port, const char *user,
                       const char *password, int auto_reconnect)
{
    client_ep_t *ep = &e->c[e->nclients++];
    ctl_client_config_t cfg;
    ctl_client_hooks_t hooks;

    memset(ep, 0, sizeof(*ep));
    net_addr_resolve(&ep->addr, addr, port);

    ctl_client_config_default(&cfg);
    cfg.username = user;
    cfg.password = password;
    cfg.server = g_server_addr;
    cfg.auto_reconnect = auto_reconnect;

    memset(&hooks, 0, sizeof(hooks));
    hooks.user = ep;
    hooks.send = cli_send;
    hooks.connected = cli_connected;
    hooks.disconnected = cli_disconnected;
    CHECK_RC(ctl_client_create(&ep->cli, &cfg, &hooks), RS_OK);
}

static void env_init(env_t *e)
{
    ctl_server_config_t cfg;
    ctl_server_hooks_t hooks;

    reset_net();
    memset(e, 0, sizeof(*e));
    ctl_server_config_default(&cfg);
    cfg.username = USERNAME;
    cfg.password = PASSWORD;
    cfg.pbkdf2_iterations = ITERATIONS;
    cfg.max_failures = 3;
    cfg.ban_ms = 60000;

    memset(&hooks, 0, sizeof(hooks));
    hooks.user = &e->obs;
    hooks.send = srv_send;
    hooks.ptt_release = srv_ptt;
    hooks.session_start = srv_start;
    hooks.session_end = srv_end;
    CHECK_RC(ctl_server_create(&e->srv, &cfg, &hooks), RS_OK);
}

static void env_free(env_t *e)
{
    int i;
    for (i = 0; i < e->nclients; i++)
        ctl_client_destroy(e->c[i].cli);
    ctl_server_destroy(e->srv);
}

static void connect_client(env_t *e, int idx)
{
    CHECK_RC(ctl_client_connect(e->c[idx].cli, g_now), RS_OK);
    pump(e);
}

/* ---- テスト -------------------------------------------------------------- */

static void test_auth_success_and_keepalive(void)
{
    env_t e;
    const ctl_server_stats_t *st;

    env_init(&e);
    add_client(&e, "2001:db8::2", 40000, USERNAME, PASSWORD, 1);
    connect_client(&e, 0);

    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_CONNECTED);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_ACTIVE);
    CHECK(ctl_server_session_id(e.srv) != 0);
    CHECK(ctl_client_session_id(e.c[0].cli) == ctl_server_session_id(e.srv));
    CHECK(e.obs.starts == 1 && e.c[0].connects == 1);

    /* 10 秒間: 1 秒周期の PING/PONG でセッションが維持される */
    advance(&e, 10000);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_CONNECTED);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_ACTIVE);
    CHECK(g_sent_by_type[CTL_PING] >= 9 && g_sent_by_type[CTL_PING] <= 11);
    CHECK(g_sent_by_type[CTL_PONG] == g_sent_by_type[CTL_PING]);
    CHECK(e.obs.ptt_releases == 0);

    st = ctl_server_stats(e.srv);
    CHECK(st->auth_ok == 1 && st->auth_fail == 0);
    CHECK(st->rx_bad_tag == 0 && st->rx_replay == 0);
    CHECK(!g_password_leaked);
    env_free(&e);
}

static void test_client_disconnect(void)
{
    env_t e;

    env_init(&e);
    add_client(&e, "2001:db8::2", 40000, USERNAME, PASSWORD, 1);
    connect_client(&e, 0);
    advance(&e, 2000);

    ctl_client_disconnect(e.c[0].cli, g_now);
    pump(&e);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_WAITING);
    CHECK(ctl_server_session_id(e.srv) == 0);
    CHECK(e.obs.ptt_releases == 1);
    CHECK(e.obs.ends == 1 && e.obs.last_end == CTL_END_CLIENT_DISCONNECT);
    CHECK(e.c[0].last_end == CTL_CLIENT_END_USER);

    /* 利用者切断後は自動再接続しない */
    advance(&e, 10000);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_IDLE);
    CHECK(e.obs.starts == 1);

    /* 待機状態から再び接続できる */
    connect_client(&e, 0);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_ACTIVE);
    CHECK(e.obs.starts == 2);
    env_free(&e);
}

static void test_keepalive_timeout(void)
{
    env_t e;

    env_init(&e);
    add_client(&e, "2001:db8::2", 40000, USERNAME, PASSWORD, 1);
    connect_client(&e, 0);
    advance(&e, 2000);

    /* 回線断: 双方向とも届かない */
    g_drop_to_server = 1;
    g_drop_to_client = 1;
    advance(&e, 4000);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_ACTIVE); /* 5 秒未満は維持 */
    advance(&e, 1200);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_WAITING);
    CHECK(e.obs.ptt_releases == 1);
    CHECK(e.obs.last_end == CTL_END_TIMEOUT);
    CHECK(e.c[0].last_end == CTL_CLIENT_END_TIMEOUT);

    /* 回線復旧後、クライアントは自動再接続する */
    g_drop_to_server = 0;
    g_drop_to_client = 0;
    advance(&e, 4000);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_CONNECTED);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_ACTIVE);
    CHECK(e.obs.starts == 2);
    env_free(&e);
}

static void test_wrong_password_and_unknown_user(void)
{
    env_t e;
    const ctl_server_stats_t *st;

    env_init(&e);
    add_client(&e, "2001:db8::3", 40000, USERNAME, "wrong password", 1);
    add_client(&e, "2001:db8::4", 40000, "intruder", PASSWORD, 1);

    connect_client(&e, 0);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_IDLE);
    CHECK(e.c[0].last_end == CTL_CLIENT_END_AUTH_FAILED);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_WAITING);

    /* 未知ユーザでもチャレンジは返る（ユーザ名の存在を漏らさない）が認証は失敗 */
    connect_client(&e, 1);
    CHECK(e.c[1].last_end == CTL_CLIENT_END_AUTH_FAILED);
    CHECK(g_sent_by_type[CTL_CHALLENGE] == 2);

    st = ctl_server_stats(e.srv);
    CHECK(st->auth_fail == 2 && st->auth_ok == 0);
    CHECK(e.obs.starts == 0 && e.obs.ptt_releases == 0);

    /* 認証失敗では自動再接続しない（自分で遮断を招かないため） */
    advance(&e, 10000);
    CHECK(st->auth_fail == 2);
    env_free(&e);
}

static void test_ip_ban(void)
{
    env_t e;
    net_addr_t attacker;
    const ctl_server_stats_t *st;
    int i;

    env_init(&e); /* max_failures = 3, ban 60 秒 */
    add_client(&e, "198.51.100.9", 40000, USERNAME, "guess", 0);
    add_client(&e, "198.51.100.9", 40001, USERNAME, PASSWORD, 0); /* 同一 IP の正規パスワード */
    net_addr_resolve(&attacker, "198.51.100.9", 0);

    for (i = 0; i < 3; i++)
        connect_client(&e, 0);
    st = ctl_server_stats(e.srv);
    CHECK(st->auth_fail == 3);
    CHECK(st->bans == 1);
    CHECK(ctl_server_banned_ms(e.srv, &attacker, g_now) > 0);

    /* 遮断中は正しいパスワードでも応答しない（ポートが違っても同一 IP） */
    g_sent_by_type[CTL_CHALLENGE] = 0;
    connect_client(&e, 1);
    CHECK(g_sent_by_type[CTL_CHALLENGE] == 0);
    CHECK(st->rx_banned >= 1);
    CHECK(ctl_client_state(e.c[1].cli) == CTL_CLIENT_HELLO_SENT);
    advance(&e, 3000); /* ハンドシェイクタイムアウト */
    CHECK(e.c[1].last_end == CTL_CLIENT_END_HANDSHAKE_TIMEOUT);

    /* 遮断期間経過後は接続できる */
    advance(&e, 60000);
    CHECK(ctl_server_banned_ms(e.srv, &attacker, g_now) == 0);
    connect_client(&e, 1);
    CHECK(ctl_client_state(e.c[1].cli) == CTL_CLIENT_CONNECTED);
    env_free(&e);
}

static void test_replay_and_tamper(void)
{
    env_t e;
    pkt_t ping;
    pkt_t auth;
    const ctl_server_stats_t *st;
    int pongs;

    env_init(&e);
    add_client(&e, "2001:db8::2", 40000, USERNAME, PASSWORD, 0);
    connect_client(&e, 0);
    auth = g_last_by_type[CTL_AUTH];
    advance(&e, 1000);
    ping = g_last_by_type[CTL_PING];
    CHECK(ping.len > 0);
    st = ctl_server_stats(e.srv);

    /* PING の再送（リプレイ）は破棄され、PONG も返らない */
    pongs = g_sent_by_type[CTL_PONG];
    ctl_server_handle_packet(e.srv, &ping.from, ping.data, ping.len, g_now);
    CHECK(st->rx_replay == 1);
    CHECK(g_sent_by_type[CTL_PONG] == pongs);

    /* シーケンス番号を進めた改ざんパケットはタグ不一致で破棄 */
    ping.data[15] += 10;
    ctl_server_handle_packet(e.srv, &ping.from, ping.data, ping.len, g_now);
    CHECK(st->rx_bad_tag == 1);
    ping.data[15] -= 10;

    /* ペイロード改ざんも同様（セッション ID を変えた場合は不明セッション） */
    ping.data[CTL_HEADER_LEN + 7] ^= 0xFF;
    ping.data[15] += 20;
    ctl_server_handle_packet(e.srv, &ping.from, ping.data, ping.len, g_now);
    CHECK(st->rx_bad_tag == 2);
    ping.data[8] ^= 0xFF;
    ctl_server_handle_packet(e.srv, &ping.from, ping.data, ping.len, g_now);
    CHECK(st->rx_unknown_session == 1);
    CHECK(g_sent_by_type[CTL_PONG] == pongs);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_ACTIVE);

    /* 過去の AUTH のリプレイではセッションを奪えない（チャレンジは 1 回限り） */
    ctl_server_handle_packet(e.srv, &auth.from, auth.data, auth.len, g_now);
    CHECK(st->rx_stale_challenge == 1);
    CHECK(st->auth_ok == 1);
    CHECK(e.obs.starts == 1);

    /* 偽造 DISCONNECT ではセッションを切れない */
    {
        uint8_t forged[CTL_MAX_PACKET_LEN];
        memcpy(forged, g_last_by_type[CTL_PING].data, g_last_by_type[CTL_PING].len);
        forged[5] = CTL_DISCONNECT;
        forged[7] = 1;
        forged[15] = 200;
        ctl_server_handle_packet(e.srv, &ping.from, forged,
                                 CTL_HEADER_LEN + 1 + CTL_TAG_LEN, g_now);
        CHECK(ctl_server_state(e.srv) == CTL_SERVER_ACTIVE);
    }

    advance(&e, 3000);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_CONNECTED);
    env_free(&e);
}

static void test_session_takeover_and_roaming(void)
{
    env_t e;

    env_init(&e);
    add_client(&e, "2001:db8::2", 40000, USERNAME, PASSWORD, 1);
    add_client(&e, "192.0.2.50", 40000, USERNAME, PASSWORD, 1); /* IPv4（mapped）クライアント */
    connect_client(&e, 0);
    advance(&e, 1000);

    /* 2 台目が認証に成功すると 1 台目は置き換えられる（PTT は一旦解除） */
    connect_client(&e, 1);
    CHECK(ctl_client_state(e.c[1].cli) == CTL_CLIENT_CONNECTED);
    CHECK(e.c[0].last_end == CTL_CLIENT_END_REPLACED);
    CHECK(e.obs.last_end == CTL_END_REPLACED);
    CHECK(e.obs.ptt_releases == 1);
    CHECK(e.obs.starts == 2);

    /* 置き換えられた側は自動再接続しない（奪い合い防止） */
    advance(&e, 10000);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_IDLE);
    CHECK(ctl_client_state(e.c[1].cli) == CTL_CLIENT_CONNECTED);

    /* クライアントのアドレスが変わっても、正当なタグがあればセッションを継続する */
    net_addr_resolve(&e.c[1].addr, "192.0.2.51", 40500);
    advance(&e, 3000);
    CHECK(ctl_client_state(e.c[1].cli) == CTL_CLIENT_CONNECTED);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_ACTIVE);
    env_free(&e);
}

static void test_server_shutdown(void)
{
    env_t e;

    env_init(&e);
    add_client(&e, "2001:db8::2", 40000, USERNAME, PASSWORD, 1);
    connect_client(&e, 0);

    ctl_server_shutdown(e.srv);
    pump(&e);
    CHECK(e.obs.ptt_releases == 1);
    CHECK(e.obs.last_end == CTL_END_SHUTDOWN);
    CHECK(e.c[0].last_end == CTL_CLIENT_END_SERVER);
    CHECK(ctl_server_state(e.srv) == CTL_SERVER_WAITING);

    /* サーバ再起動後に自動再接続 */
    advance(&e, 4000);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_CONNECTED);

    /* 破棄時もセッションがあれば PTT 解除する */
    ctl_server_destroy(e.srv);
    CHECK(e.obs.ptt_releases == 2);
    e.srv = NULL;
    ctl_client_destroy(e.c[0].cli);
}

static void test_forged_auth_ok(void)
{
    env_t e;
    uint8_t forged[CTL_MAX_PACKET_LEN];
    ctl_header_t h;
    uint8_t payload[CTL_AUTH_OK_PAYLOAD_LEN];
    uint8_t fake_key[CTL_KEY_LEN];
    size_t len;

    env_init(&e);
    add_client(&e, "2001:db8::2", 40000, USERNAME, PASSWORD, 0);

    /* AUTH をサーバに届けず、攻撃者が鍵を知らずに AUTH_OK を偽造する */
    g_drop_to_server = 1;
    CHECK_RC(ctl_client_connect(e.c[0].cli, g_now), RS_OK);
    g_drop_to_server = 0;
    pump(&e);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_HELLO_SENT);
    /* HELLO を手動で届けて CHALLENGE を受け取り、AUTH は落とす */
    ctl_server_handle_packet(e.srv, &g_last_by_type[CTL_HELLO].from,
                             g_last_by_type[CTL_HELLO].data, g_last_by_type[CTL_HELLO].len, g_now);
    g_drop_to_server = 1;
    pump(&e);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_AUTH_SENT);

    memset(payload, 0x5A, sizeof(payload));
    memset(fake_key, 0x33, sizeof(fake_key));
    h.type = CTL_AUTH_OK;
    h.payload_len = CTL_AUTH_OK_PAYLOAD_LEN;
    h.session_id = 0x12345678u;
    h.seq = 1;
    CHECK_RC(ctl_encode(forged, sizeof(forged), &h, payload, fake_key, &len), RS_OK);
    ctl_client_handle_packet(e.c[0].cli, &g_server_addr, forged, len, g_now);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_AUTH_SENT);
    CHECK(e.c[0].connects == 0);

    /* 別アドレスからのパケットは無視する */
    ctl_client_handle_packet(e.c[0].cli, &e.c[0].addr, forged, len, g_now);
    CHECK(ctl_client_state(e.c[0].cli) == CTL_CLIENT_AUTH_SENT);
    env_free(&e);
}

static void test_config_validation(void)
{
    ctl_server_config_t cfg;
    ctl_server_hooks_t hooks;
    ctl_server_t *srv = NULL;
    ctl_client_config_t ccfg;
    ctl_client_hooks_t chooks;
    ctl_client_t *cli = NULL;

    memset(&hooks, 0, sizeof(hooks));
    hooks.send = srv_send;
    ctl_server_config_default(&cfg);
    cfg.username = USERNAME;
    cfg.password = "";
    CHECK_RC(ctl_server_create(&srv, &cfg, &hooks), RS_ERR_INVALID_ARG);
    cfg.password = PASSWORD;
    cfg.username = "this-user-name-is-longer-than-32-characters";
    CHECK_RC(ctl_server_create(&srv, &cfg, &hooks), RS_ERR_INVALID_ARG);
    cfg.username = USERNAME;
    hooks.send = NULL;
    CHECK_RC(ctl_server_create(&srv, &cfg, &hooks), RS_ERR_INVALID_ARG);
    CHECK(srv == NULL);

    memset(&chooks, 0, sizeof(chooks));
    chooks.send = cli_send;
    ctl_client_config_default(&ccfg);
    ccfg.username = USERNAME;
    ccfg.password = NULL;
    CHECK_RC(ctl_client_create(&cli, &ccfg, &chooks), RS_ERR_INVALID_ARG);
    CHECK(cli == NULL);
}

int main(void)
{
    CHECK_RC(net_init(), RS_OK);
    net_addr_resolve(&g_server_addr, "2001:db8::1", 50001);

    test_auth_success_and_keepalive();
    test_client_disconnect();
    test_keepalive_timeout();
    test_wrong_password_and_unknown_user();
    test_ip_ban();
    test_replay_and_tamper();
    test_session_takeover_and_roaming();
    test_server_shutdown();
    test_forged_auth_ok();
    test_config_validation();
    CHECK(!g_password_leaked);

    net_cleanup();
    return TEST_RESULT();
}
