# USB 監視＆ハードウェアリセット設計

> ステータス: Phase 2 実装済み（`src/server/usb_device.[ch]`、`usb_backend_win.c`、`usb_backend_linux.c`、
> `usb_monitor.[ch]`）。API 一覧は [function_reference.md](function_reference.md) の 2.2 節。
> Phase 4 で server_bridge のイベントループに組み込み済み（`--usb-watch VID:PID`、§5.5）。
> Linux バックエンドは aarch64 向けクロスコンパイル（glibc 2.36、-Werror）で検証済み。Raspberry Pi 実機での動作は未確認。

## 1. 目的

IC-9100 の USB コントローラ、またはホスト側 USB ドライバがハングアップした場合、
単純なポート再オープンでは復帰できない。OS の低レイヤーから USB デバイスをリセットし、
人手を介さずに自己修復する。

## 2. モジュール構成

```text
 server_app（CI-V 中継・オーディオ中継、Phase 4）
   │ note_io_ok / note_io_timeout / note_rx_garbage / poll
   ▼
 usb_monitor ── 異常判定、リセット手順、復帰待ち、レート制限、専用ログ記録（OS 非依存）
   │ usb_backend_t（関数テーブル）
   ├── usb_backend_win.c    CfgMgr32: 列挙 / 親取得 / CM_Disable_DevNode → CM_Enable_DevNode
   ├── usb_backend_linux.c  sysfs: 列挙 / 親取得 / USBDEVFS_RESET / authorized トグル
   └── (テスト) モックバックエンド  tests/test_usb_monitor.c
```

OS 固有処理をバックエンドの関数テーブルに閉じ込めることで、リセット手順・エスカレーション・
ログ出力をモックで単体テストできる。

## 3. 対象デバイスの特定

| 項目 | 内容 |
|---|---|
| 検索条件 (`usb_match_t`) | VID / PID / シリアル番号 / OS デバイス ID の AND（未指定項目は条件なし） |
| IC-9100 の既定値 | `10C4:EA60`（Silicon Labs CP210x 系 USB シリアル）※ 要実機確認。`server_bridge --list-usb` で確認する |
| デバイス ID（Windows） | デバイスインスタンス ID（例: `USB\VID_10C4&PID_EA60\0001`）。シリアル番号を持たない機器はポート位置ベースの ID になる |
| デバイス ID（Linux） | sysfs 名（例: `1-1.2`）。ポート位置で決まり、再列挙後も変わらない |
| 使用可能 (`ready`) 判定（Windows） | `CM_Get_DevNode_Status` が `DN_STARTED` かつ `DN_HAS_PROBLEM` なし |
| 使用可能 (`ready`) 判定（Linux） | `authorized` = 1 かつ、いずれかのインタフェース (`<name>:<cfg>.<if>`) に `driver` が結合 |

### 3.1 親ハブのリセット (`reset_parent`)

IC-9100 内部で USB シリアルとオーディオ CODEC がハブ配下にある場合、親ハブをリセットすると
両方をまとめて再列挙できる。ただし親が PC 側のハブ・ルートハブだと他の USB 機器を巻き込むため:

- ルートハブ／ホストコントローラは `get_parent` が `RS_ERR_UNSUPPORTED` を返し、リセットしない
  - Windows: 親のインスタンス ID が `USB\VID_xxxx&PID_xxxx` 形式でない（`USB\ROOT_HUB30\...` 等）
  - Linux: sysfs 名に `.` がない（`1-1` の親は `usb1`）
- 既定は無効。無線機の内蔵ハブ構成を `--list-usb` 等で確認してから有効にすること
- 復帰判定は親ではなく監視対象デバイス（子）の `ready` で行う

## 4. 異常検知

| トリガー (`usb_reset_trigger_t`) | 入力 | 判定条件（既定値） | ログ detail |
|---|---|---|---|
| `serial_timeout` | `usb_monitor_note_io_timeout()` | 連続 `serial_timeout_threshold` 回（3）。`note_io_ok()` でクリア | `consecutive_timeouts=N` |
| `rx_garbage` | `usb_monitor_note_rx_garbage(pending)` | 未解釈データが `rx_garbage_threshold` バイト以上（4096） | `pending_bytes=N` |
| `device_lost`（消失） | `usb_monitor_poll()` | 使用可能だったデバイスが列挙されなくなった（遷移時に 1 回のみ） | `state=absent` |
| `device_lost`（停止） | `usb_monitor_poll()` | 列挙はされるが使用不可の状態が `recovery_timeout_ms` 継続（以後 `min_interval` ごとに再要求） | `state=not_ready` |
| `manual` | `server_bridge --usb-reset` | 運用者が実行 | `-` |

- 起動時から不在のデバイスにはリセットを要求しない。無線機の主電源は外部システムが制御するため、
  電源断による不在でリセットを繰り返さないようにしている。
- 停止状態の判定に猶予を設けるのは、正常な再列挙中（ドライバ読み込み中）の一時的な not ready と区別するため。
- 検知はポーリング方式。OS のデバイス通知（udev / `CM_Register_Notification`）による即時検知は今後の拡張とする。

## 5. リセット処理

### 5.1 共通フロー（`usb_monitor_reset`）

```text
 1. 監視対象を検索（消失時は最後に見えた情報を使用。一度も見えていなければ NOT_FOUND）
 2. レート制限: 前回リセットから min_interval 未満なら実行せず BUSY（reset_skipped を記録）
 3. リセット対象の決定（reset_parent 時は親ハブ）
 4. reset_start を記録
 5. 方式ごとに:  バックエンド reset → 監視対象が ready になるまでポーリング（recovery_timeout_ms まで）
                 → reset_attempt を記録 → 成功なら終了、失敗なら次の方式へ
 6. reset_done を記録（最終結果・所要時間）
```

呼び出し側は手順の前にシリアル／オーディオのハンドルを閉じ、成功後に開き直す。

### 5.2 Windows 10（`usb_backend_win.c`）

| 方式 | 手段 | 必要権限 | 備考 |
|---|---|---|---|
| `devnode` | `CM_Locate_DevNodeA` → `CM_Disable_DevNode(CM_DISABLE_UI_NOT_OK)` → 500ms 待機 → `CM_Enable_DevNode` | 管理者 | ドライバスタックごと停止・再起動。Enable は最大 5 回再試行 |

- デバイス列挙は `CM_Get_Device_ID_ListA("USB", CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT)`。
  コンポジットデバイスのインタフェースノード（`&MI_`）は除外する。
- `CM_DISABLE_PERSIST` は付けない。万一 Enable 前にプロセスが異常終了しても、OS 再起動で有効状態に戻る。
- SPEC の SetupAPI は使用せず、CfgMgr32 のみで完結させている（同等の機能で依存が少ない）。

| CONFIGRET | 戻り値 |
|---|---|
| `CR_ACCESS_DENIED` | `RS_ERR_PERMISSION` |
| `CR_NO_SUCH_DEVNODE` | `RS_ERR_NOT_FOUND` |
| `CR_REMOVE_VETOED` / `CR_NOT_DISABLEABLE` | `RS_ERR_BUSY`（他プロセスがハンドルを保持している等） |
| その他 | `RS_ERR_SYSTEM`（`usb_last_os_error()` = CONFIGRET） |

### 5.3 Linux / Raspberry Pi OS（`usb_backend_linux.c`）

| 方式 | 手段 | 必要権限 | 備考 |
|---|---|---|---|
| `usbdevfs` | `/dev/bus/usb/BBB/DDD` を開き `ioctl(fd, USBDEVFS_RESET, 0)` | root または該当デバイスノードへの書き込み権 | ポートリセット。インタフェースのドライバは再結合される |
| `authorized` | `/sys/bus/usb/devices/<name>/authorized` に `0` → 500ms 待機 → `1` | root | 論理的な切断と再列挙。usbdevfs で復帰しない場合に有効 |

`auto` の順序: `usbdevfs` → `authorized`（軽い方式から試す）。

| errno | 戻り値 |
|---|---|
| `EACCES` / `EPERM` | `RS_ERR_PERMISSION` |
| `ENOENT` / `ENODEV` | `RS_ERR_NOT_FOUND` |
| `EBUSY` | `RS_ERR_BUSY` |
| その他 | `RS_ERR_SYSTEM`（`usb_last_os_error()` = errno） |

root 以外で運用する場合の udev ルール例（実機で要確認）:

```text
SUBSYSTEM=="usb", ATTR{idVendor}=="10c4", ATTR{idProduct}=="ea60", MODE="0660", GROUP="rsba"
```

`authorized` は sysfs 属性のため、上記の MODE / GROUP では書き込み権限が付与されない
（`RUN+="/bin/chmod ..."` 等の別手段が必要）。root 以外で上記ルールのみの場合は `usbdevfs` のみ使用可能。

### 5.4 レート制限・エスカレーション

| 項目 | 既定値 | 内容 |
|---|---|---|
| `min_interval_ms` | 60000 | リセットの最小間隔。間隔内の要求は実行せず `RS_ERR_BUSY`。手動実行（CLI）では 0 |
| `recovery_timeout_ms` | 15000 | 1 方式あたりの再認識待ち上限 |
| `poll_interval_ms` | 250 | 再認識待ちのポーリング間隔 |
| 方式の順序 | バックエンド既定 | `auto` 時、復帰しなければ次の方式へ。方式を指定した場合はその 1 方式のみ |

## 6. ログ仕様

専用ロガーインスタンス（`usb_monitor_config_t.log`、既定ファイル `usb_reset.log`）に、モジュール名
`usb_reset` で `key=value` 形式の 1 行レコードを記録する（時刻は UTC、`rs_log` 共通形式）。

| event | レベル | 記録タイミング |
|---|---|---|
| `reset_start` | WARN | リセット開始時 |
| `reset_attempt` | INFO（成功）/ ERROR（失敗） | 方式ごとの試行完了時 |
| `reset_done` | INFO（成功）/ ERROR（失敗） | 最終結果確定時（リセット前に中止した場合も記録） |
| `reset_skipped` | WARN | レート制限で実行しなかった時 |

| フィールド | 内容 |
|---|---|
| `trigger` | トリガー契機（`serial_timeout` / `device_lost` / `rx_garbage` / `manual`） |
| `device` | 監視対象のデバイス ID（不明時 `-`） |
| `target` | 実際にリセットしたデバイス ID（親ハブリセット時は親） |
| `method` | 方式（`reset_start` / `reset_done` は設定値・最終方式、`reset_attempt` は試行方式） |
| `attempt` / `attempts` | 試行番号 / 試行数 |
| `result` | `success` / `failure` |
| `status` | 詳細結果（`rs_strerror` の文字列） |
| `recovery_ms` | 試行開始から再認識完了まで（失敗時は打ち切りまで）の所要時間 |
| `total_ms` | 全試行の所要時間 |
| `os_error` | バックエンドの OS エラーコード（CONFIGRET / errno） |
| `detail` | トリガーの詳細（連続タイムアウト数等） |

出力例:

```text
2026-10-06T03:12:45.120Z WARN  [usb_reset] event=reset_start trigger=serial_timeout device=1-1.2 target=1-1.2 method=auto detail=consecutive_timeouts=3
2026-10-06T03:13:00.130Z ERROR [usb_reset] event=reset_attempt trigger=serial_timeout device=1-1.2 target=1-1.2 method=usbdevfs attempt=1 result=failure status="timed out" recovery_ms=15004 os_error=0
2026-10-06T03:13:02.351Z INFO  [usb_reset] event=reset_attempt trigger=serial_timeout device=1-1.2 target=1-1.2 method=authorized attempt=2 result=success status="success" recovery_ms=2215 os_error=0
2026-10-06T03:13:02.351Z INFO  [usb_reset] event=reset_done trigger=serial_timeout device=1-1.2 target=1-1.2 result=success status="success" method=authorized attempts=2 recovery_ms=2215 total_ms=17231
```

ローテーションは `rs_log` の設定（`logging.max_file_bytes` / `max_backups`）に従う。

### 5.5 server_bridge への組み込み（Phase 4）

`server_bridge --usb-watch VID:PID` で有効になる（`server_app.c`）。イベントループからの入力と動作は次のとおり。

| 入力 | 呼び出し |
|---|---|
| ネットワーク → 無線機の CI-V 書き込みが 200 ms でタイムアウト | `usb_monitor_note_io_timeout` |
| 1 秒ごと: 前回から CI-V フレームを受信していれば | `usb_monitor_note_io_ok` |
| 1 秒ごと: フレーム切り出しの `garbage_pending`（最後の正常フレーム以降の破棄バイト数） | `usb_monitor_note_rx_garbage` |
| 1 秒ごと: デバイスの存在・状態確認 | `usb_monitor_poll` |

いずれかが `USB_MONITOR_RESET_REQUIRED` を返すと、PTT 強制解除を送ってから CI-V シリアルとオーディオデバイスを閉じ、
`usb_monitor_reset` を実行し、終了後ただちに開き直す。専用ログは `--usb-log`（既定 `usb_reset.log`）。
リセット中はイベントループが止まるため、制御チャネルのセッションは Keepalive 途絶で終了することがある
（クライアントは自動再接続する）。

## 7. テスト

| テスト | 内容 |
|---|---|
| `tests/test_usb_monitor.c`（モック） | 検知カウンタ、リセット成功と所要時間、レート制限、方式エスカレーション、復帰しない場合、バックエンドエラー（権限不足）、方式指定、親ハブリセットと拒否、消失デバイスの扱い、poll による消失・停止検知、専用ログの記録内容 |
| `tests/test_usb_detect.c`（実 OS） | 列挙、ID による再検索、存在しない VID:PID、親ハブ取得（またはルートハブ拒否）、未対応方式の拒否。**リセットは実行しない** |
| 手動（実機、運用開始前） | `server_bridge --usb-reset` による実リセット、USB 強制抜去、ハング状態からの自動復帰 |
