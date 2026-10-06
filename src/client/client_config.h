/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * config.client.json の読み込みと起動前検証。
 * 優先順位: 既定値 < 設定ファイル < 環境変数 RSBA_PASSWORD < コマンドライン引数
 */
#ifndef CLIENT_CONFIG_H
#define CLIENT_CONFIG_H

#include "client_app.h"
#include "json.h"
#include "rs_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLIENT_CONFIG_DEFAULT_PATH "config.client.json"

/* JSON 文書を cfg / log に適用する。戻り値はエラー件数。warnings は NULL 可。
 * cfg 内の文字列は root を指すため、root は実行終了まで解放しないこと。 */
int client_config_apply(client_app_config_t *cfg, rs_log_config_t *log, const json_value_t *root,
                        const char *source, int *warnings);

/* 上書き後の最終検証。戻り値はエラー件数 */
int client_config_validate(const client_app_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_CONFIG_H */
