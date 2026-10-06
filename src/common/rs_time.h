/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#ifndef RS_TIME_H
#define RS_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 単調増加クロック（ミリ秒）。起点は不定で、経過時間の計測にのみ使う。 */
uint64_t rs_time_monotonic_ms(void);

/* 指定ミリ秒スリープする。Linux ではシグナル割り込み時も残り時間を眠る。 */
void rs_time_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* RS_TIME_H */
