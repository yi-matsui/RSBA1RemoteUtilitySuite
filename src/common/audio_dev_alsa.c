/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * Linux ALSA バックエンド（ノンブロッキング）。RSBA_HAVE_ALSA 定義時のみ有効。
 * IC-9100 の USB Audio CODEC は例えば "plughw:CARD=CODEC,DEV=0" で指定する。
 */
#include "audio_dev.h"

#include "rs_error.h"

#ifdef RSBA_HAVE_ALSA

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

typedef struct alsa_dev {
    snd_pcm_t *cap;
    snd_pcm_t *play;
} alsa_dev_t;

static int alsa_status(int err)
{
    switch (-err) {
    case ENOENT:
    case ENODEV: return RS_ERR_NOT_FOUND;
    case EBUSY:  return RS_ERR_BUSY;
    case EACCES:
    case EPERM:  return RS_ERR_PERMISSION;
    case EINVAL: return RS_ERR_UNSUPPORTED;
    default:     return RS_ERR_SYSTEM;
    }
}

static const char *pcm_name(const char *name)
{
    return (name == NULL || name[0] == '\0') ? "default" : name;
}

static int open_stream(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t dir,
                       const audio_dev_config_t *cfg)
{
    unsigned int latency_us =
        (unsigned int)((uint64_t)cfg->queue_frames * cfg->frame_samples * 1000000u /
                       cfg->sample_rate);
    int err = snd_pcm_open(pcm, pcm_name(name), dir, SND_PCM_NONBLOCK);

    if (err < 0) {
        *pcm = NULL;
        return alsa_status(err);
    }
    err = snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, 1,
                             cfg->sample_rate, 1, latency_us);
    if (err < 0) {
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return alsa_status(err);
    }
    return RS_OK;
}

static void alsa_close(void *st)
{
    alsa_dev_t *d = st;
    if (d->cap != NULL)
        snd_pcm_close(d->cap);
    if (d->play != NULL)
        snd_pcm_close(d->play);
    free(d);
}

static int alsa_read(void *st, int16_t *pcm, size_t max, size_t *got)
{
    alsa_dev_t *d = st;
    snd_pcm_sframes_t n = snd_pcm_readi(d->cap, pcm, (snd_pcm_uframes_t)max);

    if (n >= 0) {
        *got = (size_t)n;
        return RS_OK;
    }
    *got = 0;
    if (n == -EAGAIN)
        return RS_OK;
    /* オーバーラン (-EPIPE) 等からの回復。回復できなければデバイス異常 */
    if (snd_pcm_recover(d->cap, (int)n, 1) < 0)
        return RS_ERR_IO;
    snd_pcm_start(d->cap);
    return RS_OK;
}

static size_t alsa_write_avail(void *st)
{
    alsa_dev_t *d = st;
    snd_pcm_sframes_t a = snd_pcm_avail_update(d->play);

    if (a < 0) {
        snd_pcm_recover(d->play, (int)a, 1);
        return 0;
    }
    return (size_t)a;
}

static int alsa_write(void *st, const int16_t *pcm, size_t samples)
{
    alsa_dev_t *d = st;
    snd_pcm_sframes_t n = snd_pcm_writei(d->play, pcm, (snd_pcm_uframes_t)samples);

    if (n >= 0 || n == -EAGAIN)
        return RS_OK;
    if (snd_pcm_recover(d->play, (int)n, 1) < 0)
        return RS_ERR_IO;
    return RS_OK;
}

static const audio_dev_ops_t k_alsa_ops = { alsa_read, alsa_write, alsa_write_avail, alsa_close };

int audio_dev_open_alsa(audio_dev_t *dev, const audio_dev_config_t *cfg)
{
    alsa_dev_t *d = calloc(1, sizeof(*d));
    int rc;

    if (d == NULL)
        return RS_ERR_NO_MEMORY;
    rc = open_stream(&d->cap, cfg->capture, SND_PCM_STREAM_CAPTURE, cfg);
    if (rc == RS_OK)
        rc = open_stream(&d->play, cfg->playback, SND_PCM_STREAM_PLAYBACK, cfg);
    if (rc == RS_OK && snd_pcm_start(d->cap) < 0)
        rc = RS_ERR_SYSTEM;
    if (rc != RS_OK) {
        alsa_close(d);
        return rc;
    }
    dev->ops = &k_alsa_ops;
    dev->st = d;
    return RS_OK;
}

void audio_dev_list_alsa(audio_dev_list_cb cb, void *user)
{
    void **hints = NULL;
    void **h;

    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return;
    for (h = hints; *h != NULL; h++) {
        char *name = snd_device_name_get_hint(*h, "NAME");
        char *ioid = snd_device_name_get_hint(*h, "IOID");
        if (name != NULL) {
            if (ioid == NULL || strcmp(ioid, "Input") == 0)
                cb("alsa", 1, name, user);
            if (ioid == NULL || strcmp(ioid, "Output") == 0)
                cb("alsa", 0, name, user);
        }
        free(name);
        free(ioid);
    }
    snd_device_name_free_hint(hints);
}

#else /* !RSBA_HAVE_ALSA */

int audio_dev_open_alsa(audio_dev_t *dev, const audio_dev_config_t *cfg)
{
    (void)dev;
    (void)cfg;
    return RS_ERR_UNSUPPORTED;
}

void audio_dev_list_alsa(audio_dev_list_cb cb, void *user)
{
    (void)cb;
    (void)user;
}

#endif
