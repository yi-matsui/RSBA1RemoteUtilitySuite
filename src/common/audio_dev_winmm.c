/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * Windows waveIn / waveOut バックエンド。
 * コールバックを使わず WAVEHDR のフラグをポーリングするため、ドライバのスレッドと
 * 共有する状態はヘッダのフラグのみ。
 */
#include "audio_dev.h"

#include "rs_error.h"

#ifdef _WIN32

#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <mmsystem.h>

typedef struct winmm_dev {
    HWAVEIN   hin;
    HWAVEOUT  hout;
    WAVEHDR  *in_hdr;
    int16_t  *in_mem;
    uint32_t  in_n;
    uint32_t  in_next;
    size_t    in_pos;
    WAVEHDR  *out_hdr;
    int16_t  *out_mem;
    uint32_t  out_n;
    uint32_t  out_next;
    uint32_t  frame;
} winmm_dev_t;

static DWORD flags_of(const WAVEHDR *h)
{
    return *(const volatile DWORD *)&h->dwFlags;
}

static int mm_status(MMRESULT r)
{
    switch (r) {
    case MMSYSERR_NOERROR:    return RS_OK;
    case MMSYSERR_BADDEVICEID:
    case MMSYSERR_NODRIVER:   return RS_ERR_NOT_FOUND;
    case MMSYSERR_ALLOCATED:  return RS_ERR_BUSY;
    case WAVERR_BADFORMAT:    return RS_ERR_UNSUPPORTED;
    case MMSYSERR_NOMEM:      return RS_ERR_NO_MEMORY;
    default:                  return RS_ERR_SYSTEM;
    }
}

static int contains_nocase(const char *hay, const char *needle)
{
    size_t hn = strlen(hay);
    size_t nn = strlen(needle);
    size_t i;
    size_t j;

    for (i = 0; i + nn <= hn; i++) {
        for (j = 0; j < nn; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (j == nn)
            return 1;
    }
    return 0;
}

/* デバイス名を UTF-8 で取得する（ANSI 版はコードページ依存で日本語名が化けるため W 版を使う） */
static int in_name(UINT id, char *buf, size_t cap)
{
    WAVEINCAPSW caps;
    if (waveInGetDevCapsW(id, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        return 0;
    return WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, buf, (int)cap, NULL, NULL) > 0;
}

static int out_name(UINT id, char *buf, size_t cap)
{
    WAVEOUTCAPSW caps;
    if (waveOutGetDevCapsW(id, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        return 0;
    return WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, buf, (int)cap, NULL, NULL) > 0;
}

static int is_default(const char *name)
{
    return name == NULL || name[0] == '\0' || strcmp(name, "default") == 0;
}

static int find_in_device(const char *name, UINT *id)
{
    UINT n = waveInGetNumDevs();
    UINT i;

    if (is_default(name)) {
        *id = WAVE_MAPPER;
        return RS_OK;
    }
    for (i = 0; i < n; i++) {
        char dev[128];
        if (in_name(i, dev, sizeof(dev)) && contains_nocase(dev, name)) {
            *id = i;
            return RS_OK;
        }
    }
    return RS_ERR_NOT_FOUND;
}

static int find_out_device(const char *name, UINT *id)
{
    UINT n = waveOutGetNumDevs();
    UINT i;

    if (is_default(name)) {
        *id = WAVE_MAPPER;
        return RS_OK;
    }
    for (i = 0; i < n; i++) {
        char dev[128];
        if (out_name(i, dev, sizeof(dev)) && contains_nocase(dev, name)) {
            *id = i;
            return RS_OK;
        }
    }
    return RS_ERR_NOT_FOUND;
}

static void winmm_close(void *st)
{
    winmm_dev_t *d = st;
    uint32_t i;

    if (d->hin != NULL) {
        waveInReset(d->hin);
        waveInStop(d->hin);
        for (i = 0; i < d->in_n; i++)
            if (flags_of(&d->in_hdr[i]) & WHDR_PREPARED)
                waveInUnprepareHeader(d->hin, &d->in_hdr[i], sizeof(WAVEHDR));
        waveInClose(d->hin);
    }
    if (d->hout != NULL) {
        waveOutReset(d->hout);
        for (i = 0; i < d->out_n; i++)
            if (flags_of(&d->out_hdr[i]) & WHDR_PREPARED)
                waveOutUnprepareHeader(d->hout, &d->out_hdr[i], sizeof(WAVEHDR));
        waveOutClose(d->hout);
    }
    free(d->in_hdr);
    free(d->in_mem);
    free(d->out_hdr);
    free(d->out_mem);
    free(d);
}

static int winmm_read(void *st, int16_t *pcm, size_t max, size_t *got)
{
    winmm_dev_t *d = st;
    size_t total = 0;

    while (total < max) {
        WAVEHDR *h = &d->in_hdr[d->in_next];
        size_t recorded;
        size_t n;

        if (!(flags_of(h) & WHDR_DONE))
            break;
        recorded = h->dwBytesRecorded / sizeof(int16_t);
        n = recorded - d->in_pos;
        if (n > max - total)
            n = max - total;
        memcpy(pcm + total, (const int16_t *)(const void *)h->lpData + d->in_pos,
               n * sizeof(int16_t));
        total += n;
        d->in_pos += n;
        if (d->in_pos >= recorded) {
            h->dwFlags &= ~(DWORD)WHDR_DONE;
            if (waveInAddBuffer(d->hin, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                *got = total;
                return RS_ERR_IO;
            }
            d->in_next = (d->in_next + 1) % d->in_n;
            d->in_pos = 0;
        }
    }
    *got = total;
    return RS_OK;
}

static size_t winmm_write_avail(void *st)
{
    winmm_dev_t *d = st;
    uint32_t free_hdr = 0;
    uint32_t i;

    for (i = 0; i < d->out_n; i++) {
        const WAVEHDR *h = &d->out_hdr[(d->out_next + i) % d->out_n];
        if (flags_of(h) & WHDR_INQUEUE)
            break;
        free_hdr++;
    }
    return (size_t)free_hdr * d->frame;
}

static int winmm_write(void *st, const int16_t *pcm, size_t samples)
{
    winmm_dev_t *d = st;

    if (samples % d->frame != 0 || samples > winmm_write_avail(d))
        return RS_ERR_INVALID_ARG;
    while (samples > 0) {
        WAVEHDR *h = &d->out_hdr[d->out_next];
        memcpy(h->lpData, pcm, d->frame * sizeof(int16_t));
        h->dwFlags &= ~(DWORD)WHDR_DONE;
        if (waveOutWrite(d->hout, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            return RS_ERR_IO;
        d->out_next = (d->out_next + 1) % d->out_n;
        pcm += d->frame;
        samples -= d->frame;
    }
    return RS_OK;
}

static const audio_dev_ops_t k_winmm_ops = {
    winmm_read, winmm_write, winmm_write_avail, winmm_close
};

int audio_dev_open_winmm(audio_dev_t *dev, const audio_dev_config_t *cfg)
{
    winmm_dev_t *d;
    WAVEFORMATEX fmt;
    UINT in_id = WAVE_MAPPER;
    UINT out_id = WAVE_MAPPER;
    uint32_t i;
    int rc;

    rc = find_in_device(cfg->capture, &in_id);
    if (rc == RS_OK)
        rc = find_out_device(cfg->playback, &out_id);
    if (rc != RS_OK)
        return rc;

    d = calloc(1, sizeof(*d));
    if (d == NULL)
        return RS_ERR_NO_MEMORY;
    d->frame = cfg->frame_samples;
    d->in_n = cfg->queue_frames * 2 < 8 ? 8 : cfg->queue_frames * 2;
    d->out_n = cfg->queue_frames;
    d->in_hdr = calloc(d->in_n, sizeof(WAVEHDR));
    d->in_mem = calloc((size_t)d->in_n * d->frame, sizeof(int16_t));
    d->out_hdr = calloc(d->out_n, sizeof(WAVEHDR));
    d->out_mem = calloc((size_t)d->out_n * d->frame, sizeof(int16_t));
    if (d->in_hdr == NULL || d->in_mem == NULL || d->out_hdr == NULL || d->out_mem == NULL) {
        winmm_close(d);
        return RS_ERR_NO_MEMORY;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = cfg->sample_rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = cfg->sample_rate * 2;

    rc = mm_status(waveInOpen(&d->hin, in_id, &fmt, 0, 0, CALLBACK_NULL));
    if (rc != RS_OK) {
        d->hin = NULL;
        winmm_close(d);
        return rc;
    }
    for (i = 0; i < d->in_n; i++) {
        WAVEHDR *h = &d->in_hdr[i];
        h->lpData = (LPSTR)(void *)(d->in_mem + (size_t)i * d->frame);
        h->dwBufferLength = d->frame * sizeof(int16_t);
        if (waveInPrepareHeader(d->hin, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
            waveInAddBuffer(d->hin, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            winmm_close(d);
            return RS_ERR_SYSTEM;
        }
    }

    rc = mm_status(waveOutOpen(&d->hout, out_id, &fmt, 0, 0, CALLBACK_NULL));
    if (rc != RS_OK) {
        d->hout = NULL;
        winmm_close(d);
        return rc;
    }
    for (i = 0; i < d->out_n; i++) {
        WAVEHDR *h = &d->out_hdr[i];
        h->lpData = (LPSTR)(void *)(d->out_mem + (size_t)i * d->frame);
        h->dwBufferLength = d->frame * sizeof(int16_t);
        if (waveOutPrepareHeader(d->hout, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            winmm_close(d);
            return RS_ERR_SYSTEM;
        }
    }

    if (waveInStart(d->hin) != MMSYSERR_NOERROR) {
        winmm_close(d);
        return RS_ERR_SYSTEM;
    }

    dev->ops = &k_winmm_ops;
    dev->st = d;
    return RS_OK;
}

void audio_dev_list_winmm(audio_dev_list_cb cb, void *user)
{
    UINT n;
    UINT i;

    char name[128];

    n = waveInGetNumDevs();
    for (i = 0; i < n; i++)
        if (in_name(i, name, sizeof(name)))
            cb("winmm", 1, name, user);
    n = waveOutGetNumDevs();
    for (i = 0; i < n; i++)
        if (out_name(i, name, sizeof(name)))
            cb("winmm", 0, name, user);
}

#else /* !_WIN32 */

int audio_dev_open_winmm(audio_dev_t *dev, const audio_dev_config_t *cfg)
{
    (void)dev;
    (void)cfg;
    return RS_ERR_UNSUPPORTED;
}

void audio_dev_list_winmm(audio_dev_list_cb cb, void *user)
{
    (void)cb;
    (void)user;
}

#endif
