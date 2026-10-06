/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs_error.h"
#include "rs_platform.h"

typedef struct parser {
    const char   *p;
    const char   *end;
    uint32_t      line;
    uint32_t      col;
    int           depth;
    int           status;
    json_error_t *err;
} parser_t;

static void fail(parser_t *ps, int status, const char *fmt, ...) RS_PRINTF_FMT(3, 4);

static void fail(parser_t *ps, int status, const char *fmt, ...)
{
    va_list ap;

    if (ps->status != RS_OK)
        return; /* 最初のエラーを残す */
    ps->status = status;
    if (ps->err != NULL) {
        ps->err->line = ps->line;
        ps->err->col = ps->col;
        va_start(ap, fmt);
        vsnprintf(ps->err->message, sizeof(ps->err->message), fmt, ap);
        va_end(ap);
    }
}

static int at_end(const parser_t *ps)
{
    return ps->p >= ps->end;
}

static void advance(parser_t *ps)
{
    if (*ps->p == '\n') {
        ps->line++;
        ps->col = 1;
    } else if (((unsigned char)*ps->p & 0xC0) != 0x80) {
        ps->col++; /* UTF-8 の継続バイトは桁に数えない */
    }
    ps->p++;
}

static void skip_ws(parser_t *ps)
{
    while (!at_end(ps) && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        advance(ps);
}

static json_value_t *new_value(parser_t *ps, json_type_t type)
{
    json_value_t *v = calloc(1, sizeof(*v));
    if (v == NULL) {
        fail(ps, RS_ERR_NO_MEMORY, "out of memory");
        return NULL;
    }
    v->type = type;
    v->line = ps->line;
    v->col = ps->col;
    return v;
}

void json_free(json_value_t *v)
{
    size_t i;

    if (v == NULL)
        return;
    switch (v->type) {
    case JSON_STRING:
        free(v->u.string.ptr);
        break;
    case JSON_ARRAY:
        for (i = 0; i < v->u.array.count; i++)
            json_free(v->u.array.items[i]);
        free(v->u.array.items);
        break;
    case JSON_OBJECT:
        for (i = 0; i < v->u.object.count; i++) {
            free(v->u.object.keys[i]);
            json_free(v->u.object.values[i]);
        }
        free(v->u.object.keys);
        free(v->u.object.values);
        break;
    default:
        break;
    }
    free(v);
}

/* ---- 文字列 -------------------------------------------------------------- */

typedef struct strbuf {
    char  *ptr;
    size_t len;
    size_t cap;
} strbuf_t;

static int sb_put(strbuf_t *sb, const char *data, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap == 0 ? 32 : sb->cap;
        char *p;
        while (cap < sb->len + n + 1)
            cap *= 2;
        p = realloc(sb->ptr, cap);
        if (p == NULL)
            return 0;
        sb->ptr = p;
        sb->cap = cap;
    }
    memcpy(sb->ptr + sb->len, data, n);
    sb->len += n;
    sb->ptr[sb->len] = '\0';
    return 1;
}

static int put_utf8(strbuf_t *sb, uint32_t cp)
{
    char b[4];
    size_t n;

    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return sb_put(sb, b, n);
}

static int read_hex4(parser_t *ps, uint32_t *out)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c;
        if (at_end(ps)) {
            fail(ps, RS_ERR_INVALID_ARG, "truncated \\u escape");
            return 0;
        }
        c = *ps->p;
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (uint32_t)(c - 'A' + 10);
        else {
            fail(ps, RS_ERR_INVALID_ARG, "invalid hex digit in \\u escape");
            return 0;
        }
        advance(ps);
    }
    *out = v;
    return 1;
}

/* UTF-8 の 1 文字を検証してコピーする（過長表現・サロゲート・範囲外を拒否） */
static int copy_utf8_char(parser_t *ps, strbuf_t *sb)
{
    const unsigned char *s = (const unsigned char *)ps->p;
    size_t avail = (size_t)(ps->end - ps->p);
    uint32_t cp;
    size_t n;
    size_t i;

    if (s[0] < 0x80) {
        n = 1;
        cp = s[0];
    } else if ((s[0] & 0xE0) == 0xC0) {
        n = 2;
        cp = s[0] & 0x1F;
    } else if ((s[0] & 0xF0) == 0xE0) {
        n = 3;
        cp = s[0] & 0x0F;
    } else if ((s[0] & 0xF8) == 0xF0) {
        n = 4;
        cp = s[0] & 0x07;
    } else {
        fail(ps, RS_ERR_INVALID_ARG, "invalid UTF-8 byte 0x%02X", s[0]);
        return 0;
    }
    if (n > avail) {
        fail(ps, RS_ERR_INVALID_ARG, "truncated UTF-8 sequence");
        return 0;
    }
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            fail(ps, RS_ERR_INVALID_ARG, "invalid UTF-8 continuation byte");
            return 0;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        fail(ps, RS_ERR_INVALID_ARG, "invalid UTF-8 sequence");
        return 0;
    }
    if (!sb_put(sb, ps->p, n)) {
        fail(ps, RS_ERR_NO_MEMORY, "out of memory");
        return 0;
    }
    for (i = 0; i < n; i++)
        advance(ps);
    return 1;
}

/* 開始の '"' の位置で呼ぶ。成功時は NUL 終端の文字列を返す（呼び出し側で free） */
static char *parse_string_raw(parser_t *ps, size_t *len_out)
{
    strbuf_t sb = { NULL, 0, 0 };

    advance(ps); /* '"' */
    if (!sb_put(&sb, "", 0))
        goto oom;

    for (;;) {
        unsigned char c;

        if (at_end(ps)) {
            fail(ps, RS_ERR_INVALID_ARG, "unterminated string");
            goto error;
        }
        c = (unsigned char)*ps->p;
        if (c == '"') {
            advance(ps);
            break;
        }
        if (c < 0x20) {
            fail(ps, RS_ERR_INVALID_ARG, "control character in string (use \\n, \\t, ...)");
            goto error;
        }
        if (c != '\\') {
            if (!copy_utf8_char(ps, &sb))
                goto error;
            continue;
        }

        advance(ps); /* '\\' */
        if (at_end(ps)) {
            fail(ps, RS_ERR_INVALID_ARG, "unterminated escape");
            goto error;
        }
        c = (unsigned char)*ps->p;
        advance(ps);
        switch (c) {
        case '"':  if (!sb_put(&sb, "\"", 1)) goto oom; break;
        case '\\': if (!sb_put(&sb, "\\", 1)) goto oom; break;
        case '/':  if (!sb_put(&sb, "/", 1)) goto oom; break;
        case 'b':  if (!sb_put(&sb, "\b", 1)) goto oom; break;
        case 'f':  if (!sb_put(&sb, "\f", 1)) goto oom; break;
        case 'n':  if (!sb_put(&sb, "\n", 1)) goto oom; break;
        case 'r':  if (!sb_put(&sb, "\r", 1)) goto oom; break;
        case 't':  if (!sb_put(&sb, "\t", 1)) goto oom; break;
        case 'u': {
            uint32_t cp;
            if (!read_hex4(ps, &cp))
                goto error;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo;
                if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u') {
                    fail(ps, RS_ERR_INVALID_ARG, "unpaired UTF-16 surrogate");
                    goto error;
                }
                advance(ps);
                advance(ps);
                if (!read_hex4(ps, &lo))
                    goto error;
                if (lo < 0xDC00 || lo > 0xDFFF) {
                    fail(ps, RS_ERR_INVALID_ARG, "unpaired UTF-16 surrogate");
                    goto error;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                fail(ps, RS_ERR_INVALID_ARG, "unpaired UTF-16 surrogate");
                goto error;
            } else if (cp == 0) {
                fail(ps, RS_ERR_INVALID_ARG, "\\u0000 is not allowed");
                goto error;
            }
            if (!put_utf8(&sb, cp))
                goto oom;
            break;
        }
        default:
            fail(ps, RS_ERR_INVALID_ARG, "invalid escape '\\%c'", c >= 0x20 && c < 0x7F ? c : '?');
            goto error;
        }
    }
    *len_out = sb.len;
    return sb.ptr;

oom:
    fail(ps, RS_ERR_NO_MEMORY, "out of memory");
error:
    free(sb.ptr);
    return NULL;
}

/* ---- 数値 ---------------------------------------------------------------- */

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static json_value_t *parse_number(parser_t *ps)
{
    const char *start = ps->p;
    json_value_t *v;
    char buf[64];
    size_t n;

    v = new_value(ps, JSON_NUMBER);
    if (v == NULL)
        return NULL;

    if (*ps->p == '-')
        advance(ps);
    if (at_end(ps) || !is_digit(*ps->p)) {
        fail(ps, RS_ERR_INVALID_ARG, "invalid number");
        goto error;
    }
    if (*ps->p == '0') {
        advance(ps);
        if (!at_end(ps) && is_digit(*ps->p)) {
            fail(ps, RS_ERR_INVALID_ARG, "leading zeros are not allowed");
            goto error;
        }
    } else {
        while (!at_end(ps) && is_digit(*ps->p))
            advance(ps);
    }
    if (!at_end(ps) && *ps->p == '.') {
        advance(ps);
        if (at_end(ps) || !is_digit(*ps->p)) {
            fail(ps, RS_ERR_INVALID_ARG, "digit expected after decimal point");
            goto error;
        }
        while (!at_end(ps) && is_digit(*ps->p))
            advance(ps);
    }
    if (!at_end(ps) && (*ps->p == 'e' || *ps->p == 'E')) {
        advance(ps);
        if (!at_end(ps) && (*ps->p == '+' || *ps->p == '-'))
            advance(ps);
        if (at_end(ps) || !is_digit(*ps->p)) {
            fail(ps, RS_ERR_INVALID_ARG, "digit expected in exponent");
            goto error;
        }
        while (!at_end(ps) && is_digit(*ps->p))
            advance(ps);
    }

    n = (size_t)(ps->p - start);
    if (n >= sizeof(buf)) {
        fail(ps, RS_ERR_INVALID_ARG, "number too long");
        goto error;
    }
    /* 文法は検証済み。strtod は "C" ロケール（setlocale を呼ばない）で '.' を小数点とする */
    memcpy(buf, start, n);
    buf[n] = '\0';
    v->u.number = strtod(buf, NULL);
    return v;

error:
    json_free(v);
    return NULL;
}

/* ---- 値・配列・オブジェクト ---------------------------------------------- */

static json_value_t *parse_value(parser_t *ps);

static int match_literal(parser_t *ps, const char *lit)
{
    size_t n = strlen(lit);
    size_t i;

    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0)
        return 0;
    for (i = 0; i < n; i++)
        advance(ps);
    return 1;
}

static json_value_t *parse_array(parser_t *ps)
{
    json_value_t *v = new_value(ps, JSON_ARRAY);
    size_t cap = 0;

    if (v == NULL)
        return NULL;
    advance(ps); /* '[' */
    skip_ws(ps);
    if (!at_end(ps) && *ps->p == ']') {
        advance(ps);
        return v;
    }
    for (;;) {
        json_value_t *item;

        skip_ws(ps);
        item = parse_value(ps);
        if (item == NULL)
            goto error;
        if (v->u.array.count == cap) {
            size_t ncap = cap == 0 ? 4 : cap * 2;
            json_value_t **p = realloc(v->u.array.items, ncap * sizeof(*p));
            if (p == NULL) {
                json_free(item);
                fail(ps, RS_ERR_NO_MEMORY, "out of memory");
                goto error;
            }
            v->u.array.items = p;
            cap = ncap;
        }
        v->u.array.items[v->u.array.count++] = item;

        skip_ws(ps);
        if (at_end(ps)) {
            fail(ps, RS_ERR_INVALID_ARG, "unterminated array");
            goto error;
        }
        if (*ps->p == ',') {
            advance(ps);
            skip_ws(ps);
            if (!at_end(ps) && *ps->p == ']') {
                fail(ps, RS_ERR_INVALID_ARG, "trailing comma in array");
                goto error;
            }
            continue;
        }
        if (*ps->p == ']') {
            advance(ps);
            return v;
        }
        fail(ps, RS_ERR_INVALID_ARG, "',' or ']' expected");
        goto error;
    }

error:
    json_free(v);
    return NULL;
}

static json_value_t *parse_object(parser_t *ps)
{
    json_value_t *v = new_value(ps, JSON_OBJECT);
    size_t cap = 0;

    if (v == NULL)
        return NULL;
    advance(ps); /* '{' */
    skip_ws(ps);
    if (!at_end(ps) && *ps->p == '}') {
        advance(ps);
        return v;
    }
    for (;;) {
        json_value_t *val;
        char *key;
        size_t klen;
        size_t i;
        uint32_t kline;
        uint32_t kcol;

        skip_ws(ps);
        if (at_end(ps) || *ps->p != '"') {
            fail(ps, RS_ERR_INVALID_ARG, "object key (string) expected");
            goto error;
        }
        kline = ps->line;
        kcol = ps->col;
        key = parse_string_raw(ps, &klen);
        if (key == NULL)
            goto error;
        for (i = 0; i < v->u.object.count; i++) {
            if (strcmp(v->u.object.keys[i], key) == 0) {
                ps->line = kline;
                ps->col = kcol;
                fail(ps, RS_ERR_INVALID_ARG, "duplicate key \"%.40s\"", key);
                free(key);
                goto error;
            }
        }
        skip_ws(ps);
        if (at_end(ps) || *ps->p != ':') {
            free(key);
            fail(ps, RS_ERR_INVALID_ARG, "':' expected after key");
            goto error;
        }
        advance(ps);
        skip_ws(ps);
        val = parse_value(ps);
        if (val == NULL) {
            free(key);
            goto error;
        }
        if (v->u.object.count == cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            char **k = realloc(v->u.object.keys, ncap * sizeof(*k));
            json_value_t **vv;
            if (k != NULL)
                v->u.object.keys = k;
            vv = k != NULL ? realloc(v->u.object.values, ncap * sizeof(*vv)) : NULL;
            if (vv == NULL) {
                free(key);
                json_free(val);
                fail(ps, RS_ERR_NO_MEMORY, "out of memory");
                goto error;
            }
            v->u.object.values = vv;
            cap = ncap;
        }
        v->u.object.keys[v->u.object.count] = key;
        v->u.object.values[v->u.object.count] = val;
        v->u.object.count++;

        skip_ws(ps);
        if (at_end(ps)) {
            fail(ps, RS_ERR_INVALID_ARG, "unterminated object");
            goto error;
        }
        if (*ps->p == ',') {
            advance(ps);
            skip_ws(ps);
            if (!at_end(ps) && *ps->p == '}') {
                fail(ps, RS_ERR_INVALID_ARG, "trailing comma in object");
                goto error;
            }
            continue;
        }
        if (*ps->p == '}') {
            advance(ps);
            return v;
        }
        fail(ps, RS_ERR_INVALID_ARG, "',' or '}' expected");
        goto error;
    }

error:
    json_free(v);
    return NULL;
}

static json_value_t *parse_value(parser_t *ps)
{
    json_value_t *v = NULL;

    if (at_end(ps)) {
        fail(ps, RS_ERR_INVALID_ARG, "value expected");
        return NULL;
    }
    if (++ps->depth > JSON_MAX_DEPTH) {
        fail(ps, RS_ERR_INVALID_ARG, "nesting deeper than %d", JSON_MAX_DEPTH);
        return NULL;
    }

    switch (*ps->p) {
    case '{':
        v = parse_object(ps);
        break;
    case '[':
        v = parse_array(ps);
        break;
    case '"': {
        size_t len;
        v = new_value(ps, JSON_STRING);
        if (v != NULL) {
            v->u.string.ptr = parse_string_raw(ps, &len);
            v->u.string.len = len;
            if (v->u.string.ptr == NULL) {
                json_free(v);
                v = NULL;
            }
        }
        break;
    }
    case 't':
    case 'f':
    case 'n': {
        uint32_t line = ps->line;
        uint32_t col = ps->col;
        json_type_t type;
        int b = 0;

        if (match_literal(ps, "true")) {
            type = JSON_BOOL;
            b = 1;
        } else if (match_literal(ps, "false")) {
            type = JSON_BOOL;
        } else if (match_literal(ps, "null")) {
            type = JSON_NULL;
        } else {
            fail(ps, RS_ERR_INVALID_ARG, "invalid literal");
            break;
        }
        v = new_value(ps, type);
        if (v != NULL) {
            v->u.boolean = b;
            v->line = line;
            v->col = col;
        }
        break;
    }
    default:
        if (*ps->p == '-' || is_digit(*ps->p))
            v = parse_number(ps);
        else
            fail(ps, RS_ERR_INVALID_ARG, "unexpected character '%c'",
                 (unsigned char)*ps->p >= 0x20 && (unsigned char)*ps->p < 0x7F ? *ps->p : '?');
        break;
    }
    ps->depth--;
    return v;
}

int json_parse(const char *text, size_t len, json_value_t **out, json_error_t *err)
{
    parser_t ps;
    json_value_t *v;

    if (out == NULL || (text == NULL && len > 0))
        return RS_ERR_INVALID_ARG;
    *out = NULL;
    if (err != NULL)
        memset(err, 0, sizeof(*err));
    if (len > JSON_MAX_INPUT) {
        if (err != NULL)
            snprintf(err->message, sizeof(err->message), "input larger than %u bytes",
                     (unsigned)JSON_MAX_INPUT);
        return RS_ERR_TOO_LARGE;
    }

    memset(&ps, 0, sizeof(ps));
    ps.p = text;
    ps.end = text + len;
    ps.line = 1;
    ps.col = 1;
    ps.status = RS_OK;
    ps.err = err;

    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        ps.p += 3;

    skip_ws(&ps);
    v = parse_value(&ps);
    if (v != NULL) {
        skip_ws(&ps);
        if (!at_end(&ps)) {
            fail(&ps, RS_ERR_INVALID_ARG, "unexpected data after the top-level value");
            json_free(v);
            v = NULL;
        }
    }
    if (v == NULL)
        return ps.status != RS_OK ? ps.status : RS_ERR_INVALID_ARG;
    *out = v;
    return RS_OK;
}

int json_parse_file(const char *path, json_value_t **out, json_error_t *err)
{
    FILE *fp;
    char *buf;
    long size;
    size_t n;
    int rc;

    if (path == NULL || out == NULL)
        return RS_ERR_INVALID_ARG;
    *out = NULL;
    if (err != NULL)
        memset(err, 0, sizeof(*err));

    fp = fopen(path, "rb");
    if (fp == NULL)
        return RS_ERR_NOT_FOUND;
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return RS_ERR_IO;
    }
    if ((unsigned long)size > JSON_MAX_INPUT) {
        fclose(fp);
        if (err != NULL)
            snprintf(err->message, sizeof(err->message), "file larger than %u bytes",
                     (unsigned)JSON_MAX_INPUT);
        return RS_ERR_TOO_LARGE;
    }
    buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(fp);
        return RS_ERR_NO_MEMORY;
    }
    n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (n != (size_t)size) {
        free(buf);
        return RS_ERR_IO;
    }
    rc = json_parse(buf, n, out, err);
    free(buf);
    return rc;
}

const json_value_t *json_object_get(const json_value_t *obj, const char *key)
{
    size_t i;

    if (obj == NULL || key == NULL || obj->type != JSON_OBJECT)
        return NULL;
    for (i = 0; i < obj->u.object.count; i++)
        if (strcmp(obj->u.object.keys[i], key) == 0)
            return obj->u.object.values[i];
    return NULL;
}

const json_value_t *json_get_path(const json_value_t *root, const char *path)
{
    char part[64];
    const json_value_t *cur = root;

    if (root == NULL || path == NULL)
        return NULL;
    if (path[0] == '\0')
        return root;
    while (cur != NULL && *path != '\0') {
        const char *dot = strchr(path, '.');
        size_t n = dot != NULL ? (size_t)(dot - path) : strlen(path);
        if (n == 0 || n >= sizeof(part))
            return NULL;
        memcpy(part, path, n);
        part[n] = '\0';
        cur = json_object_get(cur, part);
        path += n;
        if (*path == '.')
            path++;
    }
    return cur;
}

const char *json_type_name(json_type_t type)
{
    switch (type) {
    case JSON_NULL:   return "null";
    case JSON_BOOL:   return "boolean";
    case JSON_NUMBER: return "number";
    case JSON_STRING: return "string";
    case JSON_ARRAY:  return "array";
    case JSON_OBJECT: return "object";
    default:          return "?";
    }
}
