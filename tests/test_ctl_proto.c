/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * ctl_proto（パケット形式・タグ・リプレイウィンドウ）と ban_list / civ のテスト。
 */
#include <string.h>

#include "ban_list.h"
#include "civ.h"
#include "ctl_proto.h"
#include "net_socket.h"
#include "rs_error.h"
#include "test_util.h"

static void test_encode_decode(void)
{
    uint8_t key[CTL_KEY_LEN];
    uint8_t other_key[CTL_KEY_LEN];
    uint8_t buf[CTL_MAX_PACKET_LEN];
    uint8_t payload[CTL_PING_PAYLOAD_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ctl_header_t h;
    ctl_header_t d;
    const uint8_t *p = NULL;
    size_t len = 0;
    size_t i;

    memset(key, 0x11, sizeof(key));
    memset(other_key, 0x22, sizeof(other_key));

    h.type = CTL_PING;
    h.payload_len = CTL_PING_PAYLOAD_LEN;
    h.session_id = 0xA1B2C3D4u;
    h.seq = 42;
    CHECK_RC(ctl_encode(buf, sizeof(buf), &h, payload, key, &len), RS_OK);
    CHECK(len == CTL_HEADER_LEN + CTL_PING_PAYLOAD_LEN + CTL_TAG_LEN);
    CHECK(memcmp(buf, "RSBA", 4) == 0 && buf[4] == CTL_VERSION && buf[5] == CTL_PING);
    CHECK(buf[8] == 0xA1 && buf[11] == 0xD4 && buf[15] == 42); /* ビッグエンディアン */

    CHECK_RC(ctl_decode(buf, len, &d, &p), RS_OK);
    CHECK(d.type == CTL_PING && d.session_id == 0xA1B2C3D4u && d.seq == 42);
    CHECK(memcmp(p, payload, sizeof(payload)) == 0);
    CHECK(ctl_verify_tag(buf, len, key));
    CHECK(!ctl_verify_tag(buf, len, other_key));

    /* どのバイトを書き換えてもタグ検証に失敗する（改ざん検知） */
    for (i = 0; i < len; i++) {
        buf[i] ^= 0x01;
        CHECK(!ctl_verify_tag(buf, len, key));
        buf[i] ^= 0x01;
    }

    /* 長さ不一致・マジック不正・バージョン不正・未知の種別 */
    CHECK_RC(ctl_decode(buf, len - 1, &d, &p), RS_ERR_INVALID_ARG);
    buf[0] = 'X';
    CHECK_RC(ctl_decode(buf, len, &d, &p), RS_ERR_INVALID_ARG);
    buf[0] = 'R';
    buf[4] = 99;
    CHECK_RC(ctl_decode(buf, len, &d, &p), RS_ERR_INVALID_ARG);
    buf[4] = CTL_VERSION;
    buf[5] = 0x7F;
    CHECK_RC(ctl_decode(buf, len, &d, &p), RS_ERR_INVALID_ARG);

    /* タグの要否と鍵指定の不整合は組み立てない */
    CHECK_RC(ctl_encode(buf, sizeof(buf), &h, payload, NULL, &len), RS_ERR_INVALID_ARG);
    h.type = CTL_AUTH_FAIL;
    h.payload_len = 1;
    CHECK_RC(ctl_encode(buf, sizeof(buf), &h, payload, key, &len), RS_ERR_INVALID_ARG);
    CHECK_RC(ctl_encode(buf, sizeof(buf), &h, payload, NULL, &len), RS_OK);
    h.payload_len = 2;
    CHECK_RC(ctl_encode(buf, sizeof(buf), &h, payload, NULL, &len), RS_ERR_INVALID_ARG);
    h.payload_len = 1;
    CHECK_RC(ctl_encode(buf, 10, &h, payload, NULL, &len), RS_ERR_TRUNCATED);
}

static void test_replay_window(void)
{
    ctl_replay_t rp;

    ctl_replay_init(&rp);
    CHECK(!ctl_replay_check(&rp, 0));
    CHECK(ctl_replay_check(&rp, 1));
    ctl_replay_update(&rp, 1);
    CHECK(!ctl_replay_check(&rp, 1));          /* 重複 */

    ctl_replay_update(&rp, 5);                  /* 2〜4 を飛ばす（UDP の順序入れ替わり） */
    CHECK(ctl_replay_check(&rp, 3));
    ctl_replay_update(&rp, 3);
    CHECK(!ctl_replay_check(&rp, 3));
    CHECK(ctl_replay_check(&rp, 2));
    CHECK(!ctl_replay_check(&rp, 5));

    ctl_replay_update(&rp, 100);
    CHECK(!ctl_replay_check(&rp, 36));          /* ウィンドウ外（100 - 64） */
    CHECK(ctl_replay_check(&rp, 37));
    CHECK(!ctl_replay_check(&rp, 100));
    CHECK(ctl_replay_check(&rp, 101));
}

static void test_key_derivation(void)
{
    uint8_t salt[CTL_SALT_LEN] = { 0 };
    uint8_t k1[CTL_KEY_LEN];
    uint8_t k2[CTL_KEY_LEN];
    uint8_t cn[CTL_NONCE_LEN];
    uint8_t sn[CTL_NONCE_LEN];
    uint8_t c2s[CTL_KEY_LEN];
    uint8_t s2c[CTL_KEY_LEN];
    uint8_t c2s_b[CTL_KEY_LEN];
    uint8_t s2c_b[CTL_KEY_LEN];
    uint8_t pr1[CTL_PROOF_LEN];
    uint8_t pr2[CTL_PROOF_LEN];

    CHECK_RC(ctl_derive_password_key("secret", salt, 10, k1), RS_OK);
    CHECK_RC(ctl_derive_password_key("secreT", salt, 10, k2), RS_OK);
    CHECK(memcmp(k1, k2, sizeof(k1)) != 0);

    memset(cn, 1, sizeof(cn));
    memset(sn, 2, sizeof(sn));
    ctl_session_keys(k1, cn, sn, 7, c2s, s2c);
    CHECK(memcmp(c2s, s2c, sizeof(c2s)) != 0); /* 方向ごとに別の鍵 */
    ctl_session_keys(k1, cn, sn, 8, c2s_b, s2c_b);
    CHECK(memcmp(c2s, c2s_b, sizeof(c2s)) != 0);

    ctl_client_proof(k1, cn, sn, "op", pr1);
    ctl_client_proof(k1, cn, sn, "oq", pr2);
    CHECK(memcmp(pr1, pr2, sizeof(pr1)) != 0);
    ctl_server_proof(k1, cn, sn, 7, pr2);
    CHECK(memcmp(pr1, pr2, sizeof(pr1)) != 0); /* クライアント証明とサーバ証明は別物 */
}

static void test_ban_list(void)
{
    ban_list_t bl;
    ban_list_config_t cfg;
    net_addr_t a;
    net_addr_t a_other_port;
    net_addr_t b;

    cfg.max_failures = 3;
    cfg.failure_window_ms = 1000;
    cfg.ban_ms = 5000;
    ban_list_init(&bl, &cfg);
    net_addr_resolve(&a, "2001:db8::5", 1000);
    net_addr_resolve(&a_other_port, "2001:db8::5", 2000);
    net_addr_resolve(&b, "192.0.2.7", 1000);

    CHECK(ban_list_record_failure(&bl, &a, 0) == 0);
    CHECK(ban_list_record_failure(&bl, &a_other_port, 10) == 0); /* ポートは区別しない */
    CHECK(ban_list_banned_ms(&bl, &a, 20) == 0);
    CHECK(ban_list_record_failure(&bl, &a, 20) == 1);
    CHECK(ban_list_banned_ms(&bl, &a, 20) == 5000);
    CHECK(ban_list_banned_ms(&bl, &a_other_port, 1020) == 4000);
    CHECK(ban_list_banned_ms(&bl, &b, 20) == 0);                 /* 他のアドレスは影響なし */
    CHECK(ban_list_banned_ms(&bl, &a, 5020) == 0);               /* 期限切れで解除 */

    /* 集計期間を過ぎた失敗は数え直す */
    CHECK(ban_list_record_failure(&bl, &b, 0) == 0);
    CHECK(ban_list_record_failure(&bl, &b, 100) == 0);
    CHECK(ban_list_record_failure(&bl, &b, 2000) == 0);
    CHECK(ban_list_banned_ms(&bl, &b, 2000) == 0);

    /* 成功で履歴を消去 */
    ban_list_record_failure(&bl, &b, 2100);
    ban_list_record_success(&bl, &b);
    CHECK(ban_list_record_failure(&bl, &b, 2200) == 0);
    CHECK(ban_list_record_failure(&bl, &b, 2300) == 0);
    CHECK(ban_list_record_failure(&bl, &b, 2400) == 1);
}

static void test_civ(void)
{
    static const uint8_t expect[] = { 0xFE, 0xFE, 0x7C, 0xE0, 0x1C, 0x00, 0x00, 0xFD };
    static const uint8_t bad_body[] = { 0x1C, 0xFD };
    uint8_t buf[16];

    CHECK(civ_build_ptt_off(buf, sizeof(buf), CIV_ADDR_IC9100, CIV_ADDR_CONTROLLER) == 8);
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    CHECK(civ_build_ptt_off(buf, 7, CIV_ADDR_IC9100, CIV_ADDR_CONTROLLER) == 0);
    CHECK(civ_build_frame(buf, sizeof(buf), 0x7C, 0xE0, bad_body, sizeof(bad_body)) == 0);
}

int main(void)
{
    CHECK_RC(net_init(), RS_OK);
    test_encode_decode();
    test_replay_window();
    test_key_derivation();
    test_ban_list();
    test_civ();
    net_cleanup();
    return TEST_RESULT();
}
