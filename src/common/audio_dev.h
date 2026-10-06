/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * オーディオデバイス I/O 抽象化（モノラル 16-bit PCM、ノンブロッキング）。
 * 録音・再生を 1 つのデバイスとして扱い、呼び出し側のループから定期的に
 *   - read:        録音済みサンプルを取り出す
 *   - write_avail: 再生キューの空き（サンプル数）を問い合わせ、空いた分だけ write する
 * 再生キューの消費がデバイスのクロックで進むため、これを再生タイミングの基準とする。
 *
 * バックエンド:
 *   "null"  : 実時間で無音を録音し、再生データを破棄する（音声なし運用・試験用）
 *   "winmm" : Windows waveIn / waveOut
 *   "alsa"  : Linux ALSA（ビルド時に ALSA 開発ファイルが見つかった場合のみ）
 *   "auto"  : winmm / alsa のうち利用可能なもの
 * 単体テストでは ops を差し替えてモックとして使う。
 */
#ifndef AUDIO_DEV_H
#define AUDIO_DEV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_dev_ops {
    /* 録音済みのサンプルを最大 max 個取り出す（0 個でも RS_OK）。デバイス異常は RS_ERR_IO */
    int    (*read)(void *st, int16_t *pcm, size_t max, size_t *got);
    /* samples 個を再生キューに入れる。write_avail を超えてはならない */
    int    (*write)(void *st, const int16_t *pcm, size_t samples);
    /* ブロックせずに書ける再生サンプル数 */
    size_t (*write_avail)(void *st);
    void   (*close)(void *st);
} audio_dev_ops_t;

typedef struct audio_dev {
    const audio_dev_ops_t *ops;
    void                  *st;
} audio_dev_t;

typedef struct audio_dev_config {
    const char *backend;        /* "auto" / "null" / "winmm" / "alsa" */
    const char *capture;        /* デバイス名（部分一致、大文字小文字不問）。NULL / "" / "default" で既定 */
    const char *playback;
    uint32_t    sample_rate;    /* 既定 48000 */
    uint32_t    frame_samples;  /* 既定 480 (10 ms) */
    uint32_t    queue_frames;   /* 再生キュー・録音バッファのフレーム数。既定 4 */
} audio_dev_config_t;

typedef void (*audio_dev_list_cb)(const char *backend, int is_capture, const char *name,
                                  void *user);

void audio_dev_config_default(audio_dev_config_t *cfg);

/* RS_ERR_UNSUPPORTED（バックエンド未組み込み）/ RS_ERR_NOT_FOUND（デバイス名不一致）/
 * RS_ERR_BUSY / RS_ERR_SYSTEM */
int  audio_dev_open(audio_dev_t *dev, const audio_dev_config_t *cfg);
void audio_dev_close(audio_dev_t *dev);

int    audio_dev_read(audio_dev_t *dev, int16_t *pcm, size_t max, size_t *got);
int    audio_dev_write(audio_dev_t *dev, const int16_t *pcm, size_t samples);
size_t audio_dev_write_avail(audio_dev_t *dev);

/* 組み込み済みバックエンドのデバイス名を列挙する */
void audio_dev_list(audio_dev_list_cb cb, void *user);

static inline int audio_dev_is_open(const audio_dev_t *dev)
{
    return dev != NULL && dev->ops != NULL;
}

/* ---- バックエンド実装（audio_dev.c から呼ばれる） ------------------------ */
int  audio_dev_open_null(audio_dev_t *dev, const audio_dev_config_t *cfg);
int  audio_dev_open_winmm(audio_dev_t *dev, const audio_dev_config_t *cfg);
void audio_dev_list_winmm(audio_dev_list_cb cb, void *user);
int  audio_dev_open_alsa(audio_dev_t *dev, const audio_dev_config_t *cfg);
void audio_dev_list_alsa(audio_dev_list_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_DEV_H */
