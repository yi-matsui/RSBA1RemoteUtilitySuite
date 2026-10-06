/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "civ.h"

#include <string.h>

size_t civ_build_frame(uint8_t *buf, size_t cap, uint8_t to, uint8_t from,
                       const uint8_t *body, size_t body_len)
{
    size_t len = body_len + 5;
    size_t i;

    if (buf == NULL || (body == NULL && body_len > 0) || cap < len || len > CIV_MAX_FRAME)
        return 0;
    for (i = 0; i < body_len; i++)
        if (body[i] == CIV_EOM || body[i] == CIV_PREAMBLE)
            return 0;

    buf[0] = CIV_PREAMBLE;
    buf[1] = CIV_PREAMBLE;
    buf[2] = to;
    buf[3] = from;
    if (body_len > 0)
        memcpy(buf + 4, body, body_len);
    buf[len - 1] = CIV_EOM;
    return len;
}

size_t civ_build_ptt_off(uint8_t *buf, size_t cap, uint8_t radio_addr, uint8_t ctrl_addr)
{
    static const uint8_t body[] = { 0x1C, 0x00, 0x00 };
    return civ_build_frame(buf, cap, radio_addr, ctrl_addr, body, sizeof(body));
}

int civ_validate_frames(const uint8_t *data, size_t len)
{
    size_t i = 0;

    if (data == NULL || len == 0)
        return 0;
    while (i < len) {
        size_t start = i;
        if (len - i < CIV_MIN_FRAME || data[i] != CIV_PREAMBLE || data[i + 1] != CIV_PREAMBLE)
            return 0;
        i += 2;
        while (i < len && data[i] != CIV_EOM) {
            if (data[i] == CIV_PREAMBLE)
                return 0;
            i++;
        }
        if (i == len)
            return 0; /* FD なし */
        i++;
        if (i - start < CIV_MIN_FRAME || i - start > CIV_MAX_FRAME)
            return 0;
    }
    return 1;
}

void civ_framer_init(civ_framer_t *f)
{
    memset(f, 0, sizeof(*f));
}

static void discard(civ_framer_t *f, size_t n)
{
    f->garbage_total += n;
    f->garbage_pending += n;
}

size_t civ_framer_push(civ_framer_t *f, const uint8_t *data, size_t len,
                       civ_frame_cb cb, void *user)
{
    size_t emitted = 0;
    size_t i;

    if (f == NULL || (data == NULL && len > 0))
        return 0;

    for (i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (f->len == 0) {
            if (b == CIV_PREAMBLE)
                f->buf[f->len++] = b;
            else
                discard(f, 1);
            continue;
        }
        if (f->len == 1) {
            if (b == CIV_PREAMBLE) {
                f->buf[f->len++] = b;
            } else {
                discard(f, 2);
                f->len = 0;
            }
            continue;
        }
        if (b == CIV_PREAMBLE) {
            if (f->len == 2)
                continue; /* 3 個目以降のプリアンブル */
            /* フレーム途中の FE: それまでを破棄して新しいフレームを開始 */
            discard(f, f->len);
            f->buf[0] = b;
            f->len = 1;
            continue;
        }

        f->buf[f->len++] = b;
        if (b == CIV_EOM) {
            if (f->len >= CIV_MIN_FRAME) {
                if (cb != NULL)
                    cb(f->buf, f->len, user);
                f->frames++;
                f->garbage_pending = 0;
                emitted++;
            } else {
                discard(f, f->len);
            }
            f->len = 0;
        } else if (f->len >= CIV_MAX_FRAME) {
            discard(f, f->len);
            f->len = 0;
        }
    }
    return emitted;
}
