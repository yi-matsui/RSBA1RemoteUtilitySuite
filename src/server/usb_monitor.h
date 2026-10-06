/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * IC-9100 USB 接続の異常検知とハードウェアリセット。
 *
 * 利用側（シリアル I/O を所有するスレッド）の流れ:
 *   1. I/O 結果を usb_monitor_note_*() で通知、定期的に usb_monitor_poll() を呼ぶ
 *   2. USB_MONITOR_RESET_REQUIRED が返ったら、シリアル／オーディオのハンドルを閉じる
 *   3. usb_monitor_reset() を呼ぶ（リセット実行 → 再認識待ち → 専用ログ記録）
 *   4. 成功したらハンドルを開き直す
 * モニタはスレッドセーフではない。単一スレッドから使うこと。
 */
#ifndef USB_MONITOR_H
#define USB_MONITOR_H

#include <stddef.h>
#include <stdint.h>

#include "rs_log.h"
#include "usb_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* usb_monitor_note_*() / usb_monitor_poll() の戻り値: リセットが必要（正値、エラーコードと重複しない） */
#define USB_MONITOR_RESET_REQUIRED 1

typedef enum usb_reset_trigger {
    USB_TRIGGER_SERIAL_TIMEOUT = 0,  /* シリアル I/O タイムアウトの連続発生 */
    USB_TRIGGER_DEVICE_LOST,         /* デバイス消失・ドライバ停止 */
    USB_TRIGGER_RX_GARBAGE,          /* 不正バッファ蓄積（有効な CI-V フレームにならないデータ） */
    USB_TRIGGER_MANUAL               /* 運用者による手動実行 */
} usb_reset_trigger_t;

typedef struct usb_monitor_config {
    usb_match_t        match;                     /* 監視対象（文字列はモニタ内にコピーされる） */
    usb_reset_method_t method;                    /* 既定 USB_RESET_AUTO */
    int                reset_parent;              /* 非 0: 親ハブをリセットする（既定 0） */
    int                serial_timeout_threshold;  /* リセットを要求する連続タイムアウト回数（既定 3） */
    size_t             rx_garbage_threshold;      /* 不正データ蓄積の閾値バイト数（既定 4096） */
    uint32_t           recovery_timeout_ms;       /* 1 方式あたりの再認識待ち上限（既定 15000） */
    uint32_t           poll_interval_ms;          /* 再認識待ちのポーリング間隔（既定 250） */
    uint32_t           min_interval_ms;           /* リセットの最小間隔（既定 60000） */
    rs_log_t          *log;                       /* 専用ログ（所有しない）。NULL で既定ロガー */
} usb_monitor_config_t;

typedef struct usb_reset_result {
    usb_reset_trigger_t trigger;
    char                device_id[USB_DEVICE_ID_MAX];  /* 監視対象デバイス ID */
    char                target_id[USB_DEVICE_ID_MAX];  /* 実際にリセットしたデバイス ID */
    usb_reset_method_t  method;                        /* 最後に試行した方式 */
    int                 attempts;                      /* 試行した方式の数 */
    int                 status;                        /* 最終結果 (rs_status) */
    uint32_t            recovery_ms;                   /* 最後の試行の開始から再認識完了（失敗時は打ち切り）まで */
    uint32_t            total_ms;                      /* 全試行の所要時間 */
} usb_reset_result_t;

typedef struct usb_monitor usb_monitor_t;

void usb_monitor_config_default(usb_monitor_config_t *cfg);

/* backend が NULL の場合は usb_backend_platform() を使う */
int  usb_monitor_create(usb_monitor_t **out, const usb_monitor_config_t *cfg,
                        const usb_backend_t *backend);
void usb_monitor_destroy(usb_monitor_t *mon);

/* 正常な I/O を通知し、連続タイムアウト数と不正データ量をクリアする */
void usb_monitor_note_io_ok(usb_monitor_t *mon);
/* シリアル I/O タイムアウトを通知する。閾値到達で USB_MONITOR_RESET_REQUIRED */
int  usb_monitor_note_io_timeout(usb_monitor_t *mon);
/* フレームとして解釈できずに溜まっているバイト数を通知する。閾値以上で USB_MONITOR_RESET_REQUIRED */
int  usb_monitor_note_rx_garbage(usb_monitor_t *mon, size_t pending_bytes);

/* デバイスの存在・状態を確認する。使用可能だったデバイスが消失／停止した時点で
 * USB_MONITOR_RESET_REQUIRED（トリガー: DEVICE_LOST）。列挙失敗時は負のエラーコード。 */
int usb_monitor_poll(usb_monitor_t *mon);

/* ハードウェアリセットを実行し、再認識を待って専用ログに記録する。
 * 戻り値: RS_OK（再認識完了）/ RS_ERR_BUSY（最小間隔内のため未実行）/ RS_ERR_TIMEOUT（再認識せず）/
 *         RS_ERR_NOT_FOUND / RS_ERR_PERMISSION / RS_ERR_UNSUPPORTED / RS_ERR_SYSTEM 等。
 * result は NULL 可。 */
int usb_monitor_reset(usb_monitor_t *mon, usb_reset_trigger_t trigger, usb_reset_result_t *result);

/* 直近の検知トリガー（RESET_REQUIRED を返した理由） */
usb_reset_trigger_t usb_monitor_pending_trigger(const usb_monitor_t *mon);

const char *usb_reset_trigger_name(usb_reset_trigger_t trigger);

#ifdef __cplusplus
}
#endif

#endif /* USB_MONITOR_H */
