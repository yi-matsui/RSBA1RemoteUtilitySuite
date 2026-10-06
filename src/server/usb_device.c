/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "usb_device.h"

#include <string.h>

#include "rs_error.h"
#include "rs_platform.h"

static RS_THREAD_LOCAL int g_last_os_error;

int usb_last_os_error(void)
{
    return g_last_os_error;
}

void usb_set_last_os_error(int err)
{
    g_last_os_error = err;
}

int usb_enumerate(const usb_backend_t *backend, usb_enum_cb cb, void *user)
{
    if (backend == NULL || backend->enumerate == NULL || cb == NULL)
        return RS_ERR_INVALID_ARG;
    return backend->enumerate(backend->ctx, cb, user);
}

static int is_set(const char *s)
{
    return s != NULL && s[0] != '\0';
}

int usb_match_device(const usb_match_t *match, const usb_device_t *dev)
{
    if (match == NULL || dev == NULL)
        return 0;
    if (match->vid != 0 && match->vid != dev->vid)
        return 0;
    if (match->pid != 0 && match->pid != dev->pid)
        return 0;
    if (is_set(match->serial) && strcmp(match->serial, dev->serial) != 0)
        return 0;
    if (is_set(match->device_id) && strcmp(match->device_id, dev->id) != 0)
        return 0;
    return 1;
}

struct find_ctx {
    const usb_match_t *match;
    usb_device_t      *out;
    int                found;
};

static int find_cb(const usb_device_t *dev, void *user)
{
    struct find_ctx *fc = user;

    if (!usb_match_device(fc->match, dev))
        return 0;
    *fc->out = *dev;
    fc->found = 1;
    return 1;
}

int usb_find(const usb_backend_t *backend, const usb_match_t *match, usb_device_t *out)
{
    struct find_ctx fc;
    int rc;

    if (match == NULL || out == NULL)
        return RS_ERR_INVALID_ARG;
    if (match->vid == 0 && match->pid == 0 && !is_set(match->serial) && !is_set(match->device_id))
        return RS_ERR_INVALID_ARG;

    fc.match = match;
    fc.out = out;
    fc.found = 0;
    rc = usb_enumerate(backend, find_cb, &fc);
    if (rc != RS_OK)
        return rc;
    return fc.found ? RS_OK : RS_ERR_NOT_FOUND;
}

static int parse_hex16(const char *s, size_t len, uint16_t *out)
{
    unsigned v = 0;
    size_t i;

    if (len == 0 || len > 4)
        return 0;
    for (i = 0; i < len; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return 0;
    }
    *out = (uint16_t)v;
    return 1;
}

int usb_parse_vid_pid(const char *str, uint16_t *vid, uint16_t *pid)
{
    const char *colon;
    uint16_t v;
    uint16_t p;

    if (str == NULL || vid == NULL || pid == NULL)
        return RS_ERR_INVALID_ARG;
    colon = strchr(str, ':');
    if (colon == NULL || !parse_hex16(str, (size_t)(colon - str), &v) ||
        !parse_hex16(colon + 1, strlen(colon + 1), &p))
        return RS_ERR_INVALID_ARG;
    *vid = v;
    *pid = p;
    return RS_OK;
}

static const char *const k_method_names[] = { "auto", "usbdevfs", "authorized", "devnode" };

const char *usb_reset_method_name(usb_reset_method_t method)
{
    if ((int)method < 0 || (size_t)method >= sizeof(k_method_names) / sizeof(k_method_names[0]))
        return "?";
    return k_method_names[method];
}

int usb_reset_method_from_string(const char *str, usb_reset_method_t *out)
{
    size_t i;

    if (str == NULL || out == NULL)
        return RS_ERR_INVALID_ARG;
    for (i = 0; i < sizeof(k_method_names) / sizeof(k_method_names[0]); i++) {
        if (strcmp(str, k_method_names[i]) == 0) {
            *out = (usb_reset_method_t)i;
            return RS_OK;
        }
    }
    return RS_ERR_INVALID_ARG;
}
