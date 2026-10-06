/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 終了要求の捕捉。PTT 強制解除などの後始末を確実に実行させるため、
 *   Linux  : SIGINT / SIGTERM / SIGHUP
 *   Windows: Ctrl+C / Ctrl+Break / コンソールウィンドウを閉じる / ログオフ / シャットダウン
 * を受けたら停止フラグを立てる。Windows のウィンドウ閉鎖・ログオフ・シャットダウンでは
 * ハンドラから戻るとプロセスが強制終了されるため、rs_shutdown_done() が呼ばれるまで
 * （最大 RS_SHUTDOWN_GRACE_MS）ハンドラ内で待つ。
 * プロセスの強制終了（kill -9 / タスクマネージャ）やクラッシュは捕捉できない。その場合の
 * 安全策は対向側の Keepalive 途絶検知（サーバは PTT 解除）と、無線機側のタイムアウトタイマー。
 */
#ifndef RS_SHUTDOWN_H
#define RS_SHUTDOWN_H

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RS_SHUTDOWN_GRACE_MS 4500  /* Windows が待つ上限（約 5 秒）より短く */

void rs_shutdown_install(volatile sig_atomic_t *stop_flag);
void rs_shutdown_done(void);

#ifdef __cplusplus
}
#endif

#endif /* RS_SHUTDOWN_H */
