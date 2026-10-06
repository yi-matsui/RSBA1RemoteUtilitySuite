/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "civ_link.h"

#include <string.h>

#include "net_socket.h"
#include "rs_error.h"

void civ_link_init(civ_link_t *link, int (*send)(void *user, const void *pkt, size_t len),
                   void *user)
{
    memset(link, 0, sizeof(*link));
    civ_framer_init(&link->framer);
    link->send = send;
    link->user = user;
}

void civ_link_attach(civ_link_t *link, serial_port_t *port)
{
    link->port = port;
    civ_framer_init(&link->framer);
    link->pending_len = 0;
}

void civ_link_start(civ_link_t *link, dch_role_t role, uint32_t session_id,
                    const uint8_t k_c2s[CTL_KEY_LEN], const uint8_t k_s2c[CTL_KEY_LEN])
{
    dch_init(&link->ch, DCH_CHANNEL_CIV, role, session_id, k_c2s, k_s2c);
    link->pending_len = 0;
}

void civ_link_stop(civ_link_t *link)
{
    dch_reset(&link->ch);
    link->pending_len = 0;
}

int civ_link_active(const civ_link_t *link)
{
    return link->ch.active;
}

static void flush_pending(civ_link_t *link)
{
    uint8_t pkt[NET_MAX_UDP_PAYLOAD];
    size_t len;

    if (link->pending_len == 0)
        return;
    if (dch_seal(&link->ch, DCH_TYPE_CIV, link->pending, link->pending_len, pkt, sizeof(pkt),
                 &len) == RS_OK &&
        link->send != NULL && link->send(link->user, pkt, len) == RS_OK)
        link->stats.packets_to_net++;
    link->pending_len = 0;
}

static void on_frame(const uint8_t *frame, size_t len, void *user)
{
    civ_link_t *link = user;

    if (!link->ch.active) {
        link->stats.dropped_inactive++;
        return;
    }
    if (link->pending_len + len > sizeof(link->pending))
        flush_pending(link);
    memcpy(link->pending + link->pending_len, frame, len);
    link->pending_len += len;
    link->stats.frames_to_net++;
}

int civ_link_poll(civ_link_t *link)
{
    uint8_t buf[512];
    int rounds;

    if (link->port == NULL)
        return RS_OK;

    /* 1 回の呼び出しで読む量を制限し、他チャネルの処理を滞らせない */
    for (rounds = 0; rounds < 8; rounds++) {
        size_t got = 0;
        int rc = serial_read(link->port, buf, sizeof(buf), &got);
        if (rc != RS_OK) {
            link->stats.io_errors++;
            flush_pending(link);
            return rc;
        }
        if (got == 0)
            break;
        civ_framer_push(&link->framer, buf, got, on_frame, link);
    }
    flush_pending(link);
    return RS_OK;
}

int civ_link_handle_packet(civ_link_t *link, const uint8_t *pkt, size_t len, uint8_t *type)
{
    uint8_t payload[DCH_MAX_PAYLOAD];
    size_t plen = 0;
    uint8_t t = 0;
    int rc;

    rc = dch_open(&link->ch, pkt, len, &t, payload, sizeof(payload), &plen);
    if (rc != RS_OK)
        return rc;
    if (type != NULL)
        *type = t;

    if (t != DCH_TYPE_CIV)
        return RS_OK; /* BIND 等（認証済み） */

    if (!civ_validate_frames(payload, plen)) {
        link->stats.rejected_payload++;
        return RS_OK;
    }
    if (link->port == NULL) {
        link->stats.dropped_inactive++;
        return RS_OK;
    }
    rc = serial_write(link->port, payload, plen);
    if (rc == RS_ERR_TIMEOUT)
        link->stats.write_timeouts++;
    else if (rc != RS_OK)
        link->stats.io_errors++;
    else {
        size_t i;
        for (i = 0; i < plen; i++) /* 検査済みのため FD の数 = フレーム数 */
            if (payload[i] == CIV_EOM)
                link->stats.frames_from_net++;
    }
    return rc;
}

int civ_link_send_bind(civ_link_t *link)
{
    uint8_t pkt[DCH_OVERHEAD];
    size_t len;
    int rc = dch_seal(&link->ch, DCH_TYPE_BIND, NULL, 0, pkt, sizeof(pkt), &len);

    if (rc != RS_OK)
        return rc;
    return link->send != NULL ? link->send(link->user, pkt, len) : RS_ERR_INVALID_ARG;
}

int civ_link_write_raw(civ_link_t *link, const uint8_t *data, size_t len)
{
    if (link->port == NULL)
        return RS_ERR_NOT_FOUND;
    return serial_write(link->port, data, len);
}
