/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * net_socket のループバックテスト（IPv6 / IPv4-mapped の双方を 1 ソケットで受信）。
 */
#include <string.h>

#include "net_socket.h"
#include "rs_error.h"
#include "rs_time.h"
#include "test_util.h"

static void test_format(void)
{
    net_addr_t a;
    char buf[NET_ADDR_STRLEN];
    char tiny[8];

    CHECK_RC(net_addr_resolve(&a, "::1", 50001), RS_OK);
    CHECK_RC(net_addr_format(&a, buf, sizeof(buf)), RS_OK);
    CHECK(strcmp(buf, "[::1]:50001") == 0);
    CHECK(!net_addr_is_v4mapped(&a));

    CHECK_RC(net_addr_resolve(&a, "127.0.0.1", 50002), RS_OK);
    CHECK_RC(net_addr_format(&a, buf, sizeof(buf)), RS_OK);
    CHECK(strcmp(buf, "127.0.0.1:50002") == 0);
    CHECK(net_addr_is_v4mapped(&a));
    CHECK(net_addr_port(&a) == 50002);

    CHECK_RC(net_addr_resolve(&a, "2001:db8::10", 50003), RS_OK);
    CHECK_RC(net_addr_format(&a, buf, sizeof(buf)), RS_OK);
    CHECK(strcmp(buf, "[2001:db8::10]:50003") == 0);
    CHECK_RC(net_addr_format(&a, tiny, sizeof(tiny)), RS_ERR_TRUNCATED);

    CHECK_RC(net_addr_resolve(&a, NULL, 0), RS_OK);
    CHECK_RC(net_addr_format(&a, buf, sizeof(buf)), RS_OK);
    CHECK(strcmp(buf, "[::]:0") == 0);
}

static void test_roundtrip(net_udp_t *tx, net_udp_t *rx, const char *host, uint16_t port,
                           int expect_mapped)
{
    static const char msg[] = "hello rs-ba1";
    net_addr_t to;
    net_addr_t from;
    net_addr_t tx_local;
    char buf[64];
    size_t len = 0;

    CHECK_RC(net_addr_resolve(&to, host, port), RS_OK);
    CHECK_RC(net_udp_send(tx, msg, sizeof(msg), &to), RS_OK);
    CHECK_RC(net_udp_recv(rx, buf, sizeof(buf), &len, &from, 1000), RS_OK);
    CHECK(len == sizeof(msg));
    CHECK(memcmp(buf, msg, sizeof(msg)) == 0);
    CHECK(net_addr_is_v4mapped(&from) == expect_mapped);

    CHECK_RC(net_udp_local_addr(tx, &tx_local), RS_OK);
    CHECK(net_addr_port(&from) == net_addr_port(&tx_local));
}

static void test_limits(net_udp_t *tx, net_udp_t *rx, uint16_t port)
{
    static unsigned char big[NET_MAX_UDP_PAYLOAD + 1];
    unsigned char buf[NET_MAX_UDP_PAYLOAD + 16];
    net_addr_t to;
    size_t len = 0;
    uint64_t t0;

    memset(big, 0xA5, sizeof(big));
    CHECK_RC(net_addr_resolve(&to, "::1", port), RS_OK);

    /* 上限ちょうどは送受信できる */
    CHECK_RC(net_udp_send(tx, big, NET_MAX_UDP_PAYLOAD, &to), RS_OK);
    CHECK_RC(net_udp_recv(rx, buf, sizeof(buf), &len, NULL, 1000), RS_OK);
    CHECK(len == NET_MAX_UDP_PAYLOAD);

    /* 上限超過は送信拒否 */
    CHECK_RC(net_udp_send(tx, big, NET_MAX_UDP_PAYLOAD + 1, &to), RS_ERR_TOO_LARGE);

    /* 受信バッファ不足は切り詰め、残りは破棄される */
    CHECK_RC(net_udp_send(tx, big, 100, &to), RS_OK);
    CHECK_RC(net_udp_recv(rx, buf, 10, &len, NULL, 1000), RS_ERR_TRUNCATED);
    CHECK(len == 10);

    /* データなしはタイムアウト */
    t0 = rs_time_monotonic_ms();
    CHECK_RC(net_udp_recv(rx, buf, sizeof(buf), &len, NULL, 100), RS_ERR_TIMEOUT);
    CHECK(rs_time_monotonic_ms() - t0 >= 80);
    CHECK_RC(net_udp_recv(rx, buf, sizeof(buf), &len, NULL, 0), RS_ERR_TIMEOUT);
}

int main(void)
{
    net_udp_t rx;
    net_udp_t tx;
    net_addr_t rx_local;
    uint16_t port;

    CHECK_RC(net_init(), RS_OK);

    test_format();

    CHECK_RC(net_udp_open(&rx, NULL, NULL), RS_OK);
    CHECK_RC(net_udp_open(&tx, NULL, NULL), RS_OK);
    CHECK_RC(net_udp_local_addr(&rx, &rx_local), RS_OK);
    port = net_addr_port(&rx_local);
    CHECK(port != 0);

    test_roundtrip(&tx, &rx, "::1", port, 0);
    test_roundtrip(&tx, &rx, "127.0.0.1", port, 1);
    test_limits(&tx, &rx, port);

    CHECK_RC(net_udp_send(&tx, "x", 1, NULL), RS_ERR_INVALID_ARG);

    net_udp_close(&tx);
    net_udp_close(&rx);
    net_udp_close(&rx); /* 二重クローズは無害 */
    net_cleanup();
    return TEST_RESULT();
}
