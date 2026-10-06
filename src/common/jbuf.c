/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "jbuf.h"

#include <stdlib.h>
#include <string.h>

#include "rs_error.h"

#define TRANSIT_WINDOW   256       /* ジッター推計に使う直近の到着数（10ms フレームで約 2.5 秒） */
#define FX_ONE           65536     /* 16.16 固定小数点の 1.0 */
#define ADJUST_PERCENT   2         /* 微調整の速度差（目標からのずれが 1 フレーム以内） */
#define ADJUST_FAST_PERCENT 5      /* 同（ずれが 1 フレーム超: 急変後の早期回復） */
#define LEVEL_SMOOTHING  64        /* バッファ量の指数平滑化係数 (1/N)。揺らぎによる追従のふらつきを抑える */
#define LOW_DEADBAND_MS  2.0       /* 目標を下回った時に遅め再生を始めるまでの不感帯 */
#define TARGET_DECAY_MS  0.25      /* 1 フレーム再生あたりの目標遅延の減少量 */

typedef struct slot {
    int64_t fi;         /* フレーム番号（base からのフレーム数） */
    int     used;
    int     concealed;
} slot_t;

struct jbuf {
    jbuf_config_t cfg;
    double        frame_ms;
    slot_t       *slots;
    int16_t      *pcm;          /* capacity × frame_samples */
    int16_t      *last_good;    /* 補間元（直前に再生した実フレーム） */
    int           have_last_good;
    int           conceal_run;
    int64_t       last_accessed_fi;

    /* タイムスタンプの 64 ビット拡張 */
    int           have_ref;
    uint32_t      ref_ts;
    int64_t       ref_ext;
    int64_t       base;         /* フレーム境界の基準（最初のフレームの拡張タイムスタンプ） */

    int           have_newest;
    int64_t       newest_fi;

    jbuf_state_t  state;
    int64_t       play_fx;      /* 再生位置（拡張サンプル時刻, 16.16） */
    int64_t       drop_fx;
    int64_t       insert_fx;

    double        transit[TRANSIT_WINDOW];
    size_t        transit_n;
    size_t        transit_idx;
    double        jitter_ms;
    double        extra_ms;
    double        target_ms;
    double        level_avg_ms;
    double        level_ms;

    jbuf_stats_t  stats;
};

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

static size_t slot_index(const jbuf_t *jb, int64_t fi)
{
    int64_t cap = jb->cfg.capacity_frames;
    int64_t m = fi % cap;
    return (size_t)(m < 0 ? m + cap : m);
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void jbuf_config_default(jbuf_config_t *cfg)
{
    if (cfg == NULL)
        return;
    cfg->sample_rate = 48000;
    cfg->frame_samples = 480;
    cfg->min_delay_ms = 20;
    cfg->max_delay_ms = 80;
    cfg->capacity_frames = 32;
}

int jbuf_create(jbuf_t **out, const jbuf_config_t *cfg)
{
    jbuf_t *jb;
    double frame_ms;

    if (out == NULL || cfg == NULL || cfg->sample_rate == 0 || cfg->frame_samples == 0 ||
        cfg->min_delay_ms > cfg->max_delay_ms)
        return RS_ERR_INVALID_ARG;
    *out = NULL;
    frame_ms = (double)cfg->frame_samples * 1000.0 / cfg->sample_rate;
    /* 最大遅延 + 読み飛ばし判定の余裕 2 フレーム + 先行到着分を収められること */
    if ((double)cfg->capacity_frames * frame_ms < cfg->max_delay_ms + 4 * frame_ms)
        return RS_ERR_INVALID_ARG;

    jb = calloc(1, sizeof(*jb));
    if (jb == NULL)
        return RS_ERR_NO_MEMORY;
    jb->cfg = *cfg;
    jb->frame_ms = frame_ms;
    jb->slots = calloc(cfg->capacity_frames, sizeof(slot_t));
    jb->pcm = calloc((size_t)cfg->capacity_frames * cfg->frame_samples, sizeof(int16_t));
    jb->last_good = calloc(cfg->frame_samples, sizeof(int16_t));
    if (jb->slots == NULL || jb->pcm == NULL || jb->last_good == NULL) {
        jbuf_destroy(jb);
        return RS_ERR_NO_MEMORY;
    }
    jbuf_reset(jb);
    jb->stats.resets = 0;
    *out = jb;
    return RS_OK;
}

void jbuf_destroy(jbuf_t *jb)
{
    if (jb == NULL)
        return;
    free(jb->slots);
    free(jb->pcm);
    free(jb->last_good);
    free(jb);
}

static void clear_slots(jbuf_t *jb)
{
    memset(jb->slots, 0, sizeof(slot_t) * jb->cfg.capacity_frames);
    jb->have_newest = 0;
}

void jbuf_reset(jbuf_t *jb)
{
    if (jb == NULL)
        return;
    clear_slots(jb);
    jb->have_ref = 0;
    jb->have_last_good = 0;
    jb->conceal_run = 0;
    jb->last_accessed_fi = INT64_MIN;
    jb->state = JBUF_BUFFERING;
    jb->play_fx = 0;
    jb->transit_n = 0;
    jb->transit_idx = 0;
    jb->jitter_ms = 0;
    jb->target_ms = jb->cfg.min_delay_ms;
    jb->level_avg_ms = 0;
    jb->level_ms = 0;
    jb->stats.resets++;
}

/* ---- 投入 ---------------------------------------------------------------- */

static int64_t extend_ts(jbuf_t *jb, uint32_t ts)
{
    int64_t ext;

    if (!jb->have_ref) {
        jb->have_ref = 1;
        jb->ref_ts = ts;
        jb->ref_ext = (int64_t)ts + ((int64_t)1 << 32); /* 負方向の遅着も正の値で扱う */
        jb->base = jb->ref_ext;
        return jb->ref_ext;
    }
    ext = jb->ref_ext + (int32_t)(ts - jb->ref_ts);
    if (ext > jb->ref_ext) {
        jb->ref_ext = ext;
        jb->ref_ts = ts;
    }
    return ext;
}

static void update_jitter(jbuf_t *jb, int64_t ext, uint64_t arrival_ms)
{
    double media_ms = (double)ext * 1000.0 / jb->cfg.sample_rate;
    double mn;
    double mx;
    double desired;
    size_t i;

    jb->transit[jb->transit_idx] = (double)arrival_ms - media_ms;
    jb->transit_idx = (jb->transit_idx + 1) % TRANSIT_WINDOW;
    if (jb->transit_n < TRANSIT_WINDOW)
        jb->transit_n++;

    mn = mx = jb->transit[0];
    for (i = 1; i < jb->transit_n; i++) {
        if (jb->transit[i] < mn)
            mn = jb->transit[i];
        if (jb->transit[i] > mx)
            mx = jb->transit[i];
    }
    jb->jitter_ms = mx - mn;

    desired = clampd(jb->jitter_ms + jb->frame_ms + jb->extra_ms, jb->cfg.min_delay_ms,
                     jb->cfg.max_delay_ms);
    if (desired > jb->target_ms)
        jb->target_ms = desired; /* 揺らぎの増加には即応する */
}

static double desired_target(const jbuf_t *jb)
{
    return clampd(jb->jitter_ms + jb->frame_ms + jb->extra_ms, jb->cfg.min_delay_ms,
                  jb->cfg.max_delay_ms);
}

static int64_t play_sample(const jbuf_t *jb)
{
    return jb->play_fx >> 16;
}

static int64_t play_frame(const jbuf_t *jb)
{
    return floor_div(play_sample(jb) - jb->base, jb->cfg.frame_samples);
}

int jbuf_put(jbuf_t *jb, uint32_t timestamp, const int16_t *pcm, size_t samples,
             uint64_t arrival_ms)
{
    int64_t ext;
    int64_t rel;
    int64_t fi;
    slot_t *s;
    size_t idx;

    if (jb == NULL || pcm == NULL || samples != jb->cfg.frame_samples)
        return RS_ERR_INVALID_ARG;

    ext = extend_ts(jb, timestamp);
    rel = ext - jb->base;
    if (rel % (int64_t)jb->cfg.frame_samples != 0)
        return RS_ERR_INVALID_ARG;
    fi = rel / (int64_t)jb->cfg.frame_samples;

    update_jitter(jb, ext, arrival_ms);

    if (jb->state == JBUF_PLAYING) {
        int64_t pf = play_frame(jb);
        if (fi < pf) {
            jb->stats.frames_late++;
            return RS_OK;
        }
        if (fi >= pf + (int64_t)jb->cfg.capacity_frames) {
            /* 送信側の時刻が大きく飛んだ（送信再開など）。内容を捨てて取り直す */
            clear_slots(jb);
            jb->state = JBUF_BUFFERING;
            jb->stats.resets++;
        }
    }
    if (jb->have_newest && jb->newest_fi - fi >= (int64_t)jb->cfg.capacity_frames) {
        jb->stats.frames_late++;
        return RS_OK;
    }

    idx = slot_index(jb, fi);
    s = &jb->slots[idx];
    if (s->used && s->fi == fi) {
        if (s->concealed)
            jb->stats.frames_late++;   /* 補間済みの位置に遅れて届いた */
        else
            jb->stats.frames_duplicate++;
        return RS_OK;
    }

    s->fi = fi;
    s->used = 1;
    s->concealed = 0;
    memcpy(jb->pcm + idx * jb->cfg.frame_samples, pcm, samples * sizeof(int16_t));
    if (!jb->have_newest || fi > jb->newest_fi) {
        jb->newest_fi = fi;
        jb->have_newest = 1;
    }
    jb->stats.frames_received++;
    return RS_OK;
}

/* ---- 取り出し ------------------------------------------------------------ */

/* フレーム fi の PCM を返す。欠落していれば直前の実フレームを減衰させて補間する */
static const int16_t *frame_pcm(jbuf_t *jb, int64_t fi)
{
    size_t idx = slot_index(jb, fi);
    slot_t *s = &jb->slots[idx];
    int16_t *p = jb->pcm + idx * jb->cfg.frame_samples;
    int first_access = fi != jb->last_accessed_fi;
    size_t i;

    if (s->used && s->fi == fi) {
        if (first_access && !s->concealed) {
            memcpy(jb->last_good, p, jb->cfg.frame_samples * sizeof(int16_t));
            jb->have_last_good = 1;
            jb->conceal_run = 0;
        }
        jb->last_accessed_fi = fi;
        return p;
    }

    /* 欠落: 1 回目 70%、2 回目 35%、以降は無音 */
    jb->conceal_run++;
    for (i = 0; i < jb->cfg.frame_samples; i++) {
        int32_t v = jb->have_last_good ? jb->last_good[i] : 0;
        p[i] = (int16_t)(jb->conceal_run == 1 ? v * 7 / 10 : jb->conceal_run == 2 ? v * 7 / 20 : 0);
    }
    s->fi = fi;
    s->used = 1;
    s->concealed = 1;
    jb->last_accessed_fi = fi;
    jb->stats.frames_concealed++;
    return p;
}

/* 実フレーム（補間でない）が既にあるか。補間用の先読みで欠落補間を作らないために使う */
static int frame_present(const jbuf_t *jb, int64_t sample)
{
    int64_t fi = floor_div(sample - jb->base, jb->cfg.frame_samples);
    const slot_t *s = &jb->slots[slot_index(jb, fi)];
    return s->used && s->fi == fi;
}

static int16_t sample_at(jbuf_t *jb, int64_t sample)
{
    int64_t rel = sample - jb->base;
    int64_t fi = floor_div(rel, jb->cfg.frame_samples);
    int64_t off = rel - fi * (int64_t)jb->cfg.frame_samples;
    return frame_pcm(jb, fi)[off];
}

static int find_oldest(const jbuf_t *jb, int64_t *oldest)
{
    int found = 0;
    size_t i;

    for (i = 0; i < jb->cfg.capacity_frames; i++) {
        const slot_t *s = &jb->slots[i];
        if (s->used && !s->concealed && (!found || s->fi < *oldest)) {
            *oldest = s->fi;
            found = 1;
        }
    }
    return found;
}

static void release_before(jbuf_t *jb, int64_t fi)
{
    size_t i;
    for (i = 0; i < jb->cfg.capacity_frames; i++)
        if (jb->slots[i].used && jb->slots[i].fi < fi)
            jb->slots[i].used = 0;
}

static double level_now_ms(const jbuf_t *jb)
{
    int64_t end_fx = (jb->base + (jb->newest_fi + 1) * (int64_t)jb->cfg.frame_samples) << 16;
    return (double)(end_fx - jb->play_fx) / FX_ONE * 1000.0 / jb->cfg.sample_rate;
}

int jbuf_get(jbuf_t *jb, int16_t *out, size_t samples)
{
    int64_t step_fx = FX_ONE;
    int64_t consumed_fx;
    double target;
    double half_frame;
    size_t i;

    if (jb == NULL || out == NULL)
        return RS_ERR_INVALID_ARG;

    target = jb->target_ms;
    half_frame = jb->frame_ms / 2;

    if (jb->state == JBUF_BUFFERING) {
        int64_t oldest = 0;
        if (jb->have_newest && find_oldest(jb, &oldest) &&
            (double)(jb->newest_fi - oldest + 1) * jb->frame_ms >= target) {
            jb->state = JBUF_PLAYING;
            jb->play_fx = (jb->base + oldest * (int64_t)jb->cfg.frame_samples) << 16;
            jb->level_avg_ms = level_now_ms(jb);
            jb->conceal_run = 0;
        } else {
            memset(out, 0, samples * sizeof(int16_t));
            jb->level_ms = 0;
            return RS_OK;
        }
    }

    jb->level_ms = level_now_ms(jb);
    if (jb->level_ms <= 0) {
        /* データが尽きた: 再バッファリング */
        jb->stats.underruns++;
        clear_slots(jb);
        jb->state = JBUF_BUFFERING;
        memset(out, 0, samples * sizeof(int16_t));
        jb->level_ms = 0;
        return RS_OK;
    }

    /* オーバーラン: 最大遅延を大きく超えたら目標まで一気に読み飛ばす */
    if (jb->level_ms > jb->cfg.max_delay_ms + jb->frame_ms) {
        int64_t skip = (int64_t)((jb->level_ms - target) / jb->frame_ms);
        if (skip > 0) {
            jb->play_fx += (skip * (int64_t)jb->cfg.frame_samples) << 16;
            jb->stats.frames_skipped += (uint64_t)skip;
            jb->level_ms = level_now_ms(jb);
            jb->level_avg_ms = jb->level_ms;
        }
    }

    jb->level_avg_ms += (jb->level_ms - jb->level_avg_ms) / LEVEL_SMOOTHING;

    /* 再生ポインタの微調整。平滑値と瞬時値の両方が同じ向きにずれている時だけ動かし、
     * 平滑値の遅れで枯渇寸前に速め再生してしまうことを防ぐ。残量 1 フレーム未満では常に遅め再生 */
    {
        double dev = jb->level_avg_ms - target;
        int64_t pct = (dev > jb->frame_ms || dev < -jb->frame_ms) ? ADJUST_FAST_PERCENT
                                                                   : ADJUST_PERCENT;
        /* 不感帯は非対称: 不足（アンダーランの危険）には早めに寄せ、過剰は半フレームまで許す */
        if (dev > half_frame && jb->level_ms > target)
            step_fx = FX_ONE + FX_ONE * pct / 100;
        else if ((dev < -LOW_DEADBAND_MS && jb->level_ms < target) || jb->level_ms < jb->frame_ms)
            step_fx = FX_ONE - FX_ONE * pct / 100;
    }

    /* 調整していない間は再生位置を整数サンプルに戻す（1 サンプル未満の移動で聴感上の影響はない）。
     * 小数位置のままだと以後の全サンプルが線形補間になり、高域がわずかに落ち続けるため */
    if (step_fx == FX_ONE)
        jb->play_fx = (jb->play_fx + FX_ONE / 2) & ~(int64_t)(FX_ONE - 1);

    for (i = 0; i < samples; i++) {
        int64_t pos = jb->play_fx + (int64_t)i * step_fx;
        int64_t s = pos >> 16;
        int32_t frac = (int32_t)(pos & 0xFFFF);
        int32_t v0 = sample_at(jb, s);
        /* 次のサンプルが未着のフレームにかかる場合は補間しない（到着前に欠落扱いしない） */
        if (frac != 0 && frame_present(jb, s + 1)) {
            int32_t v1 = sample_at(jb, s + 1);
            v0 += (int32_t)(((int64_t)(v1 - v0) * frac) >> 16);
        }
        out[i] = (int16_t)v0;
    }

    consumed_fx = (int64_t)samples * step_fx;
    jb->play_fx += consumed_fx;
    if (step_fx > FX_ONE)
        jb->drop_fx += consumed_fx - ((int64_t)samples << 16);
    else if (step_fx < FX_ONE)
        jb->insert_fx += ((int64_t)samples << 16) - consumed_fx;
    jb->stats.samples_dropped = (uint64_t)(jb->drop_fx >> 16);
    jb->stats.samples_inserted = (uint64_t)(jb->insert_fx >> 16);

    release_before(jb, play_frame(jb));

    /* 揺らぎが収まったら目標遅延を緩やかに下げる */
    {
        double desired = desired_target(jb);
        double decay = TARGET_DECAY_MS * (double)samples / jb->cfg.frame_samples;
        if (jb->target_ms - decay > desired)
            jb->target_ms -= decay;
        else if (jb->target_ms > desired)
            jb->target_ms = desired;
    }
    return RS_OK;
}

jbuf_state_t jbuf_state(const jbuf_t *jb)
{
    return jb != NULL ? jb->state : JBUF_BUFFERING;
}

void jbuf_set_extra_delay(jbuf_t *jb, uint32_t extra_ms)
{
    if (jb == NULL)
        return;
    jb->extra_ms = extra_ms;
    if (desired_target(jb) > jb->target_ms)
        jb->target_ms = desired_target(jb);
}

void jbuf_get_stats(const jbuf_t *jb, jbuf_stats_t *stats)
{
    if (jb == NULL || stats == NULL)
        return;
    *stats = jb->stats;
    stats->target_ms = (uint32_t)(jb->target_ms + 0.5);
    stats->level_ms = (uint32_t)(jb->level_ms > 0 ? jb->level_ms + 0.5 : 0);
    stats->jitter_ms = (uint32_t)(jb->jitter_ms + 0.5);
    stats->extra_ms = (uint32_t)(jb->extra_ms + 0.5);
    stats->state = jb->state;
}
