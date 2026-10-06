/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "cfg_load.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs_error.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

#define MODULE "config"

void cfg_reader_init(cfg_reader_t *r, const json_value_t *root, const char *source)
{
    r->root = root;
    r->source = source != NULL ? source : "config";
    r->errors = 0;
    r->warnings = 0;
}

static void report(cfg_reader_t *r, rs_log_level_t level, const char *path,
                   const json_value_t *v, const char *fmt, va_list ap) RS_PRINTF_FMT(5, 0);

static void report(cfg_reader_t *r, rs_log_level_t level, const char *path,
                   const json_value_t *v, const char *fmt, va_list ap)
{
    char msg[256];

    vsnprintf(msg, sizeof(msg), fmt, ap);
    if (v != NULL)
        rs_log_write(NULL, level, MODULE, "%s:%u:%u: %s: %s", r->source, (unsigned)v->line,
                     (unsigned)v->col, path, msg);
    else
        rs_log_write(NULL, level, MODULE, "%s: %s: %s", r->source, path, msg);
}

void cfg_error(cfg_reader_t *r, const char *path, const json_value_t *v, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    report(r, RS_LOG_LEVEL_ERROR, path, v, fmt, ap);
    va_end(ap);
    r->errors++;
}

void cfg_warn(cfg_reader_t *r, const char *path, const json_value_t *v, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    report(r, RS_LOG_LEVEL_WARN, path, v, fmt, ap);
    va_end(ap);
    r->warnings++;
}

static const json_value_t *lookup(cfg_reader_t *r, const char *path)
{
    const json_value_t *v = json_get_path(r->root, path);
    return (v != NULL && v->type == JSON_NULL) ? NULL : v;
}

const char *cfg_string(cfg_reader_t *r, const char *path, const char *def)
{
    const json_value_t *v = lookup(r, path);

    if (v == NULL)
        return def;
    if (v->type != JSON_STRING) {
        cfg_error(r, path, v, "string expected, got %s", json_type_name(v->type));
        return def;
    }
    return v->u.string.ptr;
}

uint32_t cfg_uint(cfg_reader_t *r, const char *path, uint32_t def, uint32_t min, uint32_t max)
{
    const json_value_t *v = lookup(r, path);
    double d;

    if (v == NULL)
        return def;
    if (v->type != JSON_NUMBER) {
        cfg_error(r, path, v, "number expected, got %s", json_type_name(v->type));
        return def;
    }
    d = v->u.number;
    if (d < (double)min || d > (double)max) {
        cfg_error(r, path, v, "%g is out of range (%u..%u)", d, (unsigned)min, (unsigned)max);
        return def;
    }
    /* 範囲確認後なので uint32_t への変換は安全（libm の floor を使わず Linux で -lm 不要） */
    if ((double)(uint32_t)d != d) {
        cfg_error(r, path, v, "integer expected, got %g", d);
        return def;
    }
    return (uint32_t)d;
}

int cfg_bool(cfg_reader_t *r, const char *path, int def)
{
    const json_value_t *v = lookup(r, path);

    if (v == NULL)
        return def;
    if (v->type != JSON_BOOL) {
        cfg_error(r, path, v, "true or false expected, got %s", json_type_name(v->type));
        return def;
    }
    return v->u.boolean;
}

uint8_t cfg_byte(cfg_reader_t *r, const char *path, uint8_t def)
{
    const json_value_t *v = lookup(r, path);

    if (v == NULL)
        return def;
    if (v->type == JSON_NUMBER)
        return (uint8_t)cfg_uint(r, path, def, 0, 255);
    if (v->type == JSON_STRING) {
        const char *s = v->u.string.ptr;
        char *end;
        unsigned long n = strtoul(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10);
        if (s[0] != '\0' && *end == '\0' && n <= 255)
            return (uint8_t)n;
        cfg_error(r, path, v, "\"%s\" is not a byte value (e.g. \"0x7C\" or 124)", s);
        return def;
    }
    cfg_error(r, path, v, "number or string expected, got %s", json_type_name(v->type));
    return def;
}

void cfg_known_keys(cfg_reader_t *r, const char *object_path, const char *const known[])
{
    const json_value_t *obj = object_path[0] != '\0' ? json_get_path(r->root, object_path) : r->root;
    size_t i;

    if (obj == NULL)
        return;
    if (obj->type != JSON_OBJECT) {
        cfg_error(r, object_path[0] != '\0' ? object_path : "(root)", obj, "object expected, got %s",
                  json_type_name(obj->type));
        return;
    }
    for (i = 0; i < obj->u.object.count; i++) {
        const char *key = obj->u.object.keys[i];
        size_t k;
        int found = 0;
        for (k = 0; known[k] != NULL; k++)
            if (strcmp(known[k], key) == 0)
                found = 1;
        if (!found) {
            char path[128];
            if (object_path[0] != '\0')
                snprintf(path, sizeof(path), "%s.%s", object_path, key);
            else
                snprintf(path, sizeof(path), "%s", key);
            cfg_warn(r, path, obj->u.object.values[i], "unknown key (ignored; check spelling)");
        }
    }
}

const char *cfg_device_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || strcmp(name, "default") == 0)
        return NULL;
    return name;
}

static void path_join(char *buf, size_t cap, const char *section, const char *key)
{
    snprintf(buf, cap, "%s.%s", section, key);
}

void cfg_read_audio(cfg_reader_t *r, const char *section, audio_dev_config_t *dev,
                    audio_link_config_t *link, int *enabled)
{
    static const char *const known_base[] = {
        "backend", "codec", "sample_rate", "channels", "frame_ms", "opus_bitrate",
        "jitter_min_ms", "jitter_max_ms", "capture_device", "playback_device", NULL
    };
    char p[96];
    const char *s;
    uint32_t v;

    cfg_known_keys(r, section, known_base);

    path_join(p, sizeof(p), section, "backend");
    s = cfg_string(r, p, NULL);
    if (s != NULL) {
        if (strcmp(s, "none") == 0) {
            *enabled = 0;
        } else if (strcmp(s, "auto") == 0 || strcmp(s, "winmm") == 0 ||
                   strcmp(s, "alsa") == 0 || strcmp(s, "null") == 0) {
            *enabled = 1;
            dev->backend = s;
        } else {
            cfg_error(r, p, json_get_path(r->root, p),
                      "\"%s\" is not one of auto / winmm / alsa / null / none", s);
        }
    }

    path_join(p, sizeof(p), section, "codec");
    s = cfg_string(r, p, NULL);
    if (s != NULL && audio_codec_from_string(s, &link->codec) != RS_OK)
        cfg_error(r, p, json_get_path(r->root, p), "\"%s\" is not one of pcm / opus", s);

    /* 形式は IC-9100 の USB Audio CODEC に合わせて固定。異なる値は明示的に拒否する */
    path_join(p, sizeof(p), section, "sample_rate");
    v = cfg_uint(r, p, 48000, 1, 192000);
    if (v != 48000)
        cfg_error(r, p, json_get_path(r->root, p), "only 48000 is supported");
    path_join(p, sizeof(p), section, "channels");
    v = cfg_uint(r, p, 1, 1, 8);
    if (v != 1)
        cfg_error(r, p, json_get_path(r->root, p), "only 1 (mono) is supported");
    path_join(p, sizeof(p), section, "frame_ms");
    v = cfg_uint(r, p, 10, 1, 120);
    if (v != 10)
        cfg_error(r, p, json_get_path(r->root, p),
                  "only 10 is supported (a 20 ms PCM frame does not fit in one IPv6-safe packet)");
    link->sample_rate = 48000;
    link->frame_samples = 480;
    dev->sample_rate = 48000;
    dev->frame_samples = 480;

    path_join(p, sizeof(p), section, "opus_bitrate");
    (void)cfg_uint(r, p, 64000, 6000, 510000); /* Opus 組み込み時に使用。現状は検証のみ */

    path_join(p, sizeof(p), section, "jitter_min_ms");
    link->jitter_min_ms = cfg_uint(r, p, link->jitter_min_ms, 10, 1000);
    path_join(p, sizeof(p), section, "jitter_max_ms");
    link->jitter_max_ms = cfg_uint(r, p, link->jitter_max_ms, 10, 1000);
    if (link->jitter_min_ms > link->jitter_max_ms)
        cfg_error(r, p, json_get_path(r->root, p), "jitter_max_ms (%u) is smaller than jitter_min_ms (%u)",
                  (unsigned)link->jitter_max_ms, (unsigned)link->jitter_min_ms);

    path_join(p, sizeof(p), section, "capture_device");
    dev->capture = cfg_device_name(cfg_string(r, p, dev->capture));
    path_join(p, sizeof(p), section, "playback_device");
    dev->playback = cfg_device_name(cfg_string(r, p, dev->playback));
}

void cfg_read_logging(cfg_reader_t *r, const char *section, rs_log_config_t *log)
{
    char p[96];
    const char *s;
    rs_log_level_t lv;

    path_join(p, sizeof(p), section, "level");
    s = cfg_string(r, p, NULL);
    if (s != NULL) {
        if (rs_log_level_from_string(s, &lv) == RS_OK)
            log->level = lv;
        else
            cfg_error(r, p, json_get_path(r->root, p),
                      "\"%s\" is not one of trace / debug / info / warn / error / fatal / off", s);
    }
    path_join(p, sizeof(p), section, "file");
    s = cfg_string(r, p, log->file_path);
    log->file_path = (s != NULL && s[0] != '\0') ? s : NULL;
    path_join(p, sizeof(p), section, "max_file_bytes");
    log->max_file_bytes = cfg_uint(r, p, (uint32_t)log->max_file_bytes, 0, 1024u * 1024u * 1024u);
    path_join(p, sizeof(p), section, "max_backups");
    log->max_backups = (int)cfg_uint(r, p, (uint32_t)log->max_backups, 0, 100);
    path_join(p, sizeof(p), section, "to_stderr");
    log->to_stderr = cfg_bool(r, p, log->to_stderr);
}

int cfg_load_file(const char *path, json_value_t **doc)
{
    json_error_t err;
    int rc = json_parse_file(path, doc, &err);

    if (rc == RS_OK)
        return 0;
    if (rc == RS_ERR_NOT_FOUND)
        RS_LOG_ERROR(MODULE, "%s: file not found", path);
    else if (rc == RS_ERR_INVALID_ARG)
        RS_LOG_ERROR(MODULE, "%s:%u:%u: JSON syntax error: %s", path, (unsigned)err.line,
                     (unsigned)err.col, err.message);
    else
        RS_LOG_ERROR(MODULE, "%s: cannot read: %s%s%s", path, rs_strerror(rc),
                     err.message[0] ? " - " : "", err.message);
    return 1;
}

void cfg_check_permissions(const char *path)
{
#ifndef _WIN32
    /* グループ権限はサービス専用グループで読ませる運用（640 root:rsba）を許すため対象外 */
    struct stat st;
    if (path != NULL && stat(path, &st) == 0 && (st.st_mode & S_IRWXO) != 0)
        RS_LOG_WARN(MODULE, "%s is accessible by other users; it may contain a password "
                    "(chmod o-rwx %s)", path, path);
#else
    (void)path;
#endif
}
