/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * データチャネル（CI-V: UDP 50002 / オーディオ: UDP 50003）の秘匿パケット層。
 * 制御チャネルで確立したセッション鍵から、チャネル・方向ごとの暗号鍵と MAC 鍵を導出し、
 *   ヘッダ(16, 平文) ‖ ChaCha20(ペイロード) ‖ HMAC-SHA256(ヘッダ‖暗号文) 先頭 16 バイト
 * の形式で封緘する（Encrypt-then-MAC）。シーケンス番号とリプレイウィンドウはチャネル・方向ごとに独立。
 * 仕様: doc/protocol_spec.md §9
 */
#ifndef DCH_H
#define DCH_H

#include <stddef.h>
#include <stdint.h>

#include "ctl_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* データチャネルのパケット種別（制御チャネルの種別とは重複しない） */
#define DCH_TYPE_BIND   0x10  /* C→S アドレス登録・NAT 維持（ペイロードなし） */
#define DCH_TYPE_CIV    0x11  /* CI-V バイト列 */
#define DCH_TYPE_AUDIO  0x12  /* オーディオフレーム */

#define DCH_OVERHEAD     (CTL_HEADER_LEN + CTL_TAG_LEN)
#define DCH_MAX_PAYLOAD  (1232 - DCH_OVERHEAD)   /* NET_MAX_UDP_PAYLOAD に収まる最大値 = 1200 */

typedef enum dch_channel {
    DCH_CHANNEL_CIV = 0,
    DCH_CHANNEL_AUDIO
} dch_channel_t;

typedef enum dch_role {
    DCH_ROLE_SERVER = 0,  /* 送信 = s2c 鍵、受信 = c2s 鍵 */
    DCH_ROLE_CLIENT       /* 送信 = c2s 鍵、受信 = s2c 鍵 */
} dch_role_t;

typedef struct dch_stats {
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t rx_malformed;
    uint32_t rx_unknown_session;
    uint32_t rx_replay;
    uint32_t rx_bad_tag;
} dch_stats_t;

typedef struct dch {
    int          active;
    uint32_t     session_id;
    uint8_t      tx_enc[CTL_KEY_LEN];
    uint8_t      tx_mac[CTL_KEY_LEN];
    uint8_t      rx_enc[CTL_KEY_LEN];
    uint8_t      rx_mac[CTL_KEY_LEN];
    uint32_t     tx_seq;
    ctl_replay_t rx_window;
    dch_stats_t  stats;
} dch_t;

/* セッション鍵 (k_c2s / k_s2c) からチャネル鍵を導出して有効化する */
void dch_init(dch_t *ch, dch_channel_t channel, dch_role_t role, uint32_t session_id,
              const uint8_t k_c2s[CTL_KEY_LEN], const uint8_t k_s2c[CTL_KEY_LEN]);

/* 鍵を消去して無効化する（統計は保持） */
void dch_reset(dch_t *ch);

/* payload を封緘して out に書く。無効時 RS_ERR_BUSY、長さ超過 RS_ERR_TOO_LARGE */
int dch_seal(dch_t *ch, uint8_t type, const void *payload, size_t len,
             uint8_t *out, size_t cap, size_t *out_len);

/* 受信パケットを検証・復号する（形式 → セッション ID → リプレイ → タグ → 復号）。
 * 不合格は RS_ERR_INVALID_ARG（理由は stats）、無効時 RS_ERR_BUSY。 */
int dch_open(dch_t *ch, const uint8_t *pkt, size_t len, uint8_t *type,
             uint8_t *payload, size_t cap, size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif /* DCH_H */
