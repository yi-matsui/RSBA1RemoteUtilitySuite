/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * config.server.json の読み込みと起動前検証。
 * 優先順位: 既定値 < 設定ファイル < 環境変数 RSBA_PASSWORD < コマンドライン引数
 */
#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include "json.h"
#include "rs_log.h"
#include "server_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVER_CONFIG_DEFAULT_PATH "config.server.json"

/* JSON 文書を cfg / log に適用する。戻り値はエラー件数（0 で成功）。warnings は NULL 可。
 * cfg 内の文字列は root を指すため、root は実行終了まで解放しないこと。 */
int server_config_apply(server_app_config_t *cfg, rs_log_config_t *log, const json_value_t *root,
                        const char *source, int *warnings);

/* 上書き後の最終検証（必須項目・相互関係）。戻り値はエラー件数 */
int server_config_validate(const server_app_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CONFIG_H */
