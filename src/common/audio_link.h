/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 双方向オーディオ伝送（UDP 50003）。サーバ・クライアント共通。
 *   送信: デバイス録音 → フレーム化 → コーデック → dch で封緘 → 送信フック
 *   受信: パケット → dch で検証・復号 → コーデック → ジッターバッファ
 *   再生: デバイスの再生キューに空きがある分だけジッターバッファから取り出して書く
 * サーバ側のデバイスは IC-9100 の USB Audio CODEC、クライアント側は操作端末のデバイス。
 * スレッドセーフではない。単一スレッドから使うこと。
 */
#ifndef AUDIO_LINK_H
#define AUDIO_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "audio_codec.h"
#include "audio_dev.h"
#include "dch.h"
#include "jbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PAYLOAD_HEADER_LEN 8

typedef struct audio_link_config {
    audio_codec_id_t codec;          /* 既定 PCM16 */
    uint32_t         sample_rate;    /* 既定 48000 */
    uint32_t         frame_samples;  /* 既定 480 (10 ms)。PCM16 は 1 パケットに収まる 600 以下 */
    uint32_t         jitter_min_ms;  /* 既定 20 */
    uint32_t         jitter_max_ms;  /* 既定 80 */
} audio_link_config_t;

typedef struct audio_link_stats {
    uint32_t frames_sent;
    uint32_t frames_received;
    uint32_t rejected_format;   /* コーデック・フレーム長の不一致 */
    uint32_t decode_errors;
    uint32_t device_errors;
} audio_link_stats_t;

typedef struct audio_link audio_link_t;

void audio_link_config_default(audio_link_config_t *cfg);

/* RS_ERR_UNSUPPORTED: コーデック未組み込み / RS_ERR_TOO_LARGE: フレームがパケットに収まらない */
int  audio_link_create(audio_link_t **out, const audio_link_config_t *cfg,
                       int (*send)(void *user, const void *pkt, size_t len), void *user);
void audio_link_destroy(audio_link_t *link);

/* デバイスを関連付ける（NULL で切り離し）。所有権は移らない */
void audio_link_attach(audio_link_t *link, audio_dev_t *dev);

void audio_link_start(audio_link_t *link, dch_role_t role, uint32_t session_id,
                      const uint8_t k_c2s[CTL_KEY_LEN], const uint8_t k_s2c[CTL_KEY_LEN]);
void audio_link_stop(audio_link_t *link);
int  audio_link_active(const audio_link_t *link);

/* 録音分の送信と再生キューへの供給を行う。数 ms 周期で呼ぶ。デバイス異常は RS_ERR_IO */
int audio_link_poll(audio_link_t *link);

/* 受信パケットを処理する。認証に成功したパケットなら RS_OK と種別を返す */
int audio_link_handle_packet(audio_link_t *link, const uint8_t *pkt, size_t len,
                             uint64_t now_ms, uint8_t *type);

int audio_link_send_bind(audio_link_t *link);

void audio_link_get_stats(const audio_link_t *link, audio_link_stats_t *stats,
                          jbuf_stats_t *jstats);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_LINK_H */
