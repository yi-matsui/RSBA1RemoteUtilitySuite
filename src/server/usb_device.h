/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * USB デバイスの列挙・特定・ハードウェアリセットの OS 抽象化層。
 * OS 固有処理は usb_backend_t（Windows: CfgMgr32 / Linux: sysfs + usbdevfs）に閉じ込め、
 * 単体テストではモックバックエンドに差し替える。設計: doc/usb_reset_architecture.md
 */
#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_DEVICE_ID_MAX 256
#define USB_SERIAL_MAX    128

typedef enum usb_reset_method {
    USB_RESET_AUTO = 0,     /* バックエンド既定の順序で試行し、復帰しなければ次の方式へ */
    USB_RESET_USBDEVFS,     /* Linux: ioctl(fd, USBDEVFS_RESET, 0) */
    USB_RESET_AUTHORIZED,   /* Linux: sysfs authorized を 0 → 1 */
    USB_RESET_DEVNODE       /* Windows: CM_Disable_DevNode → CM_Enable_DevNode */
} usb_reset_method_t;

typedef struct usb_device {
    char     id[USB_DEVICE_ID_MAX];   /* Windows: デバイスインスタンス ID / Linux: sysfs 名 ("1-1.2") */
    uint16_t vid;
    uint16_t pid;
    char     serial[USB_SERIAL_MAX];  /* iSerialNumber。無い場合は "" */
    int      ready;                   /* 非 0: ドライバが起動済みで使用可能 */
    int      busnum;                  /* Linux のみ（/dev/bus/usb/BBB/DDD）。Windows では 0 */
    int      devnum;
} usb_device_t;

/* 検索条件。指定した項目すべてに一致するデバイスを探す。最低 1 項目は指定すること。 */
typedef struct usb_match {
    uint16_t    vid;        /* 0 = 条件なし */
    uint16_t    pid;        /* 0 = 条件なし */
    const char *serial;     /* NULL / "" = 条件なし */
    const char *device_id;  /* NULL / "" = 条件なし（usb_device_t.id と完全一致） */
} usb_match_t;

/* 列挙コールバック。非 0 を返すと列挙を打ち切る。 */
typedef int (*usb_enum_cb)(const usb_device_t *dev, void *user);

/* OS 固有処理のテーブル。各関数は rs_status を返す。 */
typedef struct usb_backend {
    const char *name;
    void       *ctx;
    /* 接続中の USB デバイス（インタフェースノードを除く）を列挙する */
    int (*enumerate)(void *ctx, usb_enum_cb cb, void *user);
    /* 親デバイス（ハブ）を取得する。ルートハブ / ホストコントローラは RS_ERR_UNSUPPORTED */
    int (*get_parent)(void *ctx, const usb_device_t *dev, usb_device_t *parent);
    /* target をリセットする。復帰待ちは呼び出し側が行う */
    int (*reset)(void *ctx, const usb_device_t *target, usb_reset_method_t method);
    /* USB_RESET_AUTO 時に試行する方式（先頭から順に） */
    const usb_reset_method_t *auto_methods;
    size_t n_auto_methods;
} usb_backend_t;

/* 実行環境の OS 用バックエンド（静的オブジェクト） */
const usb_backend_t *usb_backend_platform(void);

/* 直前に RS_ERR_SYSTEM 等を返したバックエンド呼び出しの OS エラーコード（スレッドローカル）。
 * Windows: CONFIGRET / Linux: errno */
int  usb_last_os_error(void);
void usb_set_last_os_error(int err); /* バックエンド実装用 */

int usb_enumerate(const usb_backend_t *backend, usb_enum_cb cb, void *user);

/* match に一致する最初のデバイスを out に格納する。見つからなければ RS_ERR_NOT_FOUND。 */
int usb_find(const usb_backend_t *backend, const usb_match_t *match, usb_device_t *out);

int usb_match_device(const usb_match_t *match, const usb_device_t *dev);

/* "10C4:EA60" 形式（16 進、大文字小文字不問）を解析する */
int usb_parse_vid_pid(const char *str, uint16_t *vid, uint16_t *pid);

const char *usb_reset_method_name(usb_reset_method_t method);
/* "auto" / "usbdevfs" / "authorized" / "devnode" */
int usb_reset_method_from_string(const char *str, usb_reset_method_t *out);

#ifdef __cplusplus
}
#endif

#endif /* USB_DEVICE_H */
