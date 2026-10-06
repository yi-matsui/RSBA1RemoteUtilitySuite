/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * IPv6 デュアルスタック UDP ソケット。
 * 単一の AF_INET6 ソケットで IPv4 / IPv6 を扱い、IPv4 ピアは IPv4-mapped
 * アドレス (::ffff:a.b.c.d) として表現する。設計: doc/ipv6_socket_design.md
 */
#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET net_os_socket_t;
#else
#include <netinet/in.h>
#include <sys/socket.h>
typedef int net_os_socket_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* IPv6 最小 MTU と、そこから IPv6 ヘッダ (40) と UDP ヘッダ (8) を除いた最大ペイロード */
#define NET_IPV6_MIN_MTU     1280
#define NET_MAX_UDP_PAYLOAD  1232

/* net_addr_format() の出力に十分なバッファ長 */
#define NET_ADDR_STRLEN      80

/* ソケットアドレス。常に AF_INET6（IPv4 は mapped 形式）で保持する。 */
typedef struct net_addr {
    struct sockaddr_in6 sa;
} net_addr_t;

typedef struct net_udp_opts {
    int rcvbuf_bytes;   /* SO_RCVBUF。0 以下で OS 既定値 */
    int sndbuf_bytes;   /* SO_SNDBUF。0 以下で OS 既定値 */
    int traffic_class;  /* IPv6 Traffic Class / IPv4 TOS (0-255)。負値で設定しない。ベストエフォート */
} net_udp_opts_t;

typedef struct net_udp {
    net_os_socket_t fd;
} net_udp_t;

/* ソケット API の初期化／終了（Windows では WSAStartup/WSACleanup）。
 * 参照カウント方式。メインスレッドから対で呼ぶこと。 */
int  net_init(void);
void net_cleanup(void);

/* 直前に RS_ERR_SYSTEM / RS_ERR_ADDRESS を返した呼び出しの OS エラーコード（スレッドローカル）。
 * RS_ERR_SYSTEM: errno / WSAGetLastError()、RS_ERR_ADDRESS: getaddrinfo() の戻り値。 */
int net_last_os_error(void);

/* host（数値アドレスまたはホスト名、"fe80::1%eth0" のスコープ表記可）を解決する。
 * host が NULL または "" の場合は any アドレス (::)。IPv6 の結果を優先する。 */
int net_addr_resolve(net_addr_t *out, const char *host, uint16_t port);

/* "[2001:db8::1]:50001" / "192.0.2.1:50001"（mapped は IPv4 表記）形式で文字列化する。 */
int net_addr_format(const net_addr_t *addr, char *buf, size_t cap);

uint16_t net_addr_port(const net_addr_t *addr);
int      net_addr_is_v4mapped(const net_addr_t *addr);
int      net_addr_equal(const net_addr_t *a, const net_addr_t *b);

void net_udp_opts_default(net_udp_opts_t *opts);

/* デュアルスタック UDP ソケットを作成し bind_addr にバインドする。
 * bind_addr が NULL の場合は [::]:0（エフェメラルポート）。opts が NULL の場合は既定値。 */
int  net_udp_open(net_udp_t *out, const net_addr_t *bind_addr, const net_udp_opts_t *opts);
void net_udp_close(net_udp_t *sock);

int net_udp_local_addr(const net_udp_t *sock, net_addr_t *out);

/* len が NET_MAX_UDP_PAYLOAD を超える場合は送信せず RS_ERR_TOO_LARGE を返す。 */
int net_udp_send(net_udp_t *sock, const void *buf, size_t len, const net_addr_t *to);

/* 1 データグラムを受信する。timeout_ms: 負値 = 無期限、0 = 即時、正値 = ミリ秒。
 * 受信長が cap を超えた場合は cap バイトを格納し RS_ERR_TRUNCATED を返す（残りは破棄）。
 * from は NULL 可。 */
int net_udp_recv(net_udp_t *sock, void *buf, size_t cap, size_t *out_len,
                 net_addr_t *from, int timeout_ms);

/* 複数ソケットのいずれかが読み込み可能になるまで待つ。ready[i] に 0/1 を設定する。
 * RS_OK（1 つ以上可能）/ RS_ERR_TIMEOUT / RS_ERR_SYSTEM。n は NET_WAIT_MAX まで。 */
#define NET_WAIT_MAX 8
int net_udp_wait(net_udp_t *const socks[], size_t n, int timeout_ms, int ready[]);

#ifdef __cplusplus
}
#endif

#endif /* NET_SOCKET_H */
