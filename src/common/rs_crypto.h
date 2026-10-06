/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 暗号プリミティブ（外部ライブラリ非依存）。
 * SHA-256 (FIPS 180-4) / HMAC-SHA256 (RFC 2104) / PBKDF2-HMAC-SHA256 (RFC 8018) /
 * OS 乱数 / 定数時間比較 / 秘密情報の消去。
 */
#ifndef RS_CRYPTO_H
#define RS_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RS_SHA256_LEN        32
#define RS_SHA256_BLOCK_LEN  64

typedef struct rs_sha256 {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t  block[RS_SHA256_BLOCK_LEN];
    size_t   block_len;
} rs_sha256_t;

void rs_sha256_init(rs_sha256_t *ctx);
void rs_sha256_update(rs_sha256_t *ctx, const void *data, size_t len);
void rs_sha256_final(rs_sha256_t *ctx, uint8_t out[RS_SHA256_LEN]);
void rs_sha256(const void *data, size_t len, uint8_t out[RS_SHA256_LEN]);

typedef struct rs_hmac_sha256 {
    rs_sha256_t inner;
    rs_sha256_t outer;
} rs_hmac_sha256_t;

void rs_hmac_sha256_init(rs_hmac_sha256_t *ctx, const void *key, size_t key_len);
void rs_hmac_sha256_update(rs_hmac_sha256_t *ctx, const void *data, size_t len);
void rs_hmac_sha256_final(rs_hmac_sha256_t *ctx, uint8_t out[RS_SHA256_LEN]);
void rs_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len,
                    uint8_t out[RS_SHA256_LEN]);

/* PBKDF2-HMAC-SHA256。out_len は任意長。iterations は 1 以上。 */
int rs_pbkdf2_hmac_sha256(const void *password, size_t password_len,
                          const void *salt, size_t salt_len, uint32_t iterations,
                          uint8_t *out, size_t out_len);

#define RS_CHACHA20_KEY_LEN   32
#define RS_CHACHA20_NONCE_LEN 12

/* ChaCha20 (RFC 8439) ストリーム暗号。in と out は同一でもよい。暗号化・復号は同じ操作。
 * 同じ (key, nonce) の組を二度使わないこと。 */
void rs_chacha20_xor(const uint8_t key[RS_CHACHA20_KEY_LEN],
                     const uint8_t nonce[RS_CHACHA20_NONCE_LEN], uint32_t counter,
                     const uint8_t *in, uint8_t *out, size_t len);

/* OS の暗号論的乱数（Win: BCryptGenRandom / Linux: getrandom）。失敗時 RS_ERR_SYSTEM */
int rs_crypto_random(void *buf, size_t len);

/* 定数時間比較。一致で 1 */
int rs_crypto_equal(const void *a, const void *b, size_t len);

/* 最適化で消されないメモリ消去 */
void rs_crypto_wipe(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RS_CRYPTO_H */
