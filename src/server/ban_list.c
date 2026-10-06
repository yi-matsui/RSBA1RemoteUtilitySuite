/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "ban_list.h"

#include <string.h>

void ban_list_init(ban_list_t *bl, const ban_list_config_t *cfg)
{
    memset(bl, 0, sizeof(*bl));
    bl->cfg = *cfg;
}

static const uint8_t *addr_bytes(const net_addr_t *addr)
{
    return addr->sa.sin6_addr.s6_addr;
}

/* 該当エントリの添字。なければ -1 */
static int find_index(const ban_list_t *bl, const net_addr_t *addr)
{
    int i;

    for (i = 0; i < BAN_LIST_CAPACITY; i++) {
        const ban_entry_t *e = &bl->entries[i];
        if (e->used && memcmp(e->addr, addr_bytes(addr), 16) == 0)
            return i;
    }
    return -1;
}

static ban_entry_t *lookup(ban_list_t *bl, const net_addr_t *addr)
{
    int i = find_index(bl, addr);
    return i >= 0 ? &bl->entries[i] : NULL;
}

/* 空き、なければ遮断中でない最古、それもなければ遮断終了が最も早いエントリ */
static ban_entry_t *allocate(ban_list_t *bl, uint64_t now_ms)
{
    ban_entry_t *victim = NULL;
    ban_entry_t *banned_victim = NULL;
    size_t i;

    for (i = 0; i < BAN_LIST_CAPACITY; i++) {
        ban_entry_t *e = &bl->entries[i];
        if (!e->used)
            return e;
        if (e->banned_until > now_ms) {
            if (banned_victim == NULL || e->banned_until < banned_victim->banned_until)
                banned_victim = e;
        } else if (victim == NULL || e->last_seen < victim->last_seen) {
            victim = e;
        }
    }
    return victim != NULL ? victim : banned_victim;
}

uint64_t ban_list_banned_ms(const ban_list_t *bl, const net_addr_t *addr, uint64_t now_ms)
{
    const ban_entry_t *e;
    int i;

    if (bl == NULL || addr == NULL)
        return 0;
    i = find_index(bl, addr);
    if (i < 0)
        return 0;
    e = &bl->entries[i];
    if (e->banned_until <= now_ms)
        return 0;
    return e->banned_until - now_ms;
}

int ban_list_record_failure(ban_list_t *bl, const net_addr_t *addr, uint64_t now_ms)
{
    ban_entry_t *e;

    if (bl == NULL || addr == NULL || bl->cfg.max_failures <= 0)
        return 0;

    e = lookup(bl, addr);
    if (e == NULL) {
        e = allocate(bl, now_ms);
        memset(e, 0, sizeof(*e));
        e->used = 1;
        memcpy(e->addr, addr_bytes(addr), 16);
        e->window_start = now_ms;
    }
    e->last_seen = now_ms;

    if (e->banned_until > now_ms)
        return 0; /* 既に遮断中 */

    if (now_ms - e->window_start > bl->cfg.failure_window_ms) {
        e->failures = 0;
        e->window_start = now_ms;
    }
    e->failures++;
    if (e->failures < bl->cfg.max_failures)
        return 0;

    /* 遮断開始。解除後は再び max_failures 回まで試行できる */
    e->banned_until = now_ms + bl->cfg.ban_ms;
    e->failures = 0;
    e->window_start = e->banned_until;
    return 1;
}

void ban_list_record_success(ban_list_t *bl, const net_addr_t *addr)
{
    ban_entry_t *e;

    if (bl == NULL || addr == NULL)
        return;
    /* 遮断中のアドレスのパケットは認証処理前に破棄されるため、ここに来るのは非遮断時のみ */
    e = lookup(bl, addr);
    if (e != NULL)
        memset(e, 0, sizeof(*e));
}
