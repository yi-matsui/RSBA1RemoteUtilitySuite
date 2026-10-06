/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * バックエンドの選択と null バックエンド。
 */
#include "audio_dev.h"

#include <stdlib.h>
#include <string.h>

#include "rs_error.h"
#include "rs_time.h"

void audio_dev_config_default(audio_dev_config_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->backend = "auto";
    cfg->sample_rate = 48000;
    cfg->frame_samples = 480;
    cfg->queue_frames = 4;
}

int audio_dev_open(audio_dev_t *dev, const audio_dev_config_t *cfg)
{
    const char *b;

    if (dev == NULL || cfg == NULL || cfg->sample_rate == 0 || cfg->frame_samples == 0 ||
        cfg->queue_frames < 2)
        return RS_ERR_INVALID_ARG;
    dev->ops = NULL;
    dev->st = NULL;
    b = cfg->backend != NULL ? cfg->backend : "auto";

    if (strcmp(b, "null") == 0)
        return audio_dev_open_null(dev, cfg);
    if (strcmp(b, "winmm") == 0)
        return audio_dev_open_winmm(dev, cfg);
    if (strcmp(b, "alsa") == 0)
        return audio_dev_open_alsa(dev, cfg);
    if (strcmp(b, "auto") == 0) {
#ifdef _WIN32
        return audio_dev_open_winmm(dev, cfg);
#else
        return audio_dev_open_alsa(dev, cfg);
#endif
    }
    return RS_ERR_INVALID_ARG;
}

void audio_dev_close(audio_dev_t *dev)
{
    if (!audio_dev_is_open(dev))
        return;
    dev->ops->close(dev->st);
    dev->ops = NULL;
    dev->st = NULL;
}

int audio_dev_read(audio_dev_t *dev, int16_t *pcm, size_t max, size_t *got)
{
    if (!audio_dev_is_open(dev) || pcm == NULL || got == NULL)
        return RS_ERR_INVALID_ARG;
    *got = 0;
    return dev->ops->read(dev->st, pcm, max, got);
}

int audio_dev_write(audio_dev_t *dev, const int16_t *pcm, size_t samples)
{
    if (!audio_dev_is_open(dev) || pcm == NULL)
        return RS_ERR_INVALID_ARG;
    return dev->ops->write(dev->st, pcm, samples);
}

size_t audio_dev_write_avail(audio_dev_t *dev)
{
    return audio_dev_is_open(dev) ? dev->ops->write_avail(dev->st) : 0;
}

void audio_dev_list(audio_dev_list_cb cb, void *user)
{
    if (cb == NULL)
        return;
    cb("null", 1, "null", user);
    cb("null", 0, "null", user);
    audio_dev_list_winmm(cb, user);
    audio_dev_list_alsa(cb, user);
}

/* ---- null バックエンド ---------------------------------------------------- */
/* 単調時計から「実時間でこれまでに録音・再生されたはずのサンプル数」を求めて振る舞う */

typedef struct null_dev {
    uint32_t rate;
    uint64_t start_ms;
    uint64_t captured;     /* read で渡した総サンプル数 */
    uint64_t written;      /* write された総サンプル数 */
    uint64_t queue_limit;  /* 再生キューの上限サンプル数 */
} null_dev_t;

static uint64_t null_elapsed_samples(const null_dev_t *d)
{
    return (rs_time_monotonic_ms() - d->start_ms) * d->rate / 1000u;
}

static int null_read(void *st, int16_t *pcm, size_t max, size_t *got)
{
    null_dev_t *d = st;
    uint64_t avail = null_elapsed_samples(d) - d->captured;
    size_t n = avail < max ? (size_t)avail : max;

    memset(pcm, 0, n * sizeof(int16_t));
    d->captured += n;
    *got = n;
    return RS_OK;
}

static size_t null_write_avail(void *st)
{
    null_dev_t *d = st;
    uint64_t played = null_elapsed_samples(d);
    uint64_t queued = d->written > played ? d->written - played : 0;

    if (d->written < played)
        d->written = played; /* アンダーラン: 再生位置に追いつかせる */
    return queued >= d->queue_limit ? 0 : (size_t)(d->queue_limit - queued);
}

static int null_write(void *st, const int16_t *pcm, size_t samples)
{
    null_dev_t *d = st;
    (void)pcm;
    d->written += samples;
    return RS_OK;
}

static void null_close(void *st)
{
    free(st);
}

static const audio_dev_ops_t k_null_ops = { null_read, null_write, null_write_avail, null_close };

int audio_dev_open_null(audio_dev_t *dev, const audio_dev_config_t *cfg)
{
    null_dev_t *d = calloc(1, sizeof(*d));

    if (d == NULL)
        return RS_ERR_NO_MEMORY;
    d->rate = cfg->sample_rate;
    d->start_ms = rs_time_monotonic_ms();
    d->queue_limit = (uint64_t)cfg->frame_samples * cfg->queue_frames;
    dev->ops = &k_null_ops;
    dev->st = d;
    return RS_OK;
}
