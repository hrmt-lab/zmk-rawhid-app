# zmk-rawhid-app

ホスト PC と ZMK キーボード（ドングル / セントラル）を **RawHID** で双方向通信するための
**アプリ層プロトコルモジュール**。[zmk-raw-hid](https://github.com/hrmt-lab/zmk-raw-hid)
（トランスポート）が発火する `raw_hid_received_event` を購読し、パケットを解析して

- **HELLO**: 疎通確認 probe への応答
- **APP_LAYER**: ホストからの ZMK レイヤー制御
- **TIME_SYNC**: ホスト時刻の同期・保持
- **AI_USAGE**: Claude Code / Codex 使用率の保持

を行い、解析結果を getter で他モジュール（例: Prospector ディスプレイ）へ公開します。

さらに、デバイス → ホストの **uplink**（device-initiated packet）として

- **BATTERY_STATUS**: 本体 / 左右ペリフェラルのバッテリー残量
- **HOST_ACTION**: キーから PC 側操作をトリガーする（`&host_action` behavior）
- **KEY_STATS**: キー位置ごとの打鍵数（キーの内容は送らない）
- **LAYER_STATE**: 現在の最上位レイヤーと layer mask（表示用）

を送信できます。いずれも `DEVICE_HELLO` の capability bit で個別に有効化されます。

キーボード固有コードは不要で、**モジュール追加 + CONFIG 有効化だけ**で組み込めます。
（uplink を使う場合のみ、`HOST_ACTION` の keymap binding と各機能の `.conf` 追加が必要です。）

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
# Optional device -> host uplinks.
CONFIG_RAWHID_APP_LAYER_STATE_REPORT=y
CONFIG_RAWHID_APP_BATTERY_REPORT=y
CONFIG_RAWHID_APP_HOST_ACTION=y
CONFIG_RAWHID_APP_KEY_STATS=y
CONFIG_RAWHID_APP_KEY_PRESS=y
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
| `RAWHID_APP_LAYER_STATE_REPORT` | LAYER_STATE uplink（現在レイヤーと mask） |
| `RAWHID_APP_BATTERY_REPORT` | BATTERY_STATUS uplink（左右ペリフェラル残量） |
| `RAWHID_APP_HOST_ACTION` | HOST_ACTION uplink（`&host_action <id> <value>`） |
| `RAWHID_APP_KEY_STATS` | KEY_STATS uplink（`uint16_t * ZMK_KEYMAP_LEN` の RAM を使用） |
| `RAWHID_APP_KEY_PRESS` | KEY_PRESS uplink（押下/離上イベントを即時送信） |

---

## 各機能の使い方

ヘッダはモジュールの `include/` にあり、ビルドに含めれば `<rawhid_app/...>` で参照できます。
いずれも `#else` の inline スタブ付きで、CONFIG 無効時でも include 側はビルド可能です。

### Host action uplink

keymap に behavior node を追加:

```dts
#include <behaviors/host_action.dtsi>
```

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_HOST_ACTION=y
```

keymap では `&host_action <action_id> <value>` を使います。split peripheral 側では
`RAWHID_APP_HOST_ACTION=n` のため送信せず no-op になります。

keymap 例（任意のレイヤーのキーに割り当てる）:

```dts
#include <behaviors/host_action.dtsi>

/ {
    keymap {
        compatible = "zmk,keymap";

        my_layer {
            bindings = <
                // action_id = 1, value = 0 を host へ送る
                &host_action 1 0
                // action_id = 2, value = 5（value は host 側で解釈）
                &host_action 2 5
            >;
        };
    };
};
```

`action_id` の意味（ウィンドウ表示・監視停止・アプリ起動など）は**ホスト側 config の許可リスト**で
デバイス単位に定義します。firmware は `action_id` / `value` をそのまま送るだけで、意味づけはしません。
ここで決めた `action_id` を、RawHID Host アプリの **「アクション」画面**（`docs/manual-app-usage.md` の
「アクション」セクション）で同じ番号に対して動作を割り当ててください。両側の `action_id` が一致して
初めて動作します。

### バッテリー残量（BATTERY_STATUS uplink）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_BATTERY_REPORT=y
```

ZMK のバッテリーイベントを購読し、本体 / 左 / 右の残量を送信します。残量変化時に加えて約5分周期でも送り、
split 切断時は `0xFF`（unknown / disconnected）を送ります。キーマップやCコードの追加は不要です。

左右ペリフェラルが未接続または電源OFFの間は、firmware 側の `levels[]` は `0xFF` のままです。この場合
RawHID Host では `--%` や `?` と表示されます。これは packet が届いていない状態ではなく、
`BATTERY_STATUS` の level が unknown であることを示します。左右ペリフェラルが接続され、
`zmk_peripheral_battery_state_changed` が発火すると `0..100` の実残量が送信されます。

### キー統計（KEY_STATS uplink）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_KEY_STATS=y
```

`position_state_changed` を購読してキー位置ごとの押下回数を数え、約45秒周期で**非ゼロの位置だけ**を
送信して 0 にクリアします。送るのは「位置と回数」だけで、キーの内容（何を入力したか）は送りません。
`uint16_t * ZMK_KEYMAP_LEN` の RAM を使うため、RAM に余裕がない構成では有効化前に使用量を確認してください。

### レイヤー逆同期（LAYER_STATE uplink）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_LAYER_STATE_REPORT=y
```

`layer_state_changed` を購読し、最上位アクティブレイヤーと layer mask をホストへ送ります（約50ms デバウンス）。
ホストはこれを**表示専用**に使い、APP_LAYER としてエコーバックしません。キーマップやCコードの追加は不要です。

### uplink 共通の挙動

- `HOST_HELLO` 受信後、約150ms 遅延して LAYER_STATE / BATTERY_STATUS の初期状態を push します
  （capability 登録前に送って host に捨てられるのを避けるため）。
- uplink は best-effort です。host が読んでいない間（監視停止中など）の packet は失われます。
- BLE transport では、下位の `zmk-raw-hid` が notify 完了まで送信 buffer を保持し、uplink をキューで
  直列化する必要があります。`DEVICE_HELLO` 直後は `LAYER_STATE` と `BATTERY_STATUS` が連続しやすいため、
  BLE で使う場合は `zmk-raw-hid` の `custom/raw-hid-custom` 相当の notify queue 実装を前提にしてください。

### レイヤー制御（APP_LAYER）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_LAYER_CONTROL=y
```

キーマップやCコードの変更は不要です。ホストが `APP_LAYER` パケットを送ると、キーボード側で
`zmk_keymap_layer_activate()` / `zmk_keymap_layer_deactivate()` が自動的に呼ばれます。

- **SET**: 指定レイヤーを有効化。前にホストが有効化したレイヤーは自動解除されます。
- **CLEAR**: ホストが有効化したレイヤーのみ解除。手動で有効化したレイヤーは触りません。

ホストが管理するのは常に1枚です。ホスト送信のレイヤー番号（0〜31）に対応するレイヤーをキーマップに定義しておくだけで動作します。

### 時刻表示（TIME_SYNC）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_TIME_SYNC=y
```

ホストが `TIME_SYNC` パケットを送った時点の `unix_time_sec + tz_offset_min` と `k_uptime_get()`
を基準に、その後はキーボード単体で時刻を進め続けます。RawHID が切断されても時刻は動き続けます。

ディスプレイ描画コードから getter を呼び出します:

```c
#include <rawhid_app/time_sync.h>

char buf[24];
if (rawhid_app_time_sync_format(buf, sizeof(buf))) {
    // buf に時刻文字列が入っている → 画面に描画
} else {
    // まだ TIME_SYNC を受信していない → "--:--" など表示
}

// 秒表示フォーマット（HMS）のときは毎秒更新、それ以外は毎分更新で十分
bool needs_fast_refresh = rawhid_app_time_sync_wants_seconds();
```

出力フォーマットはホスト側の `format_hint` / `clock_mode` に従って自動で切り替わります:

| フォーマット | 出力例 |
|---|---|
| HM（デフォルト） | `14:30` |
| HMS | `14:30:05` |
| Y-M-D | `2026-06-06` |
| M-D | `06-06` |
| datetime | `2026-06-06 14:30` |
| weekday+HM | `Fri 14:30` |

### AI 使用率表示（AI_USAGE）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_AI_USAGE=y
```

ホストが `AI_USAGE` パケットを送るたびに、プロバイダー（Codex / Claude Code）ごとに最新状態を
上書き保存します。getter はスレッドセーフにコピーを返します。

ディスプレイ描画コードから getter を呼び出します:

```c
#include <rawhid_app/ai_usage.h>

struct rawhid_app_ai_usage_provider p;

// provider: 1=Codex, 2=Claude Code
if (rawhid_app_ai_usage_get(2 /* claude_code */, &p) && p.present) {

    // 使用率は basis points（10000 = 100.00%）
    uint16_t pct_5h = p.five_hour_used_bp;   // 5時間枠の使用率
    uint16_t pct_7d = p.seven_day_used_bp;   // 7日枠の使用率

    // リセットまでの残り秒数（TIME_SYNC 不要）
    int64_t elapsed_sec = (k_uptime_get() - p.received_uptime_ms) / 1000;
    int64_t remaining_5h = (int64_t)p.five_hour_reset_unix - (int64_t)p.updated_unix - elapsed_sec;
    int64_t remaining_7d = (int64_t)p.seven_day_reset_unix - (int64_t)p.updated_unix - elapsed_sec;

    // エラーチェック
    if (p.flags & RAWHID_APP_AI_USAGE_FLAG_ERROR_PRESENT) {
        // p.error_code を見てエラー表示
    }
}
```

主要な `flags` ビット:

| フラグ | 意味 |
|---|---|
| `RAWHID_APP_AI_USAGE_FLAG_FIVE_HOUR_VALID` (bit0) | 5時間使用率が有効 |
| `RAWHID_APP_AI_USAGE_FLAG_SEVEN_DAY_VALID` (bit1) | 7日使用率が有効 |
| `RAWHID_APP_AI_USAGE_FLAG_ESTIMATED` (bit2) | 推定値（精度低め） |
| `RAWHID_APP_AI_USAGE_FLAG_STALE` (bit5) | データが古い |
| `RAWHID_APP_AI_USAGE_FLAG_ERROR_PRESENT` (bit7) | `error_code` を確認 |

### CONFIG ガードについて

3機能ともヘッダに `#else` スタブが入っているため、ディスプレイ側コードは
`#if IS_ENABLED(...)` ガードだけ書けばビルドが常に通ります:

```c
#if IS_ENABLED(CONFIG_RAWHID_APP_TIME_SYNC)
    // 時刻表示の処理
#endif

#if IS_ENABLED(CONFIG_RAWHID_APP_AI_USAGE)
    // AI 使用率表示の処理
#endif
```

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
| `0x40` | BATTERY_STATUS | D→H |
| `0x50` | HOST_ACTION | D→H |
| `0x60` | KEY_STATS | D→H |
| `0x70` | LAYER_STATE | D→H |
| `0x80` | KEY_PRESS | D→H |

`0x40` 以降は device → host の uplink です。送信するには対応する capability bit を `DEVICE_HELLO` で
立てます（host は bit が立っていない type を破棄します）。

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

### BATTERY_STATUS (`0x40`, D→H)

`4` count(1..4) / `5+2i` source[i](0=self,1=left,2=right,3=aux) / `6+2i` level[i](0..100, `0xFF`=unknown) /
以降 reserved。**seq は持ちません**（byte7 は entry 領域）。`src/battery_report.c`。

`0xFF` は unknown / disconnected を表します。hitsuki46 のような split central 構成では、左右ペリフェラルが
未接続または電源OFFのまま initial push が走ると、host には `BATTERY_STATUS` packet 自体は届いていても
level は `0xFF` になります。host 側ではこの値を `null` として扱い、`--%` / `?` 表示になります。

### HOST_ACTION (`0x50`, D→H)

`4` action_id / `5` value / `6` reserved / `7` seq / `8..31` reserved。host は同一 seq の連続受信を
1回として扱います（二重送信対策）。`action_id` / `value` の意味は host 側 config が定義します。
`src/behaviors/behavior_host_action.c`。

### KEY_STATS (`0x60`, D→H)

`4` entry_count(1..8) / `5` flags(bit0=MORE_FOLLOWS) / `6` reserved / `7` seq /
`8+3i` position[i] / `9+3i..10+3i` delta[i](u16 LE, 0 は送らない) / 以降 reserved。
非ゼロ位置のみを定期送信し 0 クリア。8 件超は複数 packet に分割し最後以外に MORE_FOLLOWS を立てます。
`src/key_stats.c`。

### LAYER_STATE (`0x70`, D→H)

`4` active_layer(0..31) / `5..6` reserved / `7` seq / `8..11` layer_mask(u32 LE, bit i = layer i active) /
`12..31` reserved。host は表示専用に使います。`src/layer_state_report.c`。

### KEY_PRESS (`0x80`, D→H)

`4` position(u8) / `5` flags / `6` reserved / `7` seq / `8..31` reserved。

flags bit0 が `1` のとき押下、`0` のとき離上です。`CONFIG_RAWHID_APP_KEY_PRESS=y` の間は
`zmk_position_state_changed` ごとに press/release を即時送信します。監視停止中や host 未接続時の送信は
既存 uplink と同じ RawHID 送信ブロックの影響を受ける可能性があります。

---

## 実装構造（移植の参考）

```
include/rawhid_app/
  packet.h      … パケット型/構造体/enum、ハンドラ宣言
  time_sync.h   … getter + #else スタブ
  ai_usage.h    … flags/struct/getter + #else スタブ
  identity.h    … device_uid_hash / capabilities の getter 宣言
  uplink.h      … uplink 共通 helper（prepare/seq/send/初期push）の宣言
src/
  dispatch.c    … raw_hid_received_event 購読・検証・分岐・HELLO応答
  layer_control.c
  time_sync.c
  ai_usage.c
  identity.c            … device_uid_hash / capabilities 生成
  uplink.c              … uplink 共通 helper・HELLO後の初期 push
  battery_report.c      … BATTERY_STATUS uplink
  key_stats.c           … KEY_STATS uplink
  layer_state_report.c  … LAYER_STATE uplink
  behaviors/
    behavior_host_action.c … &host_action behavior + HOST_ACTION uplink
dts/
  behaviors/host_action.dtsi             … &host_action node
  bindings/behaviors/zmk,behavior-host-action.yaml
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

既存の Kconfig から自動生成します。

| bit | 機能 | 対応 CONFIG |
|---|---|---|
| 0 | APP_LAYER | `RAWHID_APP_LAYER_CONTROL` |
| 1 | TIME_SYNC | `RAWHID_APP_TIME_SYNC` |
| 2 | AI_USAGE | `RAWHID_APP_AI_USAGE` |
| 3 | THEME | 未実装（常に 0） |
| 4 | BATTERY_STATUS | `RAWHID_APP_BATTERY_REPORT` |
| 5 | HOST_ACTION | `RAWHID_APP_HOST_ACTION` |
| 6 | KEY_STATS | `RAWHID_APP_KEY_STATS` |
| 7 | LAYER_STATE | `RAWHID_APP_LAYER_STATE_REPORT` |
| 8 | KEY_PRESS | `RAWHID_APP_KEY_PRESS` |

Host 側はこのビットを見て、未対応デバイスへのパケット送信をスキップできます。

---

## ライセンス

MIT License
