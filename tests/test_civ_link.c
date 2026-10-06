/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * CI-V 透過中継のテスト。モックシリアル（無線機側・操作ソフト側）と
 * メモリ上のネットワークで、フレーム切り出し・封緘・改ざん/リプレイ拒否・
 * PTT 強制解除の書き込み・制御チャネルの鍵との連携を検証する。
 */
#include <string.h>

#include "civ.h"
#include "civ_link.h"
#include "ctl_client.h"
#include "ctl_server.h"
#include "dch.h"
#include "rs_error.h"
#include "test_util.h"

/* ---- モックシリアル ------------------------------------------------------ */

typedef struct mock_serial {
    uint8_t rx[1024];     /* 相手機器 → ホスト（read で返す） */
    size_t  rx_len;
    size_t  rx_pos;
    size_t  read_chunk;   /* 1 回の read で返す最大バイト数（0 = 制限なし） */
    uint8_t tx[1024];     /* ホスト → 相手機器（write された内容） */
    size_t  tx_len;
    int     fail_read;
    int     write_timeout;
} mock_serial_t;

static int ms_read(void *st, void *buf, size_t cap, size_t *got)
{
    mock_serial_t *m = st;
    size_t n = m->rx_len - m->rx_pos;

    if (m->fail_read)
        return RS_ERR_IO;
    if (n > cap)
        n = cap;
    if (m->read_chunk > 0 && n > m->read_chunk)
        n = m->read_chunk;
    memcpy(buf, m->rx + m->rx_pos, n);
    m->rx_pos += n;
    *got = n;
    return RS_OK;
}

static int ms_write(void *st, const void *buf, size_t len)
{
    mock_serial_t *m = st;
    if (m->write_timeout)
        return RS_ERR_TIMEOUT;
    memcpy(m->tx + m->tx_len, buf, len);
    m->tx_len += len;
    return RS_OK;
}

static void ms_close(void *st)
{
    (void)st;
}

static const serial_ops_t k_mock_ops = { ms_read, ms_write, ms_close };

static void ms_feed(mock_serial_t *m, const uint8_t *data, size_t len)
{
    memcpy(m->rx + m->rx_len, data, len);
    m->rx_len += len;
}

/* ---- メモリ上のネットワーク（1 パケットだけ保持） ------------------------ */

typedef struct wire {
    uint8_t pkt[2048];
    size_t  len;
    int     count;
} wire_t;

static int wire_send(void *user, const void *pkt, size_t len)
{
    wire_t *w = user;
    memcpy(w->pkt, pkt, len);
    w->len = len;
    w->count++;
    return RS_OK;
}

typedef struct pair {
    mock_serial_t radio;   /* サーバ側: IC-9100 */
    mock_serial_t pc;      /* クライアント側: 操作ソフト */
    serial_port_t radio_port;
    serial_port_t pc_port;
    wire_t        to_client;
    wire_t        to_server;
    civ_link_t    srv;
    civ_link_t    cli;
} pair_t;

static const uint8_t k_c2s[CTL_KEY_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const uint8_t k_s2c[CTL_KEY_LEN] = { 9, 8, 7, 6, 5, 4, 3, 2 };

static void pair_init(pair_t *p, int start)
{
    memset(p, 0, sizeof(*p));
    p->radio_port.ops = &k_mock_ops;
    p->radio_port.st = &p->radio;
    p->pc_port.ops = &k_mock_ops;
    p->pc_port.st = &p->pc;
    civ_link_init(&p->srv, wire_send, &p->to_client);
    civ_link_init(&p->cli, wire_send, &p->to_server);
    civ_link_attach(&p->srv, &p->radio_port);
    civ_link_attach(&p->cli, &p->pc_port);
    if (start) {
        civ_link_start(&p->srv, DCH_ROLE_SERVER, 0x1234, k_c2s, k_s2c);
        civ_link_start(&p->cli, DCH_ROLE_CLIENT, 0x1234, k_c2s, k_s2c);
    }
}

static int deliver_to_server(pair_t *p, uint8_t *type)
{
    return civ_link_handle_packet(&p->srv, p->to_server.pkt, p->to_server.len, type);
}

static int deliver_to_client(pair_t *p, uint8_t *type)
{
    return civ_link_handle_packet(&p->cli, p->to_client.pkt, p->to_client.len, type);
}

static int contains(const uint8_t *hay, size_t hlen, const uint8_t *needle, size_t nlen)
{
    size_t i;
    for (i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return 1;
    return 0;
}

/* ---- テスト -------------------------------------------------------------- */

static void test_framer(void)
{
    static const uint8_t ok1[] = { 0xFE, 0xFE, 0x7C, 0xE0, 0x03, 0xFD };
    static const uint8_t ok2[] = { 0xFE, 0xFE, 0xE0, 0x7C, 0xFB, 0xFD };
    civ_framer_t f;
    uint8_t stream[64];
    size_t n = 0;
    size_t frames;
    int i;

    civ_framer_init(&f);
    /* 雑音 3 バイト + 正常 + 途中で途切れた frame（FE で再開）+ プリアンブル 3 個 + 正常 */
    stream[n++] = 0x00; stream[n++] = 0x55; stream[n++] = 0xFD;
    memcpy(stream + n, ok1, sizeof(ok1)); n += sizeof(ok1);
    stream[n++] = 0xFE; stream[n++] = 0xFE; stream[n++] = 0x7C;
    stream[n++] = 0xFE;
    memcpy(stream + n, ok2, sizeof(ok2)); n += sizeof(ok2);

    frames = 0;
    for (i = 0; i < (int)n; i++) /* 1 バイトずつ投入しても結果は同じ */
        frames += civ_framer_push(&f, stream + i, 1, NULL, NULL);
    CHECK(frames == 2);
    CHECK(f.frames == 2);
    CHECK(f.garbage_total == 3 + 3);
    CHECK(f.garbage_pending == 0);

    /* 長すぎるフレームは破棄 */
    civ_framer_init(&f);
    stream[0] = 0xFE;
    stream[1] = 0xFE;
    civ_framer_push(&f, stream, 2, NULL, NULL);
    for (i = 0; i < CIV_MAX_FRAME; i++) {
        uint8_t b = 0x01;
        civ_framer_push(&f, &b, 1, NULL, NULL);
    }
    CHECK(f.frames == 0);
    CHECK(f.garbage_pending >= CIV_MAX_FRAME);

    CHECK(civ_validate_frames(ok1, sizeof(ok1)));
    memcpy(stream, ok1, sizeof(ok1));
    memcpy(stream + sizeof(ok1), ok2, sizeof(ok2));
    CHECK(civ_validate_frames(stream, sizeof(ok1) + sizeof(ok2)));
    CHECK(!civ_validate_frames(ok1, sizeof(ok1) - 1));         /* FD なし */
    CHECK(!civ_validate_frames(stream + 1, sizeof(ok1) - 1));  /* プリアンブル不足 */
    CHECK(!civ_validate_frames((const uint8_t *)"\xFE\xFE\xFD", 3));
}

static void test_bidirectional_relay(void)
{
    static const uint8_t cmd[] = { 0xFE, 0xFE, 0x7C, 0xE0, 0x03, 0xFD };                /* 周波数読み出し */
    static const uint8_t rsp[] = { 0xFE, 0xFE, 0xE0, 0x7C, 0x03, 0x00, 0x50, 0x14, 0x07, 0x00, 0xFD };
    pair_t p;
    uint8_t type = 0;

    pair_init(&p, 1);

    /* 操作ソフト → 無線機 */
    ms_feed(&p.pc, cmd, sizeof(cmd));
    CHECK_RC(civ_link_poll(&p.cli), RS_OK);
    CHECK(p.to_server.count == 1);
    CHECK(!contains(p.to_server.pkt, p.to_server.len, cmd + 2, sizeof(cmd) - 2)); /* 暗号化 */
    CHECK_RC(deliver_to_server(&p, &type), RS_OK);
    CHECK(type == DCH_TYPE_CIV);
    CHECK(p.radio.tx_len == sizeof(cmd) && memcmp(p.radio.tx, cmd, sizeof(cmd)) == 0);

    /* 無線機 → 操作ソフト（2 バイトずつしか読めない場合も、フレーム単位で送る） */
    p.radio.read_chunk = 2;
    ms_feed(&p.radio, rsp, sizeof(rsp));
    CHECK_RC(civ_link_poll(&p.srv), RS_OK);
    CHECK(p.to_client.count == 1);
    CHECK_RC(deliver_to_client(&p, &type), RS_OK);
    CHECK(p.pc.tx_len == sizeof(rsp) && memcmp(p.pc.tx, rsp, sizeof(rsp)) == 0);

    /* 複数フレームは 1 パケットにまとめる。雑音は送らない */
    p.radio.read_chunk = 0;
    p.pc.tx_len = 0;
    ms_feed(&p.radio, rsp, sizeof(rsp));
    ms_feed(&p.radio, (const uint8_t *)"\x11\x22", 2);
    ms_feed(&p.radio, rsp, sizeof(rsp));
    CHECK_RC(civ_link_poll(&p.srv), RS_OK);
    CHECK(p.to_client.count == 2);
    CHECK_RC(deliver_to_client(&p, &type), RS_OK);
    CHECK(p.pc.tx_len == 2 * sizeof(rsp));
    CHECK(p.srv.framer.garbage_total == 2);

    CHECK(p.srv.stats.frames_to_net == 3 && p.cli.stats.frames_from_net == 3);
    CHECK(p.cli.stats.frames_to_net == 1 && p.srv.stats.frames_from_net == 1);
}

static void test_tamper_replay_and_validation(void)
{
    static const uint8_t cmd[] = { 0xFE, 0xFE, 0x7C, 0xE0, 0x1C, 0x00, 0x01, 0xFD }; /* 送信開始 */
    pair_t p;
    uint8_t saved[2048];
    size_t saved_len;
    uint8_t pkt[256];
    size_t len;
    uint8_t type;

    pair_init(&p, 1);
    ms_feed(&p.pc, cmd, sizeof(cmd));
    civ_link_poll(&p.cli);
    memcpy(saved, p.to_server.pkt, p.to_server.len);
    saved_len = p.to_server.len;

    /* 改ざん（暗号文の 1 ビット）→ 破棄、無線機には書かない */
    p.to_server.pkt[CTL_HEADER_LEN + 4] ^= 0x01;
    CHECK_RC(deliver_to_server(&p, &type), RS_ERR_INVALID_ARG);
    CHECK(p.srv.ch.stats.rx_bad_tag == 1);
    CHECK(p.radio.tx_len == 0);

    /* 正規パケットは受理、同じものの再送（リプレイ）は破棄 */
    CHECK_RC(civ_link_handle_packet(&p.srv, saved, saved_len, &type), RS_OK);
    CHECK(p.radio.tx_len == sizeof(cmd));
    CHECK_RC(civ_link_handle_packet(&p.srv, saved, saved_len, &type), RS_ERR_INVALID_ARG);
    CHECK(p.srv.ch.stats.rx_replay == 1);
    CHECK(p.radio.tx_len == sizeof(cmd));

    /* サーバ自身が送った向きのパケットを送り返しても受理しない（方向別の鍵） */
    ms_feed(&p.radio, cmd, sizeof(cmd));
    civ_link_poll(&p.srv);
    CHECK_RC(civ_link_handle_packet(&p.srv, p.to_client.pkt, p.to_client.len, &type),
             RS_ERR_INVALID_ARG);

    /* 認証済みでも CI-V フレーム列として不正なペイロードは書かない */
    CHECK_RC(dch_seal(&p.cli.ch, DCH_TYPE_CIV, "\x01\x02\x03", 3, pkt, sizeof(pkt), &len), RS_OK);
    CHECK_RC(civ_link_handle_packet(&p.srv, pkt, len, &type), RS_OK);
    CHECK(p.srv.stats.rejected_payload == 1);
    CHECK(p.radio.tx_len == sizeof(cmd));

    /* BIND は認証済みの登録パケットとして受理 */
    CHECK_RC(civ_link_send_bind(&p.cli), RS_OK);
    CHECK_RC(deliver_to_server(&p, &type), RS_OK);
    CHECK(type == DCH_TYPE_BIND);

    /* 別セッションの鍵では受理しない */
    civ_link_stop(&p.srv);
    civ_link_start(&p.srv, DCH_ROLE_SERVER, 0x9999, k_c2s, k_s2c);
    CHECK_RC(civ_link_send_bind(&p.cli), RS_OK);
    CHECK_RC(deliver_to_server(&p, &type), RS_ERR_INVALID_ARG);
    CHECK(p.srv.ch.stats.rx_unknown_session == 1);
}

static void test_inactive_and_errors(void)
{
    static const uint8_t cmd[] = { 0xFE, 0xFE, 0x7C, 0xE0, 0x03, 0xFD };
    static const uint8_t ptt_off[] = { 0xFE, 0xFE, 0x7C, 0xE0, 0x1C, 0x00, 0x00, 0xFD };
    pair_t p;
    uint8_t frame[16];
    size_t n;
    uint8_t type;

    /* セッションなし: 送らずに破棄、受信も BUSY */
    pair_init(&p, 0);
    ms_feed(&p.pc, cmd, sizeof(cmd));
    CHECK_RC(civ_link_poll(&p.cli), RS_OK);
    CHECK(p.to_server.count == 0);
    CHECK(p.cli.stats.dropped_inactive == 1);
    CHECK_RC(civ_link_send_bind(&p.cli), RS_ERR_BUSY);
    CHECK(!civ_link_active(&p.cli));

    /* フェイルセーフの PTT 解除はセッションと無関係にシリアルへ直接書く */
    n = civ_build_ptt_off(frame, sizeof(frame), CIV_ADDR_IC9100, CIV_ADDR_CONTROLLER);
    CHECK_RC(civ_link_write_raw(&p.srv, frame, n), RS_OK);
    CHECK(p.radio.tx_len == sizeof(ptt_off) && memcmp(p.radio.tx, ptt_off, sizeof(ptt_off)) == 0);

    /* 書き込みタイムアウト・読み込み異常は呼び出し側へ返す */
    pair_init(&p, 1);
    ms_feed(&p.pc, cmd, sizeof(cmd));
    civ_link_poll(&p.cli);
    p.radio.write_timeout = 1;
    CHECK_RC(deliver_to_server(&p, &type), RS_ERR_TIMEOUT);
    CHECK(p.srv.stats.write_timeouts == 1);
    p.radio.fail_read = 1;
    CHECK_RC(civ_link_poll(&p.srv), RS_ERR_IO);
    CHECK(p.srv.stats.io_errors == 1);

    /* ポート未接続 */
    civ_link_attach(&p.srv, NULL);
    CHECK_RC(civ_link_poll(&p.srv), RS_OK);
    CHECK_RC(civ_link_write_raw(&p.srv, frame, n), RS_ERR_NOT_FOUND);
}

/* ---- 制御チャネルで確立した鍵での中継 ------------------------------------ */

typedef struct ctl_wire {
    uint8_t    pkt[256];
    size_t     len;
    net_addr_t to;
    int        pending;
} ctl_wire_t;

static ctl_wire_t g_ctl_to_srv;
static ctl_wire_t g_ctl_to_cli;

static int ctl_srv_send(void *user, const net_addr_t *to, const void *buf, size_t len)
{
    (void)user;
    memcpy(g_ctl_to_cli.pkt, buf, len);
    g_ctl_to_cli.len = len;
    g_ctl_to_cli.to = *to;
    g_ctl_to_cli.pending = 1;
    return RS_OK;
}

static int ctl_cli_send(void *user, const net_addr_t *to, const void *buf, size_t len)
{
    (void)user;
    memcpy(g_ctl_to_srv.pkt, buf, len);
    g_ctl_to_srv.len = len;
    g_ctl_to_srv.to = *to;
    g_ctl_to_srv.pending = 1;
    return RS_OK;
}

static void test_with_control_session_keys(void)
{
    static const uint8_t cmd[] = { 0xFE, 0xFE, 0x7C, 0xE0, 0x03, 0xFD };
    ctl_server_config_t scfg;
    ctl_server_hooks_t shooks;
    ctl_client_config_t ccfg;
    ctl_client_hooks_t chooks;
    ctl_server_t *srv = NULL;
    ctl_client_t *cli = NULL;
    net_addr_t srv_addr;
    net_addr_t cli_addr;
    uint8_t sc2s[CTL_KEY_LEN], ss2c[CTL_KEY_LEN], cc2s[CTL_KEY_LEN], cs2c[CTL_KEY_LEN];
    pair_t p;
    uint8_t type;
    int guard;

    net_addr_resolve(&srv_addr, "2001:db8::1", 50001);
    net_addr_resolve(&cli_addr, "2001:db8::2", 40000);

    ctl_server_config_default(&scfg);
    scfg.username = "op";
    scfg.password = "pw";
    scfg.pbkdf2_iterations = 100;
    memset(&shooks, 0, sizeof(shooks));
    shooks.send = ctl_srv_send;
    CHECK_RC(ctl_server_create(&srv, &scfg, &shooks), RS_OK);

    ctl_client_config_default(&ccfg);
    ccfg.username = "op";
    ccfg.password = "pw";
    ccfg.server = srv_addr;
    memset(&chooks, 0, sizeof(chooks));
    chooks.send = ctl_cli_send;
    CHECK_RC(ctl_client_create(&cli, &ccfg, &chooks), RS_OK);

    /* セッション確立前は鍵を取り出せない */
    CHECK_RC(ctl_server_session_keys(srv, sc2s, ss2c), RS_ERR_NOT_FOUND);
    CHECK_RC(ctl_client_session_keys(cli, cc2s, cs2c), RS_ERR_NOT_FOUND);

    ctl_client_connect(cli, 1000);
    for (guard = 0; guard < 10 && (g_ctl_to_srv.pending || g_ctl_to_cli.pending); guard++) {
        if (g_ctl_to_srv.pending) {
            g_ctl_to_srv.pending = 0;
            ctl_server_handle_packet(srv, &cli_addr, g_ctl_to_srv.pkt, g_ctl_to_srv.len, 1000);
        }
        if (g_ctl_to_cli.pending) {
            g_ctl_to_cli.pending = 0;
            ctl_client_handle_packet(cli, &srv_addr, g_ctl_to_cli.pkt, g_ctl_to_cli.len, 1000);
        }
    }
    CHECK(ctl_client_state(cli) == CTL_CLIENT_CONNECTED);
    CHECK_RC(ctl_server_session_keys(srv, sc2s, ss2c), RS_OK);
    CHECK_RC(ctl_client_session_keys(cli, cc2s, cs2c), RS_OK);
    CHECK(memcmp(sc2s, cc2s, CTL_KEY_LEN) == 0 && memcmp(ss2c, cs2c, CTL_KEY_LEN) == 0);

    pair_init(&p, 0);
    civ_link_start(&p.srv, DCH_ROLE_SERVER, ctl_server_session_id(srv), sc2s, ss2c);
    civ_link_start(&p.cli, DCH_ROLE_CLIENT, ctl_client_session_id(cli), cc2s, cs2c);
    ms_feed(&p.pc, cmd, sizeof(cmd));
    civ_link_poll(&p.cli);
    CHECK_RC(deliver_to_server(&p, &type), RS_OK);
    CHECK(p.radio.tx_len == sizeof(cmd));

    /* 制御チャネルのタグ鍵そのものでは CI-V パケットを偽造できない（チャネル別の鍵） */
    {
        uint8_t forged[64];
        memcpy(forged, p.to_server.pkt, p.to_server.len);
        forged[15]++;
        memcpy(forged + p.to_server.len - CTL_TAG_LEN, sc2s, CTL_TAG_LEN);
        CHECK_RC(civ_link_handle_packet(&p.srv, forged, p.to_server.len, &type),
                 RS_ERR_INVALID_ARG);
    }

    ctl_client_destroy(cli);
    ctl_server_destroy(srv);
}

int main(void)
{
    CHECK_RC(net_init(), RS_OK);
    test_framer();
    test_bidirectional_relay();
    test_tamper_replay_and_validation();
    test_inactive_and_errors();
    test_with_control_session_keys();
    net_cleanup();
    return TEST_RESULT();
}
