/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 制御チャネル (UDP 50001) サーバ側プロトコルエンジン。
 * ソケットと時計は外部から注入する（送信はフック、時刻は now_ms 引数）。
 * これにより同一コードを実ソケットとモックテストの両方で使う。
 *
 * 状態: WAITING（待機）⇄ ACTIVE（セッション 1 本）
 * セッション終了時（切断要求 / Keepalive 途絶 / 置き換え / 終了）は必ず
 * ptt_release フック → session_end フックの順に呼び、鍵を消去して WAITING に戻る。
 * スレッドセーフではない。単一スレッドから使うこと。
 */
#ifndef CTL_SERVER_H
#define CTL_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "ctl_proto.h"
#include "net_socket.h"
#include "rs_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ctl_server_state {
    CTL_SERVER_WAITING = 0,
    CTL_SERVER_ACTIVE
} ctl_server_state_t;

typedef enum ctl_end_reason {
    CTL_END_CLIENT_DISCONNECT = 0,  /* クライアントの DISCONNECT */
    CTL_END_TIMEOUT,                /* Keepalive 途絶 */
    CTL_END_REPLACED,               /* 新しいクライアントが認証に成功 */
    CTL_END_SHUTDOWN                /* サーバ終了 */
} ctl_end_reason_t;

typedef struct ctl_server_hooks {
    void *user;
    /* 必須: パケット送信 */
    int  (*send)(void *user, const net_addr_t *to, const void *buf, size_t len);
    /* 切断フェイルセーフ: IC-9100 へ PTT 強制解除 (CI-V 1C 00 00) を発行する */
    void (*ptt_release)(void *user);
    void (*session_start)(void *user, uint32_t session_id, const net_addr_t *peer);
    void (*session_end)(void *user, uint32_t session_id, ctl_end_reason_t reason);
} ctl_server_hooks_t;

typedef struct ctl_server_config {
    const char *username;               /* 1〜CTL_USERNAME_MAX 文字 */
    const char *password;               /* 鍵導出後は保持しない */
    uint32_t    pbkdf2_iterations;      /* 既定 100000 */
    uint32_t    keepalive_interval_ms;  /* クライアントへ通知する PING 周期。既定 1000 */
    uint32_t    keepalive_timeout_ms;   /* 受信途絶でセッション破棄。既定 5000 */
    uint32_t    challenge_timeout_ms;   /* チャレンジの有効期間。既定 5000 */
    int         max_failures;           /* IP 遮断までの認証失敗回数。既定 5 */
    uint32_t    failure_window_ms;      /* 失敗回数の集計期間。既定 300000 */
    uint32_t    ban_ms;                 /* 遮断時間。既定 900000 */
    rs_log_t   *log;                    /* NULL で既定ロガー */
} ctl_server_config_t;

typedef struct ctl_server_stats {
    uint32_t rx_malformed;        /* 形式不正・方向違いの種別 */
    uint32_t rx_bad_tag;          /* タグ不一致（改ざん・偽造） */
    uint32_t rx_replay;           /* リプレイ・ウィンドウ外 */
    uint32_t rx_unknown_session;  /* セッション ID 不一致・セッションなし */
    uint32_t rx_banned;           /* 遮断中アドレスからの破棄 */
    uint32_t rx_stale_challenge;  /* 期限切れ・不明なチャレンジへの AUTH */
    uint32_t auth_ok;
    uint32_t auth_fail;
    uint32_t bans;
    uint32_t sessions_ended;
} ctl_server_stats_t;

typedef struct ctl_server ctl_server_t;

void ctl_server_config_default(ctl_server_config_t *cfg);

/* 鍵導出（PBKDF2）を行うため、iterations に比例した時間がかかる */
int  ctl_server_create(ctl_server_t **out, const ctl_server_config_t *cfg,
                       const ctl_server_hooks_t *hooks);
/* セッションが残っていれば SHUTDOWN として終了処理を行ってから解放する */
void ctl_server_destroy(ctl_server_t *srv);

void ctl_server_handle_packet(ctl_server_t *srv, const net_addr_t *from,
                              const uint8_t *data, size_t len, uint64_t now_ms);
/* Keepalive 途絶判定・期限切れチャレンジの破棄。100ms 程度の周期で呼ぶ */
void ctl_server_tick(ctl_server_t *srv, uint64_t now_ms);
/* セッションを SHUTDOWN として終了する（DISCONNECT 送信＋フック） */
void ctl_server_shutdown(ctl_server_t *srv);

ctl_server_state_t ctl_server_state(const ctl_server_t *srv);
uint32_t           ctl_server_session_id(const ctl_server_t *srv);  /* WAITING なら 0 */
/* ACTIVE 時にセッション鍵をコピーする（データチャネルの鍵導出用）。WAITING なら RS_ERR_NOT_FOUND。
 * session_start フック内から呼んでよい。呼び出し側は使用後に消去すること。 */
int                ctl_server_session_keys(const ctl_server_t *srv, uint8_t k_c2s[CTL_KEY_LEN],
                                           uint8_t k_s2c[CTL_KEY_LEN]);
uint64_t           ctl_server_banned_ms(const ctl_server_t *srv, const net_addr_t *addr,
                                        uint64_t now_ms);
const ctl_server_stats_t *ctl_server_stats(const ctl_server_t *srv);

const char *ctl_end_reason_name(ctl_end_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* CTL_SERVER_H */
