/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 外部ライブラリ非依存の JSON パーサ（RFC 8259 準拠の厳格モード）。設定ファイル用。
 *   - コメント・末尾カンマ・NaN/Infinity・先頭ゼロ・単一引用符は受け付けない
 *   - 文字列は UTF-8 として検証し、\uXXXX（サロゲートペア含む）を UTF-8 に変換する
 *   - 同一オブジェクト内のキー重複はエラー（設定の書き間違いを見逃さないため）
 *   - 文字列中の NUL（\u0000）はエラー（C 文字列として扱うため）
 *   - 先頭の UTF-8 BOM は読み飛ばす（Windows のメモ帳が付加するため）
 *   - 入れ子の深さ・入力サイズに上限を設ける
 * 数値は double で保持する（整数は 2^53 まで正確）。
 */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_MAX_DEPTH  32
#define JSON_MAX_INPUT  (1024u * 1024u)   /* 1 MiB */

typedef enum json_type {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type_t;

typedef struct json_value json_value_t;

struct json_value {
    json_type_t type;
    uint32_t    line;   /* 値の開始位置（1 起点、エラーメッセージ用） */
    uint32_t    col;
    union {
        int    boolean;
        double number;
        struct {
            char  *ptr;    /* NUL 終端の UTF-8 */
            size_t len;
        } string;
        struct {
            json_value_t **items;
            size_t         count;
        } array;
        struct {
            char         **keys;
            json_value_t **values;
            size_t         count;
        } object;
    } u;
};

typedef struct json_error {
    uint32_t line;
    uint32_t col;
    char     message[96];
} json_error_t;

/* text を解析する。err は NULL 可。
 * RS_ERR_INVALID_ARG（構文エラー、err に位置と理由）/ RS_ERR_TOO_LARGE / RS_ERR_NO_MEMORY */
int  json_parse(const char *text, size_t len, json_value_t **out, json_error_t *err);
/* ファイルを読み込んで解析する。RS_ERR_NOT_FOUND / RS_ERR_IO / json_parse の戻り値 */
int  json_parse_file(const char *path, json_value_t **out, json_error_t *err);
void json_free(json_value_t *v);

/* オブジェクトのメンバを取得する。なければ NULL（obj がオブジェクトでなくても NULL） */
const json_value_t *json_object_get(const json_value_t *obj, const char *key);
/* "audio.jitter_min_ms" のようなドット区切りのパスで取得する。途中がなければ NULL */
const json_value_t *json_get_path(const json_value_t *root, const char *path);

const char *json_type_name(json_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* JSON_H */
