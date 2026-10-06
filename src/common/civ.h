/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * CI-V フレームの生成と切り出し。フレーム形式: FE FE <宛先> <送信元> <コマンド/データ...> FD
 * CI-V のデータ部は BCD 等で構成され、FE / FD を含まない。
 */
#ifndef CIV_H
#define CIV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CIV_PREAMBLE         0xFE
#define CIV_EOM              0xFD
#define CIV_ADDR_IC9100      0x7C
#define CIV_ADDR_CONTROLLER  0xE0
#define CIV_MIN_FRAME        5    /* FE FE to from FD */
#define CIV_MAX_FRAME        128  /* これを超える長さはフレームとして扱わない */

/* フレームを組み立てて長さを返す。cap 不足・body 内に FE / FD がある場合は 0 */
size_t civ_build_frame(uint8_t *buf, size_t cap, uint8_t to, uint8_t from,
                       const uint8_t *body, size_t body_len);

/* PTT 強制解除（送信停止）: コマンド 1C 00 00 → FE FE <radio> <ctrl> 1C 00 00 FD */
size_t civ_build_ptt_off(uint8_t *buf, size_t cap, uint8_t radio_addr, uint8_t ctrl_addr);

/* data が完全なフレームの連続（各 FE FE … FD、長さ CIV_MIN_FRAME〜CIV_MAX_FRAME）なら 1 */
int civ_validate_frames(const uint8_t *data, size_t len);

/* ---- バイトストリームからのフレーム切り出し ------------------------------ */

typedef void (*civ_frame_cb)(const uint8_t *frame, size_t len, void *user);

typedef struct civ_framer {
    uint8_t  buf[CIV_MAX_FRAME];
    size_t   len;
    uint64_t frames;           /* 切り出したフレーム数 */
    uint64_t garbage_total;    /* フレームにならず破棄したバイト数の累計 */
    size_t   garbage_pending;  /* 最後の正常フレーム以降に破棄したバイト数（不正バッファ蓄積の指標） */
} civ_framer_t;

void civ_framer_init(civ_framer_t *f);

/* data を投入し、完成したフレームごとに cb を呼ぶ。完成したフレーム数を返す。
 * 途中までのフレームは次回の投入に持ち越す。 */
size_t civ_framer_push(civ_framer_t *f, const uint8_t *data, size_t len,
                       civ_frame_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* CIV_H */
