/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 適応型ジッターバッファ（モノラル 16-bit PCM、固定長フレーム）。
 *
 * - 受信フレームを送信側タイムスタンプ（サンプル単位）でリング状のスロットに格納し、
 *   順序の入れ替わり・重複・遅着を吸収する。
 * - 到着時刻とメディア時刻の差（transit）の直近ウィンドウ内の変動幅からジッターを推計し、
 *   目標遅延 = clamp(ジッター + 1 フレーム, min_delay_ms, max_delay_ms) とする
 *   （増加は即時、減少は緩やか）。
 * - 再生ポインタの微調整: 平滑化したバッファ量が目標から半フレーム以上ずれたら、
 *   線形補間で ±2% の速度で読み進めて目標に寄せる（送受信のクロック差も吸収）。
 * - バッファ量が max_delay_ms + 1 フレームを超えたらフレーム単位で読み飛ばす（オーバーラン対策）。
 * - 欠落フレームは直前のフレームを減衰させて補間し、データが尽きたらアンダーランとして
 *   再バッファリングする。
 * 再生側（デバイスのクロック）が jbuf_get を呼ぶ。スレッドセーフではない。
 */
#ifndef JBUF_H
#define JBUF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jbuf_config {
    uint32_t sample_rate;      /* 既定 48000 */
    uint32_t frame_samples;    /* 既定 480 (10 ms) */
    uint32_t min_delay_ms;     /* 既定 20 */
    uint32_t max_delay_ms;     /* 既定 80 */
    uint32_t capacity_frames;  /* 既定 32。max_delay_ms + 余裕を収められること */
} jbuf_config_t;

typedef enum jbuf_state {
    JBUF_BUFFERING = 0,  /* 目標遅延まで貯めている（無音を出力） */
    JBUF_PLAYING
} jbuf_state_t;

typedef struct jbuf_stats {
    uint64_t frames_received;
    uint64_t frames_late;        /* 再生位置を過ぎてから届いた */
    uint64_t frames_duplicate;
    uint64_t frames_concealed;   /* 欠落を補間した */
    uint64_t frames_skipped;     /* オーバーランで読み飛ばした */
    uint64_t underruns;          /* データが尽きて再バッファリングした回数 */
    uint64_t resets;             /* 大きな時刻の飛びで内容を破棄した回数 */
    uint64_t samples_dropped;    /* 微調整で詰めたサンプル数（速め再生） */
    uint64_t samples_inserted;   /* 微調整で伸ばしたサンプル数（遅め再生） */
    uint32_t target_ms;          /* 現在の目標遅延 */
    uint32_t level_ms;           /* 直近の jbuf_get 時点のバッファ量 */
    uint32_t jitter_ms;          /* 推計ジッター（transit の変動幅） */
    uint32_t extra_ms;           /* jbuf_set_extra_delay で与えられた再生側の余裕 */
    jbuf_state_t state;
} jbuf_stats_t;

typedef struct jbuf jbuf_t;

void jbuf_config_default(jbuf_config_t *cfg);

int  jbuf_create(jbuf_t **out, const jbuf_config_t *cfg);
void jbuf_destroy(jbuf_t *jb);
/* 内容・推計値をすべて破棄する（統計は保持） */
void jbuf_reset(jbuf_t *jb);

/* 1 フレームを投入する。timestamp は送信側のサンプル時計（フレームごとに frame_samples ずつ増加、
 * 32 ビットで周回してよい）。arrival_ms は受信時刻（単調増加ミリ秒）。
 * 遅着・重複は破棄して RS_OK（統計に計上）。samples != frame_samples や
 * フレーム境界に揃わないタイムスタンプは RS_ERR_INVALID_ARG。 */
int jbuf_put(jbuf_t *jb, uint32_t timestamp, const int16_t *pcm, size_t samples,
             uint64_t arrival_ms);

/* 再生用に samples 個を取り出す（常に samples 個を埋める。バッファリング中は無音） */
int jbuf_get(jbuf_t *jb, int16_t *out, size_t samples);

void jbuf_get_stats(const jbuf_t *jb, jbuf_stats_t *stats);

/* 再生側の取り出しの塊（デバイスが複数フレーム分の空きをまとめて返す等）による追加の遅延余裕。
 * 目標遅延 = clamp(ジッター + 1 フレーム + extra_ms, min, max) */
void jbuf_set_extra_delay(jbuf_t *jb, uint32_t extra_ms);

jbuf_state_t jbuf_state(const jbuf_t *jb);

#ifdef __cplusplus
}
#endif

#endif /* JBUF_H */
