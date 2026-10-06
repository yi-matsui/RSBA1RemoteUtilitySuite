/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 双方向オーディオ伝送のテスト。仮想時計で駆動するモックデバイス（48kHz の実時間で
 * 録音・再生が進む）と、遅延・揺らぎを与える仮想ネットワークで、サーバ⇔クライアント間の
 * 音声がビット単位で正しく届くこと、ジッター制御、形式不一致の拒否を検証する。
 */
#include <stdlib.h>
#include <string.h>

#include "audio_codec.h"
#include "audio_link.h"
#include "rs_error.h"
#include "test_util.h"

#define RATE    48000
#define FRAME   480
#define SINK    (RATE * 12)

static uint64_t g_now;          /* 仮想時計（ミリ秒） */
static uint32_t g_rng = 777;

static uint32_t rnd(uint32_t n)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 16) % n;
}

/* 録音元の波形: サンプル番号から一意に決まるランプ（周期 20000） */
static int16_t source_sample(uint64_t n, int16_t offset)
{
    return (int16_t)((int64_t)(n % 20000) - 10000 + offset);
}

/* ---- モックデバイス ------------------------------------------------------ */

typedef struct mock_dev {
    uint64_t start_ms;
    int16_t  offset;          /* 送信方向ごとに波形を変える */
    uint64_t captured;
    uint64_t written;
    uint64_t queue_limit;
    uint64_t underruns;
    uint64_t burst;           /* 再生の消費単位（サンプル）。0 = 連続。実デバイスのまとめ消費を再現 */
    int16_t *sink;            /* 再生された全サンプル */
    uint64_t sink_len;
} mock_dev_t;

static uint64_t md_elapsed(const mock_dev_t *d)
{
    return (g_now - d->start_ms) * RATE / 1000;
}

static int md_read(void *st, int16_t *pcm, size_t max, size_t *got)
{
    mock_dev_t *d = st;
    uint64_t avail = md_elapsed(d) - d->captured;
    size_t n = avail < max ? (size_t)avail : max;
    size_t i;

    for (i = 0; i < n; i++)
        pcm[i] = source_sample(d->captured + i, d->offset);
    d->captured += n;
    *got = n;
    return RS_OK;
}

static size_t md_write_avail(void *st)
{
    mock_dev_t *d = st;
    uint64_t played = md_elapsed(d);

    if (d->burst > 0)
        played -= played % d->burst;
    uint64_t queued;

    if (d->written < played) {
        if (d->written > 0)
            d->underruns++;
        d->written = played;
    }
    queued = d->written - played;
    return queued >= d->queue_limit ? 0 : (size_t)(d->queue_limit - queued);
}

static int md_write(void *st, const int16_t *pcm, size_t samples)
{
    mock_dev_t *d = st;
    size_t n = samples;

    if (d->sink_len + n > SINK)
        n = (size_t)(SINK - d->sink_len);
    memcpy(d->sink + d->sink_len, pcm, n * sizeof(int16_t));
    d->sink_len += n;
    d->written += samples;
    return RS_OK;
}

static void md_close(void *st)
{
    (void)st;
}

static const audio_dev_ops_t k_mock_ops = { md_read, md_write, md_write_avail, md_close };

static void md_init(mock_dev_t *d, audio_dev_t *dev, int16_t offset)
{
    memset(d, 0, sizeof(*d));
    d->start_ms = g_now;
    d->offset = offset;
    d->queue_limit = FRAME * 3;
    d->sink = calloc(SINK, sizeof(int16_t));
    dev->ops = &k_mock_ops;
    dev->st = d;
}

/* ---- 遅延・揺らぎのある仮想ネットワーク ---------------------------------- */

#define NETQ 4096

typedef struct net_pkt {
    uint64_t due;
    int      to_server;
    size_t   len;
    uint8_t  data[1232];
} net_pkt_t;

static net_pkt_t *g_q;
static size_t     g_qn;
static uint32_t   g_base_delay;
static uint32_t   g_jitter;
static int        g_drop_every;    /* n パケットに 1 つ落とす（0 = 落とさない） */
static uint32_t   g_sent;

static int enqueue(int to_server, const void *pkt, size_t len)
{
    net_pkt_t *p;

    g_sent++;
    if (g_drop_every > 0 && g_sent % (uint32_t)g_drop_every == 0)
        return RS_OK;
    if (g_qn >= NETQ)
        return RS_OK;
    p = &g_q[g_qn++];
    p->due = g_now + g_base_delay + (g_jitter > 0 ? rnd(g_jitter + 1) : 0);
    p->to_server = to_server;
    p->len = len;
    memcpy(p->data, pkt, len);
    return RS_OK;
}

static int send_to_server(void *user, const void *pkt, size_t len)
{
    (void)user;
    return enqueue(1, pkt, len);
}

static int send_to_client(void *user, const void *pkt, size_t len)
{
    (void)user;
    return enqueue(0, pkt, len);
}

static void deliver(audio_link_t *srv, audio_link_t *cli)
{
    size_t i = 0;

    while (i < g_qn) {
        if (g_q[i].due <= g_now) {
            net_pkt_t p = g_q[i];
            uint8_t type;
            g_q[i] = g_q[--g_qn]; /* 到着順は due 順ではなくなる（並べ替えも再現） */
            audio_link_handle_packet(p.to_server ? srv : cli, p.data, p.len, g_now, &type);
        } else {
            i++;
        }
    }
}

/* ---- 再生結果の検証 ------------------------------------------------------ */

/* sink の [from, from+len) が録音波形の連続した区間と一致するか（遅延は自動で求める） */
static int matches_source(const mock_dev_t *d, uint64_t from, uint64_t len, int16_t offset)
{
    int16_t first = d->sink[from];
    int64_t base = (int64_t)first - offset + 10000; /* 波形上の位置（周期 20000 内） */
    uint64_t i;

    if (base < 0 || base >= 20000)
        return 0;
    for (i = 0; i < len; i++) {
        if (d->sink[from + i] != source_sample((uint64_t)base + i, offset))
            return 0;
    }
    return 1;
}

typedef struct env {
    mock_dev_t    srv_md;
    mock_dev_t    cli_md;
    audio_dev_t   srv_dev;
    audio_dev_t   cli_dev;
    audio_link_t *srv;
    audio_link_t *cli;
} env_t;

static const uint8_t k_c2s[CTL_KEY_LEN] = { 0x11 };
static const uint8_t k_s2c[CTL_KEY_LEN] = { 0x22 };

static void env_init(env_t *e, int start)
{
    audio_link_config_t cfg;

    memset(e, 0, sizeof(*e));
    g_qn = 0;
    g_sent = 0;
    md_init(&e->srv_md, &e->srv_dev, 0);     /* 無線機の受信音 */
    md_init(&e->cli_md, &e->cli_dev, 1000);  /* 操作端末のマイク */
    audio_link_config_default(&cfg);
    CHECK_RC(audio_link_create(&e->srv, &cfg, send_to_client, NULL), RS_OK);
    CHECK_RC(audio_link_create(&e->cli, &cfg, send_to_server, NULL), RS_OK);
    audio_link_attach(e->srv, &e->srv_dev);
    audio_link_attach(e->cli, &e->cli_dev);
    if (start) {
        audio_link_start(e->srv, DCH_ROLE_SERVER, 42, k_c2s, k_s2c);
        audio_link_start(e->cli, DCH_ROLE_CLIENT, 42, k_c2s, k_s2c);
    }
}

static void env_run(env_t *e, uint32_t ms)
{
    uint32_t t;
    for (t = 0; t < ms; t++) {
        g_now++;
        audio_link_poll(e->srv);
        audio_link_poll(e->cli);
        deliver(e->srv, e->cli);
    }
}

static void env_free(env_t *e)
{
    audio_link_destroy(e->srv);
    audio_link_destroy(e->cli);
    free(e->srv_md.sink);
    free(e->cli_md.sink);
}

/* ---- テスト -------------------------------------------------------------- */

static void test_pcm_codec(void)
{
    audio_codec_t c;
    int16_t in[FRAME];
    int16_t out[FRAME];
    uint8_t buf[FRAME * 2];
    size_t len = 0;
    size_t n = 0;
    audio_codec_id_t id;
    int i;

    for (i = 0; i < FRAME; i++)
        in[i] = (int16_t)(i * 137 - 32768);
    CHECK_RC(audio_codec_open(&c, AUDIO_CODEC_PCM16, RATE, FRAME), RS_OK);
    CHECK_RC(audio_codec_encode(&c, in, FRAME, buf, sizeof(buf), &len), RS_OK);
    CHECK(len == FRAME * 2);
    CHECK(buf[0] == 0x80 && buf[1] == 0x00); /* -32768 のビッグエンディアン表現 */
    CHECK_RC(audio_codec_decode(&c, buf, len, out, FRAME, &n), RS_OK);
    CHECK(n == FRAME && memcmp(in, out, sizeof(in)) == 0);
    CHECK_RC(audio_codec_decode(&c, buf, 3, out, FRAME, &n), RS_ERR_INVALID_ARG);
    CHECK_RC(audio_codec_encode(&c, in, FRAME - 1, buf, sizeof(buf), &len), RS_ERR_INVALID_ARG);
    audio_codec_close(&c);

    /* Opus はインタフェースのみ（未組み込み） */
    CHECK_RC(audio_codec_open(&c, AUDIO_CODEC_OPUS, RATE, 960), RS_ERR_UNSUPPORTED);
    CHECK_RC(audio_codec_from_string("opus", &id), RS_OK);
    CHECK(id == AUDIO_CODEC_OPUS);
    CHECK_RC(audio_codec_from_string("mp3", &id), RS_ERR_INVALID_ARG);
}

/* 揺らぎなし: 双方向とも録音波形がそのまま（ビット一致で）再生される */
static void test_clean_bidirectional(void)
{
    env_t e;
    audio_link_stats_t ss, cs;
    jbuf_stats_t sj, cj;
    uint64_t from;

    g_base_delay = 15;
    g_jitter = 0;
    g_drop_every = 0;
    env_init(&e, 1);
    env_run(&e, 3000);

    audio_link_get_stats(e.srv, &ss, &sj);
    audio_link_get_stats(e.cli, &cs, &cj);
    CHECK(ss.frames_sent >= 295 && cs.frames_sent >= 295);
    CHECK(ss.frames_received >= 290 && cs.frames_received >= 290);
    CHECK(cj.underruns == 0 && cj.frames_concealed == 0);
    CHECK(sj.underruns == 0 && sj.frames_concealed == 0);
    CHECK(cj.target_ms == 20 && sj.target_ms == 20);

    /* 開始直後を除く 2 秒分がビット一致 */
    from = RATE / 2;
    CHECK(e.cli_md.sink_len > from + RATE * 2);
    CHECK(matches_source(&e.cli_md, from, RATE * 2, 0));      /* 無線機 → 操作端末 */
    CHECK(matches_source(&e.srv_md, from, RATE * 2, 1000));   /* 操作端末 → 無線機 */
    CHECK(e.cli_md.underruns == 0 && e.srv_md.underruns == 0);
    env_free(&e);
}

/* 0〜40ms の揺らぎ: 目標遅延が広がり、適応後は欠落なく再生される */
static void test_jitter_network(void)
{
    env_t e;
    jbuf_stats_t mid, end;
    uint64_t check_from;

    g_base_delay = 20;
    g_jitter = 40;
    g_drop_every = 0;
    env_init(&e, 1);
    env_run(&e, 4000);
    audio_link_get_stats(e.cli, NULL, &mid);
    check_from = e.cli_md.sink_len;
    env_run(&e, 4000);
    audio_link_get_stats(e.cli, NULL, &end);

    CHECK(end.target_ms >= 40 && end.target_ms <= 60);
    /* 適応後は揺らぎに振り回されて再生速度を調整し続けない */
    CHECK(end.samples_dropped + end.samples_inserted <=
          mid.samples_dropped + mid.samples_inserted + FRAME);
    CHECK(end.frames_concealed == mid.frames_concealed);
    CHECK(end.underruns == mid.underruns);
    CHECK(end.frames_late == mid.frames_late);
    CHECK(e.cli_md.underruns == 0);
    /* 後半は（微調整で補間が入らない限り）連続した波形。少なくとも 0.5 秒は一致する区間がある */
    {
        uint64_t pos;
        int found = 0;
        for (pos = check_from; pos + RATE / 2 < e.cli_md.sink_len && !found; pos += FRAME)
            found = matches_source(&e.cli_md, pos, RATE / 2, 0);
        CHECK(found);
    }
    env_free(&e);
}

/* 再生デバイスが 30ms 分ずつまとめて空きを返す場合（WinMM で実測した挙動）:
 * 取り出しの塊を検出して目標遅延に余裕を加え、欠落なく再生する */
static void test_bursty_playout(void)
{
    env_t e;
    jbuf_stats_t mid, end;

    g_base_delay = 5;
    g_jitter = 20;
    g_drop_every = 0;
    env_init(&e, 1);
    e.cli_md.burst = FRAME * 3;
    e.cli_md.queue_limit = FRAME * 4;
    env_run(&e, 3000);
    audio_link_get_stats(e.cli, NULL, &mid);
    env_run(&e, 5000);
    audio_link_get_stats(e.cli, NULL, &end);

    CHECK(end.extra_ms == 20);               /* 3 フレームの塊 → 2 フレーム分の余裕 */
    CHECK(end.target_ms >= 45 && end.target_ms <= 55);
    CHECK(end.frames_concealed == mid.frames_concealed);
    CHECK(end.underruns == mid.underruns);
    CHECK(e.cli_md.underruns == 0);
    env_free(&e);
}

/* パケット損失: 補間で欠落を埋め、再生は途切れない */
static void test_packet_loss(void)
{
    env_t e;
    jbuf_stats_t cj;
    jbuf_stats_t sj;

    g_base_delay = 10;
    g_jitter = 0;
    g_drop_every = 25; /* 両方向合わせて 4% */
    env_init(&e, 1);
    env_run(&e, 3000);
    audio_link_get_stats(e.cli, NULL, &cj);
    audio_link_get_stats(e.srv, NULL, &sj);
    CHECK(cj.frames_concealed + sj.frames_concealed >= 10);
    CHECK(cj.underruns == 0 && sj.underruns == 0);
    CHECK(e.cli_md.underruns == 0 && e.srv_md.underruns == 0);
    env_free(&e);
}

/* セッション前: 送信しないがデバイスへは無音を供給し続ける */
static void test_inactive(void)
{
    env_t e;
    audio_link_stats_t ss;
    uint64_t i;
    int all_zero = 1;

    g_base_delay = 10;
    g_jitter = 0;
    g_drop_every = 0;
    env_init(&e, 0);
    env_run(&e, 500);
    audio_link_get_stats(e.srv, &ss, NULL);
    CHECK(ss.frames_sent == 0);
    CHECK(g_sent == 0);
    CHECK(e.cli_md.sink_len >= RATE / 2 - FRAME);
    for (i = 0; i < e.cli_md.sink_len; i++)
        if (e.cli_md.sink[i] != 0)
            all_zero = 0;
    CHECK(all_zero);
    CHECK(!audio_link_active(e.srv));
    CHECK_RC(audio_link_send_bind(e.cli), RS_ERR_BUSY);
    env_free(&e);
}

/* 形式不一致（フレーム長違い）は拒否、不正な設定は作成時に拒否 */
static void test_format_mismatch(void)
{
    audio_link_config_t cfg;
    audio_link_t *a = NULL;
    audio_link_t *b = NULL;
    audio_link_stats_t st;
    mock_dev_t md;
    audio_dev_t dev;
    int t;

    g_base_delay = 0;
    g_jitter = 0;
    g_drop_every = 0;
    g_qn = 0;
    audio_link_config_default(&cfg);
    cfg.frame_samples = 240;
    CHECK_RC(audio_link_create(&a, &cfg, send_to_server, NULL), RS_OK);
    audio_link_config_default(&cfg);
    CHECK_RC(audio_link_create(&b, &cfg, send_to_client, NULL), RS_OK);
    md_init(&md, &dev, 0);
    audio_link_attach(a, &dev);
    audio_link_start(a, DCH_ROLE_CLIENT, 1, k_c2s, k_s2c);
    audio_link_start(b, DCH_ROLE_SERVER, 1, k_c2s, k_s2c);
    for (t = 0; t < 50; t++) {
        g_now++;
        audio_link_poll(a);
        deliver(b, b);
    }
    audio_link_get_stats(b, &st, NULL);
    CHECK(st.rejected_format >= 5);
    CHECK(st.frames_received == 0);
    audio_link_destroy(a);
    audio_link_destroy(b);
    free(md.sink);

    /* 10ms を超える PCM フレームは 1 パケットに収まらない */
    audio_link_config_default(&cfg);
    cfg.frame_samples = 960;
    CHECK_RC(audio_link_create(&a, &cfg, send_to_server, NULL), RS_ERR_TOO_LARGE);
    audio_link_config_default(&cfg);
    cfg.codec = AUDIO_CODEC_OPUS;
    CHECK_RC(audio_link_create(&a, &cfg, send_to_server, NULL), RS_ERR_UNSUPPORTED);
}

int main(void)
{
    g_q = calloc(NETQ, sizeof(net_pkt_t));
    g_now = 1000;
    test_pcm_codec();
    test_clean_bidirectional();
    test_jitter_network();
    test_bursty_playout();
    test_packet_loss();
    test_inactive();
    test_format_mismatch();
    free(g_q);
    return TEST_RESULT();
}
