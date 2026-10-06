/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "dch.h"

#include <string.h>

#include "rs_crypto.h"
#include "rs_error.h"

static void derive(const uint8_t base[CTL_KEY_LEN], const char *channel, const char *purpose,
                   uint8_t out[CTL_KEY_LEN])
{
    rs_hmac_sha256_t ctx;

    rs_hmac_sha256_init(&ctx, base, CTL_KEY_LEN);
    rs_hmac_sha256_update(&ctx, "RSBA1 ", 6);
    rs_hmac_sha256_update(&ctx, channel, strlen(channel));
    rs_hmac_sha256_update(&ctx, " ", 1);
    rs_hmac_sha256_update(&ctx, purpose, strlen(purpose));
    rs_hmac_sha256_final(&ctx, out);
}

void dch_init(dch_t *ch, dch_channel_t channel, dch_role_t role, uint32_t session_id,
              const uint8_t k_c2s[CTL_KEY_LEN], const uint8_t k_s2c[CTL_KEY_LEN])
{
    const char *name = channel == DCH_CHANNEL_CIV ? "civ" : "audio";
    const uint8_t *tx = role == DCH_ROLE_SERVER ? k_s2c : k_c2s;
    const uint8_t *rx = role == DCH_ROLE_SERVER ? k_c2s : k_s2c;
    dch_stats_t stats = ch->stats;

    memset(ch, 0, sizeof(*ch));
    ch->stats = stats;
    derive(tx, name, "enc", ch->tx_enc);
    derive(tx, name, "mac", ch->tx_mac);
    derive(rx, name, "enc", ch->rx_enc);
    derive(rx, name, "mac", ch->rx_mac);
    ch->session_id = session_id;
    ctl_replay_init(&ch->rx_window);
    ch->active = 1;
}

void dch_reset(dch_t *ch)
{
    dch_stats_t stats;

    if (ch == NULL)
        return;
    stats = ch->stats;
    rs_crypto_wipe(ch, sizeof(*ch));
    ch->stats = stats;
}

/* ノンス = 0x00000000 ‖ seq(BE) ‖ 0x00000000。鍵がチャネル・方向ごとに異なり、
 * seq は鍵ごとに単調増加するため (鍵, ノンス) は重複しない */
static void make_nonce(uint32_t seq, uint8_t nonce[RS_CHACHA20_NONCE_LEN])
{
    memset(nonce, 0, RS_CHACHA20_NONCE_LEN);
    ctl_put_u32(nonce + 4, seq);
}

int dch_seal(dch_t *ch, uint8_t type, const void *payload, size_t len,
             uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t nonce[RS_CHACHA20_NONCE_LEN];
    uint8_t tag[RS_SHA256_LEN];
    size_t total;

    if (ch == NULL || out == NULL || out_len == NULL || (payload == NULL && len > 0))
        return RS_ERR_INVALID_ARG;
    if (!ch->active)
        return RS_ERR_BUSY;
    if (len > DCH_MAX_PAYLOAD)
        return RS_ERR_TOO_LARGE;
    total = DCH_OVERHEAD + len;
    if (total > cap)
        return RS_ERR_TRUNCATED;
    if (ch->tx_seq == UINT32_MAX)
        return RS_ERR_BUSY; /* 鍵の寿命切れ（実運用では到達しない: 100 pkt/s で 497 日） */

    ch->tx_seq++;
    out[0] = CTL_MAGIC0;
    out[1] = CTL_MAGIC1;
    out[2] = CTL_MAGIC2;
    out[3] = CTL_MAGIC3;
    out[4] = CTL_VERSION;
    out[5] = type;
    out[6] = (uint8_t)(len >> 8);
    out[7] = (uint8_t)len;
    ctl_put_u32(out + 8, ch->session_id);
    ctl_put_u32(out + 12, ch->tx_seq);

    make_nonce(ch->tx_seq, nonce);
    if (len > 0)
        rs_chacha20_xor(ch->tx_enc, nonce, 1, payload, out + CTL_HEADER_LEN, len);
    rs_hmac_sha256(ch->tx_mac, CTL_KEY_LEN, out, CTL_HEADER_LEN + len, tag);
    memcpy(out + CTL_HEADER_LEN + len, tag, CTL_TAG_LEN);
    rs_crypto_wipe(tag, sizeof(tag));

    ch->stats.tx_packets++;
    *out_len = total;
    return RS_OK;
}

int dch_open(dch_t *ch, const uint8_t *pkt, size_t len, uint8_t *type,
             uint8_t *payload, size_t cap, size_t *payload_len)
{
    uint8_t nonce[RS_CHACHA20_NONCE_LEN];
    size_t plen;
    uint32_t sid;
    uint32_t seq;

    if (ch == NULL || pkt == NULL || type == NULL || payload_len == NULL ||
        (payload == NULL && cap > 0))
        return RS_ERR_INVALID_ARG;
    if (!ch->active)
        return RS_ERR_BUSY;

    if (len < DCH_OVERHEAD || pkt[0] != CTL_MAGIC0 || pkt[1] != CTL_MAGIC1 ||
        pkt[2] != CTL_MAGIC2 || pkt[3] != CTL_MAGIC3 || pkt[4] != CTL_VERSION ||
        pkt[5] < DCH_TYPE_BIND || pkt[5] > DCH_TYPE_AUDIO) {
        ch->stats.rx_malformed++;
        return RS_ERR_INVALID_ARG;
    }
    plen = (size_t)pkt[6] << 8 | pkt[7];
    if (plen + DCH_OVERHEAD != len || plen > DCH_MAX_PAYLOAD || plen > cap) {
        ch->stats.rx_malformed++;
        return RS_ERR_INVALID_ARG;
    }
    sid = ctl_get_u32(pkt + 8);
    seq = ctl_get_u32(pkt + 12);
    if (sid != ch->session_id) {
        ch->stats.rx_unknown_session++;
        return RS_ERR_INVALID_ARG;
    }
    if (!ctl_replay_check(&ch->rx_window, seq)) {
        ch->stats.rx_replay++;
        return RS_ERR_INVALID_ARG;
    }
    if (!ctl_verify_tag(pkt, len, ch->rx_mac)) {
        ch->stats.rx_bad_tag++;
        return RS_ERR_INVALID_ARG;
    }
    ctl_replay_update(&ch->rx_window, seq);

    make_nonce(seq, nonce);
    if (plen > 0)
        rs_chacha20_xor(ch->rx_enc, nonce, 1, pkt + CTL_HEADER_LEN, payload, plen);
    *type = pkt[5];
    *payload_len = plen;
    ch->stats.rx_packets++;
    return RS_OK;
}
