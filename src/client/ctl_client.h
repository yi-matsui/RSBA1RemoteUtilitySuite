/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 制御チャネル (UDP 50001) クライアント側プロトコルエンジン。
 * サーバ側と同様に送信フックと now_ms 引数で I/O・時計を注入する。
 *
 * 状態: IDLE → HELLO_SENT → AUTH_SENT → CONNECTED → (切断) → IDLE
 * サーバ証明を検証する相互認証のため、なりすましサーバには接続しない。
 * スレッドセーフではない。単一スレッドから使うこと。
 */
#ifndef CTL_CLIENT_H
#define CTL_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "ctl_proto.h"
#include "net_socket.h"
#include "rs_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ctl_client_state {
    CTL_CLIENT_IDLE = 0,
    CTL_CLIENT_HELLO_SENT,
    CTL_CLIENT_AUTH_SENT,
    CTL_CLIENT_CONNECTED
} ctl_client_state_t;

typedef enum ctl_client_end {
    CTL_CLIENT_END_USER = 0,          /* ctl_client_disconnect() */
    CTL_CLIENT_END_SERVER,            /* サーバの DISCONNECT（shutdown / timeout） */
    CTL_CLIENT_END_REPLACED,          /* 別クライアントにセッションを置き換えられた */
    CTL_CLIENT_END_TIMEOUT,           /* PONG 途絶 */
    CTL_CLIENT_END_HANDSHAKE_TIMEOUT, /* ハンドシェイク応答なし */
    CTL_CLIENT_END_AUTH_FAILED,       /* 認証拒否 */
    CTL_CLIENT_END_SERVER_PROOF       /* サーバ証明不一致（なりすましの疑い） */
} ctl_client_end_t;

typedef struct ctl_client_hooks {
    void *user;
    int  (*send)(void *user, const net_addr_t *to, const void *buf, size_t len);  /* 必須 */
    void (*connected)(void *user, uint32_t session_id);
    void (*disconnected)(void *user, ctl_client_end_t reason);
} ctl_client_hooks_t;

typedef struct ctl_client_config {
    const char *username;
    const char *password;               /* 内部にコピーし、破棄時に消去する */
    net_addr_t  server;
    uint32_t    handshake_timeout_ms;   /* 既定 3000 */
    int         auto_reconnect;         /* 非 0: 途絶・タイムアウト後に自動再接続（既定 1） */
    uint32_t    reconnect_interval_ms;  /* 既定 3000 */
    uint32_t    max_pbkdf2_iterations;  /* サーバ指定の反復回数の上限（DoS 対策）。既定 1000000 */
    rs_log_t   *log;
} ctl_client_config_t;

typedef struct ctl_client ctl_client_t;

void ctl_client_config_default(ctl_client_config_t *cfg);

int  ctl_client_create(ctl_client_t **out, const ctl_client_config_t *cfg,
                       const ctl_client_hooks_t *hooks);
/* 接続中なら DISCONNECT を送ってから解放する */
void ctl_client_destroy(ctl_client_t *cli);

/* HELLO を送信して接続を開始する。IDLE 以外では RS_ERR_BUSY */
int  ctl_client_connect(ctl_client_t *cli, uint64_t now_ms);
/* 利用者による切断。自動再接続も停止する */
void ctl_client_disconnect(ctl_client_t *cli, uint64_t now_ms);

void ctl_client_handle_packet(ctl_client_t *cli, const net_addr_t *from,
                              const uint8_t *data, size_t len, uint64_t now_ms);
/* PING 送信・タイムアウト判定・再接続。100ms 程度の周期で呼ぶ */
void ctl_client_tick(ctl_client_t *cli, uint64_t now_ms);

ctl_client_state_t ctl_client_state(const ctl_client_t *cli);
uint32_t           ctl_client_session_id(const ctl_client_t *cli);
uint32_t           ctl_client_last_rtt_ms(const ctl_client_t *cli);
/* CONNECTED 時にセッション鍵をコピーする（データチャネルの鍵導出用）。それ以外は RS_ERR_NOT_FOUND。
 * connected フック内から呼んでよい。呼び出し側は使用後に消去すること。 */
int                ctl_client_session_keys(const ctl_client_t *cli, uint8_t k_c2s[CTL_KEY_LEN],
                                           uint8_t k_s2c[CTL_KEY_LEN]);

const char *ctl_client_end_name(ctl_client_end_t reason);
const char *ctl_client_state_name(ctl_client_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* CTL_CLIENT_H */
