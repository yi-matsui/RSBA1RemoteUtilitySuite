/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 連続認証失敗による IP 一時遮断（Fail2ban 類似）。
 * 送信元アドレス（ポートは無視）ごとに失敗回数を数え、一定期間内に閾値に達したら遮断する。
 * 固定長テーブルで、満杯時は遮断中でない最も古いエントリを再利用する。
 */
#ifndef BAN_LIST_H
#define BAN_LIST_H

#include <stdint.h>

#include "net_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BAN_LIST_CAPACITY 256

typedef struct ban_list_config {
    int      max_failures;       /* この回数で遮断（0 以下で遮断無効） */
    uint32_t failure_window_ms;  /* 失敗回数を数える期間 */
    uint32_t ban_ms;             /* 遮断時間 */
} ban_list_config_t;

typedef struct ban_entry {
    uint8_t  addr[16];
    int      used;
    int      failures;
    uint64_t window_start;
    uint64_t banned_until;  /* 0 = 遮断なし */
    uint64_t last_seen;
} ban_entry_t;

typedef struct ban_list {
    ban_list_config_t cfg;
    ban_entry_t       entries[BAN_LIST_CAPACITY];
} ban_list_t;

void ban_list_init(ban_list_t *bl, const ban_list_config_t *cfg);

/* 遮断中なら残り時間（ミリ秒、1 以上）、遮断されていなければ 0 */
uint64_t ban_list_banned_ms(const ban_list_t *bl, const net_addr_t *addr, uint64_t now_ms);

/* 認証失敗を記録する。この失敗で遮断が始まった場合は 1 */
int ban_list_record_failure(ban_list_t *bl, const net_addr_t *addr, uint64_t now_ms);

/* 認証成功時に失敗履歴を消去する */
void ban_list_record_success(ban_list_t *bl, const net_addr_t *addr);

#ifdef __cplusplus
}
#endif

#endif /* BAN_LIST_H */
