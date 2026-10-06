/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * JSON 設定ファイルの型付き読み出しと検証。
 *   - 項目がなければ既定値（フォールバック）
 *   - 型違い・範囲外はエラー（"file:line:col: path: 理由" をログに出し、errors を数える）
 *   - 未知のキーは警告（書き間違いの検出）
 * エラーが 1 件でもあれば呼び出し側は起動を中止すること。
 * 文字列の戻り値は JSON 文書内を指すため、文書を解放するまで有効。
 */
#ifndef CFG_LOAD_H
#define CFG_LOAD_H

#include <stdint.h>

#include "audio_dev.h"
#include "audio_link.h"
#include "json.h"
#include "rs_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cfg_reader {
    const json_value_t *root;
    const char         *source;    /* メッセージ用のファイル名 */
    int                 errors;
    int                 warnings;
} cfg_reader_t;

void cfg_reader_init(cfg_reader_t *r, const json_value_t *root, const char *source);

/* エラー・警告を記録してログに出す（path は "audio.jitter_min_ms" 等、v は位置表示用で NULL 可） */
void cfg_error(cfg_reader_t *r, const char *path, const json_value_t *v, const char *fmt, ...)
    RS_PRINTF_FMT(4, 5);
void cfg_warn(cfg_reader_t *r, const char *path, const json_value_t *v, const char *fmt, ...)
    RS_PRINTF_FMT(4, 5);

/* 文字列。null も「未指定」として既定値を返す */
const char *cfg_string(cfg_reader_t *r, const char *path, const char *def);
/* 0 以上の整数（小数部があればエラー） */
uint32_t    cfg_uint(cfg_reader_t *r, const char *path, uint32_t def, uint32_t min, uint32_t max);
int         cfg_bool(cfg_reader_t *r, const char *path, int def);
/* 0〜255 の数値、または "0x7C" / "124" 形式の文字列 */
uint8_t     cfg_byte(cfg_reader_t *r, const char *path, uint8_t def);

/* object_path（"" でルート）のキーのうち known に含まれないものを警告する。
 * object_path が存在してオブジェクトでない場合はエラー。known は NULL 終端 */
void cfg_known_keys(cfg_reader_t *r, const char *object_path, const char *const known[]);

/* "default" / "" を NULL（既定デバイス）として扱う */
const char *cfg_device_name(const char *name);

/* audio セクション（backend / codec / sample_rate / channels / frame_ms / opus_bitrate /
 * jitter_min_ms / jitter_max_ms）を読む。backend = "none" で *enabled = 0 */
void cfg_read_audio(cfg_reader_t *r, const char *section, audio_dev_config_t *dev,
                    audio_link_config_t *link, int *enabled);

/* logging セクション（level / file / max_file_bytes / max_backups / to_stderr）を読む */
void cfg_read_logging(cfg_reader_t *r, const char *section, rs_log_config_t *log);

/* ファイルを読み込む。見つからない・構文エラーはログに出して非 0 を返す */
int cfg_load_file(const char *path, json_value_t **doc);

/* 設定ファイルに other（所有者・グループ以外）の権限があれば警告する
 * （Linux のみ。パスワードを含むため。グループは専用グループでの運用を許すため対象外） */
void cfg_check_permissions(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CFG_LOAD_H */
