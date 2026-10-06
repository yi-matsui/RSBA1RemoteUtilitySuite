/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 実 OS バックエンドの検出テスト。列挙・検索・親ハブ取得のみを行い、
 * リセットは実行しない（未対応方式の拒否のみ確認する）。
 */
#include <stdio.h>
#include <string.h>

#include "rs_error.h"
#include "test_util.h"
#include "usb_device.h"

static int g_count;
static usb_device_t g_first;

static int collect(const usb_device_t *dev, void *user)
{
    (void)user;
    printf("  %04X:%04X %-9s serial=%-20s %s\n", dev->vid, dev->pid,
           dev->ready ? "ready" : "not-ready", dev->serial[0] ? dev->serial : "-", dev->id);
    if (g_count == 0)
        g_first = *dev;
    g_count++;
    CHECK(dev->id[0] != '\0');
    return 0;
}

int main(void)
{
    const usb_backend_t *be = usb_backend_platform();
    usb_match_t match;
    usb_device_t dev;
    usb_device_t parent;
    int rc;

    CHECK(be != NULL && be->name != NULL);
    CHECK(be->n_auto_methods > 0);
    printf("backend: %s\n", be->name);

    CHECK_RC(usb_enumerate(be, collect, NULL), RS_OK);
    printf("%d USB device(s)\n", g_count);

    /* 存在しない VID:PID */
    memset(&match, 0, sizeof(match));
    match.vid = 0xFFFF;
    match.pid = 0xFFFE;
    CHECK_RC(usb_find(be, &match, &dev), RS_ERR_NOT_FOUND);

    /* 空の検索条件は拒否 */
    memset(&match, 0, sizeof(match));
    CHECK_RC(usb_find(be, &match, &dev), RS_ERR_INVALID_ARG);

    if (g_count > 0) {
        /* デバイス ID による再検索 */
        memset(&match, 0, sizeof(match));
        match.device_id = g_first.id;
        CHECK_RC(usb_find(be, &match, &dev), RS_OK);
        CHECK(dev.vid == g_first.vid && dev.pid == g_first.pid);

        /* 親ハブ: 取得できるか、ルートハブとして拒否されるか */
        rc = be->get_parent(be->ctx, &g_first, &parent);
        CHECK(rc == RS_OK || rc == RS_ERR_UNSUPPORTED);
        if (rc == RS_OK) {
            CHECK(parent.id[0] != '\0');
            printf("parent of %s: %s\n", g_first.id, parent.id);
        } else {
            printf("parent of %s: root hub (not resettable)\n", g_first.id);
        }

        /* このプラットフォームで未対応の方式は何もせず拒否される */
#ifdef _WIN32
        CHECK_RC(be->reset(be->ctx, &g_first, USB_RESET_USBDEVFS), RS_ERR_UNSUPPORTED);
#else
        CHECK_RC(be->reset(be->ctx, &g_first, USB_RESET_DEVNODE), RS_ERR_UNSUPPORTED);
#endif
    } else {
        printf("no USB devices found; skipping device-specific checks\n");
    }

    return TEST_RESULT();
}
