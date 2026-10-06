# 関数リファレンス

> ステータス: 最終版（Phase 5）。`src/` 配下の全ヘッダの公開関数を記載し、実装と同期済み
> （ヘッダの宣言 ⇔ 本書の表を機械的に照合して差分ゼロを確認）。

## 0. 共通規約

### 0.1 命名規則

| 対象 | 規則 | 例 |
|---|---|---|
| 公開関数 | `<モジュール接頭辞>_<動詞>` | `net_udp_open` |
| 型 | `<接頭辞>_<名詞>_t` | `net_udp_t` |
| 定数・マクロ | 大文字スネークケース | `NET_MAX_UDP_PAYLOAD` |

### 0.2 戻り値・エラー規約

| 項目 | 規約 |
|---|---|
| 戻り値型 | 原則 `int`（0 = 成功、負値 = エラーコード） |
| エラーコード定義 | `src/common/rs_error.h` で一元定義 |
| 出力引数 | ポインタ引数で返す。失敗時の内容は各関数の「エラー時の挙動」欄に従う |
| OS エラー詳細 | `RS_ERR_SYSTEM` 時の errno / WSAGetLastError() は各モジュールの `*_last_os_error()` で取得 |
| スレッド安全性 | 各関数の「備考」欄に明記 |

### 0.3 エラーコード一覧

| コード | 値 | 意味 |
|---|---|---|
| `RS_OK` | 0 | 成功 |
| `RS_ERR_INVALID_ARG` | -1 | 引数不正 |
| `RS_ERR_NO_MEMORY` | -2 | メモリ確保失敗 |
| `RS_ERR_SYSTEM` | -3 | OS API 失敗 |
| `RS_ERR_TIMEOUT` | -4 | タイムアウト |
| `RS_ERR_TOO_LARGE` | -5 | データ長が上限超過 |
| `RS_ERR_TRUNCATED` | -6 | 出力バッファ不足で切り詰め |
| `RS_ERR_ADDRESS` | -7 | アドレス解決失敗 |
| `RS_ERR_IO` | -8 | ファイル等の入出力失敗 |
| `RS_ERR_NOT_FOUND` | -9 | 対象（デバイス等）が見つからない |
| `RS_ERR_PERMISSION` | -10 | 権限不足（管理者 / root が必要） |
| `RS_ERR_BUSY` | -11 | 現在は実行できない（レート制限・使用中・拒否） |
| `RS_ERR_UNSUPPORTED` | -12 | 未対応、または安全上許可しない操作 |

### 0.4 表の書式

各モジュールの関数は以下の列で記載する。

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|

---

## 1. common

### 1.0 エラー・時刻ユーティリティ (`rs_error` / `rs_time`)

ヘッダ: `src/common/rs_error.h`, `src/common/rs_time.h`

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `rs_strerror` | `int status` | `const char *` | エラーコードの説明文字列（英語）を返す | 未知のコードは `"unknown error"` | 静的文字列。スレッドセーフ |
| `rs_time_monotonic_ms` | なし | `uint64_t` | 単調増加クロックをミリ秒で返す | なし | 起点は不定。Win: `GetTickCount64` / Linux: `CLOCK_MONOTONIC` |
| `rs_time_sleep_ms` | `uint32_t ms` | なし | 指定ミリ秒スリープ | なし | Linux はシグナル割り込み時も残り時間を眠る |
| `rs_shutdown_install` | `volatile sig_atomic_t *stop_flag` | なし | 終了要求で `*stop_flag = 1` にする。Linux: SIGINT / SIGTERM / SIGHUP、Windows: Ctrl+C / Ctrl+Break / コンソール閉鎖 / ログオフ / シャットダウン | なし | ヘッダ `rs_shutdown.h`。Windows の閉鎖・ログオフ・シャットダウンでは `rs_shutdown_done` まで最大 4.5 秒待ってから戻る（PTT 解除などの後始末を完了させるため） |
| `rs_shutdown_done` | なし | なし | 後始末の完了を通知 | なし | 実行本体が戻った直後に呼ぶ |

### 1.1 IPv6 デュアルスタック UDP ソケット (`net_socket`)

ヘッダ: `src/common/net_socket.h` ／ 設計詳細: [ipv6_socket_design.md](ipv6_socket_design.md)

**型・定数**

| 名前 | 内容 |
|---|---|
| `net_addr_t` | `struct sockaddr_in6` を保持。IPv4 は IPv4-mapped (`::ffff:a.b.c.d`) で表現 |
| `net_udp_t` | ソケットハンドル（`fd` メンバ） |
| `net_udp_opts_t` | `rcvbuf_bytes` / `sndbuf_bytes`（0 以下で OS 既定）、`traffic_class`（負値で未設定） |
| `NET_MAX_UDP_PAYLOAD` | 1232（= 1280 − 40 − 8） |
| `NET_ADDR_STRLEN` | `net_addr_format` 用の十分なバッファ長 (80) |

**関数**

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `net_init` | なし | `int` | ソケット API 初期化（Win: `WSAStartup(2.2)`）。参照カウント方式 | `RS_ERR_SYSTEM`（詳細は `net_last_os_error`） | Linux では何もしない。メインスレッドから `net_cleanup` と対で呼ぶ |
| `net_cleanup` | なし | なし | 参照カウントが 0 になったら `WSACleanup` | なし | 同上 |
| `net_last_os_error` | なし | `int` | 直前の `RS_ERR_SYSTEM` / `RS_ERR_ADDRESS` の OS エラーコード | なし | スレッドローカル。`RS_ERR_ADDRESS` 時は `getaddrinfo` の戻り値 |
| `net_addr_resolve` | `net_addr_t *out`, `const char *host`, `uint16_t port` | `int` | 数値アドレス・ホスト名・スコープ付き表記 (`fe80::1%eth0`) を解決。IPv6 結果を優先し、IPv4 は mapped 化。`host` が NULL/"" なら `[::]` | `RS_ERR_INVALID_ARG`（out が NULL）、`RS_ERR_ADDRESS`。`*out` は不定 | ホスト名の場合 DNS 問い合わせでブロックしうる |
| `net_addr_format` | `const net_addr_t *addr`, `char *buf`, `size_t cap` | `int` | `[2001:db8::1]:50001` / `192.0.2.1:50001`（mapped）/ `[fe80::1%2]:50001` 形式で文字列化 | `RS_ERR_INVALID_ARG`、`RS_ERR_TRUNCATED`（buf 不足） | `NET_ADDR_STRLEN` のバッファで常に成功 |
| `net_addr_port` | `const net_addr_t *addr` | `uint16_t` | ホストバイトオーダーのポート番号 | NULL なら 0 | |
| `net_addr_is_v4mapped` | `const net_addr_t *addr` | `int` | IPv4-mapped なら非 0 | NULL なら 0 | |
| `net_addr_equal` | `const net_addr_t *a`, `const net_addr_t *b` | `int` | アドレス・ポート・スコープ ID が一致すれば非 0 | どちらか NULL なら 0 | セッションのピア照合用 |
| `net_udp_opts_default` | `net_udp_opts_t *opts` | なし | 既定値（バッファ OS 既定、Traffic Class 未設定）を設定 | NULL なら何もしない | |
| `net_udp_open` | `net_udp_t *out`, `const net_addr_t *bind_addr`, `const net_udp_opts_t *opts` | `int` | `AF_INET6` UDP ソケット作成、`IPV6_V6ONLY=0`、オプション適用、Win では `SIO_UDP_CONNRESET` 無効化、bind | `RS_ERR_INVALID_ARG`、`RS_ERR_SYSTEM`。ソケットは閉じられ `out->fd` は無効値 | `bind_addr` NULL で `[::]:0`、`opts` NULL で既定値。Traffic Class はベストエフォート |
| `net_udp_close` | `net_udp_t *sock` | なし | ソケットを閉じ無効値にする | なし | NULL・二重クローズは無害 |
| `net_udp_local_addr` | `const net_udp_t *sock`, `net_addr_t *out` | `int` | `getsockname` でバインド先を取得 | `RS_ERR_INVALID_ARG`、`RS_ERR_SYSTEM` | ポート 0 でバインドした場合の実ポート取得に使う |
| `net_udp_send` | `net_udp_t *sock`, `const void *buf`, `size_t len`, `const net_addr_t *to` | `int` | 1 データグラムを送信 | `RS_ERR_TOO_LARGE`（len > 1232、送信しない）、`RS_ERR_SYSTEM`、`RS_ERR_IO`（部分送信） | Linux の `EINTR` は内部で再試行。同一ソケットへの並行送信は OS 依存のため避ける |
| `net_udp_recv` | `net_udp_t *sock`, `void *buf`, `size_t cap`, `size_t *out_len`, `net_addr_t *from`, `int timeout_ms` | `int` | 1 データグラムを受信。`timeout_ms`: 負=無期限、0=即時、正=ミリ秒 | `RS_ERR_TIMEOUT`（`*out_len`=0）、`RS_ERR_TRUNCATED`（cap バイト格納、残り破棄、`*from` 有効）、`RS_ERR_SYSTEM` | `from` は NULL 可。シグナル割り込み・偽の readable 通知は残り時間内で再試行 |
| `net_udp_wait` | `net_udp_t *const socks[]`, `size_t n`, `int timeout_ms`, `int ready[]` | `int` | 複数ソケット（最大 `NET_WAIT_MAX` = 8）のいずれかが読み込み可能になるまで待ち、`ready[i]` に 0/1 を設定（Win: `select` / Linux: `poll`） | `RS_ERR_TIMEOUT`、`RS_ERR_SYSTEM`、`RS_ERR_INVALID_ARG`（n = 0、n > 8、無効なソケット） | 実行本体のイベントループで使用 |

### 1.2 ロガー (`rs_log`)

ヘッダ: `src/common/rs_log.h`

出力形式（1 行 1 レコード、UTC）: `2026-10-06T12:34:56.789Z INFO  [module] message`

**型・定数**

| 名前 | 内容 |
|---|---|
| `rs_log_level_t` | `RS_LOG_LEVEL_TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` / `FATAL` / `OFF` |
| `rs_log_config_t` | `file_path`（NULL でファイルなし）、`level`、`to_stderr`、`max_file_bytes`（0 でローテーションなし）、`max_backups` |
| `rs_log_t` | 不透明型のロガーインスタンス |
| `RS_LOG_LINE_MAX` | 1 レコードの最大長 2048（改行含む）。超過分は末尾 `...` で切り詰め |
| `RS_LOG_TRACE` 〜 `RS_LOG_FATAL` | 既定ロガーへ出力するマクロ `(module, fmt, ...)` |

**関数**

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `rs_log_config_default` | `rs_log_config_t *cfg` | なし | 既定値（ファイルなし / INFO / stderr 出力 / 10 MiB / 5 世代）を設定 | NULL なら何もしない | |
| `rs_log_open` | `rs_log_t **out`, `const rs_log_config_t *cfg` | `int` | ロガーを生成し、ファイル指定時は追記モードで開く | `RS_ERR_INVALID_ARG`、`RS_ERR_NO_MEMORY`、`RS_ERR_IO`（ファイルを開けない）。`*out` は NULL | USB リセット専用ログ等は別インスタンスとして開く |
| `rs_log_close` | `rs_log_t *log` | なし | ファイルを閉じて解放。既定ロガーなら既定設定も解除 | なし | 他スレッドが使用中でないこと |
| `rs_log_set_level` | `rs_log_t *log`, `rs_log_level_t level` | なし | 出力しきい値を変更 | 範囲外の値は無視 | 実行中に変更可 |
| `rs_log_get_level` | `const rs_log_t *log` | `rs_log_level_t` | 現在のしきい値 | NULL なら `INFO` | |
| `rs_log_write` | `rs_log_t *log`, `rs_log_level_t level`, `const char *module`, `const char *fmt`, `...` | なし | printf 形式で 1 レコード出力。サイズ超過時はローテーション後に書き込み、毎回 flush | 書き込み失敗は黙って破棄。ファイルが開けていなければ次回書き込み時に再オープンを試行 | `log` NULL で既定ロガー、未設定なら INFO 以上を stderr へ。スレッドセーフ |
| `rs_log_vwrite` | `rs_log_t *log`, `rs_log_level_t level`, `const char *module`, `const char *fmt`, `va_list ap` | なし | `rs_log_write` の va_list 版 | 同上 | 同上 |
| `rs_log_set_default` | `rs_log_t *log` | なし | 既定ロガーを設定（NULL で解除） | なし | 起動時・終了時にメインスレッドから呼ぶ |
| `rs_log_get_default` | なし | `rs_log_t *` | 既定ロガー | 未設定なら NULL | |
| `rs_log_level_name` | `rs_log_level_t level` | `const char *` | `"INFO"` 等の名前 | 範囲外は `"?"` | |
| `rs_log_level_from_string` | `const char *str`, `rs_log_level_t *out` | `int` | `"trace"`〜`"off"` を大文字小文字不問で解析 | `RS_ERR_INVALID_ARG`（不明な文字列、`*out` 不変） | 設定ファイルの `logging.level` 用 |

ローテーション: `path` → `path.1` → … → `path.N`（N = `max_backups`）。`max_backups` = 0 なら旧ファイルを削除して新規作成。

### 1.3 暗号・認証

#### 1.3.1 暗号プリミティブ (`rs_crypto`)

ヘッダ: `src/common/rs_crypto.h`。外部ライブラリ非依存。テストベクタ: FIPS 180-4 / RFC 4231 / RFC 7914。

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `rs_sha256_init` | `rs_sha256_t *ctx` | なし | SHA-256 の逐次計算を開始 | なし | |
| `rs_sha256_update` | `rs_sha256_t *ctx`, `const void *data`, `size_t len` | なし | データを追加 | なし | 任意の長さで分割可 |
| `rs_sha256_final` | `rs_sha256_t *ctx`, `uint8_t out[32]` | なし | ダイジェストを出力 | なし | ctx を消去する |
| `rs_sha256` | `const void *data`, `size_t len`, `uint8_t out[32]` | なし | SHA-256 一括計算 | なし | |
| `rs_hmac_sha256_init` | `rs_hmac_sha256_t *ctx`, `const void *key`, `size_t key_len` | なし | HMAC-SHA256 の逐次計算を開始 | なし | 64 バイト超の鍵はハッシュして使用 |
| `rs_hmac_sha256_update` | `rs_hmac_sha256_t *ctx`, `const void *data`, `size_t len` | なし | データを追加 | なし | |
| `rs_hmac_sha256_final` | `rs_hmac_sha256_t *ctx`, `uint8_t out[32]` | なし | MAC を出力 | なし | ctx を消去する |
| `rs_hmac_sha256` | `const void *key`, `size_t key_len`, `const void *data`, `size_t len`, `uint8_t out[32]` | なし | HMAC-SHA256 一括計算 | なし | |
| `rs_pbkdf2_hmac_sha256` | `const void *password`, `size_t password_len`, `const void *salt`, `size_t salt_len`, `uint32_t iterations`, `uint8_t *out`, `size_t out_len` | `int` | PBKDF2-HMAC-SHA256（任意長出力） | `RS_ERR_INVALID_ARG`（iterations = 0 等） | 所要時間は iterations に比例 |
| `rs_chacha20_xor` | `const uint8_t key[32]`, `const uint8_t nonce[12]`, `uint32_t counter`, `const uint8_t *in`, `uint8_t *out`, `size_t len` | なし | ChaCha20（RFC 8439）の鍵ストリームを XOR（暗号化・復号は同じ操作） | なし | `in == out` 可。同じ (key, nonce) を再利用しないこと |
| `rs_crypto_random` | `void *buf`, `size_t len` | `int` | OS の暗号論的乱数（Win: `BCryptGenRandom` / Linux: `getrandom`） | `RS_ERR_SYSTEM` | スレッドセーフ |
| `rs_crypto_equal` | `const void *a`, `const void *b`, `size_t len` | `int` | 定数時間比較。一致で 1 | なし | 証明値・タグの比較に使用 |
| `rs_crypto_wipe` | `void *buf`, `size_t len` | なし | 最適化で除去されないゼロ埋め | なし | 鍵・パスワードの消去に使用 |

#### 1.3.2 制御プロトコル (`ctl_proto`)

ヘッダ: `src/common/ctl_proto.h` ／ 仕様: [protocol_spec.md](protocol_spec.md)

**型・定数**

| 名前 | 内容 |
|---|---|
| `ctl_type_t` | `CTL_HELLO`(1) / `CHALLENGE`(2) / `AUTH`(3) / `AUTH_OK`(4) / `AUTH_FAIL`(5) / `PING`(6) / `PONG`(7) / `DISCONNECT`(8) |
| `ctl_fail_reason_t` | `CTL_FAIL_CREDENTIALS`(1) / `CTL_FAIL_PROTOCOL`(2) |
| `ctl_disc_reason_t` | `CTL_DISC_NORMAL`(0) / `SHUTDOWN`(1) / `REPLACED`(2) / `TIMEOUT`(3) |
| `ctl_header_t` | `type`、`payload_len`、`session_id`、`seq` |
| `ctl_replay_t` | リプレイウィンドウ（`top` + 64 ビット `bitmap`） |
| `CTL_HEADER_LEN` / `CTL_TAG_LEN` / `CTL_KEY_LEN` | 16 / 16 / 32 |
| `CTL_NONCE_LEN` / `CTL_SALT_LEN` / `CTL_PROOF_LEN` / `CTL_USERNAME_MAX` | 16 / 16 / 32 / 32 |
| `CTL_MAX_PACKET_LEN` | 制御パケットの受信バッファに十分な長さ (128) |

**関数**

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `ctl_type_has_tag` | `uint8_t type` | `int` | タグ付き種別（AUTH_OK / PING / PONG / DISCONNECT）なら 1 | なし | |
| `ctl_encode` | `uint8_t *buf`, `size_t cap`, `const ctl_header_t *hdr`, `const void *payload`, `const uint8_t mac_key[32]`, `size_t *out_len` | `int` | ヘッダ（BE）＋ペイロードを組み立て、`mac_key` 指定時は HMAC タグを付加 | `RS_ERR_INVALID_ARG`（種別と長さ・鍵有無の不整合）、`RS_ERR_TRUNCATED`（cap 不足） | 種別ごとの固定長を強制 |
| `ctl_decode` | `const uint8_t *buf`, `size_t len`, `ctl_header_t *hdr`, `const uint8_t **payload` | `int` | magic・version・種別・長さ（タグ分を含む）を検証してヘッダを解析 | `RS_ERR_INVALID_ARG` | タグは検証しない |
| `ctl_verify_tag` | `const uint8_t *buf`, `size_t len`, `const uint8_t key[32]` | `int` | 末尾 16 バイトのタグを定数時間比較。一致で 1 | 不一致・引数不正で 0 | |
| `ctl_replay_init` | `ctl_replay_t *rp` | なし | ウィンドウ初期化 | なし | |
| `ctl_replay_check` | `const ctl_replay_t *rp`, `uint32_t seq` | `int` | 未受理かつウィンドウ内（最大値 − 63 以上）なら 1 | seq = 0 は常に 0 | 状態を変えない |
| `ctl_replay_update` | `ctl_replay_t *rp`, `uint32_t seq` | なし | 受理を記録（ウィンドウを前進） | なし | タグ検証成功後にのみ呼ぶ |
| `ctl_derive_password_key` | `const char *password`, `const uint8_t salt[16]`, `uint32_t iterations`, `uint8_t key[32]` | `int` | `K` = PBKDF2-HMAC-SHA256 | `RS_ERR_INVALID_ARG` | |
| `ctl_client_proof` | `key`, `cn`, `sn`, `const char *username`, `uint8_t proof[32]` | なし | HMAC(K, "RSBA1 client" ‖ cn ‖ sn ‖ username) | なし | |
| `ctl_server_proof` | `key`, `cn`, `sn`, `uint32_t session_id`, `uint8_t proof[32]` | なし | HMAC(K, "RSBA1 server" ‖ sn ‖ cn ‖ session_id) | なし | |
| `ctl_session_keys` | `key`, `cn`, `sn`, `uint32_t session_id`, `uint8_t k_c2s[32]`, `uint8_t k_s2c[32]` | なし | 方向別セッション鍵を導出 | なし | 鍵は送信されない |
| `ctl_put_u32` / `ctl_put_u64` / `ctl_get_u32` / `ctl_get_u64` | バッファ, 値 | — | ビッグエンディアン入出力 | なし | |
| `ctl_type_name` / `ctl_disc_reason_name` | 値 | `const char *` | ログ用の名前 | 未知は `"?"` | |

### 1.4 データチャネル封緘 (`dch`)

ヘッダ: `src/common/dch.h` ／ 仕様: [protocol_spec.md](protocol_spec.md) §8

| 名前 | 内容 |
|---|---|
| `DCH_TYPE_BIND` / `DCH_TYPE_CIV` / `DCH_TYPE_AUDIO` | `0x10` / `0x11` / `0x12` |
| `DCH_OVERHEAD` / `DCH_MAX_PAYLOAD` | 32（ヘッダ 16 + タグ 16）/ 1200 |
| `dch_channel_t` | `DCH_CHANNEL_CIV` / `DCH_CHANNEL_AUDIO`（鍵導出のラベル） |
| `dch_role_t` | `DCH_ROLE_SERVER`（送信 s2c / 受信 c2s）/ `DCH_ROLE_CLIENT`（逆） |
| `dch_t` | 有効フラグ、session_id、送受信の enc / mac 鍵、`tx_seq`、受信リプレイウィンドウ、`stats`（送受信数と破棄理由別の数） |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `dch_init` | `dch_t *ch`, `dch_channel_t channel`, `dch_role_t role`, `uint32_t session_id`, `const uint8_t k_c2s[32]`, `const uint8_t k_s2c[32]` | なし | セッション鍵からチャネル・方向別の enc / mac 鍵を導出して有効化。seq・ウィンドウを初期化 | なし | 統計は保持 |
| `dch_reset` | `dch_t *ch` | なし | 鍵を消去して無効化 | NULL は無害 | 統計は保持 |
| `dch_seal` | `dch_t *ch`, `uint8_t type`, `const void *payload`, `size_t len`, `uint8_t *out`, `size_t cap`, `size_t *out_len` | `int` | ヘッダ ‖ ChaCha20(ペイロード) ‖ HMAC タグを組み立てる | `RS_ERR_BUSY`（無効・seq 枯渇）、`RS_ERR_TOO_LARGE`（> 1200）、`RS_ERR_TRUNCATED`、`RS_ERR_INVALID_ARG` | 送信ごとに seq を 1 進める |
| `dch_open` | `dch_t *ch`, `const uint8_t *pkt`, `size_t len`, `uint8_t *type`, `uint8_t *payload`, `size_t cap`, `size_t *payload_len` | `int` | 形式 → session_id → リプレイ → タグ → 復号の順に検査して平文を返す | `RS_ERR_INVALID_ARG`（不合格、理由は `stats`）、`RS_ERR_BUSY`（無効） | タグ検証成功後にのみウィンドウを更新 |

### 1.5 オーディオコーデック (`audio_codec`)

ヘッダ: `src/common/audio_codec.h`。モノラル 16-bit PCM のフレーム単位で符号化・復号する抽象化。

| 名前 | 内容 |
|---|---|
| `audio_codec_id_t` | `AUDIO_CODEC_PCM16`(1) / `AUDIO_CODEC_OPUS`(2、予約) |
| `audio_codec_ops_t` | `name`、`encode`、`decode`、`conceal`（PLC、NULL 可）、`destroy`。新しいコーデックはこれを実装して `audio_codec_open` に登録する |
| `audio_codec_t` | `id`、`ops`、`st`（コーデック状態）、`sample_rate`、`frame_samples` |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `audio_codec_open` | `audio_codec_t *c`, `audio_codec_id_t id`, `uint32_t sample_rate`, `uint32_t frame_samples` | `int` | コーデックを初期化 | `RS_ERR_UNSUPPORTED`（Opus: 未組み込み）、`RS_ERR_INVALID_ARG` | |
| `audio_codec_close` | `audio_codec_t *c` | なし | 解放 | 未初期化は無害 | |
| `audio_codec_encode` | `audio_codec_t *c`, `const int16_t *pcm`, `size_t samples`, `uint8_t *out`, `size_t cap`, `size_t *out_len` | `int` | 1 フレームを符号化（PCM16: ビッグエンディアン 2 バイト/サンプル） | `RS_ERR_INVALID_ARG`（samples ≠ frame_samples）、`RS_ERR_TRUNCATED` | |
| `audio_codec_decode` | `audio_codec_t *c`, `const uint8_t *in`, `size_t len`, `int16_t *pcm`, `size_t cap_samples`, `size_t *out_samples` | `int` | 1 フレームを復号 | `RS_ERR_INVALID_ARG`（奇数長等）、`RS_ERR_TRUNCATED` | |
| `audio_codec_from_string` / `audio_codec_name` | `"pcm"` / `"opus"` ⇔ ID | `int` / `const char *` | 設定・ログ用 | 不明は `RS_ERR_INVALID_ARG` / `"?"` | |

### 1.6 適応型ジッターバッファ (`jbuf`)

ヘッダ: `src/common/jbuf.h` ／ 仕様: [protocol_spec.md](protocol_spec.md) §10.4。非スレッドセーフ。

| 名前 | 内容 |
|---|---|
| `jbuf_config_t` | `sample_rate`(48000)、`frame_samples`(480)、`min_delay_ms`(20)、`max_delay_ms`(80)、`capacity_frames`(32) |
| `jbuf_state_t` | `JBUF_BUFFERING`（目標まで貯めている。無音を出力）/ `JBUF_PLAYING` |
| `jbuf_stats_t` | `frames_received` / `_late` / `_duplicate` / `_concealed` / `_skipped`、`underruns`、`resets`、`samples_dropped` / `_inserted`（微調整量）、`target_ms`、`level_ms`、`jitter_ms`、`extra_ms`、`state` |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `jbuf_config_default` | `jbuf_config_t *cfg` | なし | 既定値を設定 | NULL なら何もしない | |
| `jbuf_create` | `jbuf_t **out`, `const jbuf_config_t *cfg` | `int` | バッファを確保 | `RS_ERR_INVALID_ARG`（min > max、容量 < 最大遅延 + 4 フレーム）、`RS_ERR_NO_MEMORY`。`*out` は NULL | |
| `jbuf_destroy` | `jbuf_t *jb` | なし | 解放 | NULL は無害 | |
| `jbuf_reset` | `jbuf_t *jb` | なし | 内容・推計値を破棄して BUFFERING へ | NULL は無害 | セッション開始時に呼ぶ。統計・`extra_ms` は保持 |
| `jbuf_put` | `jbuf_t *jb`, `uint32_t timestamp`, `const int16_t *pcm`, `size_t samples`, `uint64_t arrival_ms` | `int` | フレームを格納し、到着時刻からジッター・目標遅延を更新 | `RS_ERR_INVALID_ARG`（samples ≠ frame_samples、フレーム境界に揃わない timestamp）。遅着・重複は破棄して `RS_OK`（統計に計上） | timestamp は 32 ビット周回可 |
| `jbuf_get` | `jbuf_t *jb`, `int16_t *out`, `size_t samples` | `int` | 再生用に samples 個を必ず出力（BUFFERING 中は無音、欠落は補間、尽きたらアンダーラン）。オーバーラン時の読み飛ばし、±2 % / ±5 % の再生ポインタ微調整を行う | `RS_ERR_INVALID_ARG`（NULL） | 再生デバイスのクロックで呼ぶ |
| `jbuf_get_stats` | `const jbuf_t *jb`, `jbuf_stats_t *stats` | なし | 統計と現在値 | NULL なら何もしない | |
| `jbuf_set_extra_delay` | `jbuf_t *jb`, `uint32_t extra_ms` | なし | 再生側の取り出しの塊に対する追加余裕を設定（目標遅延に加算） | NULL は無害 | `audio_link` が自動設定 |
| `jbuf_state` | `const jbuf_t *jb` | `jbuf_state_t` | 現在状態 | NULL なら BUFFERING | |

### 1.7 シリアルポート (`serial_port`)

ヘッダ: `src/common/serial_port.h`。8N1・フロー制御なし・DTR/RTS ネゲート。読み込みはノンブロッキング、書き込みは上限 `SERIAL_WRITE_TIMEOUT_MS`（200 ms）。

| 名前 | 内容 |
|---|---|
| `serial_ops_t` | `read` / `write` / `close`（モックで差し替え可能） |
| `serial_port_t` | `ops`、`st`（呼び出し側が確保する値型） |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `serial_open` | `serial_port_t *port`, `const char *name`, `uint32_t baud` | `int` | OS のシリアルを開いて設定。Win: `CreateFileA` + `SetCommState` + `SetCommTimeouts` / Linux: `open(O_NONBLOCK)` + termios raw | `RS_ERR_NOT_FOUND`、`RS_ERR_BUSY`（使用中）、`RS_ERR_PERMISSION`、`RS_ERR_INVALID_ARG`（未対応ボーレート）、`RS_ERR_SYSTEM`。`port->ops` は NULL | Win は `\\.\` 接頭辞を自動付与（COM10 以上対応）。Linux は 1200〜115200 の標準値 |
| `serial_close` | `serial_port_t *port` | なし | 閉じる | 未オープンは無害 | |
| `serial_read` | `serial_port_t *port`, `void *buf`, `size_t cap`, `size_t *got` | `int` | 受信済みの分だけ即時に読む（0 バイトでも `RS_OK`） | `RS_ERR_IO`（デバイス消失等） | |
| `serial_write` | `serial_port_t *port`, `const void *buf`, `size_t len` | `int` | 全バイトを書く | `RS_ERR_TIMEOUT`（200 ms で書き切れない）、`RS_ERR_IO` | |
| `serial_last_os_error` | なし | `int` | 直前の OS エラー（GetLastError / errno） | なし | スレッドローカル |
| `serial_is_open` | `const serial_port_t *port` | `int` | 開いていれば非 0 | NULL は 0 | inline |

### 1.8 CI-V フレーム (`civ`)

ヘッダ: `src/common/civ.h`（Phase 3 の `src/server/civ.h` から移動）

| 名前 | 内容 |
|---|---|
| `CIV_PREAMBLE` / `CIV_EOM` | `0xFE` / `0xFD` |
| `CIV_ADDR_IC9100` / `CIV_ADDR_CONTROLLER` | `0x7C` / `0xE0` |
| `CIV_MIN_FRAME` / `CIV_MAX_FRAME` | 5 / 128 バイト |
| `civ_framer_t` | 切り出し途中のフレーム、`frames`（切り出し数）、`garbage_total`（破棄バイト累計）、`garbage_pending`（最後の正常フレーム以降の破棄バイト数） |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `civ_build_frame` | `uint8_t *buf`, `size_t cap`, `uint8_t to`, `uint8_t from`, `const uint8_t *body`, `size_t body_len` | `size_t` | `FE FE to from body FD` を組み立て、長さを返す | 0（cap 不足、body に FE / FD を含む、128 バイト超） | |
| `civ_build_ptt_off` | `uint8_t *buf`, `size_t cap`, `uint8_t radio_addr`, `uint8_t ctrl_addr` | `size_t` | PTT 強制解除 `FE FE 7C E0 1C 00 00 FD`（8 バイト） | 0（cap < 8） | フェイルセーフで使用 |
| `civ_validate_frames` | `const uint8_t *data`, `size_t len` | `int` | 完全なフレームの連続なら 1 | 0 | 受信ペイロードの検査に使用 |
| `civ_framer_init` | `civ_framer_t *f` | なし | 初期化 | なし | |
| `civ_framer_push` | `civ_framer_t *f`, `const uint8_t *data`, `size_t len`, `civ_frame_cb cb`, `void *user` | `size_t` | バイト列を投入し、完成したフレームごとに `cb(frame, len, user)` を呼ぶ。完成数を返す | 雑音・途切れ・長すぎるフレームは破棄して `garbage_*` に計上 | 途中のフレームは次回に持ち越す。プリアンブル 3 個以上も許容 |

### 1.9 CI-V 透過中継 (`civ_link`)

ヘッダ: `src/common/civ_link.h` ／ 仕様: [protocol_spec.md](protocol_spec.md) §9。サーバ・クライアント共通、非スレッドセーフ。

| 名前 | 内容 |
|---|---|
| `civ_link_t` | `port`（NULL = 未接続）、`ch`（dch）、`framer`、送信フック、まとめ送り用バッファ、`stats` |
| `civ_link_stats_t` | `frames_to_net` / `frames_from_net` / `packets_to_net` / `dropped_inactive` / `rejected_payload` / `write_timeouts` / `io_errors` |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `civ_link_init` | `civ_link_t *link`, `int (*send)(void *user, const void *pkt, size_t len)`, `void *user` | なし | 初期化 | なし | |
| `civ_link_attach` | `civ_link_t *link`, `serial_port_t *port` | なし | シリアルを関連付け（NULL で切り離し）。切り出し状態を初期化 | なし | 所有権は移らない |
| `civ_link_start` | `civ_link_t *link`, `dch_role_t role`, `uint32_t session_id`, `k_c2s`, `k_s2c` | なし | セッション鍵で中継を有効化 | なし | |
| `civ_link_stop` | `civ_link_t *link` | なし | 鍵を消去して無効化 | なし | |
| `civ_link_active` | `const civ_link_t *link` | `int` | 有効なら非 0 | なし | |
| `civ_link_poll` | `civ_link_t *link` | `int` | シリアルを読み、完成したフレームを 1 パケットにまとめて送信（1 回あたり最大 8 × 512 バイト） | `RS_ERR_IO`（シリアル異常。呼び出し側で閉じて再接続） | 無効時はフレームを破棄（`dropped_inactive`） |
| `civ_link_handle_packet` | `civ_link_t *link`, `const uint8_t *pkt`, `size_t len`, `uint8_t *type` | `int` | 検査・復号し、CIV ならフレーム列を検証してシリアルへ書く | `RS_ERR_INVALID_ARG` / `RS_ERR_BUSY`（認証不合格・無効）、`RS_ERR_TIMEOUT` / `RS_ERR_IO`（書き込み） | `RS_OK` / `TIMEOUT` / `IO` は認証済み → ピアアドレス学習の根拠 |
| `civ_link_send_bind` | `civ_link_t *link` | `int` | BIND を送信 | `RS_ERR_BUSY`（無効） | クライアントが 1 秒ごとに送る |
| `civ_link_write_raw` | `civ_link_t *link`, `const uint8_t *data`, `size_t len` | `int` | シリアルへ直接書く（セッション状態に依存しない） | `RS_ERR_NOT_FOUND`（ポート未接続）、`serial_write` のエラー | PTT 強制解除の送出に使用 |

### 1.10 オーディオデバイス (`audio_dev`)

ヘッダ: `src/common/audio_dev.h`。モノラル 16-bit PCM、ノンブロッキング。再生キューの消費（デバイスのクロック）が再生タイミングの基準。

| 名前 | 内容 |
|---|---|
| `audio_dev_ops_t` | `read`（録音済みを取り出す）/ `write`（再生キューへ）/ `write_avail`（空きサンプル数）/ `close`（モックで差し替え可能） |
| `audio_dev_config_t` | `backend`（`auto` / `null` / `winmm` / `alsa`）、`capture` / `playback`（デバイス名の部分一致、大文字小文字不問、NULL / `default` で既定）、`sample_rate`(48000)、`frame_samples`(480)、`queue_frames`(4) |
| バックエンド | `null`: 実時間で無音を録音し再生を破棄 / `winmm`: waveIn・waveOut（WAVEHDR のフラグをポーリング、デバイス名は UTF-8）/ `alsa`: `snd_pcm` ノンブロッキング（ALSA 開発ファイルがある場合のみビルド） |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `audio_dev_config_default` | `audio_dev_config_t *cfg` | なし | 既定値 | NULL なら何もしない | |
| `audio_dev_open` | `audio_dev_t *dev`, `const audio_dev_config_t *cfg` | `int` | バックエンドを選んで録音・再生を開く | `RS_ERR_UNSUPPORTED`（未組み込み・形式非対応）、`RS_ERR_NOT_FOUND`（名前不一致）、`RS_ERR_BUSY`、`RS_ERR_PERMISSION`、`RS_ERR_SYSTEM`、`RS_ERR_INVALID_ARG`（queue_frames < 2 等） | |
| `audio_dev_close` | `audio_dev_t *dev` | なし | 閉じる | 未オープンは無害 | |
| `audio_dev_read` | `audio_dev_t *dev`, `int16_t *pcm`, `size_t max`, `size_t *got` | `int` | 録音済みを最大 max 個取り出す（0 個でも `RS_OK`） | `RS_ERR_IO` | ALSA はオーバーランから自動回復 |
| `audio_dev_write` | `audio_dev_t *dev`, `const int16_t *pcm`, `size_t samples` | `int` | 再生キューへ書く | `RS_ERR_INVALID_ARG`（空き超過、WinMM ではフレーム単位でない）、`RS_ERR_IO` | `write_avail` 以下で呼ぶ |
| `audio_dev_write_avail` | `audio_dev_t *dev` | `size_t` | ブロックせずに書けるサンプル数 | 未オープンは 0 | |
| `audio_dev_list` | `audio_dev_list_cb cb`, `void *user` | なし | 組み込み済みバックエンドのデバイス名を列挙 | なし | `--list-audio` |
| `audio_dev_is_open` | `const audio_dev_t *dev` | `int` | 開いていれば非 0 | NULL は 0 | inline |
| `audio_dev_open_null` / `audio_dev_open_winmm` / `audio_dev_open_alsa` | `audio_dev_t *dev`, `const audio_dev_config_t *cfg` | `int` | 各バックエンドの実装（`audio_dev_open` から呼ばれる） | `audio_dev_open` と同じ。そのプラットフォームで未組み込みなら `RS_ERR_UNSUPPORTED` | 新バックエンド（WASAPI 等）はこの形で追加する |
| `audio_dev_list_winmm` / `audio_dev_list_alsa` | `audio_dev_list_cb cb`, `void *user` | なし | 各バックエンドのデバイス名を列挙 | 未組み込みなら何もしない | |

### 1.11 オーディオ伝送 (`audio_link`)

ヘッダ: `src/common/audio_link.h` ／ 仕様: [protocol_spec.md](protocol_spec.md) §10。サーバ・クライアント共通、非スレッドセーフ。

| 名前 | 内容 |
|---|---|
| `audio_link_config_t` | `codec`(PCM16)、`sample_rate`(48000)、`frame_samples`(480)、`jitter_min_ms`(20)、`jitter_max_ms`(80) |
| `audio_link_stats_t` | `frames_sent` / `frames_received` / `rejected_format` / `decode_errors` / `device_errors` |
| `AUDIO_PAYLOAD_HEADER_LEN` | 8（codec / channels / frame_samples / timestamp） |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `audio_link_config_default` | `audio_link_config_t *cfg` | なし | 既定値 | NULL なら何もしない | |
| `audio_link_create` | `audio_link_t **out`, `const audio_link_config_t *cfg`, `send`, `void *user` | `int` | コーデック・ジッターバッファ（容量 = 最大遅延の 2 倍 + 8 フレーム以上）を用意 | `RS_ERR_UNSUPPORTED`（コーデック未組み込み）、`RS_ERR_TOO_LARGE`（PCM フレームが 1 パケットに収まらない）、`RS_ERR_INVALID_ARG`、`RS_ERR_NO_MEMORY` | |
| `audio_link_destroy` | `audio_link_t *link` | なし | 鍵を消去して解放 | NULL は無害 | |
| `audio_link_attach` | `audio_link_t *link`, `audio_dev_t *dev` | なし | デバイスを関連付け（NULL で切り離し） | なし | 所有権は移らない |
| `audio_link_start` | `audio_link_t *link`, `dch_role_t role`, `uint32_t session_id`, `k_c2s`, `k_s2c` | なし | 鍵を設定、ジッターバッファ初期化、送信タイムスタンプを乱数で初期化 | なし | |
| `audio_link_stop` | `audio_link_t *link` | なし | 鍵を消去、ジッターバッファ初期化 | なし | |
| `audio_link_active` | `const audio_link_t *link` | `int` | 有効なら非 0 | なし | |
| `audio_link_poll` | `audio_link_t *link` | `int` | 録音を 480 サンプルごとに符号化・送信し、再生キューの空き分をジッターバッファから供給。再生中の取り出しの塊を計測してジッターバッファの追加余裕に反映 | `RS_ERR_IO`（デバイス異常） | 無効時は送信せず無音を供給 |
| `audio_link_handle_packet` | `audio_link_t *link`, `const uint8_t *pkt`, `size_t len`, `uint64_t now_ms`, `uint8_t *type` | `int` | 検査・復号し、AUDIO なら形式を確認・復号してジッターバッファへ | `RS_ERR_INVALID_ARG` / `RS_ERR_BUSY`（認証不合格・無効）。形式不一致・復号失敗は破棄して `RS_OK`（統計に計上） | `RS_OK` は認証済み → ピアアドレス学習の根拠 |
| `audio_link_send_bind` | `audio_link_t *link` | `int` | BIND を送信 | `RS_ERR_BUSY`（無効） | |
| `audio_link_get_stats` | `const audio_link_t *link`, `audio_link_stats_t *stats`, `jbuf_stats_t *jstats` | なし | 統計 | NULL の出力は省略 | |

### 1.12 JSON パーサ (`json`)

ヘッダ: `src/common/json.h`。外部ライブラリ非依存、RFC 8259 準拠の厳格モード。

| 名前 | 内容 |
|---|---|
| `json_type_t` | `JSON_NULL` / `JSON_BOOL` / `JSON_NUMBER` / `JSON_STRING` / `JSON_ARRAY` / `JSON_OBJECT` |
| `json_value_t` | `type`、`line` / `col`（値の開始位置）、`u.boolean` / `u.number`（double）/ `u.string`（`ptr` NUL 終端 UTF-8、`len`）/ `u.array`（`items`、`count`）/ `u.object`（`keys`、`values`、`count`） |
| `json_error_t` | `line`、`col`、`message` |
| `JSON_MAX_DEPTH` / `JSON_MAX_INPUT` | 32 / 1 MiB |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `json_parse` | `const char *text`, `size_t len`, `json_value_t **out`, `json_error_t *err` | `int` | 文書を解析して木を返す。先頭の UTF-8 BOM は読み飛ばす | `RS_ERR_INVALID_ARG`（構文エラー: 末尾カンマ、コメント、先頭ゼロ、NaN、キー重複、不正な UTF-8 / エスケープ、対にならないサロゲート、`\u0000`、深さ超過、後続データ等。`err` に位置と理由）、`RS_ERR_TOO_LARGE`、`RS_ERR_NO_MEMORY`。`*out` は NULL | `err` は NULL 可。数値は "C" ロケールの `strtod` |
| `json_parse_file` | `const char *path`, `json_value_t **out`, `json_error_t *err` | `int` | ファイルを読んで解析 | `RS_ERR_NOT_FOUND`、`RS_ERR_IO`、`RS_ERR_TOO_LARGE`、`json_parse` のエラー | |
| `json_free` | `json_value_t *v` | なし | 木を解放 | NULL は無害 | |
| `json_object_get` | `const json_value_t *obj`, `const char *key` | `const json_value_t *` | メンバを取得 | なければ・オブジェクトでなければ NULL | |
| `json_get_path` | `const json_value_t *root`, `const char *path` | `const json_value_t *` | `"audio.jitter_min_ms"` 形式のパスで取得（`""` はルート） | 途中がなければ NULL | |
| `json_type_name` | `json_type_t type` | `const char *` | `"object"` 等の名前 | 範囲外は `"?"` | |

### 1.13 設定の読み出し・検証 (`cfg_load`)

ヘッダ: `src/common/cfg_load.h`。エラーは `"file:line:col: path: 理由"` 形式でログに出し、件数を数える
（呼び出し側は 1 件でもあれば起動しない）。文字列の戻り値は JSON 文書内を指す。

| 名前 | 内容 |
|---|---|
| `cfg_reader_t` | `root`、`source`（ファイル名）、`errors`、`warnings` |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `cfg_reader_init` | `cfg_reader_t *r`, `const json_value_t *root`, `const char *source` | なし | 初期化 | なし | |
| `cfg_error` / `cfg_warn` | `cfg_reader_t *r`, `const char *path`, `const json_value_t *v`, `const char *fmt`, `...` | なし | 位置付きでログに出し、件数を加算 | なし | `v` は NULL 可（位置なし） |
| `cfg_string` | `cfg_reader_t *r`, `const char *path`, `const char *def` | `const char *` | 文字列を取得。未指定・`null` は `def` | 型違いはエラーを記録して `def` | |
| `cfg_uint` | `cfg_reader_t *r`, `const char *path`, `uint32_t def`, `uint32_t min`, `uint32_t max` | `uint32_t` | 0 以上の整数を取得 | 型違い・小数・範囲外はエラーを記録して `def` | libm 不要 |
| `cfg_bool` | `cfg_reader_t *r`, `const char *path`, `int def` | `int` | 真偽値を取得 | 型違いはエラー | |
| `cfg_byte` | `cfg_reader_t *r`, `const char *path`, `uint8_t def` | `uint8_t` | 0〜255 の数値、または `"0x7C"` / `"124"` 形式の文字列 | 範囲外・不正形式はエラー | CI-V アドレス用 |
| `cfg_known_keys` | `cfg_reader_t *r`, `const char *object_path`, `const char *const known[]` | なし | 未知のキーを警告（書き間違いの検出） | パスがオブジェクトでなければエラー | `known` は NULL 終端、`""` はルート |
| `cfg_device_name` | `const char *name` | `const char *` | `"default"` / `""` / NULL を NULL（既定デバイス）に正規化 | なし | |
| `cfg_read_audio` | `cfg_reader_t *r`, `const char *section`, `audio_dev_config_t *dev`, `audio_link_config_t *link`, `int *enabled` | なし | audio セクション（backend / codec / sample_rate / channels / frame_ms / opus_bitrate / jitter_min_ms / jitter_max_ms / capture_device / playback_device）を読む。`backend: "none"` で `*enabled = 0` | 48000 Hz・モノラル・10 ms 以外、未知の backend / codec、jitter min > max はエラー | |
| `cfg_read_logging` | `cfg_reader_t *r`, `const char *section`, `rs_log_config_t *log` | なし | logging セクション（level / file / max_file_bytes / max_backups / to_stderr）を読む | 不正なレベル・範囲外はエラー | |
| `cfg_load_file` | `const char *path`, `json_value_t **doc` | `int` | ファイルを読み込み解析 | 見つからない・構文エラーをログに出して非 0 | |
| `cfg_check_permissions` | `const char *path` | なし | Linux: other（所有者・グループ以外）に権限があれば警告（パスワードを含むため） | なし | グループは専用グループでの運用（640）を許すため対象外。Windows では何もしない |

---

## 2. server

### 2.1 サーバ実行本体 (`server_app`)

ヘッダ: `src/server/server_app.h`。制御・CI-V・オーディオの 3 ソケット、IC-9100 のシリアル・オーディオデバイス、
USB 監視を 1 つのイベントループ（待機 2 ms）で統合する。

| 名前 | 内容 |
|---|---|
| `server_app_config_t` | `username` / `password` / `pbkdf2_iterations`、`listen_addr`、`ctl_port` / `civ_port` / `audio_port`（50001〜50003）、`keepalive_interval_ms` / `keepalive_timeout_ms` / `challenge_timeout_ms`、`max_failures` / `failure_window_ms` / `ban_ms`、`civ_device` / `civ_baud`(19200) / `radio_addr`(0x7C) / `ctrl_addr`(0xE0)、`audio_enabled` / `audio_dev` / `audio_link`、`usb_watch` / `usb` / `usb_log_path` |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `server_app_config_default` | `server_app_config_t *cfg` | なし | 既定値を設定 | なし | |
| `server_app_run` | `const server_app_config_t *cfg`, `volatile sig_atomic_t *stop` | `int` | `*stop` が立つまで中継を実行。セッション開始で CI-V / オーディオの鍵を設定し、終了で消去。シリアルは 2 秒、オーディオは 5 秒ごとに再オープンを試み、CI-V ポートを開くたびに PTT 解除を送出。`usb_watch` 時は書き込みタイムアウト・不正バッファ・デバイス消失で USB リセット | 0 = 正常終了、1 = 起動失敗（ソケット・初期化） | 終了時は制御を先に破棄して PTT 解除・DISCONNECT を送ってからシリアルを閉じる |

### 2.1.1 サーバ設定 (`server_config`)

ヘッダ: `src/server/server_config.h`。優先順位: 既定値 < 設定ファイル < 環境変数 `RSBA_PASSWORD` < コマンドライン引数。
キーの一覧は README の「設定ファイル」を参照。

| 名前 | 内容 |
|---|---|
| `SERVER_CONFIG_DEFAULT_PATH` | `"config.server.json"`（`--config` 未指定時、カレントにあれば読む） |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `server_config_apply` | `server_app_config_t *cfg`, `rs_log_config_t *log`, `const json_value_t *root`, `const char *source`, `int *warnings` | `int`（エラー件数） | listen / auth / session / radio / audio / usb_reset / logging を読み、未指定は既定値のまま。秒単位のキーはミリ秒に変換 | 型違い・範囲外・不正値はエラーとして記録（全件を報告）。未知キーは警告 | `cfg` 内の文字列は `root` を指すため、実行終了まで `root` を解放しない |
| `server_config_validate` | `const server_app_config_t *cfg` | `int`（エラー件数） | 上書き後の最終検証: ユーザ名 1〜32 文字、パスワード必須かつ `CHANGE_ME` でない、3 ポートが相異なる、Keepalive タイムアウト ≥ 周期 × 2、jitter min ≤ max、USB 監視時は検索条件あり | 違反をログに出して数える | 警告: パスワード 8 文字未満、PBKDF2 10000 回未満、CI-V 未設定（PTT 解除が無効） |

### 2.2 USB 監視・ハードウェアリセット (`usb_device` / `usb_monitor`)

ヘッダ: `src/server/usb_device.h`, `src/server/usb_monitor.h` ／ 設計詳細: [usb_reset_architecture.md](usb_reset_architecture.md)

#### 2.2.1 `usb_device`（列挙・検索・OS バックエンド）

**型・定数**

| 名前 | 内容 |
|---|---|
| `usb_reset_method_t` | `USB_RESET_AUTO` / `USB_RESET_USBDEVFS`（Linux）/ `USB_RESET_AUTHORIZED`（Linux）/ `USB_RESET_DEVNODE`（Windows） |
| `usb_device_t` | `id`（Win: インスタンス ID / Linux: sysfs 名）、`vid`、`pid`、`serial`、`ready`、`busnum` / `devnum`（Linux のみ） |
| `usb_match_t` | 検索条件 `vid` / `pid`（0 = 条件なし）、`serial` / `device_id`（NULL・"" = 条件なし）。指定項目の AND |
| `usb_enum_cb` | `int (*)(const usb_device_t *dev, void *user)`。非 0 で列挙打ち切り |
| `usb_backend_t` | OS 固有処理の関数テーブル `enumerate` / `get_parent` / `reset`、`auto_methods`（AUTO 時の試行順）、`name`、`ctx` |
| `USB_DEVICE_ID_MAX` / `USB_SERIAL_MAX` | 256 / 128 |

**関数**

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `usb_backend_platform` | なし | `const usb_backend_t *` | 実行 OS 用バックエンド（`windows-cfgmgr32` / `linux-sysfs`） | なし | 静的オブジェクト。ビルド時に OS ごとの実装を選択 |
| `usb_last_os_error` | なし | `int` | 直前のバックエンド呼び出しの OS エラー（CONFIGRET / errno） | なし | スレッドローカル |
| `usb_set_last_os_error` | `int err` | なし | 上記を設定 | なし | バックエンド実装用 |
| `usb_enumerate` | `const usb_backend_t *backend`, `usb_enum_cb cb`, `void *user` | `int` | 接続中の USB デバイスを列挙（インタフェースノード・ルートハブは除外） | `RS_ERR_INVALID_ARG`、バックエンドのエラー | 列挙は都度 OS に問い合わせる |
| `usb_find` | `const usb_backend_t *backend`, `const usb_match_t *match`, `usb_device_t *out` | `int` | 条件に一致する最初のデバイスを取得 | `RS_ERR_INVALID_ARG`（条件が空）、`RS_ERR_NOT_FOUND`、列挙エラー | |
| `usb_match_device` | `const usb_match_t *match`, `const usb_device_t *dev` | `int` | 一致すれば非 0 | NULL なら 0 | |
| `usb_parse_vid_pid` | `const char *str`, `uint16_t *vid`, `uint16_t *pid` | `int` | `"10C4:EA60"` 形式（16 進 1〜4 桁、大文字小文字不問）を解析 | `RS_ERR_INVALID_ARG`（出力は不変） | |
| `usb_reset_method_name` | `usb_reset_method_t method` | `const char *` | `"auto"` 等の名前 | 範囲外は `"?"` | |
| `usb_reset_method_from_string` | `const char *str`, `usb_reset_method_t *out` | `int` | `auto` / `usbdevfs` / `authorized` / `devnode` を解析 | `RS_ERR_INVALID_ARG`（出力は不変） | 設定ファイル・CLI 用 |

**バックエンド関数（`usb_backend_t` のメンバ）**

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `enumerate` | `void *ctx`, `usb_enum_cb cb`, `void *user` | `int` | Win: `CM_Get_Device_ID_ListA` + `CM_Get_DevNode_Status` / Linux: `/sys/bus/usb/devices` 走査 | `RS_ERR_SYSTEM` 等 | |
| `get_parent` | `void *ctx`, `const usb_device_t *dev`, `usb_device_t *parent` | `int` | 親ハブを取得。Win: `CM_Get_Parent` / Linux: sysfs 名の末尾 `.N` を除去 | `RS_ERR_UNSUPPORTED`（親がルートハブ・ホストコントローラ）、`RS_ERR_NOT_FOUND` | 他機器を巻き込むリセットを防ぐ安全装置 |
| `reset` | `void *ctx`, `const usb_device_t *target`, `usb_reset_method_t method` | `int` | Win: `CM_Disable_DevNode` → 500ms → `CM_Enable_DevNode`（最大 5 回再試行）/ Linux: `USBDEVFS_RESET` ioctl または `authorized` 0→1 | `RS_ERR_UNSUPPORTED`（その OS で未対応の方式、何もしない）、`RS_ERR_PERMISSION`、`RS_ERR_NOT_FOUND`、`RS_ERR_BUSY`、`RS_ERR_SYSTEM` | 復帰待ちは行わない（`usb_monitor_reset` が担当）。要管理者 / root |

#### 2.2.2 `usb_monitor`（異常検知・リセット実行・専用ログ）

**型・定数**

| 名前 | 内容 |
|---|---|
| `USB_MONITOR_RESET_REQUIRED` | 1。`note_*` / `poll` が「リセットが必要」を示す正値 |
| `usb_reset_trigger_t` | `USB_TRIGGER_SERIAL_TIMEOUT` / `DEVICE_LOST` / `RX_GARBAGE` / `MANUAL` |
| `usb_monitor_config_t` | `match`、`method`（AUTO）、`reset_parent`（0）、`serial_timeout_threshold`（3）、`rx_garbage_threshold`（4096）、`recovery_timeout_ms`（15000）、`poll_interval_ms`（250）、`min_interval_ms`（60000）、`log`（専用ロガー、NULL で既定ロガー） |
| `usb_reset_result_t` | `trigger`、`device_id`、`target_id`、`method`（最後に試行した方式）、`attempts`、`status`、`recovery_ms`、`total_ms` |
| `usb_monitor_t` | 不透明型 |

**関数**

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `usb_monitor_config_default` | `usb_monitor_config_t *cfg` | なし | 上記の既定値を設定（`match` は空） | NULL なら何もしない | `match` は呼び出し側で必ず設定する |
| `usb_monitor_create` | `usb_monitor_t **out`, `const usb_monitor_config_t *cfg`, `const usb_backend_t *backend` | `int` | モニタを生成。`match` の文字列は内部にコピー | `RS_ERR_INVALID_ARG`（検索条件が空、`poll_interval_ms` = 0 等）、`RS_ERR_NO_MEMORY`。`*out` は NULL | `backend` NULL で `usb_backend_platform()`。`cfg->log` は所有しない |
| `usb_monitor_destroy` | `usb_monitor_t *mon` | なし | 解放 | NULL は無害 | |
| `usb_monitor_note_io_ok` | `usb_monitor_t *mon` | なし | 連続タイムアウト数をクリア | NULL は無害 | |
| `usb_monitor_note_io_timeout` | `usb_monitor_t *mon` | `int` | 連続タイムアウト数を加算し、閾値以上で `USB_MONITOR_RESET_REQUIRED` | `RS_ERR_INVALID_ARG` | 閾値 0 以下で無効 |
| `usb_monitor_note_rx_garbage` | `usb_monitor_t *mon`, `size_t pending_bytes` | `int` | 未解釈データ量が閾値以上で `USB_MONITOR_RESET_REQUIRED` | `RS_ERR_INVALID_ARG` | 閾値 0 で無効 |
| `usb_monitor_poll` | `usb_monitor_t *mon` | `int` | デバイス状態を確認。使用可能 → 消失で 1 回、使用不可が `recovery_timeout_ms` 継続で（以後最小間隔ごとに）`USB_MONITOR_RESET_REQUIRED` | `RS_ERR_INVALID_ARG`、列挙エラー | 起動時から不在のデバイスには要求しない。親ハブ情報をキャッシュする |
| `usb_monitor_reset` | `usb_monitor_t *mon`, `usb_reset_trigger_t trigger`, `usb_reset_result_t *result` | `int` | 対象特定 → レート制限確認 → 方式ごとにリセットと再認識待ち → 専用ログ記録。成功で検知カウンタをクリア | `RS_ERR_BUSY`（最小間隔内、未実行）、`RS_ERR_TIMEOUT`（全方式で再認識せず）、`RS_ERR_NOT_FOUND`（一度も見えていない）、`RS_ERR_UNSUPPORTED`（親がルートハブ等）、`RS_ERR_PERMISSION`、`RS_ERR_SYSTEM`。いずれも `reset_done`（BUSY は `reset_skipped`）を記録 | 同期実行（最大 `recovery_timeout_ms` × 方式数 + α ブロック）。呼び出し前にシリアル／オーディオのハンドルを閉じること。`result` は NULL 可 |
| `usb_monitor_pending_trigger` | `const usb_monitor_t *mon` | `usb_reset_trigger_t` | 直近に `RESET_REQUIRED` を返した理由 | NULL なら `MANUAL` | `usb_monitor_reset` の `trigger` 引数に渡す |
| `usb_reset_trigger_name` | `usb_reset_trigger_t trigger` | `const char *` | `"serial_timeout"` 等の名前 | 範囲外は `"?"` | |

スレッド安全性: `usb_monitor_t` は非スレッドセーフ。シリアル I/O を所有する単一スレッドから使う。

### 2.3 制御チャネル サーバ・セッション管理 (`ctl_server` / `ban_list`)

ヘッダ: `src/server/ctl_server.h`, `src/server/ban_list.h` ／ 仕様: [protocol_spec.md](protocol_spec.md) §5〜§7

#### 2.3.1 `ctl_server`

ソケットと時計は注入する（送信は `hooks.send`、時刻は `now_ms` 引数に単調増加ミリ秒）。非スレッドセーフ。

**型**

| 名前 | 内容 |
|---|---|
| `ctl_server_state_t` | `CTL_SERVER_WAITING` / `CTL_SERVER_ACTIVE` |
| `ctl_end_reason_t` | `CTL_END_CLIENT_DISCONNECT` / `TIMEOUT` / `REPLACED` / `SHUTDOWN` |
| `ctl_server_hooks_t` | `user`、`send`（必須）、`ptt_release`（切断フェイルセーフ）、`session_start`、`session_end` |
| `ctl_server_config_t` | `username`、`password`、`pbkdf2_iterations`(100000)、`keepalive_interval_ms`(1000)、`keepalive_timeout_ms`(5000)、`challenge_timeout_ms`(5000)、`max_failures`(5)、`failure_window_ms`(300000)、`ban_ms`(900000)、`log` |
| `ctl_server_stats_t` | `rx_malformed` / `rx_bad_tag` / `rx_replay` / `rx_unknown_session` / `rx_banned` / `rx_stale_challenge` / `auth_ok` / `auth_fail` / `bans` / `sessions_ended` |

**関数**

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `ctl_server_config_default` | `ctl_server_config_t *cfg` | なし | 上記の既定値を設定（`username` / `password` は NULL） | NULL なら何もしない | |
| `ctl_server_create` | `ctl_server_t **out`, `const ctl_server_config_t *cfg`, `const ctl_server_hooks_t *hooks` | `int` | ソルト・偽ソルト用秘密値を生成し、パスワード鍵を導出 | `RS_ERR_INVALID_ARG`（ユーザ名 0 / 33 文字以上、空パスワード、`send` なし等）、`RS_ERR_NO_MEMORY`、`RS_ERR_SYSTEM`（乱数）。`*out` は NULL | パスワードは保持しない。PBKDF2 のため時間がかかる |
| `ctl_server_destroy` | `ctl_server_t *srv` | なし | セッションがあれば SHUTDOWN として終了処理（PTT 解除・DISCONNECT）後、秘密値を消去して解放 | NULL は無害 | |
| `ctl_server_handle_packet` | `ctl_server_t *srv`, `const net_addr_t *from`, `const uint8_t *data`, `size_t len`, `uint64_t now_ms` | なし | 受信パケットを処理（遮断判定 → 形式 → セッション ID → リプレイ → タグの順に検査）。HELLO / AUTH / PING / DISCONNECT に応答・状態遷移 | 不正パケットは破棄して統計を加算（応答しない） | |
| `ctl_server_tick` | `ctl_server_t *srv`, `uint64_t now_ms` | なし | Keepalive 途絶でセッション破棄（TIMEOUT）、期限切れチャレンジの破棄 | なし | 100ms 程度の周期で呼ぶ |
| `ctl_server_shutdown` | `ctl_server_t *srv` | なし | セッションを SHUTDOWN として終了 | セッションなしなら何もしない | |
| `ctl_server_state` | `const ctl_server_t *srv` | `ctl_server_state_t` | 現在状態 | NULL なら WAITING | |
| `ctl_server_session_id` | `const ctl_server_t *srv` | `uint32_t` | 現在のセッション ID | WAITING なら 0 | |
| `ctl_server_banned_ms` | `const ctl_server_t *srv`, `const net_addr_t *addr`, `uint64_t now_ms` | `uint64_t` | 遮断残り時間 | 遮断なしは 0 | |
| `ctl_server_session_keys` | `const ctl_server_t *srv`, `uint8_t k_c2s[32]`, `uint8_t k_s2c[32]` | `int` | ACTIVE 時にセッション鍵をコピー（データチャネルの鍵導出用） | `RS_ERR_NOT_FOUND`（WAITING）、`RS_ERR_INVALID_ARG` | `session_start` フック内から呼べる。使用後は消去すること |
| `ctl_server_stats` | `const ctl_server_t *srv` | `const ctl_server_stats_t *` | 統計カウンタ | NULL なら NULL | |
| `ctl_end_reason_name` | `ctl_end_reason_t reason` | `const char *` | ログ用の名前 | 範囲外は `"?"` | |

セッション終了時のフック呼び出し順序（全終了理由共通）: `ptt_release` → （DISCONNECT 送信） → `session_end`。

#### 2.3.2 `ban_list`

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `ban_list_init` | `ban_list_t *bl`, `const ban_list_config_t *cfg` | なし | テーブル初期化（容量 `BAN_LIST_CAPACITY` = 256） | なし | `ban_list_t` は呼び出し側で確保（動的確保なし） |
| `ban_list_banned_ms` | `const ban_list_t *bl`, `const net_addr_t *addr`, `uint64_t now_ms` | `uint64_t` | 遮断中なら残りミリ秒 | 遮断なし・NULL は 0 | アドレスのみで判定（ポート無視） |
| `ban_list_record_failure` | `ban_list_t *bl`, `const net_addr_t *addr`, `uint64_t now_ms` | `int` | 失敗を記録。集計期間内に `max_failures` 回で遮断開始し 1 を返す | `max_failures` ≤ 0 なら常に 0 | 満杯時は遮断中でない最古のエントリを再利用 |
| `ban_list_record_success` | `ban_list_t *bl`, `const net_addr_t *addr` | なし | そのアドレスの失敗履歴を消去 | なし | |

---

## 3. client

### 3.0 制御チャネル クライアント (`ctl_client`)

ヘッダ: `src/client/ctl_client.h` ／ 仕様: [protocol_spec.md](protocol_spec.md) §6〜§7。I/O・時計の注入方式はサーバと同じ。非スレッドセーフ。

**型**

| 名前 | 内容 |
|---|---|
| `ctl_client_state_t` | `CTL_CLIENT_IDLE` / `HELLO_SENT` / `AUTH_SENT` / `CONNECTED` |
| `ctl_client_end_t` | `CTL_CLIENT_END_USER` / `SERVER` / `REPLACED` / `TIMEOUT` / `HANDSHAKE_TIMEOUT` / `AUTH_FAILED` / `SERVER_PROOF` |
| `ctl_client_hooks_t` | `user`、`send`（必須）、`connected`、`disconnected` |
| `ctl_client_config_t` | `username`、`password`、`server`（`net_addr_t`）、`handshake_timeout_ms`(3000)、`auto_reconnect`(1)、`reconnect_interval_ms`(3000)、`max_pbkdf2_iterations`(1000000)、`log` |

**関数**

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `ctl_client_config_default` | `ctl_client_config_t *cfg` | なし | 上記の既定値を設定 | NULL なら何もしない | `server` は呼び出し側で設定 |
| `ctl_client_create` | `ctl_client_t **out`, `const ctl_client_config_t *cfg`, `const ctl_client_hooks_t *hooks` | `int` | クライアントを生成。ユーザ名・パスワードを内部にコピー | `RS_ERR_INVALID_ARG`（ユーザ名 0 / 33 文字以上、パスワード空 / 257 文字以上、`send` なし）、`RS_ERR_NO_MEMORY`。`*out` は NULL | 破棄時にパスワード・鍵を消去 |
| `ctl_client_destroy` | `ctl_client_t *cli` | なし | CONNECTED なら DISCONNECT(shutdown) を送り、秘密値を消去して解放 | NULL は無害 | |
| `ctl_client_connect` | `ctl_client_t *cli`, `uint64_t now_ms` | `int` | HELLO を送信し HELLO_SENT へ | `RS_ERR_BUSY`（IDLE 以外）、`RS_ERR_SYSTEM`（乱数） | |
| `ctl_client_disconnect` | `ctl_client_t *cli`, `uint64_t now_ms` | なし | CONNECTED なら DISCONNECT(normal) を送り IDLE へ。自動再接続も停止 | なし | `disconnected(USER)` を通知 |
| `ctl_client_handle_packet` | `ctl_client_t *cli`, `const net_addr_t *from`, `const uint8_t *data`, `size_t len`, `uint64_t now_ms` | なし | サーバ以外のアドレス・形式不正・タグ不一致・リプレイを破棄し、CHALLENGE / AUTH_OK / AUTH_FAIL / PONG / DISCONNECT を処理 | 不正パケットは黙って破棄 | AUTH_OK はタグとサーバ証明の両方を検証（相互認証） |
| `ctl_client_tick` | `ctl_client_t *cli`, `uint64_t now_ms` | なし | PING 送信、ハンドシェイク / Keepalive タイムアウト判定、予約済み再接続の実行 | なし | 100ms 程度の周期で呼ぶ |
| `ctl_client_state` | `const ctl_client_t *cli` | `ctl_client_state_t` | 現在状態 | NULL なら IDLE | |
| `ctl_client_session_id` | `const ctl_client_t *cli` | `uint32_t` | セッション ID | 未接続は 0 | |
| `ctl_client_last_rtt_ms` | `const ctl_client_t *cli` | `uint32_t` | 直近の PING/PONG 往復時間 | 未計測は 0 | |
| `ctl_client_session_keys` | `const ctl_client_t *cli`, `uint8_t k_c2s[32]`, `uint8_t k_s2c[32]` | `int` | CONNECTED 時にセッション鍵をコピー（データチャネルの鍵導出用） | `RS_ERR_NOT_FOUND`（CONNECTED 以外）、`RS_ERR_INVALID_ARG` | `connected` フック内から呼べる。使用後は消去すること |
| `ctl_client_end_name` / `ctl_client_state_name` | 値 | `const char *` | ログ用の名前 | 範囲外は `"?"` | |

自動再接続の対象: `SERVER` / `TIMEOUT` / `HANDSHAKE_TIMEOUT`。`USER` / `AUTH_FAILED` / `SERVER_PROOF` / `REPLACED` では再接続しない。

### 3.1 クライアント実行本体 (`client_app`)

ヘッダ: `src/client/client_app.h`。制御・CI-V・オーディオの 3 ソケット（エフェメラルポート）と、操作端末側のシリアル
（操作ソフトの仮想 COM 等）・オーディオデバイスを 1 つのイベントループ（待機 2 ms）で統合する。

| 名前 | 内容 |
|---|---|
| `client_app_config_t` | `server_host`、`ctl_port` / `civ_port` / `audio_port`、`username` / `password`、`auto_reconnect`、`bind_addr`、`handshake_timeout_ms` / `reconnect_interval_ms`、`civ_device` / `civ_baud`、`audio_enabled` / `audio_dev` / `audio_link` |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `client_app_config_default` | `client_app_config_t *cfg` | なし | 既定値を設定 | なし | |
| `client_app_run` | `const client_app_config_t *cfg`, `volatile sig_atomic_t *stop` | `int` | 接続・認証し、接続中は CI-V / オーディオを中継、1 秒ごとに BIND を送る。シリアル・オーディオは失敗時に再オープン | 0 = 正常終了、1 = 認証失敗・サーバ証明不一致・置き換え、2 = 起動失敗 | 終了時は DISCONNECT を送る |

### 3.2 クライアント設定 (`client_config`)

ヘッダ: `src/client/client_config.h`。優先順位はサーバと同じ。

| 名前 | 内容 |
|---|---|
| `CLIENT_CONFIG_DEFAULT_PATH` | `"config.client.json"` |

| 関数 | 引数 | 戻り値 | 処理内容 | エラー時の挙動 | 備考 |
|---|---|---|---|---|---|
| `client_config_apply` | `client_app_config_t *cfg`, `rs_log_config_t *log`, `const json_value_t *root`, `const char *source`, `int *warnings` | `int`（エラー件数） | server / local / auth / session / civ / audio / logging を読む | サーバと同じ | 同上 |
| `client_config_validate` | `const client_app_config_t *cfg` | `int`（エラー件数） | 最終検証: server.host 必須、ユーザ名 1〜32 文字、パスワード必須かつ `CHANGE_ME` でなく 256 文字以下、3 ポートが相異なる、jitter min ≤ max | 違反をログに出して数える | |
