/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "audio_codec.h"

#include <string.h>

#include "rs_error.h"

/* ---- PCM16 (L16, big-endian) -------------------------------------------- */

static int pcm16_encode(void *st, const int16_t *pcm, size_t samples,
                        uint8_t *out, size_t cap, size_t *out_len)
{
    size_t i;

    (void)st;
    if (cap < samples * 2)
        return RS_ERR_TRUNCATED;
    for (i = 0; i < samples; i++) {
        uint16_t v = (uint16_t)pcm[i];
        out[i * 2] = (uint8_t)(v >> 8);
        out[i * 2 + 1] = (uint8_t)v;
    }
    *out_len = samples * 2;
    return RS_OK;
}

static int pcm16_decode(void *st, const uint8_t *in, size_t len,
                        int16_t *pcm, size_t cap_samples, size_t *out_samples)
{
    size_t n = len / 2;
    size_t i;

    (void)st;
    if (len % 2 != 0)
        return RS_ERR_INVALID_ARG;
    if (n > cap_samples)
        return RS_ERR_TRUNCATED;
    for (i = 0; i < n; i++)
        pcm[i] = (int16_t)(uint16_t)((unsigned)in[i * 2] << 8 | in[i * 2 + 1]);
    *out_samples = n;
    return RS_OK;
}

static const audio_codec_ops_t k_pcm16_ops = {
    "pcm", pcm16_encode, pcm16_decode, NULL, NULL
};

/* ---- 共通 ---------------------------------------------------------------- */

int audio_codec_open(audio_codec_t *c, audio_codec_id_t id, uint32_t sample_rate,
                     uint32_t frame_samples)
{
    if (c == NULL || sample_rate == 0 || frame_samples == 0)
        return RS_ERR_INVALID_ARG;
    memset(c, 0, sizeof(*c));

    switch (id) {
    case AUDIO_CODEC_PCM16:
        c->ops = &k_pcm16_ops;
        break;
    case AUDIO_CODEC_OPUS:
        /* libopus 組み込み時: opus_encoder_create / opus_decoder_create を st に保持し、
         * decode(NULL) を conceal に割り当てる */
        return RS_ERR_UNSUPPORTED;
    default:
        return RS_ERR_INVALID_ARG;
    }
    c->id = id;
    c->sample_rate = sample_rate;
    c->frame_samples = frame_samples;
    return RS_OK;
}

void audio_codec_close(audio_codec_t *c)
{
    if (c == NULL || c->ops == NULL)
        return;
    if (c->ops->destroy != NULL)
        c->ops->destroy(c->st);
    memset(c, 0, sizeof(*c));
}

int audio_codec_encode(audio_codec_t *c, const int16_t *pcm, size_t samples,
                       uint8_t *out, size_t cap, size_t *out_len)
{
    if (c == NULL || c->ops == NULL || pcm == NULL || out == NULL || out_len == NULL ||
        samples != c->frame_samples)
        return RS_ERR_INVALID_ARG;
    return c->ops->encode(c->st, pcm, samples, out, cap, out_len);
}

int audio_codec_decode(audio_codec_t *c, const uint8_t *in, size_t len,
                       int16_t *pcm, size_t cap_samples, size_t *out_samples)
{
    if (c == NULL || c->ops == NULL || in == NULL || pcm == NULL || out_samples == NULL)
        return RS_ERR_INVALID_ARG;
    return c->ops->decode(c->st, in, len, pcm, cap_samples, out_samples);
}

int audio_codec_from_string(const char *s, audio_codec_id_t *out)
{
    if (s == NULL || out == NULL)
        return RS_ERR_INVALID_ARG;
    if (strcmp(s, "pcm") == 0)
        *out = AUDIO_CODEC_PCM16;
    else if (strcmp(s, "opus") == 0)
        *out = AUDIO_CODEC_OPUS;
    else
        return RS_ERR_INVALID_ARG;
    return RS_OK;
}

const char *audio_codec_name(audio_codec_id_t id)
{
    switch (id) {
    case AUDIO_CODEC_PCM16: return "pcm";
    case AUDIO_CODEC_OPUS:  return "opus";
    default:                return "?";
    }
}
