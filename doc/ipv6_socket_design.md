# IPv6 デュアルスタックソケット設計

> ステータス: Phase 1 実装済み（`src/common/net_socket.[ch]`）。API 一覧は [function_reference.md](function_reference.md) の 1.1 節。

## 1. 方針

- 単一の `AF_INET6` UDP ソケットで IPv4 / IPv6 の双方を受け付ける。
- `setsockopt(IPPROTO_IPV6, IPV6_V6ONLY, 0)` を明示的に設定する（OS 既定値に依存しない）。
- IPv4 ピアは IPv4-mapped IPv6 アドレス（`::ffff:a.b.c.d`）として扱う。アドレス型 `net_addr_t` は
  常に `struct sockaddr_in6` を保持し、上位層は IPv4 / IPv6 を区別せずに扱える。
- 名前解決で IPv4 と IPv6 の両方が得られた場合は IPv6 を優先する（IPv6 ネイティブ化の方針）。

## 2. プラットフォーム差異と吸収方法

| 項目 | Linux | Windows 10 | 吸収方法 |
|---|---|---|---|
| API | BSD sockets | Winsock2 | `net_init` / `net_cleanup` で `WSAStartup` / `WSACleanup` |
| ソケット型 | `int` | `SOCKET` | `net_os_socket_t` |
| エラー取得 | `errno` | `WSAGetLastError()` | `net_last_os_error()`（スレッドローカル） |
| `IPV6_V6ONLY` 既定値 | 0（`net.ipv6.bindv6only` 依存） | 1 | 常に 0 を明示設定 |
| クローズ | `close()` | `closesocket()` | `net_udp_close` |
| 受信待機 | `poll()` | `select()` | `net_udp_recv` の `timeout_ms`、複数ソケットは `net_udp_wait` |
| ICMP Port Unreachable | 未接続 UDP には通知されない | 以後の `recvfrom` が `WSAECONNRESET` で失敗 | Windows で `SIO_UDP_CONNRESET` を無効化 |
| 受信バッファ超過 | `MSG_TRUNC` で実長を取得 | `WSAEMSGSIZE` | どちらも `RS_ERR_TRUNCATED` |
| シグナル割り込み | `EINTR` | ― | 残りタイムアウト時間内で再試行 |

ソケットはブロッキングモードで使用する。Linux では `poll` が readable を返した後でもチェックサム不正
パケットの破棄等で `recvfrom` がブロックしうるため、`MSG_DONTWAIT` を付け `EAGAIN` 時は待機に戻る。

## 3. パケットサイズ

| 項目 | 値 |
|---|---|
| IPv6 最小 MTU | 1280 バイト |
| 最大 UDP ペイロード (`NET_MAX_UDP_PAYLOAD`) | 1232 バイト（1280 − IPv6 ヘッダ 40 − UDP ヘッダ 8） |

`net_udp_send` は上限超過時に送信せず `RS_ERR_TOO_LARGE` を返す（分割送信は行わない）。
上限は拡張ヘッダなしを前提とする。暗号化等のオーバーヘッドは上位層でこの値に収めること。

## 4. ソケットオプション

| オプション | 設定値 | 目的 |
|---|---|---|
| `IPV6_V6ONLY` | 0（必須） | デュアルスタック化 |
| `SO_RCVBUF` / `SO_SNDBUF` | `net_udp_opts_t` で指定（既定: OS 既定値） | 音声バースト吸収 |
| `IPV6_TCLASS` / `IP_TOS` | `net_udp_opts_t.traffic_class`（既定: 未設定） | 音声パケットの DSCP 優先制御。ベストエフォート（Windows では QoS2 API 経由でないと反映されない場合がある） |
| `SIO_UDP_CONNRESET` (Win) | FALSE | 上記 ICMP 起因の受信エラー抑止 |
| `SO_REUSEADDR` | 設定しない | UDP では TIME_WAIT がなく不要。Windows ではポート横取りを許すため使用しない |

## 5. アドレス表現

| 項目 | 仕様 |
|---|---|
| 内部表現 | `net_addr_t`（`struct sockaddr_in6`、ネットワークバイトオーダー） |
| any アドレス | `net_addr_resolve(&a, NULL, port)` → `[::]` |
| 文字列（IPv6） | `[2001:db8::1]:50001` |
| 文字列（スコープ付き） | `[fe80::1%2]:50001`（スコープ ID は数値表記） |
| 文字列（IPv4-mapped） | `192.0.2.1:50001`（ログ可読性のため IPv4 表記に戻す） |
| 比較 | `net_addr_equal`: アドレス 16 バイト・ポート・スコープ ID の一致 |

## 6. エラー処理

| 事象 | 戻り値 |
|---|---|
| 引数不正 | `RS_ERR_INVALID_ARG` |
| 名前解決失敗 | `RS_ERR_ADDRESS`（`net_last_os_error()` = `getaddrinfo` の戻り値） |
| OS API 失敗 | `RS_ERR_SYSTEM`（`net_last_os_error()` = errno / WSA エラー） |
| 受信タイムアウト | `RS_ERR_TIMEOUT` |
| 送信長超過 | `RS_ERR_TOO_LARGE` |
| 受信バッファ不足 | `RS_ERR_TRUNCATED`（データは cap まで格納、送信元アドレスは有効） |

## 7. テスト項目（`tests/test_net_socket.c`）

| No. | 内容 | 期待結果 |
|---|---|---|
| 1 | アドレス文字列化（IPv6 / IPv4-mapped / any / バッファ不足） | 規定の形式、不足時 `RS_ERR_TRUNCATED` |
| 2 | `::1` 宛の送受信 | 受信成功、送信元は非 mapped、ポート一致 |
| 3 | `127.0.0.1` 宛の送受信（同一ソケット） | 受信成功、送信元は IPv4-mapped |
| 4 | 1232 バイト送受信 | 成功 |
| 5 | 1233 バイト送信 | `RS_ERR_TOO_LARGE` |
| 6 | 100 バイトを 10 バイトバッファで受信 | `RS_ERR_TRUNCATED`、長さ 10 |
| 7 | データなしで 100ms 待機 / 即時 | `RS_ERR_TIMEOUT`、経過時間 ≥ 80ms |
