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
    /* p.flags, p.five_hour_used_bp, p.seven_day_used_bp,
       p.five_hour_reset_unix, p.seven_day_reset_unix, p.updated_unix,
       p.received_uptime_ms (= 受信時 k_uptime_get), p.error_code */
    /* TIME_SYNC 非依存のリセット残り時間:
       remaining = reset_unix - updated_unix - (k_uptime_get() - received_uptime_ms)/1000 */
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

## デバイス個体識別（`device_uid_hash` + `capabilities`）

DEVICE_HELLO は `capabilities (u32)` と `device_uid_hash (u64)` を持ちます。
RawHID-Host が複数デバイスを個別識別し、デバイスごとに異なる設定を適用するために使います。

**DEVICE_HELLO のフォーマット:**
```
byte 0..1   magic "HL"
byte 2      version 0x01
byte 3      DEVICE_HELLO (0x02)
byte 4      protocol_min = 0x01
byte 5      protocol_max = 0x01
byte 6      reserved
byte 7      seq
byte 8..11  capabilities  u32 LE
byte 12..19 device_uid_hash  u64 LE
byte 20..31 reserved
```

**関連ファイル:**

- `include/rawhid_app/identity.h` — `rawhid_app_identity_get_uid_hash()` / `rawhid_app_identity_get_capabilities()` の宣言
- `src/identity.c` — 実装本体

**`device_uid_hash` の生成ロジック:**

```
1. CONFIG_HWINFO が有効かつ hwinfo_get_device_id() 成功
   → ハードウェア ID を FNV-1a 64bit hash 化した値を使用
   → USB ポートを変えても同じ hash になる

2. hwinfo が使えない場合（CONFIG_SETTINGS が有効）
   → NVS から "raw_hid/identity_seed" (128bit) を読む
   → 存在しなければ起動時に乱数で生成して保存し、以後同じ値を使用

3. 両方とも無効
   → device_uid_hash = 0（identity unavailable）
```

raw なハードウェア ID は Host へ送らず、namespace `"zmk-raw-hid-device-uid-v1"` と seed を
FNV-1a 64bit でハッシュ化した値のみを送ります。hash 結果が 0 の場合は 1 に補正します。

**`capabilities` の生成ロジック:**

既存の Kconfig から自動生成します。新規 CONFIG は不要です。

| bit | 機能 | 対応 CONFIG |
|---|---|---|
| 0 | APP_LAYER | `RAWHID_APP_LAYER_CONTROL` |
| 1 | TIME_SYNC | `RAWHID_APP_TIME_SYNC` |
| 2 | AI_USAGE | `RAWHID_APP_AI_USAGE` |
| 3 | THEME | 未実装（常に 0） |

Host 側はこのビットを見て、未対応デバイスへのパケット送信をスキップできます。

---

## ライセンス

MIT License
