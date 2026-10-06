/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * rs_crypto のテスト（FIPS 180-4 / RFC 4231 / RFC 7914 のテストベクタ）。
 */
#include <stdio.h>
#include <string.h>

#include "rs_crypto.h"
#include "rs_error.h"
#include "test_util.h"

static int hex_eq(const uint8_t *bin, size_t len, const char *hex)
{
    char buf[256];
    size_t i;

    for (i = 0; i < len; i++)
        sprintf(buf + i * 2, "%02x", bin[i]);
    return strcmp(buf, hex) == 0;
}

static void test_sha256(void)
{
    uint8_t out[32];
    rs_sha256_t ctx;
    static char million_a[1000];
    int i;

    rs_sha256("", 0, out);
    CHECK(hex_eq(out, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    rs_sha256("abc", 3, out);
    CHECK(hex_eq(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    rs_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    CHECK(hex_eq(out, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    /* 100 万文字の 'a'（分割入力でブロック境界をまたぐ） */
    memset(million_a, 'a', sizeof(million_a));
    rs_sha256_init(&ctx);
    for (i = 0; i < 1000; i++)
        rs_sha256_update(&ctx, million_a, sizeof(million_a));
    rs_sha256_final(&ctx, out);
    CHECK(hex_eq(out, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void test_hmac(void)
{
    uint8_t key[131];
    uint8_t out[32];

    memset(key, 0x0b, 20);
    rs_hmac_sha256(key, 20, "Hi There", 8, out);
    CHECK(hex_eq(out, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

    rs_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    CHECK(hex_eq(out, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    /* ブロック長を超える鍵（RFC 4231 Test Case 6） */
    memset(key, 0xaa, sizeof(key));
    rs_hmac_sha256(key, sizeof(key), "Test Using Larger Than Block-Size Key - Hash Key First", 54,
                   out);
    CHECK(hex_eq(out, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

static void test_pbkdf2(void)
{
    uint8_t out[64];

    CHECK_RC(rs_pbkdf2_hmac_sha256("password", 8, "salt", 4, 1, out, 32), RS_OK);
    CHECK(hex_eq(out, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    CHECK_RC(rs_pbkdf2_hmac_sha256("password", 8, "salt", 4, 2, out, 32), RS_OK);
    CHECK(hex_eq(out, 32, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
    CHECK_RC(rs_pbkdf2_hmac_sha256("password", 8, "salt", 4, 4096, out, 32), RS_OK);
    CHECK(hex_eq(out, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));

    /* RFC 7914 §11: 64 バイト出力（2 ブロック） */
    CHECK_RC(rs_pbkdf2_hmac_sha256("passwd", 6, "salt", 4, 1, out, 64), RS_OK);
    CHECK(hex_eq(out, 64,
                 "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                 "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));

    CHECK_RC(rs_pbkdf2_hmac_sha256("p", 1, "s", 1, 0, out, 32), RS_ERR_INVALID_ARG);
}

static void test_chacha20(void)
{
    static const char plain[] =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for "
        "the future, sunscreen would be it.";
    uint8_t key[32];
    uint8_t nonce[12] = { 0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    uint8_t buf[sizeof(plain) - 1];
    int i;

    for (i = 0; i < 32; i++)
        key[i] = (uint8_t)i;

    /* RFC 8439 §2.4.2 */
    rs_chacha20_xor(key, nonce, 1, (const uint8_t *)plain, buf, sizeof(buf));
    CHECK(hex_eq(buf, 64,
                 "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
                 "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"));
    CHECK(hex_eq(buf + 64, 50,
                 "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
                 "5af90bbf74a35be6b40b8eedf2785e42874d"));

    /* 同じ操作で復号できる（in == out も可） */
    rs_chacha20_xor(key, nonce, 1, buf, buf, sizeof(buf));
    CHECK(memcmp(buf, plain, sizeof(buf)) == 0);
}

static void test_misc(void)
{
    uint8_t a[32];
    uint8_t b[32];
    static const uint8_t zero[32] = { 0 };

    CHECK_RC(rs_crypto_random(a, sizeof(a)), RS_OK);
    CHECK_RC(rs_crypto_random(b, sizeof(b)), RS_OK);
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    CHECK(memcmp(a, zero, sizeof(a)) != 0);

    CHECK(rs_crypto_equal(a, a, sizeof(a)));
    memcpy(b, a, sizeof(a));
    b[31] ^= 1;
    CHECK(!rs_crypto_equal(a, b, sizeof(a)));

    rs_crypto_wipe(a, sizeof(a));
    CHECK(memcmp(a, zero, sizeof(a)) == 0);
}

int main(void)
{
    test_sha256();
    test_hmac();
    test_pbkdf2();
    test_chacha20();
    test_misc();
    return TEST_RESULT();
}
