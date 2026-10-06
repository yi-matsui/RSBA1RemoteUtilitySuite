# RS-BA1 Remote Utility Suite (IC-9100)

アイコム IC-9100 と操作端末（純正 RS-BA1 Remote Control 等）の間を IPv6 ネットワーク経由でブリッジする、
RS-BA1 Remote Utility 互換のサーバ／クライアントスイート。

- **server_bridge**（シャック側）: IC-9100 の USB シリアル（CI-V）と USB Audio CODEC を中継し、USB の異常を自己修復する
- **client_bridge**（操作端末側）: 操作ソフト向けのシリアルポート（仮想 COM 等）とオーディオデバイスを中継する
- 通信: UDP 50001（制御）/ 50002（CI-V）/ 50003（音声）、IPv4 / IPv6 デュアルスタック
- 秘匿性: パスワードによる相互認証（PBKDF2 + チャレンジ・レスポンス）、全データの暗号化（ChaCha20）と
  改ざん・リプレイ検知（HMAC-SHA256 + シーケンス番号）、連続認証失敗時の IP 一時遮断
- 音声: 48 kHz / 16 bit / モノラル、10 ms フレームの非圧縮 PCM、適応型ジッターバッファ（20〜80 ms）
- 安全: 切断・Keepalive 途絶（5 秒）・終了時に PTT 強制解除（CI-V `FE FE 7C E0 1C 00 00 FD`）を無線機へ送出

| 項目 | 内容 |
|---|---|
| 言語 / ビルド | C11 / CMake 3.21 以上。外部ライブラリ不要（Linux の音声のみ ALSA） |
| 対象 | Windows 10 / 11（x64）、Raspberry Pi OS 64-bit（aarch64） |
| ライセンス | [BSD 2-Clause](LICENSE) |

> **未実装**: Opus コーデック（インタフェースのみ。現状は PCM で 1 方向あたり約 800 kbps）、
> 純正 Remote Utility との互換モード。Raspberry Pi 実機での動作は未確認（クロスコンパイルでの検証のみ。
> 「[検証状況](#検証状況)」参照）。

---

## 目次

1. [構成](#構成)
2. [ビルド](#ビルド)
3. [設定ファイルの作成](#設定ファイルの作成)
4. [起動](#起動)
5. [常駐運用（systemd / Windows）](#常駐運用)
6. [運用上の注意](#運用上の注意)
7. [設定項目リファレンス](#設定項目リファレンス)
8. [コマンドライン引数](#コマンドライン引数)
9. [検証状況](#検証状況)
10. [ドキュメント](#ドキュメント)

---

## 構成

```text
.
├── LICENSE / README.md / SPEC.md
├── CMakeLists.txt / CMakePresets.json     # プリセット: windows-msvc / linux / linux-aarch64-cross
├── config.server.example.json             # サーバ設定例 → config.server.json にコピーして編集
├── config.client.example.json             # クライアント設定例 → config.client.json にコピーして編集
├── cmake/toolchain-aarch64-linux-gnu.cmake
├── scripts/build.ps1 / build.sh
├── doc/                                   # 関数リファレンス・プロトコル仕様・設計書
├── src/
│   ├── common/      # 共通基盤（rsba_common）
│   │   ├── net_socket     IPv6 デュアルスタック UDP
│   │   ├── ctl_proto      制御プロトコル（パケット・鍵導出・リプレイ検知）
│   │   ├── dch            データチャネルの暗号化・改ざん検知
│   │   ├── civ / civ_link CI-V フレーム処理と透過中継
│   │   ├── serial_port    シリアル（Win32 / termios）
│   │   ├── audio_*        音声デバイス（null / WinMM / ALSA）・コーデック・伝送
│   │   ├── jbuf           適応型ジッターバッファ
│   │   ├── json / cfg_load  JSON パーサと設定の型付き読み出し・検証
│   │   └── rs_crypto / rs_log / rs_shutdown / rs_time / rs_error
│   ├── server/      # server_bridge（ctl_server、ban_list、usb_*、server_app、server_config）
│   └── client/      # client_bridge（ctl_client、client_app、client_config）
└── tests/           # 単体テスト 12 本（CTest）
```

`build/` はビルド出力先で、リポジトリには含めない。

---

## ビルド

### Windows 10 / 11

必要なもの: Visual Studio 2022（「C++ によるデスクトップ開発」ワークロード）、CMake 3.21 以上。

```powershell
.\scripts\build.ps1
```

またはプリセットを直接使う（警告をエラー扱いにする場合は `-DRSBA_WARNINGS_AS_ERRORS=ON` を付ける）:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

成果物: `build\windows-msvc\src\server\Release\server_bridge.exe`、`build\windows-msvc\src\client\Release\client_bridge.exe`

### Raspberry Pi OS（実機でのネイティブビルド）

```bash
sudo apt install build-essential cmake ninja-build libasound2-dev
./scripts/build.sh
```

成果物: `build/linux/src/server/server_bridge`、`build/linux/src/client/client_bridge`。
`linux` プリセットは `RSBA_REQUIRE_ALSA=ON` のため、`libasound2-dev` がないと構成の段階で停止する
（音声なしのバイナリを誤って作らないため）。

### aarch64 クロスビルド（Debian / Ubuntu / WSL）

```bash
sudo apt install gcc-aarch64-linux-gnu cmake ninja-build
./scripts/build.sh cross
```

Windows 上の Arm GNU Toolchain を使う場合は `RSBA_CROSS_PREFIX=aarch64-none-linux-gnu-` を設定して
`cmake --preset linux-aarch64-cross` を実行する。クロスビルドで音声（ALSA）を有効にするには、
Raspberry Pi の sysroot を環境変数 `RSBA_SYSROOT` に指定する。クロスビルドしたテストは実機で実行すること。

### CMake オプション

| オプション | 既定 | 内容 |
|---|---|---|
| `RSBA_BUILD_TESTS` | ON | 単体テストをビルドする |
| `RSBA_WARNINGS_AS_ERRORS` | OFF | 警告をエラー扱いにする（`/WX` / `-Werror`） |
| `RSBA_REQUIRE_ALSA` | OFF（`linux` プリセットは ON） | Linux で ALSA が見つからなければ構成を失敗させる |

---

## 設定ファイルの作成

設定は JSON ファイルに書く。値の優先順位は **既定値 < 設定ファイル < 環境変数 `RSBA_PASSWORD` < コマンドライン引数**。

1. 設定例をコピーする（`config.server.json` / `config.client.json` は `.gitignore` 済み）。

   ```bash
   cp config.server.example.json config.server.json     # シャック側
   cp config.client.example.json config.client.json     # 操作端末側
   ```

2. 最低限、次を編集する。

   | ファイル | キー | 内容 |
   |---|---|---|
   | 両方 | `auth.username` / `auth.password` | 両側で同じ値。**`CHANGE_ME` のままでは起動しない** |
   | サーバ | `radio.serial_device` | IC-9100 の USB シリアル（Linux `/dev/ttyUSB0` / Windows `COM3` 等） |
   | サーバ | `radio.audio_capture_device` / `audio_playback_device` | IC-9100 の USB Audio CODEC（Linux `plughw:CARD=CODEC,DEV=0` / Windows `"USB Audio CODEC"`） |
   | サーバ | `usb_reset.vid_pid` | `server_bridge --list-usb` で IC-9100 の USB シリアルを確認して設定 |
   | クライアント | `server.host` | サーバのアドレス（IPv6 推奨）またはホスト名 |
   | クライアント | `civ.serial_device`、`audio.capture_device` / `playback_device` | 操作ソフト側のポートとデバイス（[Windows での接続例](#windows-での操作ソフトとの接続)） |

   デバイス名の一覧は `--list-audio` / `--list-usb` で確認できる。

3. 検証する（誤りがあれば `file:行:桁: キー: 理由` の形ですべて表示され、終了コード 2 で終わる）。

   ```bash
   ./server_bridge --config config.server.json --check-config
   ./client_bridge --config config.client.json --check-config
   ```

   - 型違い・範囲外・不正な値はエラー（起動しない）、知らないキーは警告（書き間違いの検出）
   - 項目を省略した場合は既定値を使う（全項目が省略可能。ユーザ名・パスワード・クライアントの `server.host` のみ必須）
   - 文字コードは UTF-8（BOM 付きも可）。コメントや末尾カンマは JSON の仕様どおり使えない

4. パスワードを保護する。設定ファイルにパスワードを書く場合は所有者以外が読めないようにする
   （Linux では `chmod 600`、サービス専用グループで読ませる場合は `640`。所有者・グループ以外に権限があると起動時に警告する）。
   ファイルに書かずに環境変数 `RSBA_PASSWORD` で渡すこともでき、その場合は環境変数が優先される。
   パスワードをコマンドライン引数で渡す方法は、プロセス一覧に露出するため用意していない。

`--config` を省略すると、カレントディレクトリの `config.server.json` / `config.client.json` があれば読む。

---

## 起動

```bash
# シャック側（Raspberry Pi）
./server_bridge --config /etc/rsba/config.server.json

# 操作端末側
client_bridge.exe --config config.client.json
```

- 一時的な変更はコマンドライン引数で上書きできる（例: `--log-level debug`、`--civ-device COM5`）。
- `--log-level debug` で 10 秒ごとに中継統計（CI-V フレーム数、RTT、音声の揺らぎ・目標遅延・欠落数）を出力する。
- 終了は Ctrl+C（または SIGTERM）。サーバはセッションがあれば PTT 強制解除と切断通知を送ってから終了する。
- クライアントは回線断やサーバ再起動の後に自動で再接続する。認証失敗・別クライアントへの置き換えでは
  再接続せず終了コード 1 で終わる。

### Windows での操作ソフトとの接続

client_bridge と操作ソフト（RS-BA1 Remote Control 等）は同じ PC 上の仮想デバイスでつなぐ。

| 用途 | 仮想デバイス | client_bridge 側 | 操作ソフト側 |
|---|---|---|---|
| CI-V | com0com 等の仮想 COM ペア（例: COM10 ⇔ COM11） | `civ.serial_device: "COM10"` | COM11 |
| 受信音（無線機 → 操作端末） | VB-Audio Virtual Cable（例: CABLE-A） | `audio.playback_device: "CABLE-A Input"` | 録音デバイス `CABLE-A Output` |
| 送信音（操作端末 → 無線機） | 別の仮想ケーブル（例: CABLE-B） | `audio.capture_device: "CABLE-B Output"` | 再生デバイス `CABLE-B Input` |

---

## 常駐運用

### Raspberry Pi（systemd）

```bash
sudo useradd --system --no-create-home --groups dialout,audio rsba
sudo install -d -o root -g rsba -m 750 /etc/rsba
sudo install -o root -g rsba -m 640 config.server.json /etc/rsba/config.server.json
sudo install -d -o rsba -g rsba /var/log/rsba
sudo install -m 755 build/linux/src/server/server_bridge /usr/local/bin/
```

`/etc/systemd/system/rsba-server.service`:

```ini
[Unit]
Description=RS-BA1 compatible server bridge (IC-9100)
After=network-online.target sound.target
Wants=network-online.target

[Service]
User=rsba
Group=rsba
SupplementaryGroups=dialout audio
ExecStart=/usr/local/bin/server_bridge --config /etc/rsba/config.server.json
Restart=always
RestartSec=3
KillSignal=SIGTERM
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now rsba-server
journalctl -u rsba-server -f
```

- `/etc/rsba` は所有者 root・グループ rsba・モード 750 / 640 にする（rsba が読めて、他ユーザーは読めない）。
- **USB 自動リセット**: `authorized` 方式は root が必要。rsba ユーザーで運用する場合は
  `usb_reset.method` を `"usbdevfs"` にし、次の udev ルールでデバイスノードへの書き込みを許可する
  （`/etc/udev/rules.d/99-rsba.rules`、VID:PID は `--list-usb` の結果に合わせる）:

  ```text
  SUBSYSTEM=="usb", ATTR{idVendor}=="10c4", ATTR{idProduct}=="ea60", MODE="0660", GROUP="rsba"
  ```

- 設定を変えたら `server_bridge --config /etc/rsba/config.server.json --check-config` で確認してから
  `sudo systemctl restart rsba-server`。

### Windows

タスク スケジューラで「ログオン時」に「最上位の特権で実行」する（USB 自動リセットに管理者権限が必要なため）。

```powershell
schtasks /Create /TN "RSBA server_bridge" /SC ONLOGON /RL HIGHEST `
  /TR "\"C:\rsba\server_bridge.exe\" --config C:\rsba\config.server.json"
```

- Windows ファイアウォールでサーバの UDP 50001〜50003 の受信を許可する。
- コンソールウィンドウを閉じる・ログオフ・シャットダウンの際も、PTT 強制解除を送ってから終了する。
- Windows サービス（セッション 0）として動かした場合の音声デバイスへのアクセスは未確認のため、ログオン時実行を推奨する。

---

## 運用上の注意

| 項目 | 推奨・注意 |
|---|---|
| **送信タイムアウトタイマー（TOT）** | **IC-9100 側の TOT（タイムアウトタイマー）を必ず有効にする**。PTT 強制解除はセッション終了・Keepalive 途絶・終了要求で送るが、server_bridge 自体の強制終了（kill -9）・クラッシュ・電源断・USB の断線ではソフトウェアから停止できない。TOT は無線機側で送信を打ち切る最後の安全装置になる |
| USB SEND / キーイング | server_bridge はシリアルの DTR / RTS を常にオフにするが、IC-9100 のメニューで USB SEND・USB キーイングに DTR / RTS を割り当てている場合は、他のソフトがポートを開いた際に送信状態になりうる。リモート運用では割り当てを必要最小限にする |
| 起動時の受信状態 | server_bridge は起動時と CI-V ポートを開き直すたびに PTT 解除を送る。ただし CI-V ポート未設定時は PTT 解除を送れない（起動時に警告） |
| パスワード | 推測困難な長いものにする（8 文字未満は警告）。`CHANGE_ME` では起動しない。設定ファイルの権限を絞る |
| ネットワーク公開 | サーバの UDP 50001〜50003 のみを開ける。連続認証失敗（既定: 5 分間に 5 回）でその IP を 15 分遮断する。ポート番号は `listen.*_port` で変更できる |
| IPv6 | サーバは 1 つのソケットで IPv4 / IPv6 両方を受ける。IPv6 で直接到達できる環境（ファイアウォールで受信許可）を推奨 |
| 帯域・遅延 | 音声は PCM で 1 方向あたり約 800 kbps（上下で約 1.6 Mbps）。音声の遅延は ネットワーク遅延 + ジッターバッファ 20〜80 ms（揺らぎに応じて自動調整）+ デバイスバッファ（WinMM で約 40 ms）。CI-V は片道 数 ms + ネットワーク遅延 |
| 同時接続 | サーバのセッションは 1 本。別のクライアントが認証すると前の接続は置き換えられる（PTT 解除のうえ切断） |
| USB 自動リセット | `usb_reset.vid_pid` は実機で `--list-usb` により確認する（`10C4:EA60` は想定値）。リセット中（最大 15 秒 × 方式数）は中継が止まり、セッションが切れることがある。`reset_parent` は無線機の内蔵ハブ構成を確認してから有効にする。動作の記録は `logging.usb_reset_file` |
| ログ | `logging.file` は `max_file_bytes` ごとに `max_backups` 世代までローテーションする。systemd 運用では標準エラー出力も journal に残る |

---

## 設定項目リファレンス

すべて省略可能（省略時は既定値）。秒単位のキーは名前が `_seconds`、ミリ秒は `_ms`。

### サーバ（`config.server.json`）

| キー | 既定 | 範囲・値 | 内容 |
|---|---|---|---|
| `listen.address` | `"::"` | アドレス | 待受アドレス。`::` で IPv4 / IPv6 両方 |
| `listen.control_port` / `civ_port` / `audio_port` | 50001 / 50002 / 50003 | 1〜65535、相異なる | UDP ポート |
| `auth.username` | （必須） | 1〜32 文字 | アカウント名 |
| `auth.password` | （必須） | `CHANGE_ME` 以外 | パスワード（環境変数 `RSBA_PASSWORD` が優先） |
| `auth.pbkdf2_iterations` | 100000 | 1〜10000000 | 鍵導出の反復回数（10000 未満は警告。クライアントは 1000000 超を拒否） |
| `auth.max_failures` | 5 | 0〜1000（0 で遮断なし） | IP 遮断までの認証失敗回数 |
| `auth.failure_window_seconds` | 300 | 1〜86400 | 失敗回数の集計期間 |
| `auth.ban_seconds` | 900 | 1〜604800 | 遮断時間 |
| `session.keepalive_interval_ms` | 1000 | 100〜60000 | クライアントの PING 周期 |
| `session.keepalive_timeout_ms` | 5000 | 500〜600000、周期 × 2 以上 | 受信途絶でセッションを破棄（PTT 解除）するまでの時間 |
| `session.challenge_timeout_ms` | 5000 | 500〜60000 | 認証チャレンジの有効期間 |
| `radio.model` | — | `"IC-9100"` | 情報用（他の値は警告） |
| `radio.civ_address` / `controller_address` | `"0x7C"` / `"0xE0"` | 0〜255 または `"0x.."` | CI-V アドレス（PTT 解除フレームに使用） |
| `radio.serial_device` | （なし） | ポート名、`""` で無効 | IC-9100 の USB シリアル |
| `radio.serial_baud` | 19200 | 1200〜115200 | CI-V ボーレート（無線機の設定に合わせる） |
| `radio.audio_capture_device` / `audio_playback_device` | 既定デバイス | 名前の部分一致、`"default"` | IC-9100 の USB Audio CODEC |
| `audio.*` | | | 下の「共通: audio」 |
| `usb_reset.enabled` | false | | USB 監視・自動リセット |
| `usb_reset.vid_pid` | — | `"VID:PID"`（16 進） | 監視対象。`enabled` 時は必須 |
| `usb_reset.serial` / `device_id` | — | | 同じ VID:PID が複数ある場合の絞り込み |
| `usb_reset.reset_parent` | false | | 親ハブ（無線機の内蔵ハブ）をリセットする。ルートハブは対象外 |
| `usb_reset.method` | `"auto"` | `auto` / `usbdevfs` / `authorized` / `devnode` | リセット方式（Linux: usbdevfs → authorized、Windows: devnode） |
| `usb_reset.serial_timeout_threshold` | 3 | 0〜1000 | CI-V 書き込みタイムアウトの連続回数でリセット |
| `usb_reset.rx_garbage_threshold` | 4096 | 0〜1048576 バイト | CI-V フレームにならない受信データの蓄積でリセット |
| `usb_reset.recovery_timeout_ms` | 15000 | 1000〜120000 | 1 方式あたりの再認識待ち |
| `usb_reset.poll_interval_ms` | 250 | 10〜5000 | 再認識待ちのポーリング間隔 |
| `usb_reset.min_interval_seconds` | 60 | 0〜86400 | リセットの最小間隔 |
| `logging.*` | | | 下の「共通: logging」 |
| `logging.usb_reset_file` | `"usb_reset.log"` | パス | USB リセット専用ログ |

### クライアント（`config.client.json`）

| キー | 既定 | 範囲・値 | 内容 |
|---|---|---|---|
| `server.host` | （必須） | アドレス・ホスト名 | サーバ |
| `server.control_port` / `civ_port` / `audio_port` | 50001 / 50002 / 50003 | 1〜65535、相異なる | サーバのポート |
| `local.bind_address` | `"::"` | アドレス | 送信元アドレス（ポートは自動） |
| `auth.username` / `auth.password` | （必須） | サーバと同じ | |
| `session.handshake_timeout_ms` | 3000 | 500〜60000 | 認証応答の待ち時間 |
| `session.auto_reconnect` | true | | 途絶・タイムアウト後に自動再接続 |
| `session.reconnect_interval_ms` | 3000 | 100〜600000 | 再接続間隔 |
| `civ.serial_device` | （なし） | ポート名、`""` で無効 | 操作ソフトと接続するシリアル |
| `civ.serial_baud` | 19200 | 1200〜115200 | |
| `audio.*` / `logging.*` | | | 下の共通項目 |

### 共通: audio

| キー | 既定 | 範囲・値 | 内容 |
|---|---|---|---|
| `backend` | `"auto"` | `auto`（Win = winmm / Linux = alsa）/ `winmm` / `alsa` / `null` / `none` | `none` で音声中継なし、`null` は無音（試験用） |
| `codec` | `"pcm"` | `pcm` / `opus` | `opus` は未組み込み（指定すると起動時にエラー） |
| `sample_rate` / `channels` / `frame_ms` | 48000 / 1 / 10 | この値のみ | IC-9100 の USB Audio CODEC 形式。20 ms の PCM フレームは 1 パケットに収まらないため 10 ms |
| `opus_bitrate` | 64000 | 6000〜510000 | Opus 組み込み時に使用 |
| `jitter_min_ms` / `jitter_max_ms` | 20 / 80 | 10〜1000、min ≤ max | 適応型ジッターバッファの範囲 |
| `capture_device` / `playback_device` | 既定デバイス | 名前の部分一致、`"default"` | クライアントのデバイス（サーバは `radio.audio_*_device`） |

### 共通: logging

| キー | 既定 | 範囲・値 | 内容 |
|---|---|---|---|
| `level` | `"info"` | `trace` / `debug` / `info` / `warn` / `error` / `fatal` / `off` | 出力レベル |
| `file` | （なし） | パス | ログファイル（指定時はファイルにも出力） |
| `max_file_bytes` / `max_backups` | 10485760 / 5 | 0〜1 GiB / 0〜100 | ローテーション |
| `to_stderr` | true | | 標準エラーにも出力 |

---

## コマンドライン引数

引数は設定ファイルの値を上書きする。`--help` で一覧を表示する。

### server_bridge

| 引数 | 内容 |
|---|---|
| `--config FILE` / `--check-config` | 設定ファイル / 設定を検証して終了 |
| `--user NAME` | アカウント名 |
| `--listen ADDR` / `--port N` | 待受アドレス / 制御ポート（CI-V = N+1、音声 = N+2） |
| `--iterations N` | PBKDF2 反復回数 |
| `--civ-device NAME` / `--civ-baud N` | IC-9100 のシリアル |
| `--audio-backend B` / `--audio-capture NAME` / `--audio-playback NAME` / `--audio-codec C` | 音声 |
| `--jitter-min MS` / `--jitter-max MS` | ジッターバッファの範囲 |
| `--usb-watch VID:PID` | USB 監視・自動リセットを有効化 |
| `--serial S` / `--device-id ID` / `--method M` / `--reset-parent` / `--usb-log PATH` | USB 監視・リセットの詳細 |
| `--list-usb` / `--list-audio` | デバイス一覧を表示して終了 |
| `--usb-reset VID:PID` | 手動で 1 回ハードウェアリセットして終了（要管理者 / root） |
| `--log-level LEVEL` / `--log-file PATH` | ログ |

### client_bridge

| 引数 | 内容 |
|---|---|
| `--config FILE` / `--check-config` | 設定ファイル / 設定を検証して終了 |
| `--server HOST` / `--port N` / `--bind ADDR` | 接続先 / 制御ポート / 送信元アドレス |
| `--user NAME` / `--no-reconnect` | アカウント名 / 自動再接続しない |
| `--civ-device NAME` / `--civ-baud N` | 操作ソフト側のシリアル |
| `--audio-*` / `--jitter-*` / `--list-audio` / `--log-level` / `--log-file` | サーバと同じ |

---

## 検証状況

| 項目 | 結果 |
|---|---|
| Windows（MSVC、`/W4 /WX`）クリーンビルド | 警告ゼロ |
| 単体テスト（CTest 12 本） | 全通過（暗号のテストベクタ、JSON、設定、プロトコル、セッション、USB、ジッターバッファ、CI-V、音声） |
| Windows 実機 E2E | com0com 仮想 COM と VB-Audio 仮想ケーブルで server_bridge ⇔ client_bridge を接続し、CI-V 双方向中継・雑音除去・起動時と切断時の PTT 解除送出・WinMM 双方向音声（欠落ゼロ）を確認。設定ファイルのみでの起動も確認 |
| Linux（aarch64、glibc 2.36）クロスコンパイル | clang（zig cc）で全ソース・テストをビルド・リンク（`-Wall -Wextra -Wpedantic -Wshadow -Werror` に加え `-Wformat=2 -Wcast-qual -Wmissing-prototypes` 等でも警告ゼロ）。ALSA は API シグネチャで型検査 |
| **未確認** | Raspberry Pi 実機での実行（termios・ALSA・USB リセット・テストの実行）、gcc での警告、IC-9100 実機との接続 |

Raspberry Pi で最初に行う確認:

```bash
./scripts/build.sh                       # ビルドとテスト（gcc）
./build/linux/src/server/server_bridge --list-usb
./build/linux/src/server/server_bridge --list-audio
./build/linux/src/server/server_bridge --config config.server.json --check-config
```

---

## ドキュメント

- [関数リファレンス](doc/function_reference.md) — 全公開関数の引数・戻り値・エラー時の挙動
- [プロトコル仕様](doc/protocol_spec.md) — パケット形式、認証、暗号化、CI-V・音声伝送、状態遷移
- [USB リセットアーキテクチャ](doc/usb_reset_architecture.md) — 異常検知、OS 別リセット、専用ログ
- [IPv6 ソケット設計](doc/ipv6_socket_design.md) — デュアルスタックソケット
