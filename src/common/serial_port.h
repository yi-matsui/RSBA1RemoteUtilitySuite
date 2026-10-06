/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * シリアルポート抽象化（Windows: COMx / Linux: /dev/ttyUSBx 等）。
 * 読み込みはノンブロッキング（利用可能な分だけ即時に返す）、書き込みは上限時間付き。
 * 8N1・フロー制御なし・DTR/RTS はネゲート（IC-9100 の USB SEND / キーイングに
 * DTR/RTS が割り当てられていても意図せず送信状態にしないため）。
 * 単体テストでは ops を差し替えてモックとして使う。
 */
#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_WRITE_TIMEOUT_MS 200

typedef struct serial_ops {
    /* 利用可能なバイトを読む（0 バイトでも RS_OK）。デバイス異常は RS_ERR_IO */
    int  (*read)(void *st, void *buf, size_t cap, size_t *got);
    /* 全バイトを書く。時間内に書き切れなければ RS_ERR_TIMEOUT、デバイス異常は RS_ERR_IO */
    int  (*write)(void *st, const void *buf, size_t len);
    void (*close)(void *st);
} serial_ops_t;

typedef struct serial_port {
    const serial_ops_t *ops;
    void               *st;
} serial_port_t;

/* OS のシリアルポートを開く。name: "COM3" / "\\\\.\\COM10" / "/dev/ttyUSB0" 等。
 * RS_ERR_NOT_FOUND（存在しない）/ RS_ERR_BUSY（他プロセスが使用中）/ RS_ERR_PERMISSION /
 * RS_ERR_INVALID_ARG（未対応のボーレート）/ RS_ERR_SYSTEM */
int  serial_open(serial_port_t *port, const char *name, uint32_t baud);
void serial_close(serial_port_t *port);

int serial_read(serial_port_t *port, void *buf, size_t cap, size_t *got);
int serial_write(serial_port_t *port, const void *buf, size_t len);

/* 直前の OS エラーコード（GetLastError / errno、スレッドローカル） */
int serial_last_os_error(void);

static inline int serial_is_open(const serial_port_t *port)
{
    return port != NULL && port->ops != NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_PORT_H */
