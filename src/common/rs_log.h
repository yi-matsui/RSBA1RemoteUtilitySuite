/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * スレッドセーフなロガー。ファイル出力（サイズベースのローテーション付き）と
 * 標準エラー出力に対応する。USB リセット専用ログ等は別インスタンスとして開く。
 *
 * 出力形式（1 行 1 レコード、時刻は UTC）:
 *   2026-10-06T12:34:56.789Z INFO  [net] message
 */
#ifndef RS_LOG_H
#define RS_LOG_H

#include <stdarg.h>
#include <stddef.h>

#include "rs_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rs_log_level {
    RS_LOG_LEVEL_TRACE = 0,
    RS_LOG_LEVEL_DEBUG,
    RS_LOG_LEVEL_INFO,
    RS_LOG_LEVEL_WARN,
    RS_LOG_LEVEL_ERROR,
    RS_LOG_LEVEL_FATAL,
    RS_LOG_LEVEL_OFF      /* しきい値として指定すると全出力を抑止 */
} rs_log_level_t;

/* 1 レコードの最大長（改行含む）。超過分は "..." で切り詰める。 */
#define RS_LOG_LINE_MAX 2048

typedef struct rs_log_config {
    const char     *file_path;       /* NULL でファイル出力なし */
    rs_log_level_t  level;           /* 出力しきい値 */
    int             to_stderr;       /* 非 0 で標準エラーにも出力 */
    size_t          max_file_bytes;  /* ローテーション閾値。0 でローテーションなし */
    int             max_backups;     /* 保持する世代数 (path.1 ... path.N)。0 で旧ファイルを削除 */
} rs_log_config_t;

typedef struct rs_log rs_log_t;

/* 既定値: ファイルなし / INFO / stderr 出力 / 10 MiB / 5 世代 */
void rs_log_config_default(rs_log_config_t *cfg);

int  rs_log_open(rs_log_t **out, const rs_log_config_t *cfg);
void rs_log_close(rs_log_t *log);

void           rs_log_set_level(rs_log_t *log, rs_log_level_t level);
rs_log_level_t rs_log_get_level(const rs_log_t *log);

/* log が NULL の場合は既定ロガーへ出力する。既定ロガー未設定時は INFO 以上を stderr へ出力する。 */
void rs_log_write(rs_log_t *log, rs_log_level_t level, const char *module,
                  const char *fmt, ...) RS_PRINTF_FMT(4, 5);
void rs_log_vwrite(rs_log_t *log, rs_log_level_t level, const char *module,
                   const char *fmt, va_list ap) RS_PRINTF_FMT(4, 0);

/* 既定ロガーの設定。log を閉じる前に NULL に戻すこと（rs_log_close は自動で解除する）。 */
void      rs_log_set_default(rs_log_t *log);
rs_log_t *rs_log_get_default(void);

const char *rs_log_level_name(rs_log_level_t level);
/* "trace" / "debug" / "info" / "warn" / "error" / "fatal" / "off"（大文字小文字不問） */
int rs_log_level_from_string(const char *str, rs_log_level_t *out);

/* 既定ロガー向けショートハンド */
#define RS_LOG_TRACE(mod, ...) rs_log_write(NULL, RS_LOG_LEVEL_TRACE, (mod), __VA_ARGS__)
#define RS_LOG_DEBUG(mod, ...) rs_log_write(NULL, RS_LOG_LEVEL_DEBUG, (mod), __VA_ARGS__)
#define RS_LOG_INFO(mod, ...)  rs_log_write(NULL, RS_LOG_LEVEL_INFO,  (mod), __VA_ARGS__)
#define RS_LOG_WARN(mod, ...)  rs_log_write(NULL, RS_LOG_LEVEL_WARN,  (mod), __VA_ARGS__)
#define RS_LOG_ERROR(mod, ...) rs_log_write(NULL, RS_LOG_LEVEL_ERROR, (mod), __VA_ARGS__)
#define RS_LOG_FATAL(mod, ...) rs_log_write(NULL, RS_LOG_LEVEL_FATAL, (mod), __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* RS_LOG_H */
