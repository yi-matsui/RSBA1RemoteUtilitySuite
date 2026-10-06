/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 共通エラーコード定義。公開関数は原則 int を返し、0 = 成功、負値 = 下記コード。
 */
#ifndef RS_ERROR_H
#define RS_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

enum rs_status {
    RS_OK                  = 0,
    RS_ERR_INVALID_ARG     = -1,  /* 引数不正 */
    RS_ERR_NO_MEMORY       = -2,  /* メモリ確保失敗 */
    RS_ERR_SYSTEM          = -3,  /* OS API 失敗（詳細は各モジュールの last_os_error） */
    RS_ERR_TIMEOUT         = -4,  /* タイムアウト */
    RS_ERR_TOO_LARGE       = -5,  /* データ長が上限超過 */
    RS_ERR_TRUNCATED       = -6,  /* 出力バッファ不足で切り詰め */
    RS_ERR_ADDRESS         = -7,  /* アドレス解決失敗 */
    RS_ERR_IO              = -8,  /* ファイル等の入出力失敗 */
    RS_ERR_NOT_FOUND       = -9,  /* 対象（デバイス等）が見つからない */
    RS_ERR_PERMISSION      = -10, /* 権限不足（管理者 / root が必要） */
    RS_ERR_BUSY            = -11, /* 現在は実行できない（レート制限・使用中・拒否） */
    RS_ERR_UNSUPPORTED     = -12  /* 未対応、または安全上許可しない操作 */
};

/* エラーコードの説明文字列を返す。未知のコードには "unknown error" を返す。 */
const char *rs_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif /* RS_ERROR_H */
