/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * client_bridge の実行本体。制御・CI-V・オーディオの 3 ソケットと、操作端末側の
 * シリアルポート（操作ソフトの仮想 COM 等）・オーディオデバイスを 1 つのイベントループで統合する。
 */
#ifndef CLIENT_APP_H
#define CLIENT_APP_H

#include <signal.h>
#include <stdint.h>

#include "audio_dev.h"
#include "audio_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct client_app_config {
    const char         *server_host;
    uint16_t            ctl_port;        /* 既定 50001（CI-V = +1、オーディオ = +2） */
    uint16_t            civ_port;
    uint16_t            audio_port;
    const char         *username;
    const char         *password;
    int                 auto_reconnect;  /* 既定 1 */
    const char         *bind_addr;       /* ローカル送信元アドレス。NULL = "::"（エフェメラルポート） */
    uint32_t            handshake_timeout_ms;   /* 既定 3000 */
    uint32_t            reconnect_interval_ms;  /* 既定 3000 */

    const char         *civ_device;      /* NULL = CI-V 中継なし */
    uint32_t            civ_baud;        /* 既定 19200 */

    int                 audio_enabled;
    audio_dev_config_t  audio_dev;
    audio_link_config_t audio_link;
} client_app_config_t;

void client_app_config_default(client_app_config_t *cfg);

/* *stop が非 0 になるか、再接続しない理由で切断されるまで実行する。
 * 戻り値: 0 = 正常終了、1 = 認証失敗・置き換え等、2 = 起動失敗 */
int client_app_run(const client_app_config_t *cfg, volatile sig_atomic_t *stop);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_APP_H */
