/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "audio_link.h"

#include <stdlib.h>
#include <string.h>

#include "net_socket.h"
#include "rs_crypto.h"
#include "rs_error.h"

#define MAX_FRAMES_PER_POLL 16
#define BURST_HISTORY       64   /* 再生側の取り出しの塊を記録する回数（約 0.6〜2 秒分） */

struct audio_link {
    audio_link_config_t cfg;
    audio_dev_t        *dev;
    audio_codec_t       codec;
    jbuf_t             *jb;
    dch_t               ch;
    int               (*send)(void *user, const void *pkt, size_t len);
    void               *user;
    int16_t            *cap_buf;
    size_t              cap_fill;
    int16_t            *play_buf;
    int16_t            *dec_buf;
    uint32_t            tx_ts;
    uint8_t             burst_hist[BURST_HISTORY];
    size_t              burst_idx;
    uint32_t            burst_peak;
    audio_link_stats_t  stats;
};

void audio_link_config_default(audio_link_config_t *cfg)
{
    if (cfg == NULL)
        return;
    cfg->codec = AUDIO_CODEC_PCM16;
    cfg->sample_rate = 48000;
    cfg->frame_samples = 480;
    cfg->jitter_min_ms = 20;
    cfg->jitter_max_ms = 80;
}

int audio_link_create(audio_link_t **out, const audio_link_config_t *cfg,
                      int (*send)(void *user, const void *pkt, size_t len), void *user)
{
    audio_link_t *link;
    jbuf_config_t jcfg;
    int rc;

    if (out == NULL || cfg == NULL || send == NULL || cfg->frame_samples == 0)
        return RS_ERR_INVALID_ARG;
    *out = NULL;
    if (cfg->codec == AUDIO_CODEC_PCM16 &&
        AUDIO_PAYLOAD_HEADER_LEN + (size_t)cfg->frame_samples * 2 > DCH_MAX_PAYLOAD)
        return RS_ERR_TOO_LARGE;

    link = calloc(1, sizeof(*link));
    if (link == NULL)
        return RS_ERR_NO_MEMORY;
    link->cfg = *cfg;
    link->send = send;
    link->user = user;

    rc = audio_codec_open(&link->codec, cfg->codec, cfg->sample_rate, cfg->frame_samples);
    if (rc != RS_OK) {
        free(link);
        return rc;
    }

    jbuf_config_default(&jcfg);
    jcfg.sample_rate = cfg->sample_rate;
    jcfg.frame_samples = cfg->frame_samples;
    jcfg.min_delay_ms = cfg->jitter_min_ms;
    jcfg.max_delay_ms = cfg->jitter_max_ms;
    {
        /* 最大遅延の 2 倍 + 余裕を収める */
        uint32_t frame_ms = cfg->frame_samples * 1000u / cfg->sample_rate;
        uint32_t need = frame_ms > 0 ? (cfg->jitter_max_ms * 2) / frame_ms + 8 : 64;
        jcfg.capacity_frames = need < 32 ? 32 : need;
    }
    rc = jbuf_create(&link->jb, &jcfg);

    link->cap_buf = calloc(cfg->frame_samples, sizeof(int16_t));
    link->play_buf = calloc(cfg->frame_samples, sizeof(int16_t));
    link->dec_buf = calloc(cfg->frame_samples, sizeof(int16_t));
    if (rc == RS_OK && (link->cap_buf == NULL || link->play_buf == NULL || link->dec_buf == NULL))
        rc = RS_ERR_NO_MEMORY;
    if (rc != RS_OK) {
        audio_link_destroy(link);
        return rc;
    }
    *out = link;
    return RS_OK;
}

void audio_link_destroy(audio_link_t *link)
{
    if (link == NULL)
        return;
    dch_reset(&link->ch);
    audio_codec_close(&link->codec);
    jbuf_destroy(link->jb);
    free(link->cap_buf);
    free(link->play_buf);
    free(link->dec_buf);
    free(link);
}

void audio_link_attach(audio_link_t *link, audio_dev_t *dev)
{
    link->dev = dev;
    link->cap_fill = 0;
}

void audio_link_start(audio_link_t *link, dch_role_t role, uint32_t session_id,
                      const uint8_t k_c2s[CTL_KEY_LEN], const uint8_t k_s2c[CTL_KEY_LEN])
{
    dch_init(&link->ch, DCH_CHANNEL_AUDIO, role, session_id, k_c2s, k_s2c);
    jbuf_reset(link->jb);
    link->cap_fill = 0;
    /* RTP と同様、タイムスタンプの初期値は推測できない値にする（フレーム境界に揃える） */
    if (rs_crypto_random(&link->tx_ts, sizeof(link->tx_ts)) != RS_OK)
        link->tx_ts = 0;
    link->tx_ts -= link->tx_ts % link->cfg.frame_samples;
}

void audio_link_stop(audio_link_t *link)
{
    dch_reset(&link->ch);
    jbuf_reset(link->jb);
    link->cap_fill = 0;
}

int audio_link_active(const audio_link_t *link)
{
    return link->ch.active;
}

static void send_frame(audio_link_t *link, const int16_t *pcm)
{
    uint8_t payload[DCH_MAX_PAYLOAD];
    uint8_t pkt[NET_MAX_UDP_PAYLOAD];
    size_t enc_len = 0;
    size_t len;

    payload[0] = (uint8_t)link->codec.id;
    payload[1] = 1; /* チャンネル数 */
    payload[2] = (uint8_t)(link->cfg.frame_samples >> 8);
    payload[3] = (uint8_t)link->cfg.frame_samples;
    ctl_put_u32(payload + 4, link->tx_ts);
    link->tx_ts += link->cfg.frame_samples;

    if (audio_codec_encode(&link->codec, pcm, link->cfg.frame_samples,
                           payload + AUDIO_PAYLOAD_HEADER_LEN,
                           sizeof(payload) - AUDIO_PAYLOAD_HEADER_LEN, &enc_len) != RS_OK)
        return;
    if (dch_seal(&link->ch, DCH_TYPE_AUDIO, payload, AUDIO_PAYLOAD_HEADER_LEN + enc_len,
                 pkt, sizeof(pkt), &len) != RS_OK)
        return;
    if (link->send(link->user, pkt, len) == RS_OK)
        link->stats.frames_sent++;
}

/* デバイスが一度に要求したフレーム数の直近最大値を、ジッターバッファの追加余裕にする。
 * 例: 再生デバイスが 3 フレーム分の空きをまとめて返すなら、その瞬間に 30ms 分が一度に
 * 取り出されるため、到着の揺らぎとは別に 20ms（2 フレーム分）の余裕が要る */
static void note_playout_burst(audio_link_t *link, uint32_t frames)
{
    uint32_t peak = 0;
    size_t i;

    link->burst_hist[link->burst_idx] = (uint8_t)frames;
    link->burst_idx = (link->burst_idx + 1) % BURST_HISTORY;
    for (i = 0; i < BURST_HISTORY; i++)
        if (link->burst_hist[i] > peak)
            peak = link->burst_hist[i];
    if (peak != link->burst_peak) {
        link->burst_peak = peak;
        jbuf_set_extra_delay(link->jb, (peak - 1) * link->cfg.frame_samples * 1000u /
                                           link->cfg.sample_rate);
    }
}

int audio_link_poll(audio_link_t *link)
{
    size_t frame = link->cfg.frame_samples;
    int frames;

    if (link->dev == NULL)
        return RS_OK;

    /* 録音 → 送信 */
    for (frames = 0; frames < MAX_FRAMES_PER_POLL; ) {
        size_t got = 0;
        int rc = audio_dev_read(link->dev, link->cap_buf + link->cap_fill,
                                frame - link->cap_fill, &got);
        if (rc != RS_OK) {
            link->stats.device_errors++;
            return RS_ERR_IO;
        }
        if (got == 0)
            break;
        link->cap_fill += got;
        if (link->cap_fill == frame) {
            if (link->ch.active)
                send_frame(link, link->cap_buf);
            link->cap_fill = 0;
            frames++;
        }
    }

    /* ジッターバッファ → 再生（セッションがなくても無音を供給してデバイスを止めない） */
    for (frames = 0; frames < MAX_FRAMES_PER_POLL && audio_dev_write_avail(link->dev) >= frame;
         frames++) {
        jbuf_get(link->jb, link->play_buf, frame);
        if (audio_dev_write(link->dev, link->play_buf, frame) != RS_OK) {
            link->stats.device_errors++;
            return RS_ERR_IO;
        }
    }
    /* バッファリング中（無音出力・起動時のキュー充填）の塊は再生の揺らぎではないので数えない */
    if (frames > 0 && jbuf_state(link->jb) == JBUF_PLAYING)
        note_playout_burst(link, (uint32_t)frames);
    return RS_OK;
}

int audio_link_handle_packet(audio_link_t *link, const uint8_t *pkt, size_t len,
                             uint64_t now_ms, uint8_t *type)
{
    uint8_t payload[DCH_MAX_PAYLOAD];
    size_t plen = 0;
    size_t samples = 0;
    uint8_t t = 0;
    int rc;

    rc = dch_open(&link->ch, pkt, len, &t, payload, sizeof(payload), &plen);
    if (rc != RS_OK)
        return rc;
    if (type != NULL)
        *type = t;
    if (t != DCH_TYPE_AUDIO)
        return RS_OK;

    if (plen < AUDIO_PAYLOAD_HEADER_LEN || payload[0] != (uint8_t)link->codec.id ||
        payload[1] != 1 || ((uint32_t)payload[2] << 8 | payload[3]) != link->cfg.frame_samples) {
        link->stats.rejected_format++;
        return RS_OK;
    }
    if (audio_codec_decode(&link->codec, payload + AUDIO_PAYLOAD_HEADER_LEN,
                           plen - AUDIO_PAYLOAD_HEADER_LEN, link->dec_buf,
                           link->cfg.frame_samples, &samples) != RS_OK ||
        samples != link->cfg.frame_samples) {
        link->stats.decode_errors++;
        return RS_OK;
    }
    if (jbuf_put(link->jb, ctl_get_u32(payload + 4), link->dec_buf, samples, now_ms) == RS_OK)
        link->stats.frames_received++;
    else
        link->stats.rejected_format++;
    return RS_OK;
}

int audio_link_send_bind(audio_link_t *link)
{
    uint8_t pkt[DCH_OVERHEAD];
    size_t len;
    int rc = dch_seal(&link->ch, DCH_TYPE_BIND, NULL, 0, pkt, sizeof(pkt), &len);
    if (rc != RS_OK)
        return rc;
    return link->send(link->user, pkt, len);
}

void audio_link_get_stats(const audio_link_t *link, audio_link_stats_t *stats,
                          jbuf_stats_t *jstats)
{
    if (link == NULL)
        return;
    if (stats != NULL)
        *stats = link->stats;
    if (jstats != NULL)
        jbuf_get_stats(link->jb, jstats);
}
