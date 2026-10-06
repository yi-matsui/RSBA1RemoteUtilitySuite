/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * CI-V シリアル透過中継（UDP 50002）。サーバ・クライアント共通。
 *   シリアル → フレーム切り出し → dch で封緘 → 送信フック
 *   受信パケット → dch で検証・復号 → フレーム検査 → シリアル
 * サーバ側のシリアルは IC-9100、クライアント側は操作ソフトの仮想 COM / 物理ポート。
 * ポートの開閉・再接続は呼び出し側が行い、attach / detach で渡す。
 * スレッドセーフではない。単一スレッドから使うこと。
 */
#ifndef CIV_LINK_H
#define CIV_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "civ.h"
#include "dch.h"
#include "serial_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct civ_link_stats {
    uint32_t frames_to_net;     /* シリアル → ネットワーク */
    uint32_t frames_from_net;   /* ネットワーク → シリアル */
    uint32_t packets_to_net;
    uint32_t dropped_inactive;  /* セッションなし・宛先未確定で送れなかったフレーム */
    uint32_t rejected_payload;  /* 認証済みだが CI-V フレーム列として不正なペイロード */
    uint32_t write_timeouts;
    uint32_t io_errors;
} civ_link_stats_t;

typedef struct civ_link {
    serial_port_t   *port;     /* NULL = 未接続 */
    dch_t            ch;
    civ_framer_t     framer;
    int            (*send)(void *user, const void *pkt, size_t len);
    void            *user;
    uint8_t          pending[DCH_MAX_PAYLOAD];
    size_t           pending_len;
    civ_link_stats_t stats;
} civ_link_t;

void civ_link_init(civ_link_t *link, int (*send)(void *user, const void *pkt, size_t len),
                   void *user);

/* シリアルポートを関連付ける（NULL で切り離し）。所有権は移らない */
void civ_link_attach(civ_link_t *link, serial_port_t *port);

/* セッション開始時に鍵を設定して中継を有効にする / 終了時に鍵を消去する */
void civ_link_start(civ_link_t *link, dch_role_t role, uint32_t session_id,
                    const uint8_t k_c2s[CTL_KEY_LEN], const uint8_t k_s2c[CTL_KEY_LEN]);
void civ_link_stop(civ_link_t *link);
int  civ_link_active(const civ_link_t *link);

/* シリアルの受信済みデータを読み、完成したフレームを 1 パケットにまとめて送信する。
 * シリアル異常は RS_ERR_IO（呼び出し側でポートを閉じて再接続する）。 */
int civ_link_poll(civ_link_t *link);

/* 受信パケットを処理する。認証に成功したパケットなら RS_OK と種別を返す
 * （呼び出し側はこれを根拠にピアアドレスを学習・更新する）。
 * CIV ならシリアルへ書き込む。書き込みタイムアウトは RS_ERR_TIMEOUT、異常は RS_ERR_IO。 */
int civ_link_handle_packet(civ_link_t *link, const uint8_t *pkt, size_t len, uint8_t *type);

/* 空の BIND パケットを送る（クライアント: アドレス登録・NAT 維持） */
int civ_link_send_bind(civ_link_t *link);

/* シリアルへ直接書き込む（PTT 強制解除などのフェイルセーフ用。セッション状態に依存しない） */
int civ_link_write_raw(civ_link_t *link, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CIV_LINK_H */
