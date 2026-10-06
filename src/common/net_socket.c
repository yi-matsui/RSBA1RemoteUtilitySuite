/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "net_socket.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "rs_error.h"
#include "rs_platform.h"
#include "rs_time.h"

#ifdef _WIN32
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#define NET_INVALID_SOCKET INVALID_SOCKET
#define net_os_close closesocket
static int os_error(void) { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#define NET_INVALID_SOCKET (-1)
#define net_os_close close
static int os_error(void) { return errno; }
#endif

/* 内部用: EINTR / EAGAIN 等で再試行が必要なことを示す（公開エラーコードとは重複しない正値） */
#define NET_RETRY 1

static RS_THREAD_LOCAL int g_last_os_error;

#ifdef _WIN32
static int g_init_count;
#endif

static int fail_system(void)
{
    g_last_os_error = os_error();
    return RS_ERR_SYSTEM;
}

int net_init(void)
{
#ifdef _WIN32
    if (g_init_count == 0) {
        WSADATA wsa;
        int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            g_last_os_error = rc;
            return RS_ERR_SYSTEM;
        }
    }
    g_init_count++;
#endif
    return RS_OK;
}

void net_cleanup(void)
{
#ifdef _WIN32
    if (g_init_count > 0 && --g_init_count == 0)
        WSACleanup();
#endif
}

int net_last_os_error(void)
{
    return g_last_os_error;
}

/* ---- アドレス ---------------------------------------------------------- */

int net_addr_resolve(net_addr_t *out, const char *host, uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    const struct addrinfo *ai;
    const struct addrinfo *pick = NULL;
    int rc;

    if (out == NULL)
        return RS_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->sa.sin6_family = AF_INET6;
    out->sa.sin6_port = htons(port);

    if (host == NULL || host[0] == '\0')
        return RS_OK; /* in6addr_any (全ゼロ) */

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        g_last_os_error = rc;
        return RS_ERR_ADDRESS;
    }

    for (ai = res; ai != NULL && pick == NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET6)
            pick = ai;
    }
    for (ai = res; ai != NULL && pick == NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET)
            pick = ai;
    }

    if (pick == NULL) {
        freeaddrinfo(res);
        g_last_os_error = 0;
        return RS_ERR_ADDRESS;
    }

    if (pick->ai_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)pick->ai_addr;
        out->sa.sin6_addr = s6->sin6_addr;
        out->sa.sin6_scope_id = s6->sin6_scope_id;
    } else {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)(const void *)pick->ai_addr;
        uint8_t *b = out->sa.sin6_addr.s6_addr;
        b[10] = 0xff;
        b[11] = 0xff;
        memcpy(b + 12, &s4->sin_addr, 4);
    }

    freeaddrinfo(res);
    return RS_OK;
}

int net_addr_is_v4mapped(const net_addr_t *addr)
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

    if (addr == NULL)
        return 0;
    return memcmp(addr->sa.sin6_addr.s6_addr, prefix, sizeof(prefix)) == 0;
}

uint16_t net_addr_port(const net_addr_t *addr)
{
    return addr != NULL ? ntohs(addr->sa.sin6_port) : 0;
}

int net_addr_equal(const net_addr_t *a, const net_addr_t *b)
{
    if (a == NULL || b == NULL)
        return 0;
    return memcmp(a->sa.sin6_addr.s6_addr, b->sa.sin6_addr.s6_addr, 16) == 0 &&
           a->sa.sin6_port == b->sa.sin6_port &&
           a->sa.sin6_scope_id == b->sa.sin6_scope_id;
}

int net_addr_format(const net_addr_t *addr, char *buf, size_t cap)
{
    char host[INET6_ADDRSTRLEN];
    unsigned port;
    int n;

    if (addr == NULL || buf == NULL || cap == 0)
        return RS_ERR_INVALID_ARG;

    port = net_addr_port(addr);
    if (net_addr_is_v4mapped(addr)) {
        if (inet_ntop(AF_INET, addr->sa.sin6_addr.s6_addr + 12, host, sizeof(host)) == NULL)
            return fail_system();
        n = snprintf(buf, cap, "%s:%u", host, port);
    } else {
        if (inet_ntop(AF_INET6, &addr->sa.sin6_addr, host, sizeof(host)) == NULL)
            return fail_system();
        if (addr->sa.sin6_scope_id != 0)
            n = snprintf(buf, cap, "[%s%%%lu]:%u", host,
                         (unsigned long)addr->sa.sin6_scope_id, port);
        else
            n = snprintf(buf, cap, "[%s]:%u", host, port);
    }

    if (n < 0)
        return RS_ERR_INVALID_ARG;
    if ((size_t)n >= cap)
        return RS_ERR_TRUNCATED;
    return RS_OK;
}

/* ---- UDP ソケット ------------------------------------------------------ */

void net_udp_opts_default(net_udp_opts_t *opts)
{
    if (opts == NULL)
        return;
    opts->rcvbuf_bytes = 0;
    opts->sndbuf_bytes = 0;
    opts->traffic_class = -1;
}

static int set_int_opt(net_os_socket_t fd, int level, int name, int value)
{
    return setsockopt(fd, level, name, (const char *)&value, (int)sizeof(value));
}

int net_udp_open(net_udp_t *out, const net_addr_t *bind_addr, const net_udp_opts_t *opts)
{
    net_udp_opts_t o;
    net_addr_t any;
    net_os_socket_t fd;

    if (out == NULL)
        return RS_ERR_INVALID_ARG;
    out->fd = NET_INVALID_SOCKET;

    if (opts != NULL)
        o = *opts;
    else
        net_udp_opts_default(&o);

    if (bind_addr == NULL) {
        net_addr_resolve(&any, NULL, 0);
        bind_addr = &any;
    }

    fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == NET_INVALID_SOCKET)
        return fail_system();

    /* OS 既定値（Windows = 1, Linux = bindv6only 依存）に頼らず明示的に 0 にする */
    if (set_int_opt(fd, IPPROTO_IPV6, IPV6_V6ONLY, 0) != 0)
        goto fail;

    if (o.rcvbuf_bytes > 0 && set_int_opt(fd, SOL_SOCKET, SO_RCVBUF, o.rcvbuf_bytes) != 0)
        goto fail;
    if (o.sndbuf_bytes > 0 && set_int_opt(fd, SOL_SOCKET, SO_SNDBUF, o.sndbuf_bytes) != 0)
        goto fail;

    if (o.traffic_class >= 0 && o.traffic_class <= 255) {
        /* ベストエフォート: Windows では QoS API 経由でないと反映されないことが多い */
#ifdef IPV6_TCLASS
        (void)set_int_opt(fd, IPPROTO_IPV6, IPV6_TCLASS, o.traffic_class);
#endif
#ifdef IP_TOS
        (void)set_int_opt(fd, IPPROTO_IP, IP_TOS, o.traffic_class);
#endif
    }

#ifdef _WIN32
    {
        /* 送信先から ICMP Port Unreachable が返ると以後の recvfrom が
         * WSAECONNRESET で失敗する Windows 固有の挙動を無効化する */
        BOOL report = FALSE;
        DWORD bytes = 0;
        if (WSAIoctl(fd, SIO_UDP_CONNRESET, &report, sizeof(report),
                     NULL, 0, &bytes, NULL, NULL) == SOCKET_ERROR)
            goto fail;
    }
#endif

    if (bind(fd, (const struct sockaddr *)&bind_addr->sa, (int)sizeof(bind_addr->sa)) != 0)
        goto fail;

    out->fd = fd;
    return RS_OK;

fail:
    g_last_os_error = os_error();
    net_os_close(fd);
    return RS_ERR_SYSTEM;
}

void net_udp_close(net_udp_t *sock)
{
    if (sock == NULL || sock->fd == NET_INVALID_SOCKET)
        return;
    net_os_close(sock->fd);
    sock->fd = NET_INVALID_SOCKET;
}

int net_udp_local_addr(const net_udp_t *sock, net_addr_t *out)
{
    socklen_t len;

    if (sock == NULL || out == NULL || sock->fd == NET_INVALID_SOCKET)
        return RS_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    len = (socklen_t)sizeof(out->sa);
    if (getsockname(sock->fd, (struct sockaddr *)&out->sa, &len) != 0)
        return fail_system();
    return RS_OK;
}

int net_udp_send(net_udp_t *sock, const void *buf, size_t len, const net_addr_t *to)
{
    if (sock == NULL || to == NULL || (buf == NULL && len > 0) || sock->fd == NET_INVALID_SOCKET)
        return RS_ERR_INVALID_ARG;
    if (len > NET_MAX_UDP_PAYLOAD)
        return RS_ERR_TOO_LARGE;

    for (;;) {
#ifdef _WIN32
        int n = sendto(sock->fd, (const char *)buf, (int)len, 0,
                       (const struct sockaddr *)&to->sa, (int)sizeof(to->sa));
        if (n == SOCKET_ERROR)
            return fail_system();
#else
        ssize_t n = sendto(sock->fd, buf, len, 0,
                           (const struct sockaddr *)&to->sa, sizeof(to->sa));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return fail_system();
        }
#endif
        return (size_t)n == len ? RS_OK : RS_ERR_IO;
    }
}

/* 読み込み可能になるまで待つ。RS_OK / RS_ERR_TIMEOUT / RS_ERR_SYSTEM / NET_RETRY */
static int wait_readable(net_os_socket_t fd, int timeout_ms)
{
#ifdef _WIN32
    fd_set rd;
    struct timeval tv;
    struct timeval *ptv = NULL;
    int rc;

    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    rc = select(0, &rd, NULL, NULL, ptv);
    if (rc == SOCKET_ERROR)
        return fail_system();
    return rc == 0 ? RS_ERR_TIMEOUT : RS_OK;
#else
    struct pollfd pfd;
    int rc;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    rc = poll(&pfd, 1, timeout_ms < 0 ? -1 : timeout_ms);
    if (rc < 0)
        return errno == EINTR ? NET_RETRY : fail_system();
    return rc == 0 ? RS_ERR_TIMEOUT : RS_OK;
#endif
}

int net_udp_wait(net_udp_t *const socks[], size_t n, int timeout_ms, int ready[])
{
    size_t i;

    if (socks == NULL || ready == NULL || n == 0 || n > NET_WAIT_MAX)
        return RS_ERR_INVALID_ARG;
    for (i = 0; i < n; i++) {
        ready[i] = 0;
        if (socks[i] == NULL || socks[i]->fd == NET_INVALID_SOCKET)
            return RS_ERR_INVALID_ARG;
    }

#ifdef _WIN32
    {
        fd_set rd;
        struct timeval tv;
        struct timeval *ptv = NULL;
        int rc;

        FD_ZERO(&rd);
        for (i = 0; i < n; i++)
            FD_SET(socks[i]->fd, &rd);
        if (timeout_ms >= 0) {
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            ptv = &tv;
        }
        rc = select(0, &rd, NULL, NULL, ptv);
        if (rc == SOCKET_ERROR)
            return fail_system();
        if (rc == 0)
            return RS_ERR_TIMEOUT;
        for (i = 0; i < n; i++)
            ready[i] = FD_ISSET(socks[i]->fd, &rd) ? 1 : 0;
        return RS_OK;
    }
#else
    {
        struct pollfd pfd[NET_WAIT_MAX];
        int rc;

        for (i = 0; i < n; i++) {
            pfd[i].fd = socks[i]->fd;
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
        }
        do {
            rc = poll(pfd, (nfds_t)n, timeout_ms < 0 ? -1 : timeout_ms);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            return fail_system();
        if (rc == 0)
            return RS_ERR_TIMEOUT;
        for (i = 0; i < n; i++)
            ready[i] = (pfd[i].revents & (POLLIN | POLLERR)) ? 1 : 0;
        return RS_OK;
    }
#endif
}

/* 1 回だけ recvfrom する。RS_OK / RS_ERR_TRUNCATED / RS_ERR_SYSTEM / NET_RETRY */
static int recv_once(net_udp_t *sock, void *buf, size_t cap, size_t *out_len, net_addr_t *from)
{
    struct sockaddr_in6 sa;
    socklen_t salen = (socklen_t)sizeof(sa);
    int truncated = 0;

    memset(&sa, 0, sizeof(sa));
#ifdef _WIN32
    {
        int icap = cap > (size_t)INT_MAX ? INT_MAX : (int)cap;
        int n = recvfrom(sock->fd, (char *)buf, icap, 0, (struct sockaddr *)&sa, &salen);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEMSGSIZE) {
                *out_len = (size_t)icap;
                truncated = 1;
            } else if (e == WSAEWOULDBLOCK || e == WSAEINTR) {
                return NET_RETRY;
            } else {
                g_last_os_error = e;
                return RS_ERR_SYSTEM;
            }
        } else {
            *out_len = (size_t)n;
        }
    }
#else
    {
        /* MSG_TRUNC: 切り詰め前の実データ長を返させる。MSG_DONTWAIT: poll 後の偽陽性対策 */
        ssize_t n = recvfrom(sock->fd, buf, cap, MSG_TRUNC | MSG_DONTWAIT,
                             (struct sockaddr *)&sa, &salen);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                return NET_RETRY;
            return fail_system();
        }
        if ((size_t)n > cap) {
            *out_len = cap;
            truncated = 1;
        } else {
            *out_len = (size_t)n;
        }
    }
#endif

    if (from != NULL) {
        memset(from, 0, sizeof(*from));
        if (sa.sin6_family == AF_INET6)
            from->sa = sa;
    }
    return truncated ? RS_ERR_TRUNCATED : RS_OK;
}

int net_udp_recv(net_udp_t *sock, void *buf, size_t cap, size_t *out_len,
                 net_addr_t *from, int timeout_ms)
{
    uint64_t deadline = 0;
    int remaining = timeout_ms;

    if (sock == NULL || buf == NULL || cap == 0 || out_len == NULL ||
        sock->fd == NET_INVALID_SOCKET)
        return RS_ERR_INVALID_ARG;

    *out_len = 0;
    if (timeout_ms > 0)
        deadline = rs_time_monotonic_ms() + (uint64_t)timeout_ms;

    for (;;) {
        int rc = wait_readable(sock->fd, remaining);
        if (rc == RS_OK)
            rc = recv_once(sock, buf, cap, out_len, from);
        if (rc != NET_RETRY)
            return rc;

        if (timeout_ms == 0)
            return RS_ERR_TIMEOUT;
        if (timeout_ms > 0) {
            uint64_t now = rs_time_monotonic_ms();
            if (now >= deadline)
                return RS_ERR_TIMEOUT;
            remaining = (int)(deadline - now);
        }
    }
}
