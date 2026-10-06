/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "rs_crypto.h"

#include <string.h>

#include "rs_error.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <sys/random.h>
#endif

/* ---- SHA-256 ------------------------------------------------------------- */

static const uint32_t k_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_compress(uint32_t st[8], const uint8_t blk[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)blk[i * 4] << 24 | (uint32_t)blk[i * 4 + 1] << 16 |
               (uint32_t)blk[i * 4 + 2] << 8 | (uint32_t)blk[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = st[0]; b = st[1]; c = st[2]; d = st[3];
    e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k_sha256_k[i] + w[i];
        uint32_t s0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    rs_crypto_wipe(w, sizeof(w));
}

void rs_sha256_init(rs_sha256_t *ctx)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(ctx->state, iv, sizeof(iv));
    ctx->total_len = 0;
    ctx->block_len = 0;
}

void rs_sha256_update(rs_sha256_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = data;

    ctx->total_len += len;
    while (len > 0) {
        size_t n = RS_SHA256_BLOCK_LEN - ctx->block_len;
        if (n > len)
            n = len;
        memcpy(ctx->block + ctx->block_len, p, n);
        ctx->block_len += n;
        p += n;
        len -= n;
        if (ctx->block_len == RS_SHA256_BLOCK_LEN) {
            sha256_compress(ctx->state, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void rs_sha256_final(rs_sha256_t *ctx, uint8_t out[RS_SHA256_LEN])
{
    uint64_t bits = ctx->total_len * 8u;
    int i;

    ctx->block[ctx->block_len++] = 0x80;
    if (ctx->block_len > 56) {
        memset(ctx->block + ctx->block_len, 0, RS_SHA256_BLOCK_LEN - ctx->block_len);
        sha256_compress(ctx->state, ctx->block);
        ctx->block_len = 0;
    }
    memset(ctx->block + ctx->block_len, 0, 56 - ctx->block_len);
    for (i = 0; i < 8; i++)
        ctx->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_compress(ctx->state, ctx->block);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
    rs_crypto_wipe(ctx, sizeof(*ctx));
}

void rs_sha256(const void *data, size_t len, uint8_t out[RS_SHA256_LEN])
{
    rs_sha256_t ctx;
    rs_sha256_init(&ctx);
    rs_sha256_update(&ctx, data, len);
    rs_sha256_final(&ctx, out);
}

/* ---- HMAC-SHA256 --------------------------------------------------------- */

void rs_hmac_sha256_init(rs_hmac_sha256_t *ctx, const void *key, size_t key_len)
{
    uint8_t k[RS_SHA256_BLOCK_LEN];
    uint8_t pad[RS_SHA256_BLOCK_LEN];
    int i;

    memset(k, 0, sizeof(k));
    if (key_len > RS_SHA256_BLOCK_LEN)
        rs_sha256(key, key_len, k);
    else if (key_len > 0)
        memcpy(k, key, key_len);

    for (i = 0; i < RS_SHA256_BLOCK_LEN; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x36);
    rs_sha256_init(&ctx->inner);
    rs_sha256_update(&ctx->inner, pad, sizeof(pad));

    for (i = 0; i < RS_SHA256_BLOCK_LEN; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x5c);
    rs_sha256_init(&ctx->outer);
    rs_sha256_update(&ctx->outer, pad, sizeof(pad));

    rs_crypto_wipe(k, sizeof(k));
    rs_crypto_wipe(pad, sizeof(pad));
}

void rs_hmac_sha256_update(rs_hmac_sha256_t *ctx, const void *data, size_t len)
{
    rs_sha256_update(&ctx->inner, data, len);
}

void rs_hmac_sha256_final(rs_hmac_sha256_t *ctx, uint8_t out[RS_SHA256_LEN])
{
    uint8_t ih[RS_SHA256_LEN];

    rs_sha256_final(&ctx->inner, ih);
    rs_sha256_update(&ctx->outer, ih, sizeof(ih));
    rs_sha256_final(&ctx->outer, out);
    rs_crypto_wipe(ih, sizeof(ih));
}

void rs_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len,
                    uint8_t out[RS_SHA256_LEN])
{
    rs_hmac_sha256_t ctx;
    rs_hmac_sha256_init(&ctx, key, key_len);
    rs_hmac_sha256_update(&ctx, data, len);
    rs_hmac_sha256_final(&ctx, out);
}

/* ---- PBKDF2-HMAC-SHA256 -------------------------------------------------- */

int rs_pbkdf2_hmac_sha256(const void *password, size_t password_len,
                          const void *salt, size_t salt_len, uint32_t iterations,
                          uint8_t *out, size_t out_len)
{
    rs_hmac_sha256_t base;
    uint32_t block_index = 1;

    if ((password == NULL && password_len > 0) || (salt == NULL && salt_len > 0) ||
        out == NULL || iterations == 0)
        return RS_ERR_INVALID_ARG;

    /* パスワード鍵のパディング処理は全ブロック共通なので 1 回だけ行う */
    rs_hmac_sha256_init(&base, password, password_len);

    while (out_len > 0) {
        rs_hmac_sha256_t ctx = base;
        uint8_t be[4];
        uint8_t u[RS_SHA256_LEN];
        uint8_t t[RS_SHA256_LEN];
        size_t n = out_len < RS_SHA256_LEN ? out_len : RS_SHA256_LEN;
        uint32_t i;
        int j;

        be[0] = (uint8_t)(block_index >> 24);
        be[1] = (uint8_t)(block_index >> 16);
        be[2] = (uint8_t)(block_index >> 8);
        be[3] = (uint8_t)block_index;
        rs_hmac_sha256_update(&ctx, salt, salt_len);
        rs_hmac_sha256_update(&ctx, be, sizeof(be));
        rs_hmac_sha256_final(&ctx, u);
        memcpy(t, u, sizeof(t));

        for (i = 1; i < iterations; i++) {
            ctx = base;
            rs_hmac_sha256_update(&ctx, u, sizeof(u));
            rs_hmac_sha256_final(&ctx, u);
            for (j = 0; j < RS_SHA256_LEN; j++)
                t[j] ^= u[j];
        }

        memcpy(out, t, n);
        out += n;
        out_len -= n;
        block_index++;
        rs_crypto_wipe(u, sizeof(u));
        rs_crypto_wipe(t, sizeof(t));
    }

    rs_crypto_wipe(&base, sizeof(base));
    return RS_OK;
}

/* ---- ChaCha20 ------------------------------------------------------------ */

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d)                       \
    do {                                     \
        a += b; d ^= a; d = ROTL(d, 16);     \
        c += d; b ^= c; b = ROTL(b, 12);     \
        a += b; d ^= a; d = ROTL(d, 8);      \
        c += d; b ^= c; b = ROTL(b, 7);      \
    } while (0)

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void chacha20_block(const uint32_t in[16], uint8_t out[64])
{
    uint32_t x[16];
    int i;

    memcpy(x, in, sizeof(x));
    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16; i++) {
        uint32_t v = x[i] + in[i];
        out[i * 4] = (uint8_t)v;
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
    rs_crypto_wipe(x, sizeof(x));
}

void rs_chacha20_xor(const uint8_t key[RS_CHACHA20_KEY_LEN],
                     const uint8_t nonce[RS_CHACHA20_NONCE_LEN], uint32_t counter,
                     const uint8_t *in, uint8_t *out, size_t len)
{
    uint32_t st[16];
    uint8_t ks[64];
    size_t i;

    st[0] = 0x61707865;
    st[1] = 0x3320646e;
    st[2] = 0x79622d32;
    st[3] = 0x6b206574;
    for (i = 0; i < 8; i++)
        st[4 + i] = load_le32(key + i * 4);
    st[12] = counter;
    st[13] = load_le32(nonce);
    st[14] = load_le32(nonce + 4);
    st[15] = load_le32(nonce + 8);

    while (len > 0) {
        size_t n = len < sizeof(ks) ? len : sizeof(ks);
        chacha20_block(st, ks);
        for (i = 0; i < n; i++)
            out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += n;
        out += n;
        len -= n;
        st[12]++;
    }
    rs_crypto_wipe(st, sizeof(st));
    rs_crypto_wipe(ks, sizeof(ks));
}

/* ---- 乱数・比較・消去 ---------------------------------------------------- */

int rs_crypto_random(void *buf, size_t len)
{
#ifdef _WIN32
    uint8_t *p = buf;
    while (len > 0) {
        ULONG n = len > 0x7fffffffu ? 0x7fffffffu : (ULONG)len;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            return RS_ERR_SYSTEM;
        p += n;
        len -= n;
    }
    return RS_OK;
#else
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return RS_ERR_SYSTEM;
        }
        p += n;
        len -= (size_t)n;
    }
    return RS_OK;
#endif
}

int rs_crypto_equal(const void *a, const void *b, size_t len)
{
    const volatile uint8_t *pa = a;
    const volatile uint8_t *pb = b;
    uint8_t diff = 0;
    size_t i;

    for (i = 0; i < len; i++)
        diff |= (uint8_t)(pa[i] ^ pb[i]);
    return diff == 0;
}

void rs_crypto_wipe(void *buf, size_t len)
{
    volatile uint8_t *p = buf;
    while (len-- > 0)
        *p++ = 0;
}
