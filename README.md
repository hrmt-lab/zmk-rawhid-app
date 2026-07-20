# zmk-rawhid-app

[English](README_EN.md)

[ホスト PC](https://github.com/hrmt-lab/Keylink-Studio) と ZMK キーボード（ドングル / セントラル）を
**RawHID** で双方向連携させる ZMK モジュール。組み込むと、キーボードで次のことが
できるようになります。

- **ディスプレイ表示の強化**: ホストの時刻、AI 使用率（Claude Code / Codex）、
  Central / ペリフェラルのバッテリー残量を、Prospector などのディスプレイに表示できます
- **ホストからのレイヤー切り替え**: アクティブなアプリに応じたレイヤー自動切り替えなど、
  PC 側からキーボードのレイヤーを制御できます（現在レイヤーのホストへの通知も可能）
- **キーから PC を操作**: キーに `&host_action` を割り当て、ウィンドウ表示や
  アプリ起動などの PC 側アクションを実行できます
- **打鍵の可視化**: キー位置ごとの打鍵数や押下イベントをホストへ送り、
  ヒートマップなどの統計表示に使えます（キーの内容は送りません）
- **Keylink Studio からの設定変更**: ZMK StudioではできないエンコーダやComboの編集・保存が再ビルドなしで行えます

キーボード固有コードは不要で、**モジュール追加 + CONFIG 有効化だけ**で組み込めます。
（uplink を使う場合のみ、`&host_action` の keymap 割り当てと各機能の `.conf` 追加が必要です。）

トランスポート層には [zmk-raw-hid](https://github.com/hrmt-lab/zmk-raw-hid) を使用し、
本モジュールはそのアプリ層プロトコル（Host Link v2）を実装します。各機能は
`DEVICE_HELLO` の capability bit で個別に有効化されます。

---

## インストール

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

`.conf`:

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
# ZMK Studio から encoder binding を編集・保存する場合
CONFIG_RAWHID_APP_CONFIG_RPC=y
```

`CONFIG_RAWHID_APP` 単体で HELLO 応答が有効になり、各サブ機能を個別に足せます。

---

## CONFIG

いずれもキーボード側 config（`zmk-config-xxx` の `.conf`）に `CONFIG_` プレフィックス付きで設定します。
デフォルトはすべて `n` で、サブ機能をどれか1つ有効にすると `RAWHID_APP` 本体は自動で有効になります。

| CONFIG | デフォルト | 説明 |
|---|---|---|
| `RAWHID_APP` | `n` | RawHID アプリ層を有効化（依存: `RAW_HID`）。HELLO 応答 |
| `RAWHID_APP_LAYER_CONTROL` | `n` | APP_LAYER（ホストからのレイヤー制御） |
| `RAWHID_APP_TIME_SYNC` | `n` | TIME_SYNC（時刻同期＋getter） |
| `RAWHID_APP_AI_USAGE` | `n` | AI_USAGE（使用率保持＋getter） |
| `RAWHID_APP_AI_CLIENT_STATE` | `n` | AI Client State Core（検証・到着順LWW・15秒timeout） |
| `RAWHID_APP_AI_CLIENT_STATE_RENDERER` | `n` | AI状態を利用するRendererが存在することを静的に宣言 |
| `RAWHID_APP_LAYER_STATE_REPORT` | `n` | LAYER_STATE uplink（現在レイヤーと mask） |
| `RAWHID_APP_BATTERY_REPORT` | `n` | BATTERY_STATUS uplink（Central/Self とペリフェラル残量） |
| `RAWHID_APP_HOST_ACTION` | `n` | HOST_ACTION uplink（`&host_action <id> <value>`） |
| `RAWHID_APP_KEY_STATS` | `n` | KEY_STATS uplink（`uint16_t * ZMK_KEYMAP_LEN` の RAM を使用） |
| `RAWHID_APP_KEY_PRESS` | `n` | KEY_PRESS uplink（押下/離上イベントを即時送信） |
| `RAWHID_APP_CONFIG_RPC` | `n` | Config RPC の ENCODER feature（編集・settings保存）を有効化。Combo runtime有効時はCombo featureの照会も提供 |
| `RAWHID_APP_COMBO_RUNTIME` | `n` | Keylink runtime Combo engine。overlay で `/combos` を `status = "disabled"` にして有効化 |
| `RAWHID_APP_COMBO_SETTINGS` | `n` | Combo Settings table をNVSへ保存・読込。Combo runtimeとConfig RPCが必要 |

`CONFIG_RAWHID_APP_COMBO_RUNTIME=y` を有効にする場合は、Combo の定義自体は通常どおり
キーボードの `.keymap` ファイルに記載し、`status = "disabled"` の上書きだけを
overlay（例: Central 側の `<keyboard>_left.overlay`）に記載してください。`.keymap` 側で
`/combos` を無効化してはいけません。

```dts
// 例: <keyboard>_left.overlay（Central 側）
/ {
    combos {
        status = "disabled";
    };
};
```

これにより、Keylink runtime を使用するビルドでは ZMK 標準の Combo listener を生成せず、
ZMK 標準と Keylink runtime の両方が同じキーイベントを処理することを防ぎます。一方、overlay を
適用しない他のビルドまで `/combos` が無効化されることはありません。ノードを削除するのではなく
overlay で無効化することで、`/combos` の子ノードは Devicetree 上に残り、Keylink runtime が
初期 Combo 定義として利用できます。

---

## 各機能の使い方

各機能を有効にする `CONFIG_RAWHID_APP_*` の一覧とデフォルト値は、前掲の「CONFIG」表を参照してください。

ヘッダはモジュールの `include/` にあり、ビルドに含めれば `<rawhid_app/...>` で参照できます。
`time_sync.h` と `ai_usage.h` のgetterには、機能CONFIGが無効な場合の inline スタブがあります。
その他のruntime APIは、対応する機能を有効にした構成でのみ利用してください。

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
ここで決めた `action_id` を、[Keylink Studio](https://github.com/hrmt-lab/Keylink-Studio) アプリの **「アクション」画面**（`docs/manual-app-usage.md` の
「アクション」セクション）で同じ番号に対して動作を割り当ててください。両側の `action_id` が一致して
初めて動作します。

### ZMK Studio encoder override（CONFIG_RPC）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_CONFIG_RPC=y
```

Keylink Studioでエンコーダにマウススクロールを割り当て、`.keymap`と同じスクロール量と
押下時間を使用する場合は、`.keymap`に次のbridgeノードも追加します。

```dts
keylink_encoder_runtime: keylink_encoder_runtime {
    compatible = "keylink,encoder-runtime";
    scroll-value = <ZMK_POINTING_DEFAULT_SCRL_VAL>;
    sensor-behavior = <&inc_dec_ms>;
};
```

`ZMK_POINTING_DEFAULT_SCRL_VAL` は`.keymap`で定義した既存のスクロール量、
`&inc_dec_ms` は`tap-ms`を設定した既存のsensor behaviorを参照します。値をここへ
重複して書く必要はありません。このノードはKeylink Studio用に設定値をrawhid-appへ
渡すだけで、通常の`sensor-bindings`を置き換えず、ZMK本体の変更も不要です。

Host Link v2のConfig RPCで、encoderごとにCW/CCWのruntime overrideを編集できます。
ENCODER featureは `GET_INFO`、`GET_BINDINGS`、`SET_BINDINGS`、`GET_DIRTY`、`SAVE`、`DISCARD`、
`CLEAR_OVERRIDE` をサポートします。`SET_BINDINGS` はRAM上の変更だけを行い、`SAVE` がsettings/NVSへ
保存します。

overrideはレイヤーindexではなく ZMK Studio の `(Layer.id, encoder_id)` で管理します。途中のレイヤーを
削除しても後続レイヤーのoverrideは移動しません。削除レイヤーのrecordはtombstoneとして保持され、SAVE時に
対応する `keylink/enc/v1/l<layer_id>/e<encoder_id>` だけを削除します。DISCARDでは、Studio側のレイヤー
DISCARDにより同じLayer.idが復元済みの場合だけsaved bindingを復元します。

現在存在しないLayer.idを持つsettings recordは起動時にも保持し、実行や別レイヤーへの適用はしません。
`GET_DIRTY` はこのorphan recordをdirtyとして返し、次回SAVEで削除します。overrideがないencoderイベントは
従来どおり `.keymap` の `sensor-bindings` へ渡されます。

### Combo runtime の照会（CONFIG_RPC）

`CONFIG_RAWHID_APP_COMBO_RUNTIME=y` と `CONFIG_RAWHID_APP_CONFIG_RPC=y` を有効にすると、
CONFIG_RPC の COMBO feature（`feature=0x02`）でruntime Comboを照会できます。
現在は読み取り専用で、`GET_INFO=0x01` と `GET_COMBO=0x02` のみをサポートします。
`SET_COMBO`、`GET_DIRTY`、`SAVE`、`DISCARD`、`DELETE_COMBO`、`RESET_TO_KEYMAP` はop値を予約しているだけで、
現在は `UNSUPPORTED_OP` を返します。

Comboの保存済みtableを使う場合は、加えて `CONFIG_RAWHID_APP_COMBO_SETTINGS=y` を有効にします。
この設定はNVS Settings backendを必要とし、`keylink/cmb/v1/table` に固定長tableを保存します。
Combo runtimeだけでもkeymap由来の定義を読み取り専用で利用できます。

### バッテリー残量（BATTERY_STATUS uplink）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_BATTERY_REPORT=y
```

ZMK のバッテリーイベントを受け取り、Central/Self とペリフェラルの残量を送信します。
残量変化時に加えて約5分周期でも送り、未取得・非対応・split 切断時は
`0xFF`（unknown / not available / disconnected）を送ります。キーマップやCコードの追加は不要です。

`source=0` は Central/Self（RawHID endpoint）、`source=1..3` は Peripheral 1..3 です。
USB 給電 dongle のように Central/Self の残量が取れない構成では `source=0, level=0xFF`
を送ります。host 側は `0xFF` の source を通常表示から外せるため、この場合は
`P1:N%` のようにペリフェラルだけ表示できます。

ペリフェラルが未接続または電源OFFの間は、firmware 側の該当 level は `0xFF` のままです。
これは packet が届いていない状態ではなく、`BATTERY_STATUS` の level が unknown であることを示します。
ペリフェラルが接続され、`zmk_peripheral_battery_state_changed` が通知されると `0..100` の実残量が送信されます。

### キー統計（KEY_STATS uplink）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_KEY_STATS=y
```

`position_state_changed` を受け取ってキー位置ごとの押下回数を数え、約45秒周期で**非ゼロの位置だけ**を
送信して 0 にクリアします。送るのは「位置と回数」だけで、キーの内容（何を入力したか）は送りません。
`uint16_t * ZMK_KEYMAP_LEN` の RAM を使うため、RAM に余裕がない構成では有効化前に使用量を確認してください。

### レイヤー逆同期（LAYER_STATE uplink）

`.conf` に追加:

```ini
CONFIG_RAWHID_APP_LAYER_STATE_REPORT=y
```

`layer_state_changed` を受け取り、最上位アクティブレイヤーと layer mask をホストへ送ります（約50ms デバウンス）。
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

## プロトコル仕様（v2）

Host Link packet は **64 byte 固定**。リトルエンディアン。

| offset | 内容 |
|---|---|
| 0..1 | magic `"HL"` |
| 2 | version `0x02` |
| 3 | packet type |
| 4 | seq |
| 5 | feature |
| 6 | op |
| 7 | status_or_flags |
| 8 | payload_len |
| 9..11 | reserved = 0 |
| 12..63 | payload（`payload_len` 以降の未使用域は 0） |

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
| `0x90` | CONFIG_REQUEST | H→D |
| `0x91` | CONFIG_RESPONSE | D→H |
| `0xA0` | STATE_UPDATE | H→D |

`0x40`〜`0x80` は device → host の uplink です。送信するには対応する capability bit を
`DEVICE_HELLO` で立てます（host は bit が立っていない type を破棄します）。
`0x90` は host → device のCONFIG_REQUEST、`0x91` は device → host のCONFIG_RESPONSEです。

検証（`src/dispatch.c`）: magic / version / 既知 type / length==64 / `payload_len<=52` /
header reserved と payload 未使用域が 0。

### HELLO

HOST_HELLO は `payload_len=0`。HOST_HELLO に対し同じ header seq で DEVICE_HELLO を返す。
DEVICE_HELLO は `payload_len=12`、payload に `capabilities u32 LE` と `device_uid_hash u64 LE` を持つ。

### AI Client State (`0xA0`)

`feature=0x0A`、`op=0`、`flags=0`、`payload_len=6`。Payloadは
`client_type`、`client_variant`、`session_active`、`activity_state`、`revision u16 LE`。
意味検証を通過した最後のPacketをrevisionの大小に関係なく受理し、15秒間正常な更新が
なければ現在状態を無効化します。

### APP_LAYER (`0x30`)

`payload_len=2`。`payload[0]` action(1=set,2=clear) / `payload[1]` layer(0..31)。
ホストが有効化したレイヤーを1枚だけ追跡（`src/layer_control.c`）。

### TIME_SYNC (`0x20`)

`payload_len=9`。`payload[0..4]` unix_sec(u32) / `payload[4..6]` tz_offset_min(i16) /
`payload[6]` weekday(1=Mon..7=Sun) / `payload[7]` format_hint(0:HM 1:HMS 2:Y-M-D 3:M-D 4:datetime 5:weekday+HM) /
`payload[8]` clock_mode(0:24h,1:12h)。受信時刻 + `k_uptime` 基準で現在時刻を算出（`src/time_sync.c`）。

### AI_USAGE (`0x10`)

`payload_len=19`。`payload[0]` provider(1=codex,2=claude_code) / `payload[1]` flags /
`payload[2..4]` 5h_used_bp(u16) / `payload[4..6]` 7d_used_bp(u16) /
`payload[6..10]` 5h_reset(u32) / `payload[10..14]` 7d_reset(u32) /
`payload[14..18]` updated(u32) / `payload[18]` error_code。

bp は basis points（10000=100.00%、受信時 0..10000 clamp）。provider ごとに保持（`src/ai_usage.c`）。

flags ビット: `0` 5h_valid / `1` 7d_valid / `2` estimated / `3` local_history / `4` quota_source /
`5` stale / `6` fallback_limit / `7` error_present。

error_code: `0` none / `1` source_disabled / `2` missing_credentials / `3` expired_credentials /
`4` auth_failed / `5` rate_limited / `6` fetch_failed / `7` parse_failed / `8` no_usage_data /
`9` missing_limit。

### BATTERY_STATUS (`0x40`, D→H)

`payload_len=1+count*2`。`payload[0]` count(1..4) /
`payload[1+2i]` source[i](0=Central/Self,1=Peripheral 1,2=Peripheral 2,3=Peripheral 3) /
`payload[2+2i]` level[i](0..100, `0xFF`=unknown/not available/disconnected)。
MVP では header seq は 0 固定。`src/battery_report.c`。

`source=0` は常に Central/Self を表します。Central/Self の残量が取れない構成では
`source=0, level=0xFF` を送ります。`source=1..3` は左右を意味せず、ZMK の peripheral slot を
1-based にした値です。

例: USB給電 dongle + peripheral 1つでは `source=0, level=0xFF` と
`source=1, level=0..100` を送ります。host 側では `0xFF` の source を通常表示から外し、
`P1:N%` のみ表示できます。

### HOST_ACTION (`0x50`, D→H)

`payload_len=2`。`payload[0]` action_id / `payload[1]` value。
`action_id` / `value` の意味は host 側 config が定義します。
`src/behaviors/behavior_host_action.c`。

### KEY_STATS (`0x60`, D→H)

`payload_len=4+count*3`。`payload[0]` entry_count(1..8) /
`payload[1]` flags(bit0=MORE_FOLLOWS) / `payload[2..4]` reserved /
`payload[4+3i]` position[i] / `payload[5+3i..7+3i]` delta[i](u16 LE, 0 は送らない)。
非ゼロ位置のみを定期送信し 0 クリア。8 件超は複数 packet に分割し最後以外に MORE_FOLLOWS を立てます。
`src/key_stats.c`。

### LAYER_STATE (`0x70`, D→H)

`payload_len=8`。`payload[0]` active_layer(0..31) / `payload[1..4]` reserved /
`payload[4..8]` layer_mask(u32 LE, bit i = layer i active)。host は表示専用に使います。
`src/layer_state_report.c`。

### KEY_PRESS (`0x80`, D→H)

`payload_len=2`。`payload[0]` position(u8) / `payload[1]` flags。

flags bit0 が `1` のとき押下、`0` のとき離上です。`CONFIG_RAWHID_APP_KEY_PRESS=y` の間は
`zmk_position_state_changed` ごとに press/release を即時送信します。監視停止中や host 未接続時の送信は
既存 uplink と同じ RawHID 送信ブロックの影響を受ける可能性があります。

### CONFIG_REQUEST / CONFIG_RESPONSE (`0x90` / `0x91`)

`CONFIG_RAWHID_APP_CONFIG_RPC=y` のとき、ENCODER feature (`feature=0x01`) を処理します。opは
`GET_INFO=0x01`、`GET_BINDINGS=0x02`、`SET_BINDINGS=0x03`、`GET_DIRTY=0x04`、`SAVE=0x05`、
`DISCARD=0x06`、`CLEAR_OVERRIDE=0x07` です。Layer.idはpayloadのu32 LE、encoder_idはu8で指定します。

`CONFIG_RAWHID_APP_COMBO_RUNTIME=y` も有効な場合は、COMBO feature（`feature=0x02`）の
`GET_INFO=0x01` と `GET_COMBO=0x02` を処理します。ほかのCombo opは将来用に予約されており、
現時点では `UNSUPPORTED_OP` を返します。

Config RPCのheaderとop/payload形式はHost Link v2で固定です。SAVEはworkqueueでsettings操作を行い、
完了後にresponseを返します。Hostのtimeout retryには短寿命response cacheで同じresponseを再送します。

---

## 実装構造（移植の参考）

```
include/rawhid_app/
  packet.h      … パケット型/構造体/enum、ハンドラ宣言
  time_sync.h   … getter + #else スタブ
  ai_usage.h    … flags/struct/getter + #else スタブ
  identity.h    … device_uid_hash / capabilities の getter 宣言
  uplink.h      … uplink 共通 helper（prepare/seq/send/初期push）の宣言
  encoder_runtime.h … encoder override runtime API
  behavior_identity.h … settings record用behavior identity hash
  combo_runtime.h … read-only Combo runtime API
src/
  dispatch.c    … raw_hid_received_event 受信・検証・分岐・HELLO応答
  layer_control.c
  time_sync.c
  ai_usage.c
  identity.c            … device_uid_hash / capabilities 生成
  studio_identity.c     … ZMK Studio get_device_info().serial_number を UID hex で上書き
  uplink.c              … uplink 共通 helper・HELLO後の初期 push
  battery_report.c      … BATTERY_STATUS uplink
  key_stats.c           … KEY_STATS uplink
  key_press.c           … KEY_PRESS uplink
  layer_state_report.c  … LAYER_STATE uplink
  encoder_runtime.c     … Config RPC ENCODER override・settings保存・sensor event先取り
  behavior_identity.c   … behavior identity hashの実装
  combo_runtime.c       … Combo runtime・Settings tableの読込
  combo_runtime_contract.c … /combos とCombo runtimeの構成チェック
  behaviors/
    behavior_host_action.c … &host_action behavior + HOST_ACTION uplink
dts/
  behaviors/host_action.dtsi             … &host_action node
  bindings/behaviors/zmk,behavior-host-action.yaml
```

ポイント:
- **疎結合**: 時刻・AI使用率のgetterは `K_MUTEX` 付き状態で公開し、機能CONFIGが無効な場合にも
  inlineスタブで呼び出せます。ほかのruntime APIは対応機能を有効にした構成で利用します。
- **拡張**: 独自パケット型を足したい場合は `dispatch.c` の `switch` に分岐を追加するか、
  将来的にコールバック登録口を設ける。

新キーボードに必要なのは west.yml / build.yaml / .conf の設定とキーマップ割当のみで、C コードは不要。

## デバイス個体識別（`device_uid_hash` + `capabilities`）

DEVICE_HELLO は `capabilities (u32)` と `device_uid_hash (u64)` を持ちます。
RawHID-Host が複数デバイスを個別識別し、デバイスごとに異なる設定を適用するために使います。

**DEVICE_HELLO のフォーマット:**
```
byte 0..1   magic "HL"
byte 2      version 0x02
byte 3      DEVICE_HELLO (0x02)
byte 4      seq
byte 5      feature = 0
byte 6      op = 0
byte 7      status_or_flags = 0
byte 8      payload_len = 12
byte 9..11  reserved
byte 12..15 capabilities  u32 LE
byte 16..23 device_uid_hash  u64 LE
byte 24..63 reserved
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
| 9 | CONFIG_RPC | `RAWHID_APP_CONFIG_RPC` |
| 10 | AI_CLIENT_STATE | `RAWHID_APP_AI_CLIENT_STATE` と `RAWHID_APP_AI_CLIENT_STATE_RENDERER`の両方 |

Host 側はこのビットを見て、未対応デバイスへのパケット送信をスキップできます。

## ZMK Studio の個体識別（`get_device_info().serial_number`）

RawHID-Host は、Host Link で接続したデバイスと **ZMK Studio** で接続したデバイスを
「同一個体」として紐付けたい（キーテスター／ヒートマップの統計紐付けなど）。

upstream の ZMK は `get_device_info().serial_number` に `hwinfo_get_device_id()` の**生バイト**を
入れるため、(1) Host Link の `device_uid_hash` とは表現が異なり一致せず、(2) hwinfo が無い個体や
BLE 接続では**空**になりやすい。

そこで `CONFIG_RAWHID_APP` + `CONFIG_ZMK_STUDIO_RPC` のとき、`src/studio_identity.c` が
`core/get_device_info` ハンドラを上書きし、`serial_number` に
**`device_uid_hash` と同じ u64 を 16 桁の小文字 hex 文字列**（`%016llx` 相当・最上位ニブル先頭）で入れる。
これにより常に非空・再起動後も安定・同一名キーボードが複数台あっても区別可能になる。

```
serial_number(ASCII, UTF-8) = lower_hex(device_uid_hash_u64, 16桁, 最上位ニブル先頭)
```

- `device_uid_hash == 0`（identity unavailable）の場合も `"0000000000000000"` を返して空にはしないが、
  複数個体で衝突するため `LOG_WRN` で警告を出す。通常は `rawhid_app_identity_get_uid_hash()` が
  非ゼロの安定 UID を返す前提。
- proto（`serial_number` は `bytes`）も `device_uid_hash` の生成ルールも変更していない。

**Host 側の照合:** `DEVICE_HELLO` の `device_uid_hash`(u64 LE) を同じく `%016x`（小文字・最上位ニブル先頭）
に整形した文字列と、`get_device_info().serial_number`(UTF-8) を直接比較すれば同一個体と判定できる。

**上書きの仕組み（メモ）:** ZMK は RPC ハンドラを iterable section にシンボル名順（リンカ `SORT_BY_NAME`）で
並べ、`request_choice` が一致した**最初の1件**を呼ぶ。`src/studio_identity.c` のハンドラ変数名
`core_subsystem_handler_00_get_device_info` は、`core_` プレフィックスで core ブロック内に収まりつつ
upstream の `core_subsystem_handler_get_device_info` より前にソートされるよう意図的に命名している
（この名前で upstream を上書きしているため、安易にリネームしないこと）。

---

## ライセンス

MIT License
