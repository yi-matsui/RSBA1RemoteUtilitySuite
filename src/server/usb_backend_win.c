/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * Windows 用 USB バックエンド。CfgMgr32 でデバイスノードを列挙し、
 * CM_Disable_DevNode / CM_Enable_DevNode でドライバスタックごと再起動する。
 * リセットには管理者権限が必要。
 */
#include "usb_device.h"

#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <cfgmgr32.h>

#include "rs_error.h"
#include "rs_time.h"

/* Disable と Enable の間の待ち時間 */
#define WIN_DISABLE_HOLD_MS   500
#define WIN_ENABLE_RETRIES    5
#define WIN_ENABLE_RETRY_MS   200

static int cr_to_status(CONFIGRET cr)
{
    usb_set_last_os_error((int)cr);
    switch (cr) {
    case CR_SUCCESS:          return RS_OK;
    case CR_ACCESS_DENIED:    return RS_ERR_PERMISSION;
    case CR_NO_SUCH_DEVNODE:  return RS_ERR_NOT_FOUND; /* = CR_NO_SUCH_DEVINST */
    case CR_REMOVE_VETOED:
    case CR_NOT_DISABLEABLE:  return RS_ERR_BUSY;
    case CR_OUT_OF_MEMORY:    return RS_ERR_NO_MEMORY;
    default:                  return RS_ERR_SYSTEM;
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "VID_xxxx" 等の直後 4 桁を読む */
static int parse_tag_hex4(const char *id, const char *tag, uint16_t *out)
{
    const char *p = strstr(id, tag);
    unsigned v = 0;
    int i;

    if (p == NULL)
        return 0;
    p += strlen(tag);
    for (i = 0; i < 4; i++) {
        int d = hex_digit(p[i]);
        if (d < 0)
            return 0;
        v = (v << 4) | (unsigned)d;
    }
    *out = (uint16_t)v;
    return 1;
}

static int is_ready(DEVINST inst)
{
    ULONG status = 0;
    ULONG problem = 0;

    if (CM_Get_DevNode_Status(&status, &problem, inst, 0) != CR_SUCCESS)
        return 0;
    return (status & DN_STARTED) != 0 && (status & DN_HAS_PROBLEM) == 0;
}

/* インスタンス ID から usb_device_t を作る。USB デバイスノードでなければ 0 */
static int fill_device(const char *instance_id, DEVINST inst, usb_device_t *dev)
{
    const char *last;

    if (_strnicmp(instance_id, "USB\\", 4) != 0 || strstr(instance_id, "&MI_") != NULL)
        return 0; /* USB 以外、またはコンポジットデバイスのインタフェースノード */

    memset(dev, 0, sizeof(*dev));
    if (!parse_tag_hex4(instance_id, "VID_", &dev->vid) ||
        !parse_tag_hex4(instance_id, "PID_", &dev->pid))
        return 0; /* ルートハブ等 */

    strncpy(dev->id, instance_id, sizeof(dev->id) - 1);

    /* 末尾要素はシリアル番号。シリアルを持たないデバイスは "&" を含む
     * ポート位置ベースの生成 ID になるため、その場合は空とする */
    last = strrchr(instance_id, '\\');
    if (last != NULL && strchr(last + 1, '&') == NULL)
        strncpy(dev->serial, last + 1, sizeof(dev->serial) - 1);

    dev->ready = is_ready(inst);
    return 1;
}

static int win_enumerate(void *ctx, usb_enum_cb cb, void *user)
{
    ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR;
    char *list = NULL;
    ULONG len = 0;
    CONFIGRET cr;
    const char *p;
    int attempt;

    (void)ctx;
#ifdef CM_GETIDLIST_FILTER_PRESENT
    flags |= CM_GETIDLIST_FILTER_PRESENT;
#endif

    /* 取得の合間にデバイスが増えると CR_BUFFER_SMALL になるため再試行する */
    for (attempt = 0; attempt < 5; attempt++) {
        cr = CM_Get_Device_ID_List_SizeA(&len, "USB", flags);
        if (cr != CR_SUCCESS)
            return cr_to_status(cr);
        free(list);
        list = malloc(len);
        if (list == NULL)
            return RS_ERR_NO_MEMORY;
        cr = CM_Get_Device_ID_ListA("USB", list, len, flags);
        if (cr != CR_BUFFER_SMALL)
            break;
    }
    if (cr != CR_SUCCESS) {
        free(list);
        return cr_to_status(cr);
    }

    for (p = list; *p != '\0'; p += strlen(p) + 1) {
        DEVINST inst;
        usb_device_t dev;

        /* NORMAL 指定では未接続（ファントム）ノードは見つからない */
        if (CM_Locate_DevNodeA(&inst, (DEVINSTID_A)p, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue;
        if (!fill_device(p, inst, &dev))
            continue;
        if (cb(&dev, user) != 0)
            break;
    }

    free(list);
    return RS_OK;
}

static int win_get_parent(void *ctx, const usb_device_t *dev, usb_device_t *parent)
{
    DEVINST inst;
    DEVINST pinst;
    char pid[MAX_DEVICE_ID_LEN + 1];
    CONFIGRET cr;

    (void)ctx;
    if (dev == NULL || parent == NULL)
        return RS_ERR_INVALID_ARG;

    cr = CM_Locate_DevNodeA(&inst, (DEVINSTID_A)dev->id, CM_LOCATE_DEVNODE_NORMAL);
    if (cr != CR_SUCCESS)
        return cr_to_status(cr);
    cr = CM_Get_Parent(&pinst, inst, 0);
    if (cr != CR_SUCCESS)
        return cr_to_status(cr);
    cr = CM_Get_Device_IDA(pinst, pid, sizeof(pid), 0);
    if (cr != CR_SUCCESS)
        return cr_to_status(cr);

    /* ルートハブ・ホストコントローラ（VID/PID を持たない、または USB 以外）は
     * 他の USB 機器を巻き込むためリセット対象として返さない */
    if (!fill_device(pid, pinst, parent))
        return RS_ERR_UNSUPPORTED;
    return RS_OK;
}

static int win_reset(void *ctx, const usb_device_t *target, usb_reset_method_t method)
{
    DEVINST inst;
    CONFIGRET cr;
    int i;

    (void)ctx;
    if (target == NULL)
        return RS_ERR_INVALID_ARG;
    if (method != USB_RESET_DEVNODE)
        return RS_ERR_UNSUPPORTED;

    cr = CM_Locate_DevNodeA(&inst, (DEVINSTID_A)target->id, CM_LOCATE_DEVNODE_NORMAL);
    if (cr != CR_SUCCESS)
        return cr_to_status(cr);

    /* CM_DISABLE_PERSIST は付けない: 万一 Enable 前に異常終了しても再起動で元に戻る */
    cr = CM_Disable_DevNode(inst, CM_DISABLE_UI_NOT_OK);
    if (cr != CR_SUCCESS)
        return cr_to_status(cr);

    rs_time_sleep_ms(WIN_DISABLE_HOLD_MS);

    /* 無効化したまま放置すると手動復旧が必要になるため、Enable は再試行する */
    for (i = 0; i < WIN_ENABLE_RETRIES; i++) {
        cr = CM_Enable_DevNode(inst, 0);
        if (cr == CR_SUCCESS)
            return RS_OK;
        rs_time_sleep_ms(WIN_ENABLE_RETRY_MS);
    }
    return cr_to_status(cr);
}

static const usb_reset_method_t k_win_auto[] = { USB_RESET_DEVNODE };

static const usb_backend_t k_win_backend = {
    "windows-cfgmgr32",
    NULL,
    win_enumerate,
    win_get_parent,
    win_reset,
    k_win_auto,
    sizeof(k_win_auto) / sizeof(k_win_auto[0])
};

const usb_backend_t *usb_backend_platform(void)
{
    return &k_win_backend;
}
