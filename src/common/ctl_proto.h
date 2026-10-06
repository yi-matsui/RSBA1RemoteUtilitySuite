/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 制御チャネル (UDP 50001) の拡張モードプロトコル定義。サーバ・クライアント共通。
 * パケット形式・鍵導出・リプレイ検知ウィンドウを提供する。仕様: doc/protocol_spec.md
 */
#ifndef CTL_PROTO_H
#define CTL_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTL_MAGIC0          'R'
#define CTL_MAGIC1          'S'
#define CTL_MAGIC2          'B'
#define CTL_MAGIC3          'A'
#define CTL_VERSION         1

#define CTL_HEADER_LEN      16
#define CTL_TAG_LEN         16   /* HMAC-SHA256 の先頭 16 バイト */
#define CTL_NONCE_LEN       16
#define CTL_SALT_LEN        16
#define CTL_KEY_LEN         32
#define CTL_PROOF_LEN       32
#define CTL_USERNAME_MAX    32

/* HELLO は増幅攻撃対策で CHALLENGE 以上の長さに固定する */
#define CTL_HELLO_PAYLOAD_LEN      96
#define CTL_CHALLENGE_PAYLOAD_LEN  (CTL_NONCE_LEN * 2 + CTL_SALT_LEN + 4)
#define CTL_AUTH_PAYLOAD_LEN       (CTL_NONCE_LEN * 2 + CTL_PROOF_LEN)
#define CTL_AUTH_OK_PAYLOAD_LEN    (CTL_PROOF_LEN + 8)
#define CTL_AUTH_FAIL_PAYLOAD_LEN  1
#define CTL_PING_PAYLOAD_LEN       8
#define CTL_DISCONNECT_PAYLOAD_LEN 1

#define CTL_MAX_PACKET_LEN  (CTL_HEADER_LEN + CTL_HELLO_PAYLOAD_LEN + CTL_TAG_LEN)

typedef enum ctl_type {
    CTL_HELLO      = 1,  /* C→S 接続要求 */
    CTL_CHALLENGE  = 2,  /* S→C チャレンジ */
    CTL_AUTH       = 3,  /* C→S 応答（クライアント証明） */
    CTL_AUTH_OK    = 4,  /* S→C 認証成功（サーバ証明・セッション ID、タグ付き） */
    CTL_AUTH_FAIL  = 5,  /* S→C 認証失敗 */
    CTL_PING       = 6,  /* C→S Keepalive（タグ付き） */
    CTL_PONG       = 7,  /* S→C Keepalive 応答（タグ付き） */
    CTL_DISCONNECT = 8   /* 双方向 切断通知（タグ付き） */
} ctl_type_t;

typedef enum ctl_fail_reason {
    CTL_FAIL_CREDENTIALS = 1,  /* ユーザ名またはパスワード不一致（区別しない） */
    CTL_FAIL_PROTOCOL    = 2   /* チャレンジ期限切れ・不整合 */
} ctl_fail_reason_t;

typedef enum ctl_disc_reason {
    CTL_DISC_NORMAL   = 0,  /* 利用者による切断 */
    CTL_DISC_SHUTDOWN = 1,  /* 送信側の終了 */
    CTL_DISC_REPLACED = 2,  /* 新しいセッションに置き換えられた */
    CTL_DISC_TIMEOUT  = 3   /* Keepalive 途絶 */
} ctl_disc_reason_t;

typedef struct ctl_header {
    uint8_t  type;
    uint16_t payload_len;
    uint32_t session_id;
    uint32_t seq;
} ctl_header_t;

/* 64 パケット幅のスライディングウィンドウ（RFC 4303 方式） */
typedef struct ctl_replay {
    uint32_t top;     /* 受理済み最大シーケンス番号（0 = なし） */
    uint64_t bitmap;  /* bit n: top - n を受理済み */
} ctl_replay_t;

/* タグを付けるべき種別か（セッション確立後のパケット） */
int ctl_type_has_tag(uint8_t type);

/* ヘッダ＋ペイロードを buf に組み立てる。mac_key が非 NULL ならタグを付加する。 */
int ctl_encode(uint8_t *buf, size_t cap, const ctl_header_t *hdr, const void *payload,
               const uint8_t mac_key[CTL_KEY_LEN], size_t *out_len);

/* ヘッダを解析し、マジック・バージョン・長さ（タグ有無を含む）を検証する。
 * タグの検証は行わない（鍵が必要なため ctl_verify_tag で行う）。 */
int ctl_decode(const uint8_t *buf, size_t len, ctl_header_t *hdr, const uint8_t **payload);

/* 末尾 CTL_TAG_LEN バイトのタグを定数時間で検証する。一致で 1 */
int ctl_verify_tag(const uint8_t *buf, size_t len, const uint8_t key[CTL_KEY_LEN]);

void ctl_replay_init(ctl_replay_t *rp);
/* seq が未受理かつウィンドウ内なら 1（状態は変えない） */
int  ctl_replay_check(const ctl_replay_t *rp, uint32_t seq);
/* タグ検証後に受理を記録する */
void ctl_replay_update(ctl_replay_t *rp, uint32_t seq);

/* K = PBKDF2-HMAC-SHA256(password, salt, iterations) */
int ctl_derive_password_key(const char *password, const uint8_t salt[CTL_SALT_LEN],
                            uint32_t iterations, uint8_t key[CTL_KEY_LEN]);
/* HMAC(K, "RSBA1 client" | cn | sn | username) */
void ctl_client_proof(const uint8_t key[CTL_KEY_LEN], const uint8_t cn[CTL_NONCE_LEN],
                      const uint8_t sn[CTL_NONCE_LEN], const char *username,
                      uint8_t proof[CTL_PROOF_LEN]);
/* HMAC(K, "RSBA1 server" | sn | cn | session_id) */
void ctl_server_proof(const uint8_t key[CTL_KEY_LEN], const uint8_t cn[CTL_NONCE_LEN],
                      const uint8_t sn[CTL_NONCE_LEN], uint32_t session_id,
                      uint8_t proof[CTL_PROOF_LEN]);
/* 方向別セッション鍵: HMAC(K, "RSBA1 c2s" / "RSBA1 s2c" | cn | sn | session_id) */
void ctl_session_keys(const uint8_t key[CTL_KEY_LEN], const uint8_t cn[CTL_NONCE_LEN],
                      const uint8_t sn[CTL_NONCE_LEN], uint32_t session_id,
                      uint8_t k_c2s[CTL_KEY_LEN], uint8_t k_s2c[CTL_KEY_LEN]);

/* ビッグエンディアン入出力 */
void     ctl_put_u32(uint8_t *p, uint32_t v);
void     ctl_put_u64(uint8_t *p, uint64_t v);
uint32_t ctl_get_u32(const uint8_t *p);
uint64_t ctl_get_u64(const uint8_t *p);

const char *ctl_type_name(uint8_t type);
const char *ctl_disc_reason_name(uint8_t reason);

#ifdef __cplusplus
}
#endif

#endif /* CTL_PROTO_H */
