/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "rs_log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rs_error.h"

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK rs_log_lock_t;
#define lock_init(l)    InitializeSRWLock(l)
#define lock_destroy(l) ((void)(l))
#define lock_acquire(l) AcquireSRWLockExclusive(l)
#define lock_release(l) ReleaseSRWLockExclusive(l)
#else
#include <pthread.h>
typedef pthread_mutex_t rs_log_lock_t;
#define lock_init(l)    pthread_mutex_init((l), NULL)
#define lock_destroy(l) pthread_mutex_destroy(l)
#define lock_acquire(l) pthread_mutex_lock(l)
#define lock_release(l) pthread_mutex_unlock(l)
#endif

struct rs_log {
    rs_log_lock_t lock;
    FILE         *fp;
    char         *path;
    size_t        file_size;
    size_t        max_file_bytes;
    int           max_backups;
    int           to_stderr;
    volatile int  level;
};

static rs_log_t *volatile g_default;

static const char *const k_level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"
};

void rs_log_config_default(rs_log_config_t *cfg)
{
    if (cfg == NULL)
        return;
    cfg->file_path = NULL;
    cfg->level = RS_LOG_LEVEL_INFO;
    cfg->to_stderr = 1;
    cfg->max_file_bytes = 10u * 1024u * 1024u;
    cfg->max_backups = 5;
}

const char *rs_log_level_name(rs_log_level_t level)
{
    if ((int)level < 0 || level > RS_LOG_LEVEL_OFF)
        return "?";
    return k_level_names[level];
}

static int ascii_ieq(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

int rs_log_level_from_string(const char *str, rs_log_level_t *out)
{
    int i;

    if (str == NULL || out == NULL)
        return RS_ERR_INVALID_ARG;
    for (i = 0; i <= (int)RS_LOG_LEVEL_OFF; i++) {
        if (ascii_ieq(str, k_level_names[i])) {
            *out = (rs_log_level_t)i;
            return RS_OK;
        }
    }
    return RS_ERR_INVALID_ARG;
}

/* 呼び出し側でロック済みであること */
static int open_file(rs_log_t *log)
{
    long pos;

    log->fp = fopen(log->path, "ab");
    if (log->fp == NULL)
        return RS_ERR_IO;
    if (fseek(log->fp, 0, SEEK_END) == 0 && (pos = ftell(log->fp)) > 0)
        log->file_size = (size_t)pos;
    else
        log->file_size = 0;
    return RS_OK;
}

/* path -> path.1 -> ... -> path.N とずらし、新しいファイルを開く。呼び出し側でロック済みであること */
static void rotate(rs_log_t *log)
{
    size_t cap = strlen(log->path) + 16;
    char *from = malloc(cap);
    char *to = malloc(cap);
    int i;

    fclose(log->fp);
    log->fp = NULL;

    if (from != NULL && to != NULL) {
        if (log->max_backups <= 0) {
            remove(log->path);
        } else {
            for (i = log->max_backups; i >= 1; i--) {
                snprintf(to, cap, "%s.%d", log->path, i);
                if (i == 1)
                    snprintf(from, cap, "%s", log->path);
                else
                    snprintf(from, cap, "%s.%d", log->path, i - 1);
                remove(to); /* Windows の rename は既存ファイルを上書きしない */
                rename(from, to);
            }
        }
    }
    free(from);
    free(to);

    (void)open_file(log);
}

int rs_log_open(rs_log_t **out, const rs_log_config_t *cfg)
{
    rs_log_t *log;

    if (out == NULL || cfg == NULL || (int)cfg->level < 0 || cfg->level > RS_LOG_LEVEL_OFF ||
        cfg->max_backups < 0)
        return RS_ERR_INVALID_ARG;
    *out = NULL;

    log = calloc(1, sizeof(*log));
    if (log == NULL)
        return RS_ERR_NO_MEMORY;

    log->level = (int)cfg->level;
    log->to_stderr = cfg->to_stderr;
    log->max_file_bytes = cfg->max_file_bytes;
    log->max_backups = cfg->max_backups;

    if (cfg->file_path != NULL && cfg->file_path[0] != '\0') {
        size_t len = strlen(cfg->file_path);
        log->path = malloc(len + 1);
        if (log->path == NULL) {
            free(log);
            return RS_ERR_NO_MEMORY;
        }
        memcpy(log->path, cfg->file_path, len + 1);
        if (open_file(log) != RS_OK) {
            free(log->path);
            free(log);
            return RS_ERR_IO;
        }
    }

    lock_init(&log->lock);
    *out = log;
    return RS_OK;
}

void rs_log_close(rs_log_t *log)
{
    if (log == NULL)
        return;
    if (g_default == log)
        g_default = NULL;

    lock_acquire(&log->lock);
    if (log->fp != NULL) {
        fclose(log->fp);
        log->fp = NULL;
    }
    lock_release(&log->lock);

    lock_destroy(&log->lock);
    free(log->path);
    free(log);
}

void rs_log_set_level(rs_log_t *log, rs_log_level_t level)
{
    if (log == NULL || (int)level < 0 || level > RS_LOG_LEVEL_OFF)
        return;
    log->level = (int)level;
}

rs_log_level_t rs_log_get_level(const rs_log_t *log)
{
    return log != NULL ? (rs_log_level_t)log->level : RS_LOG_LEVEL_INFO;
}

void rs_log_set_default(rs_log_t *log)
{
    g_default = log;
}

rs_log_t *rs_log_get_default(void)
{
    return g_default;
}

static void format_timestamp(char *buf, size_t cap)
{
    struct tm tm;
    time_t sec;
    int msec;

#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER u;
    uint64_t unix_ms;

    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 1601-01-01 起点の 100ns 単位 → UNIX エポック起点のミリ秒 */
    unix_ms = (u.QuadPart - 116444736000000000ULL) / 10000u;
    sec = (time_t)(unix_ms / 1000u);
    msec = (int)(unix_ms % 1000u);
    gmtime_s(&tm, &sec);
#else
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    sec = ts.tv_sec;
    msec = (int)(ts.tv_nsec / 1000000);
    gmtime_r(&sec, &tm);
#endif

    snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, msec);
}

void rs_log_vwrite(rs_log_t *log, rs_log_level_t level, const char *module,
                   const char *fmt, va_list ap)
{
    char line[RS_LOG_LINE_MAX];
    char ts[32];
    int threshold;
    int n;
    int m;
    size_t len;

    if (log == NULL)
        log = g_default;
    threshold = log != NULL ? log->level : (int)RS_LOG_LEVEL_INFO;
    if (fmt == NULL || (int)level < threshold || (int)level < 0 || level >= RS_LOG_LEVEL_OFF)
        return;

    format_timestamp(ts, sizeof(ts));
    n = snprintf(line, sizeof(line), "%s %-5s [%s] ", ts, rs_log_level_name(level),
                 module != NULL ? module : "-");
    if (n < 0)
        return;
    if ((size_t)n > sizeof(line) - 2)
        n = (int)(sizeof(line) - 2);

    m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    if (m < 0)
        m = 0;

    if ((size_t)n + (size_t)m > sizeof(line) - 2) {
        /* 改行と NUL の分を残して切り詰め、末尾を "..." にする */
        len = sizeof(line) - 2;
        memcpy(line + len - 3, "...", 3);
    } else {
        len = (size_t)n + (size_t)m;
    }
    line[len++] = '\n';
    line[len] = '\0';

    if (log == NULL) {
        fputs(line, stderr);
        return;
    }

    lock_acquire(&log->lock);
    if (log->to_stderr)
        fputs(line, stderr);
    if (log->path != NULL) {
        if (log->fp != NULL && log->max_file_bytes > 0 && log->file_size > 0 &&
            log->file_size + len > log->max_file_bytes)
            rotate(log);
        if (log->fp == NULL)
            (void)open_file(log); /* 前回のオープン失敗からの再試行 */
        if (log->fp != NULL) {
            if (fwrite(line, 1, len, log->fp) == len)
                log->file_size += len;
            fflush(log->fp);
        }
    }
    lock_release(&log->lock);
}

void rs_log_write(rs_log_t *log, rs_log_level_t level, const char *module, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    rs_log_vwrite(log, level, module, fmt, ap);
    va_end(ap);
}
