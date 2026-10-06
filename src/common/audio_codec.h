/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * オーディオコーデック抽象化。モノラル 16-bit PCM のフレーム単位で符号化・復号する。
 *   AUDIO_CODEC_PCM16: 非圧縮 L16（ビッグエンディアン、RFC 3551 と同じ並び）。常に利用可能。
 *   AUDIO_CODEC_OPUS : 予約。libopus 組み込み時に audio_codec_ops_t を実装して登録する
 *                      （現状は RS_ERR_UNSUPPORTED）。
 */
#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum audio_codec_id {
    AUDIO_CODEC_PCM16 = 1,
    AUDIO_CODEC_OPUS  = 2
} audio_codec_id_t;

typedef struct audio_codec_ops {
    const char *name;
    /* frame_samples 個の PCM を符号化する */
    int  (*encode)(void *st, const int16_t *pcm, size_t samples,
                   uint8_t *out, size_t cap, size_t *out_len);
    /* 1 フレームを復号する */
    int  (*decode)(void *st, const uint8_t *in, size_t len,
                   int16_t *pcm, size_t cap_samples, size_t *out_samples);
    /* 欠落フレームの補間（PLC）。NULL ならジッターバッファ既定の補間を使う */
    int  (*conceal)(void *st, int16_t *pcm, size_t samples);
    void (*destroy)(void *st);
} audio_codec_ops_t;

typedef struct audio_codec {
    audio_codec_id_t         id;
    const audio_codec_ops_t *ops;
    void                    *st;
    uint32_t                 sample_rate;
    uint32_t                 frame_samples;
} audio_codec_t;

/* RS_ERR_UNSUPPORTED: 未組み込みのコーデック */
int  audio_codec_open(audio_codec_t *c, audio_codec_id_t id, uint32_t sample_rate,
                      uint32_t frame_samples);
void audio_codec_close(audio_codec_t *c);

int audio_codec_encode(audio_codec_t *c, const int16_t *pcm, size_t samples,
                       uint8_t *out, size_t cap, size_t *out_len);
int audio_codec_decode(audio_codec_t *c, const uint8_t *in, size_t len,
                       int16_t *pcm, size_t cap_samples, size_t *out_samples);

/* "pcm" / "opus" */
int         audio_codec_from_string(const char *s, audio_codec_id_t *out);
const char *audio_codec_name(audio_codec_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CODEC_H */
