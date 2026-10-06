/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * JSON パーサのテスト: 正常系・RFC 8259 違反の拒否・UTF-8 / エスケープ・深さとサイズの上限・
 * エラー位置・変異入力（ファジング）での堅牢性。
 */
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "rs_error.h"
#include "test_util.h"

static int parse_ok(const char *text)
{
    json_value_t *v = NULL;
    int rc = json_parse(text, strlen(text), &v, NULL);
    json_free(v);
    return rc == RS_OK;
}

static int parse_fails(const char *text)
{
    json_value_t *v = NULL;
    json_error_t err;
    int rc = json_parse(text, strlen(text), &v, &err);
    int ok = rc != RS_OK && v == NULL && err.message[0] != '\0';
    if (!ok)
        fprintf(stderr, "  expected failure: %s\n", text);
    json_free(v);
    return ok;
}

static void test_document(void)
{
    static const char doc[] =
        "{\n"
        "  \"listen\": { \"address\": \"::\", \"control_port\": 50001 },\n"
        "  \"flags\": [true, false, null],\n"
        "  \"ratio\": -12.5e-1,\n"
        "  \"name\": \"IC-9100 \\u00e9\\ud83d\\ude00 日本語\",\n"
        "  \"empty\": {}, \"none\": []\n"
        "}";
    json_value_t *v = NULL;
    const json_value_t *x;

    CHECK_RC(json_parse(doc, sizeof(doc) - 1, &v, NULL), RS_OK);
    CHECK(v != NULL && v->type == JSON_OBJECT && v->u.object.count == 6);

    x = json_get_path(v, "listen.control_port");
    CHECK(x != NULL && x->type == JSON_NUMBER && x->u.number == 50001);
    CHECK(x != NULL && x->line == 2);
    x = json_get_path(v, "listen.address");
    CHECK(x != NULL && x->type == JSON_STRING && strcmp(x->u.string.ptr, "::") == 0);
    x = json_object_get(v, "flags");
    CHECK(x != NULL && x->type == JSON_ARRAY && x->u.array.count == 3);
    CHECK(x != NULL && x->u.array.items[0]->type == JSON_BOOL && x->u.array.items[0]->u.boolean == 1);
    CHECK(x != NULL && x->u.array.items[1]->u.boolean == 0);
    CHECK(x != NULL && x->u.array.items[2]->type == JSON_NULL);
    x = json_object_get(v, "ratio");
    CHECK(x != NULL && x->u.number == -1.25);
    x = json_object_get(v, "name");
    /* é = C3 A9、😀 = F0 9F 98 80、日本語は UTF-8 のまま */
    CHECK(x != NULL && strcmp(x->u.string.ptr,
                              "IC-9100 \xC3\xA9\xF0\x9F\x98\x80 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") == 0);
    CHECK(json_get_path(v, "listen.missing") == NULL);
    CHECK(json_get_path(v, "ratio.deeper") == NULL);
    CHECK(json_get_path(v, "") == v);
    CHECK(json_object_get(x, "anything") == NULL); /* オブジェクト以外 */
    CHECK(strcmp(json_type_name(JSON_OBJECT), "object") == 0);
    json_free(v);
}

static void test_scalars(void)
{
    CHECK(parse_ok("0"));
    CHECK(parse_ok("-0"));
    CHECK(parse_ok("1.5e3"));
    CHECK(parse_ok("-1E-2"));
    CHECK(parse_ok("\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t\""));
    CHECK(parse_ok("  true  "));
    CHECK(parse_ok("null"));

    CHECK(parse_fails("01"));          /* 先頭ゼロ */
    CHECK(parse_fails("1."));
    CHECK(parse_fails(".5"));
    CHECK(parse_fails("+1"));
    CHECK(parse_fails("-"));
    CHECK(parse_fails("1e"));
    CHECK(parse_fails("NaN"));
    CHECK(parse_fails("Infinity"));
    CHECK(parse_fails("tru"));
    CHECK(parse_fails("truex"));
    CHECK(parse_fails("'single'"));
    CHECK(parse_fails("\"\\x41\""));          /* 未定義のエスケープ */
    CHECK(parse_fails("\"\\ud800\""));        /* 対にならないサロゲート */
    CHECK(parse_fails("\"\\udc00\""));
    CHECK(parse_fails("\"\\ud800\\u0041\""));
    CHECK(parse_fails("\"\\u0000\""));        /* NUL は C 文字列に入らない */
    CHECK(parse_fails("\"\\u12g4\""));
    CHECK(parse_fails("\"tab\there\""));      /* 生の制御文字 */
    CHECK(parse_fails("\"\xC3\x28\""));       /* 不正な UTF-8 */
    CHECK(parse_fails("\"\xC0\xAF\""));       /* 過長表現 */
    CHECK(parse_fails("\"\xED\xA0\x80\""));   /* UTF-8 でのサロゲート */
    CHECK(parse_fails("\"\xF4\x90\x80\x80\""));/* U+10FFFF 超 */
    CHECK(parse_fails("\"unterminated"));
}

static void test_structure(void)
{
    CHECK(parse_ok("[]"));
    CHECK(parse_ok("{}"));
    CHECK(parse_ok("[1, [2, [3]], {\"a\": {\"b\": []}}]"));

    CHECK(parse_fails(""));
    CHECK(parse_fails("   \n "));
    CHECK(parse_fails("[1, 2,]"));               /* 末尾カンマ */
    CHECK(parse_fails("{\"a\": 1,}"));
    CHECK(parse_fails("{\"a\" 1}"));
    CHECK(parse_fails("{a: 1}"));                /* キーは文字列のみ */
    CHECK(parse_fails("{\"a\": 1, \"a\": 2}"));  /* キー重複 */
    CHECK(parse_fails("[1 2]"));
    CHECK(parse_fails("[1, 2"));
    CHECK(parse_fails("{\"a\": 1} {}"));         /* 後続データ */
    CHECK(parse_fails("{\"a\": 1} // comment")); /* コメント */
    CHECK(parse_fails("/* c */ {}"));
}

static void test_bom_depth_size_position(void)
{
    char deep[128];
    char *big;
    json_value_t *v = NULL;
    json_error_t err;
    int i;

    /* UTF-8 BOM（メモ帳が付ける）は読み飛ばす */
    CHECK(parse_ok("\xEF\xBB\xBF{\"a\": 1}"));

    /* 深さ 32 まで可、33 は拒否 */
    for (i = 0; i < 32; i++)
        deep[i] = '[';
    for (i = 0; i < 32; i++)
        deep[32 + i] = ']';
    deep[64] = '\0';
    CHECK(parse_ok(deep));
    memmove(deep + 1, deep, 65);
    deep[0] = '[';
    deep[65] = ']';
    deep[66] = '\0';
    CHECK(parse_fails(deep));

    /* サイズ上限 */
    big = malloc(JSON_MAX_INPUT + 2);
    CHECK(big != NULL);
    if (big != NULL) {
        memset(big, ' ', JSON_MAX_INPUT + 1);
        big[0] = '0';
        CHECK_RC(json_parse(big, JSON_MAX_INPUT + 1, &v, &err), RS_ERR_TOO_LARGE);
        CHECK_RC(json_parse(big, JSON_MAX_INPUT, &v, &err), RS_OK);
        json_free(v);
        free(big);
    }

    /* エラー位置（行・桁） */
    v = NULL;
    CHECK_RC(json_parse("{\n  \"a\": 1,\n  \"b\": 01\n}", 24, &v, &err), RS_ERR_INVALID_ARG);
    CHECK(err.line == 3);
    CHECK(err.col == 9);
    CHECK(strstr(err.message, "leading zeros") != NULL);
    CHECK_RC(json_parse("{\"x\": 1, \"x\": 2}", 16, &v, &err), RS_ERR_INVALID_ARG);
    CHECK(err.line == 1 && err.col == 10 && strstr(err.message, "duplicate") != NULL);

    CHECK_RC(json_parse_file("no-such-file.json", &v, &err), RS_ERR_NOT_FOUND);
    CHECK_RC(json_parse(NULL, 0, NULL, NULL), RS_ERR_INVALID_ARG);
}

/* 正しい文書をランダムに壊しても、クラッシュせず成功か失敗のどちらかを返す */
static void test_mutation_fuzz(void)
{
    static const char seed[] =
        "{\"auth\": {\"username\": \"op\", \"password\": \"p\\u00e9\"}, "
        "\"ports\": [50001, 50002, 50003], \"ratio\": -1.5e2, \"on\": true, \"n\": null}";
    char buf[sizeof(seed)];
    uint32_t rng = 4242;
    int ok = 0;
    int bad = 0;
    int i;

    for (i = 0; i < 20000; i++) {
        json_value_t *v = NULL;
        size_t len = sizeof(seed) - 1;
        int k;
        memcpy(buf, seed, sizeof(seed));
        for (k = 0; k < 1 + (int)(rng % 4); k++) {
            rng = rng * 1103515245u + 12345u;
            buf[(rng >> 8) % len] = (char)(rng >> 16);
        }
        rng = rng * 1103515245u + 12345u;
        if (rng % 5 == 0)
            len = (rng >> 8) % len; /* 途中で切れた入力 */
        if (json_parse(buf, len, &v, NULL) == RS_OK) {
            CHECK(v != NULL);
            ok++;
        } else {
            CHECK(v == NULL);
            bad++;
        }
        json_free(v);
    }
    CHECK(ok > 0 && bad > 0);
}

int main(void)
{
    test_document();
    test_scalars();
    test_structure();
    test_bom_depth_size_position();
    test_mutation_fuzz();
    return TEST_RESULT();
}
