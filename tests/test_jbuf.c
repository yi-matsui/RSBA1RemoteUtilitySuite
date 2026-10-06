/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 適応型ジッターバッファのテスト。仮想時計と決定的な乱数で到着パターンを再現する。
 * 各フレームはフレーム番号から決まる一定値で埋め、出力値からどのフレームかを判別する。
 */
#include <stdlib.h>
#include <string.h>

#include "jbuf.h"
#include "rs_error.h"
#include "test_util.h"

#define FRAME   480
#define TS0     0xFFFFF000u   /* 開始直後に 32 ビット周回させる */

static uint32_t g_rng = 12345;

static uint32_t rnd(uint32_t n)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 16) % n;
}

static int16_t frame_value(int64_t n)
{
    return (int16_t)(100 + (n % 300) * 100);
}

static void make_frame(int64_t n, int16_t *buf)
{
    int i;
    for (i = 0; i < FRAME; i++)
        buf[i] = frame_value(n);
}

static uint32_t ts_of(int64_t n)
{
    return TS0 + (uint32_t)(n * FRAME);
}

static jbuf_t *make_jb(void)
{
    jbuf_config_t cfg;
    jbuf_t *jb = NULL;
    jbuf_config_default(&cfg);
    CHECK_RC(jbuf_create(&jb, &cfg), RS_OK);
    return jb;
}

static void put_frame(jbuf_t *jb, int64_t n, uint64_t now)
{
    int16_t buf[FRAME];
    make_frame(n, buf);
    CHECK_RC(jbuf_put(jb, ts_of(n), buf, FRAME, now), RS_OK);
}

/* 等間隔・揺らぎなし: 目標 20ms で開始し、欠落・調整なしで順に再生される */
static void test_steady(void)
{
    jbuf_t *jb = make_jb();
    jbuf_stats_t st;
    int16_t out[FRAME];
    uint64_t now = 1000;
    int64_t expect = -1;
    int ok_seq = 1;
    int t;

    for (t = 0; t < 500; t++) {
        now += 10;
        put_frame(jb, t, now);
        jbuf_get(jb, out, FRAME);
        if (t == 0)
            CHECK(out[0] == 0); /* バッファリング中は無音 */
        if (out[0] != 0) {
            if (expect < 0)
                expect = 0;
            if (out[0] != frame_value(expect) || out[FRAME - 1] != frame_value(expect))
                ok_seq = 0;
            expect++;
        }
    }
    jbuf_get_stats(jb, &st);
    CHECK(ok_seq);
    CHECK(expect == 499); /* 開始は 2 フレーム目投入時（20ms 分たまった時点） */
    CHECK(st.state == JBUF_PLAYING);
    CHECK(st.target_ms == 20);
    CHECK(st.level_ms == 20);
    CHECK(st.jitter_ms == 0);
    CHECK(st.underruns == 0 && st.frames_concealed == 0 && st.frames_late == 0);
    CHECK(st.samples_dropped == 0 && st.samples_inserted == 0 && st.frames_skipped == 0);
    jbuf_destroy(jb);
}

/* 0〜40ms のランダムな遅延（順序入れ替わりあり）: 目標遅延が広がり、適応後は欠落しない */
static void test_jitter_adapts(void)
{
    enum { N = 1000 };
    jbuf_t *jb = make_jb();
    jbuf_stats_t mid;
    jbuf_stats_t end;
    int16_t out[FRAME];
    uint64_t arrival[N];
    int delivered[N];
    uint64_t now;
    int i;

    memset(delivered, 0, sizeof(delivered));
    memset(&mid, 0, sizeof(mid));
    for (i = 0; i < N; i++)
        arrival[i] = 10000 + (uint64_t)i * 10 + rnd(41);

    for (now = 10000; now < 10000 + (uint64_t)N * 10; now++) {
        for (i = 0; i < N; i++) {
            if (!delivered[i] && arrival[i] <= now) {
                put_frame(jb, i, now);
                delivered[i] = 1;
            }
        }
        if (now % 10 == 5) {
            jbuf_get(jb, out, FRAME);
            if (now == 10000 + 5000 + 5)
                jbuf_get_stats(jb, &mid);
        }
    }
    jbuf_get_stats(jb, &end);

    CHECK(end.jitter_ms >= 35 && end.jitter_ms <= 41);
    CHECK(end.target_ms >= 45 && end.target_ms <= 52);
    CHECK(end.level_ms >= 30 && end.level_ms <= 70);
    /* 適応完了後（後半 5 秒）は遅着・欠落・アンダーランなし */
    CHECK(end.frames_late == mid.frames_late);
    CHECK(end.frames_concealed == mid.frames_concealed);
    CHECK(end.underruns == mid.underruns);
    CHECK(end.underruns <= 2);
    CHECK(end.samples_inserted > 0); /* 目標まで遅め再生でバッファを積み増した */
    jbuf_destroy(jb);
}

/* 1 フレーム欠落: 直前フレームの 70% で補間し、続きは正常に再生される */
static void test_loss_concealment(void)
{
    jbuf_t *jb = make_jb();
    jbuf_stats_t st;
    int16_t out[FRAME];
    uint64_t now = 0;
    int saw_conceal = 0;
    int t;

    for (t = 0; t < 200; t++) {
        now += 10;
        if (t != 100)
            put_frame(jb, t, now);
        jbuf_get(jb, out, FRAME);
        if (out[0] == frame_value(99) * 7 / 10)
            saw_conceal = 1;
    }
    jbuf_get_stats(jb, &st);
    CHECK(saw_conceal);
    CHECK(st.frames_concealed == 1);
    CHECK(st.underruns == 0);
    /* 1 フレームの欠落では再生速度の調整はほぼ不要（最後の出力の末尾がフレーム 198） */
    CHECK(st.samples_inserted < 20 && st.samples_dropped < 20);
    CHECK(out[FRAME - 1] == frame_value(198));

    /* 遅れて届いた欠落フレームは破棄される */
    put_frame(jb, 100, now);
    jbuf_get_stats(jb, &st);
    CHECK(st.frames_late == 1);
    jbuf_destroy(jb);
}

/* 重複・遅着・不正入力 */
static void test_duplicate_late_invalid(void)
{
    jbuf_t *jb = make_jb();
    jbuf_config_t cfg;
    jbuf_stats_t st;
    int16_t buf[FRAME];
    int16_t out[FRAME];
    uint64_t now = 0;
    int t;

    for (t = 0; t < 50; t++) {
        now += 10;
        put_frame(jb, t, now);
        if (t == 30)
            put_frame(jb, t, now);  /* 重複 */
        jbuf_get(jb, out, FRAME);
    }
    put_frame(jb, 10, now);         /* 再生済みの位置 */
    jbuf_get_stats(jb, &st);
    CHECK(st.frames_duplicate == 1);
    CHECK(st.frames_late == 1);

    make_frame(0, buf);
    CHECK_RC(jbuf_put(jb, ts_of(60), buf, FRAME - 1, now), RS_ERR_INVALID_ARG);
    CHECK_RC(jbuf_put(jb, ts_of(60) + 7, buf, FRAME, now), RS_ERR_INVALID_ARG);

    jbuf_destroy(jb);

    jbuf_config_default(&cfg);
    cfg.min_delay_ms = 90;
    CHECK_RC(jbuf_create(&jb, &cfg), RS_ERR_INVALID_ARG);   /* min > max */
    jbuf_config_default(&cfg);
    cfg.capacity_frames = 8;
    CHECK_RC(jbuf_create(&jb, &cfg), RS_ERR_INVALID_ARG);   /* 容量不足 */
    CHECK(jb == NULL);
    jbuf_destroy(NULL);
}

/* 送信停止でアンダーラン → 再開で再バッファリングから復帰 */
static void test_underrun_recovery(void)
{
    jbuf_t *jb = make_jb();
    jbuf_stats_t st;
    int16_t out[FRAME];
    uint64_t now = 0;
    int64_t n = 0;
    int t;

    for (t = 0; t < 400; t++) {
        now += 10;
        if (t < 100 || t >= 150)
            put_frame(jb, n++, now); /* 送信側は 50 フレーム分止まる（時刻は連続） */
        jbuf_get(jb, out, FRAME);
    }
    jbuf_get_stats(jb, &st);
    CHECK(st.underruns == 1);
    CHECK(st.state == JBUF_PLAYING);
    CHECK(out[0] != 0);
    CHECK(st.frames_concealed <= 1); /* 枯渇直前の遅め再生ではみ出した数サンプル分 */
    jbuf_destroy(jb);
}

/* 回線停滞後のバースト到着（オーバーラン）: 最大遅延を超えた分を読み飛ばす */
static void test_overrun_burst(void)
{
    jbuf_t *jb = make_jb();
    jbuf_stats_t st;
    int16_t out[FRAME];
    uint64_t now = 0;
    int64_t sent = 0;
    int t;

    for (t = 0; t < 300; t++) {
        now += 10;
        if (t < 100 || t >= 125) {
            /* 停滞中に溜まった分（25 フレーム）は停滞明けに一度に届く */
            while (sent <= t)
                put_frame(jb, sent++, now);
        }
        jbuf_get(jb, out, FRAME);
    }
    jbuf_get_stats(jb, &st);
    CHECK(st.frames_skipped >= 10);
    CHECK(st.target_ms == 80);          /* 250ms の揺らぎは上限 80ms に制限 */
    CHECK(st.level_ms <= 80 + 10);
    CHECK(st.state == JBUF_PLAYING);
    jbuf_destroy(jb);
}

/* 送受信のクロック差（±1%）: 再生ポインタの微調整で吸収し、溢れも枯渇もしない */
static void test_clock_drift(int faster)
{
    jbuf_t *jb = make_jb();
    jbuf_stats_t st;
    int16_t out[FRAME];
    uint64_t now = 0;
    int64_t n = 0;
    int t;

    for (t = 0; t < 10000; t++) {
        now += 10;
        if (faster) {
            put_frame(jb, n++, now);
            if (t % 100 == 99)
                put_frame(jb, n++, now); /* 送信側が 1% 速い */
        } else if (t % 100 != 99) {
            put_frame(jb, n++, now);     /* 送信側が 1% 遅い */
        }
        jbuf_get(jb, out, FRAME);
    }
    jbuf_get_stats(jb, &st);
    CHECK(st.underruns == 0);
    CHECK(st.frames_concealed == 0);
    CHECK(st.frames_skipped == 0);
    CHECK(st.level_ms <= st.target_ms + 25);
    /* 100 秒間の 1% = 48000 サンプルの差を、微調整の正味（詰め − 伸ばし）でほぼ吸収している */
    if (faster) {
        CHECK(st.samples_dropped >= st.samples_inserted + 40000);
    } else {
        CHECK(st.samples_inserted >= st.samples_dropped + 40000);
    }
    jbuf_destroy(jb);
}

int main(void)
{
    test_steady();
    test_jitter_adapts();
    test_loss_concealment();
    test_duplicate_late_invalid();
    test_underrun_recovery();
    test_overrun_burst();
    test_clock_drift(1);
    test_clock_drift(0);
    return TEST_RESULT();
}
