/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * Linux 用 USB バックエンド。/sys/bus/usb/devices でデバイスを列挙し、
 *   - USBDEVFS_RESET: /dev/bus/usb/BBB/DDD に対する ioctl（ポートリセット＋再列挙）
 *   - authorized:     sysfs authorized を 0 → 1（ドライバ切り離し＋再列挙）
 * でリセットする。どちらも root、または udev ルールによる権限付与が必要。
 */
#include "usb_device.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/usbdevice_fs.h>

#include "rs_error.h"
#include "rs_time.h"

#define SYSFS_USB_DEVICES     "/sys/bus/usb/devices"
#define AUTHORIZED_HOLD_MS    500

static int errno_to_status(int err)
{
    usb_set_last_os_error(err);
    switch (err) {
    case EACCES:
    case EPERM:  return RS_ERR_PERMISSION;
    case ENOENT:
    case ENODEV: return RS_ERR_NOT_FOUND;
    case EBUSY:  return RS_ERR_BUSY;
    case ENOMEM: return RS_ERR_NO_MEMORY;
    default:     return RS_ERR_SYSTEM;
    }
}

/* sysfs 属性を 1 行読み、末尾の改行を除去する */
static int read_attr(const char *name, const char *attr, char *buf, size_t cap)
{
    char path[512];
    FILE *fp;
    size_t n;

    snprintf(path, sizeof(path), SYSFS_USB_DEVICES "/%s/%s", name, attr);
    fp = fopen(path, "r");
    if (fp == NULL)
        return 0;
    if (fgets(buf, (int)cap, fp) == NULL) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return 1;
}

static int write_attr(const char *name, const char *attr, const char *value)
{
    char path[512];
    int fd;
    ssize_t len = (ssize_t)strlen(value);
    ssize_t n;

    snprintf(path, sizeof(path), SYSFS_USB_DEVICES "/%s/%s", name, attr);
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return errno_to_status(errno);
    n = write(fd, value, (size_t)len);
    if (n != len) {
        int err = n < 0 ? errno : EIO;
        close(fd);
        return errno_to_status(err);
    }
    close(fd);
    return RS_OK;
}

/* "<name>:<config>.<ifnum>" のいずれかのインタフェースにドライバが結合していれば 1 */
static int has_bound_interface(const char *name)
{
    char dirpath[512];
    char path[1024];
    size_t nlen = strlen(name);
    DIR *d;
    struct dirent *e;
    int bound = 0;

    snprintf(dirpath, sizeof(dirpath), SYSFS_USB_DEVICES "/%s", name);
    d = opendir(dirpath);
    if (d == NULL)
        return 0;
    while (!bound && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, name, nlen) != 0 || e->d_name[nlen] != ':')
            continue;
        snprintf(path, sizeof(path), "%s/%s/driver", dirpath, e->d_name);
        if (access(path, F_OK) == 0)
            bound = 1;
    }
    closedir(d);
    return bound;
}

static int load_device(const char *name, usb_device_t *dev)
{
    char buf[USB_SERIAL_MAX];

    memset(dev, 0, sizeof(*dev));
    if (strlen(name) >= sizeof(dev->id))
        return 0;
    strcpy(dev->id, name);

    if (!read_attr(name, "idVendor", buf, sizeof(buf)))
        return 0;
    dev->vid = (uint16_t)strtoul(buf, NULL, 16);
    if (!read_attr(name, "idProduct", buf, sizeof(buf)))
        return 0;
    dev->pid = (uint16_t)strtoul(buf, NULL, 16);
    if (read_attr(name, "serial", buf, sizeof(buf)))
        strcpy(dev->serial, buf);
    if (read_attr(name, "busnum", buf, sizeof(buf)))
        dev->busnum = atoi(buf);
    if (read_attr(name, "devnum", buf, sizeof(buf)))
        dev->devnum = atoi(buf);

    dev->ready = read_attr(name, "authorized", buf, sizeof(buf)) && strcmp(buf, "1") == 0 &&
                 has_bound_interface(name);
    return 1;
}

static int linux_enumerate(void *ctx, usb_enum_cb cb, void *user)
{
    DIR *d;
    struct dirent *e;

    (void)ctx;
    d = opendir(SYSFS_USB_DEVICES);
    if (d == NULL)
        return errno_to_status(errno);

    while ((e = readdir(d)) != NULL) {
        usb_device_t dev;

        /* "." / ".." / インタフェース ("1-1.2:1.0") / ルートハブ ("usb1") は除外
         * （Windows バックエンドと列挙対象を揃える） */
        if (e->d_name[0] == '.' || strchr(e->d_name, ':') != NULL ||
            strncmp(e->d_name, "usb", 3) == 0)
            continue;
        if (!load_device(e->d_name, &dev))
            continue;
        if (cb(&dev, user) != 0)
            break;
    }
    closedir(d);
    return RS_OK;
}

static int linux_get_parent(void *ctx, const usb_device_t *dev, usb_device_t *parent)
{
    char name[USB_DEVICE_ID_MAX];
    char *dot;

    (void)ctx;
    if (dev == NULL || parent == NULL)
        return RS_ERR_INVALID_ARG;

    /* "1-1.2" の親は "1-1"。"1-1" の親はルートハブ "usb1" なので拒否する */
    strcpy(name, dev->id);
    dot = strrchr(name, '.');
    if (dot == NULL || strncmp(name, "usb", 3) == 0)
        return RS_ERR_UNSUPPORTED;
    *dot = '\0';

    if (!load_device(name, parent))
        return RS_ERR_NOT_FOUND;
    return RS_OK;
}

static int reset_usbdevfs(const usb_device_t *target)
{
    char path[64];
    int fd;

    if (target->busnum <= 0 || target->devnum <= 0)
        return RS_ERR_NOT_FOUND;
    snprintf(path, sizeof(path), "/dev/bus/usb/%03d/%03d", target->busnum, target->devnum);
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return errno_to_status(errno);
    if (ioctl(fd, USBDEVFS_RESET, 0) < 0) {
        int err = errno;
        close(fd);
        return errno_to_status(err);
    }
    close(fd);
    return RS_OK;
}

static int reset_authorized(const usb_device_t *target)
{
    int rc = write_attr(target->id, "authorized", "0");
    if (rc != RS_OK)
        return rc;
    rs_time_sleep_ms(AUTHORIZED_HOLD_MS);
    return write_attr(target->id, "authorized", "1");
}

static int linux_reset(void *ctx, const usb_device_t *target, usb_reset_method_t method)
{
    (void)ctx;
    if (target == NULL)
        return RS_ERR_INVALID_ARG;
    switch (method) {
    case USB_RESET_USBDEVFS:   return reset_usbdevfs(target);
    case USB_RESET_AUTHORIZED: return reset_authorized(target);
    default:                   return RS_ERR_UNSUPPORTED;
    }
}

/* 軽いポートリセットを先に試し、復帰しなければ再列挙を伴う authorized トグルへ */
static const usb_reset_method_t k_linux_auto[] = { USB_RESET_USBDEVFS, USB_RESET_AUTHORIZED };

static const usb_backend_t k_linux_backend = {
    "linux-sysfs",
    NULL,
    linux_enumerate,
    linux_get_parent,
    linux_reset,
    k_linux_auto,
    sizeof(k_linux_auto) / sizeof(k_linux_auto[0])
};

const usb_backend_t *usb_backend_platform(void)
{
    return &k_linux_backend;
}
