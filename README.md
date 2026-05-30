# zmk-rawhid-app

ホスト PC と ZMK キーボード（ドングル / セントラル）を **RawHID** で双方向通信するための
**アプリ層プロトコルモジュール**。[zmk-raw-hid](https://github.com/hrmt-lab/zmk-raw-hid)
（トランスポート）が発火する `raw_hid_received_event` を購読し、パケットを解析して

- **HELLO**: 疎通確認 probe への応答
- **APP_LAYER**: ホストからの ZMK レイヤー制御
- **TIME_SYNC**: ホスト時刻の同期・保持
- **AI_USAGE**: Claude Code / Codex 使用率の保持

を行い、解析結果を getter で他モジュール（例: Prospector ディスプレイ）へ公開します。

キーボード固有コードは不要で、**モジュール追加 + CONFIG 有効化だけ**で組み込めます。

---

## 取り込み方

`config/west.yml` に追加:

```yaml
manifest:
  remotes:
    - name: hrmt-lab
      url-base: https://github.com/hrmt-lab
  projects:
    - name: zmk-raw-hid            # トランスポート（必須）
      remote: hrmt-lab
      revision: custom/raw-hid-custom
    - name: zmk-rawhid-app         # 本モジュール
      remote: hrmt-lab
      revision: main
```

`build.yaml` のドングルに `raw_hid_adapter` シールド（zmk-raw-hid）を併設:

```yaml
include:
  - board: xiao_ble/nrf52840/zmk
    shield: <keyboard>_dongle raw_hid_adapter   # 表示するなら prospector_adapter も
```

ドングルの `.conf`:

```ini
CONFIG_RAW_HID=y
CONFIG_RAWHID_APP=y
CONFIG_RAWHID_APP_LAYER_CONTROL=y
CONFIG_RAWHID_APP_TIME_SYNC=y
CONFIG_RAWHID_APP_AI_USAGE=y
```

`CONFIG_RAWHID_APP` 単体で HELLO 応答が有効になり、各サブ機能を個別に足せます。

---

## CONFIG

| CONFIG | 説明 |
|---|---|
| `RAWHID_APP` | RawHID アプリ層を有効化（依存: `RAW_HID`）。HELLO 応答 |
| `RAWHID_APP_LAYER_CONTROL` | APP_LAYER（ホストからのレイヤー制御） |
| `RAWHID_APP_TIME_SYNC` | TIME_SYNC（時刻同期＋getter） |
| `RAWHID_APP_AI_USAGE` | AI_USAGE（使用率保持＋getter） |

---

## getter（表示側から利用）

ヘッダはモジュールの `include/` にあり、ビルドに含めれば `<rawhid_app/...>` で参照できます。
いずれも `#else` の inline スタブ付きで、CONFIG 無効時でも include 側はビルド可能です。

```c
#include <rawhid_app/ai_usage.h>
struct rawhid_app_ai_usage_provider p;     // provider 1=codex, 2=claude_code
if (rawhid_app_ai_usage_get(2 /* claude */, &p) && p.present) {
    /* p.flags, p.five_hour_used_bp, p.seven_day_used_bp, ... */
}

#include <rawhid_app/time_sync.h>
char buf[24];
if (rawhid_app_time_sync_format(buf, sizeof(buf))) { /* 現在時刻文字列 */ }
bool fast = rawhid_app_time_sync_wants_seconds();  // 秒表示なら更新間隔を短く
```

Prospector の AI Usage 画面はこの getter を `#if IS_ENABLED(CONFIG_RAWHID_APP_AI_USAGE)`
ガードで呼び出します。

---

## プロトコル仕様（v1）

RawHID レポートは **32 byte 固定**。リトルエンディアン。

| offset | 内容 |
|---|---|
| 0..1 | magic `"HL"` |
| 2 | version `0x01` |
| 3 | packet type |
| 4..31 | type ごとのペイロード（未使用域 0） |

| 値 | 名称 | 方向 |
|---|---|---|
| `0x01` | HOST_HELLO | H→D |
| `0x02` | DEVICE_HELLO | D→H |
| `0x03` | ERROR | 予約 |
| `0x04` | PING | 予約 |
| `0x05` | PONG | 予約 |
| `0x10` | AI_USAGE | H→D |
| `0x20` | TIME_SYNC | H→D |
| `0x30` | APP_LAYER | H→D |

検証（`src/dispatch.c`）: magic / version / 既知 type / length==32 / 各 reserved バイトが 0。

### HELLO

`4..6` reserved / `7` seq / `8..31` reserved。HOST_HELLO に対し同じ seq で DEVICE_HELLO を返す。

### APP_LAYER (`0x30`)

`4` action(1=set,2=clear) / `5` layer(0..31) / `6` reserved / `7` seq / `8..31` reserved。
ホストが有効化したレイヤーを1枚だけ追跡（`src/layer_control.c`）。

### TIME_SYNC (`0x20`)

`4..7` unix_sec(u32) / `8..9` tz_offset_min(i16) / `10` weekday(1=Mon..7=Sun) /
`11` format_hint(0:HM 1:HMS 2:Y-M-D 3:M-D 4:datetime 5:weekday+HM) / `12` clock_mode(0:24h,1:12h) /
`13..31` reserved。受信時刻 + `k_uptime` 基準で現在時刻を算出（`src/time_sync.c`）。

### AI_USAGE (`0x10`)

`4` provider(1=codex,2=claude_code) / `5` flags / `6..7` 5h_used_bp(u16) / `8..9` 7d_used_bp(u16) /
`10..13` 5h_reset(u32) / `14..17` 7d_reset(u32) / `18..21` updated(u32) / `22` error_code / `23..31` reserved。

bp は basis points（10000=100.00%、受信時 0..10000 clamp）。provider ごとに保持（`src/ai_usage.c`）。

flags ビット: `0` 5h_valid / `1` 7d_valid / `2` estimated / `3` local_history / `4` quota_source /
`5` stale / `6` fallback_limit / `7` error_present。

error_code: `0` none / `1` source_disabled / `2` missing_credentials / `3` expired_credentials /
`4` auth_failed / `5` rate_limited / `6` fetch_failed / `7` parse_failed / `8` no_usage_data /
`9` missing_limit。

---

## 実装構造（移植の参考）

```
include/rawhid_app/
  packet.h      … パケット型/構造体/enum、ハンドラ宣言
  time_sync.h   … getter + #else スタブ
  ai_usage.h    … flags/struct/getter + #else スタブ
src/
  dispatch.c    … raw_hid_received_event 購読・検証・分岐・HELLO応答
  layer_control.c
  time_sync.c
  ai_usage.c
```

ポイント:
- **疎結合**: 各 feature は `K_MUTEX` 付き状態 + getter で公開。`#else` inline スタブにより、
  CONFIG 無効時でも include 側はビルド可能。
- **拡張**: 独自パケット型を足したい場合は `dispatch.c` の `switch` に分岐を追加するか、
  将来的にコールバック登録口を設ける。

新キーボードに必要なのは west.yml / build.yaml / .conf の設定とキーマップ割当のみで、C コードは不要。

## ライセンス

MIT License
