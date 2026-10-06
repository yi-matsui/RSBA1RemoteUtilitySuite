/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "ctl_proto.h"

#include <string.h>

#include "rs_crypto.h"
#include "rs_error.h"

/* 増幅攻撃対策: 未認証の要求 (HELLO) は応答 (CHALLENGE) 以上の長さでなければならない */
typedef char ctl_hello_not_shorter_than_challenge
    [(CTL_HELLO_PAYLOAD_LEN >= CTL_CHALLENGE_PAYLOAD_LEN) ? 1 : -1];

void ctl_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void ctl_put_u64(uint8_t *p, uint64_t v)
{
    ctl_put_u32(p, (uint32_t)(v >> 32));
    ctl_put_u32(p + 4, (uint32_t)v);
}

uint32_t ctl_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

uint64_t ctl_get_u64(const uint8_t *p)
{
    return (uint64_t)ctl_get_u32(p) << 32 | ctl_get_u32(p + 4);
}

int ctl_type_has_tag(uint8_t type)
{
    /* AUTH_OK は s2c 鍵でタグ付けし、セッション ID・Keepalive パラメータの改ざんを防ぐ */
    return type == CTL_AUTH_OK || type == CTL_PING || type == CTL_PONG || type == CTL_DISCONNECT;
}

/* 種別ごとの正しいペイロード長（-1 = 未知の種別） */
static int expected_payload_len(uint8_t type)
{
    switch (type) {
    case CTL_HELLO:      return CTL_HELLO_PAYLOAD_LEN;
    case CTL_CHALLENGE:  return CTL_CHALLENGE_PAYLOAD_LEN;
    case CTL_AUTH:       return CTL_AUTH_PAYLOAD_LEN;
    case CTL_AUTH_OK:    return CTL_AUTH_OK_PAYLOAD_LEN;
    case CTL_AUTH_FAIL:  return CTL_AUTH_FAIL_PAYLOAD_LEN;
    case CTL_PING:
    case CTL_PONG:       return CTL_PING_PAYLOAD_LEN;
    case CTL_DISCONNECT: return CTL_DISCONNECT_PAYLOAD_LEN;
    default:             return -1;
    }
}

int ctl_encode(uint8_t *buf, size_t cap, const ctl_header_t *hdr, const void *payload,
               const uint8_t mac_key[CTL_KEY_LEN], size_t *out_len)
{
    size_t len;

    if (buf == NULL || hdr == NULL || out_len == NULL || (payload == NULL && hdr->payload_len > 0))
        return RS_ERR_INVALID_ARG;
    if (expected_payload_len(hdr->type) != (int)hdr->payload_len)
        return RS_ERR_INVALID_ARG;
    if ((mac_key != NULL) != (ctl_type_has_tag(hdr->type) != 0))
        return RS_ERR_INVALID_ARG;

    len = CTL_HEADER_LEN + hdr->payload_len + (mac_key != NULL ? CTL_TAG_LEN : 0);
    if (len > cap)
        return RS_ERR_TRUNCATED;

    buf[0] = CTL_MAGIC0;
    buf[1] = CTL_MAGIC1;
    buf[2] = CTL_MAGIC2;
    buf[3] = CTL_MAGIC3;
    buf[4] = CTL_VERSION;
    buf[5] = hdr->type;
    buf[6] = (uint8_t)(hdr->payload_len >> 8);
    buf[7] = (uint8_t)hdr->payload_len;
    ctl_put_u32(buf + 8, hdr->session_id);
    ctl_put_u32(buf + 12, hdr->seq);
    if (hdr->payload_len > 0)
        memcpy(buf + CTL_HEADER_LEN, payload, hdr->payload_len);

    if (mac_key != NULL) {
        uint8_t tag[32];
        size_t body = CTL_HEADER_LEN + hdr->payload_len;
        rs_hmac_sha256(mac_key, CTL_KEY_LEN, buf, body, tag);
        memcpy(buf + body, tag, CTL_TAG_LEN);
        rs_crypto_wipe(tag, sizeof(tag));
    }

    *out_len = len;
    return RS_OK;
}

int ctl_decode(const uint8_t *buf, size_t len, ctl_header_t *hdr, const uint8_t **payload)
{
    int expect;
    size_t total;

    if (buf == NULL || hdr == NULL)
        return RS_ERR_INVALID_ARG;
    if (len < CTL_HEADER_LEN || buf[0] != CTL_MAGIC0 || buf[1] != CTL_MAGIC1 ||
        buf[2] != CTL_MAGIC2 || buf[3] != CTL_MAGIC3 || buf[4] != CTL_VERSION)
        return RS_ERR_INVALID_ARG;

    hdr->type = buf[5];
    hdr->payload_len = (uint16_t)(buf[6] << 8 | buf[7]);
    hdr->session_id = ctl_get_u32(buf + 8);
    hdr->seq = ctl_get_u32(buf + 12);

    expect = expected_payload_len(hdr->type);
    if (expect < 0 || (int)hdr->payload_len != expect)
        return RS_ERR_INVALID_ARG;
    total = CTL_HEADER_LEN + hdr->payload_len + (ctl_type_has_tag(hdr->type) ? CTL_TAG_LEN : 0);
    if (total != len)
        return RS_ERR_INVALID_ARG;

    if (payload != NULL)
        *payload = buf + CTL_HEADER_LEN;
    return RS_OK;
}

int ctl_verify_tag(const uint8_t *buf, size_t len, const uint8_t key[CTL_KEY_LEN])
{
    uint8_t tag[32];
    int ok;

    if (buf == NULL || key == NULL || len < CTL_HEADER_LEN + CTL_TAG_LEN)
        return 0;
    rs_hmac_sha256(key, CTL_KEY_LEN, buf, len - CTL_TAG_LEN, tag);
    ok = rs_crypto_equal(tag, buf + len - CTL_TAG_LEN, CTL_TAG_LEN);
    rs_crypto_wipe(tag, sizeof(tag));
    return ok;
}

/* ---- リプレイ検知 -------------------------------------------------------- */

void ctl_replay_init(ctl_replay_t *rp)
{
    rp->top = 0;
    rp->bitmap = 0;
}

int ctl_replay_check(const ctl_replay_t *rp, uint32_t seq)
{
    uint32_t diff;

    if (seq == 0)
        return 0;
    if (seq > rp->top)
        return 1;
    diff = rp->top - seq;
    if (diff >= 64)
        return 0;
    return (rp->bitmap & ((uint64_t)1 << diff)) == 0;
}

void ctl_replay_update(ctl_replay_t *rp, uint32_t seq)
{
    if (seq > rp->top) {
        uint32_t shift = seq - rp->top;
        rp->bitmap = shift >= 64 ? 0 : rp->bitmap << shift;
        rp->bitmap |= 1;
        rp->top = seq;
    } else if (rp->top - seq < 64) {
        rp->bitmap |= (uint64_t)1 << (rp->top - seq);
    }
}

/* ---- 鍵導出 -------------------------------------------------------------- */

int ctl_derive_password_key(const char *password, const uint8_t salt[CTL_SALT_LEN],
                            uint32_t iterations, uint8_t key[CTL_KEY_LEN])
{
    if (password == NULL || salt == NULL || key == NULL)
        return RS_ERR_INVALID_ARG;
    return rs_pbkdf2_hmac_sha256(password, strlen(password), salt, CTL_SALT_LEN,
                                 iterations, key, CTL_KEY_LEN);
}

static void hmac_label(const uint8_t key[CTL_KEY_LEN], const char *label,
                       const uint8_t *a, const uint8_t *b, const void *extra, size_t extra_len,
                       uint8_t out[32])
{
    rs_hmac_sha256_t ctx;

    rs_hmac_sha256_init(&ctx, key, CTL_KEY_LEN);
    rs_hmac_sha256_update(&ctx, label, strlen(label));
    rs_hmac_sha256_update(&ctx, a, CTL_NONCE_LEN);
    rs_hmac_sha256_update(&ctx, b, CTL_NONCE_LEN);
    if (extra_len > 0)
        rs_hmac_sha256_update(&ctx, extra, extra_len);
    rs_hmac_sha256_final(&ctx, out);
}

void ctl_client_proof(const uint8_t key[CTL_KEY_LEN], const uint8_t cn[CTL_NONCE_LEN],
                      const uint8_t sn[CTL_NONCE_LEN], const char *username,
                      uint8_t proof[CTL_PROOF_LEN])
{
    hmac_label(key, "RSBA1 client", cn, sn, username, strlen(username), proof);
}

void ctl_server_proof(const uint8_t key[CTL_KEY_LEN], const uint8_t cn[CTL_NONCE_LEN],
                      const uint8_t sn[CTL_NONCE_LEN], uint32_t session_id,
                      uint8_t proof[CTL_PROOF_LEN])
{
    uint8_t sid[4];
    ctl_put_u32(sid, session_id);
    hmac_label(key, "RSBA1 server", sn, cn, sid, sizeof(sid), proof);
}

void ctl_session_keys(const uint8_t key[CTL_KEY_LEN], const uint8_t cn[CTL_NONCE_LEN],
                      const uint8_t sn[CTL_NONCE_LEN], uint32_t session_id,
                      uint8_t k_c2s[CTL_KEY_LEN], uint8_t k_s2c[CTL_KEY_LEN])
{
    uint8_t sid[4];
    ctl_put_u32(sid, session_id);
    hmac_label(key, "RSBA1 c2s", cn, sn, sid, sizeof(sid), k_c2s);
    hmac_label(key, "RSBA1 s2c", cn, sn, sid, sizeof(sid), k_s2c);
}

const char *ctl_type_name(uint8_t type)
{
    switch (type) {
    case CTL_HELLO:      return "HELLO";
    case CTL_CHALLENGE:  return "CHALLENGE";
    case CTL_AUTH:       return "AUTH";
    case CTL_AUTH_OK:    return "AUTH_OK";
    case CTL_AUTH_FAIL:  return "AUTH_FAIL";
    case CTL_PING:       return "PING";
    case CTL_PONG:       return "PONG";
    case CTL_DISCONNECT: return "DISCONNECT";
    default:             return "?";
    }
}

const char *ctl_disc_reason_name(uint8_t reason)
{
    switch (reason) {
    case CTL_DISC_NORMAL:   return "normal";
    case CTL_DISC_SHUTDOWN: return "shutdown";
    case CTL_DISC_REPLACED: return "replaced";
    case CTL_DISC_TIMEOUT:  return "timeout";
    default:                return "?";
    }
}
