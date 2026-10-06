/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * コンパイラ差異を吸収するマクロ。
 */
#ifndef RS_PLATFORM_H
#define RS_PLATFORM_H

/* MSVC の C モードは _Thread_local 未対応のため __declspec(thread) を使う */
#if defined(_MSC_VER)
#define RS_THREAD_LOCAL __declspec(thread)
#else
#define RS_THREAD_LOCAL _Thread_local
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RS_PRINTF_FMT(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define RS_PRINTF_FMT(fmt_idx, arg_idx)
#endif

#endif /* RS_PLATFORM_H */
