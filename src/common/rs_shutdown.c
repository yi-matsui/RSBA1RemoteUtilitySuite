/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "rs_shutdown.h"

#include <stddef.h>

static volatile sig_atomic_t *g_flag;

#ifdef _WIN32
#include <windows.h>

static HANDLE g_done;

static BOOL WINAPI console_handler(DWORD type)
{
    if (g_flag != NULL)
        *g_flag = 1;
    switch (type) {
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        /* 戻るとプロセスが終了させられるため、メインスレッドの後始末を待つ */
        if (g_done != NULL)
            WaitForSingleObject(g_done, RS_SHUTDOWN_GRACE_MS);
        return TRUE;
    default:
        return TRUE; /* Ctrl+C / Ctrl+Break: メインループが停止フラグを見て終了する */
    }
}

void rs_shutdown_install(volatile sig_atomic_t *stop_flag)
{
    g_flag = stop_flag;
    if (g_done == NULL)
        g_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(console_handler, TRUE);
}

void rs_shutdown_done(void)
{
    if (g_done != NULL)
        SetEvent(g_done);
}

#else

static void on_signal(int sig)
{
    (void)sig;
    if (g_flag != NULL)
        *g_flag = 1;
}

void rs_shutdown_install(volatile sig_atomic_t *stop_flag)
{
    g_flag = stop_flag;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
}

void rs_shutdown_done(void)
{
}

#endif
