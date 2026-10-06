/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * server_bridge の実行本体。制御 (50001) / CI-V (50002) / オーディオ (50003) の 3 ソケットと
 * IC-9100 のシリアル・オーディオデバイス、USB 監視を 1 つのイベントループで統合する。
 */
#ifndef SERVER_APP_H
#define SERVER_APP_H

#include <signal.h>
#include <stdint.h>

#include "audio_dev.h"
#include "audio_link.h"
#include "usb_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct server_app_config {
    const char         *username;
    const char         *password;
    uint32_t            pbkdf2_iterations;
    const char         *listen_addr;     /* NULL = "::" */
    uint16_t            ctl_port;        /* 既定 50001 */
    uint16_t            civ_port;        /* 既定 50002 */
    uint16_t            audio_port;      /* 既定 50003 */

    uint32_t            keepalive_interval_ms;  /* 既定 1000 */
    uint32_t            keepalive_timeout_ms;   /* 既定 5000 */
    uint32_t            challenge_timeout_ms;   /* 既定 5000 */
    int                 max_failures;           /* 既定 5 */
    uint32_t            failure_window_ms;      /* 既定 300000 */
    uint32_t            ban_ms;                 /* 既定 900000 */

    const char         *civ_device;      /* NULL = CI-V 中継なし */
    uint32_t            civ_baud;        /* 既定 19200 */
    uint8_t             radio_addr;      /* 既定 0x7C */
    uint8_t             ctrl_addr;       /* 既定 0xE0 */

    int                 audio_enabled;   /* 0 = オーディオ中継なし */
    audio_dev_config_t  audio_dev;
    audio_link_config_t audio_link;

    int                 usb_watch;       /* 非 0: USB 監視・自動リセットを有効化 */
    usb_monitor_config_t usb;
    const char         *usb_log_path;    /* USB リセット専用ログ */
} server_app_config_t;

void server_app_config_default(server_app_config_t *cfg);

/* *stop が非 0 になるまで実行する。終了時はセッションがあれば PTT 解除・切断通知を行う */
int server_app_run(const server_app_config_t *cfg, volatile sig_atomic_t *stop);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_APP_H */
