# zmk-rawhid-app

[日本語](README.md)

A ZMK module that links your [host PC](https://github.com/hrmt-lab/Keylink-Studio) and
ZMK keyboard (dongle / central) bidirectionally over **RawHID**. Adding it to your build
unlocks the following on your keyboard:

- **Richer display info**: show host time, AI usage (Claude Code / Codex), and
  Central/Self and peripheral battery levels on displays such as Prospector
- **Host-driven layer switching**: let the host control keyboard layers, e.g. auto-switching
  layers based on the active app (the current layer can also be reported back to the host)
- **Trigger PC actions from a key**: bind `&host_action` to a key to run host-side actions
  such as showing a window or launching an app
- **Keystroke visibility**: send per-position keystroke counts and press/release events to
  the host for use in heatmaps and other statistics (key content itself is never sent)
- **Live configuration from Keylink Studio**: edit and save encoder and Combo settings
  that ZMK Studio can't handle, without rebuilding firmware

No keyboard-specific code is required — just **add the module and enable the CONFIG
options** you need. (Using uplinks additionally requires the `&host_action` keymap
binding and the relevant `.conf` options.)

The transport layer is [zmk-raw-hid](https://github.com/hrmt-lab/zmk-raw-hid); this module
implements the application-layer protocol (Host Link v2) on top of it. Each feature is
enabled individually via a capability bit in `DEVICE_HELLO`.

---

## Installation

Add to `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: hrmt-lab
      url-base: https://github.com/hrmt-lab
  projects:
    - name: zmk-raw-hid            # transport (required)
      remote: hrmt-lab
      revision: custom/raw-hid-custom
    - name: zmk-rawhid-app         # this module
      remote: hrmt-lab
      revision: main
```

Add the `raw_hid_adapter` shield (from zmk-raw-hid) alongside the dongle in `build.yaml`:

```yaml
include:
  - board: xiao_ble/nrf52840/zmk
    shield: <keyboard>_dongle raw_hid_adapter   # add prospector_adapter too if you use a display
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
# If you want to edit/save encoder bindings from ZMK Studio
CONFIG_RAWHID_APP_CONFIG_RPC=y
```

`CONFIG_RAWHID_APP` alone enables the HELLO response; add each sub-feature individually
on top of it.

---

## CONFIG

All of these are set with the `CONFIG_` prefix in the keyboard-side config (the `.conf`
in `zmk-config-xxx`). They all default to `n`, and enabling any sub-feature automatically
enables `RAWHID_APP` itself.

| CONFIG | Default | Description |
|---|---|---|
| `RAWHID_APP` | `n` | Enables the RawHID application layer (depends on `RAW_HID`). HELLO response |
| `RAWHID_APP_LAYER_CONTROL` | `n` | APP_LAYER (host-driven layer control) |
| `RAWHID_APP_TIME_SYNC` | `n` | TIME_SYNC (time sync + getter) |
| `RAWHID_APP_AI_USAGE` | `n` | AI_USAGE (usage state + getter) |
| `RAWHID_APP_AI_CLIENT_STATE` | `n` | AI Client State Core (validation, arrival-order LWW, 15-second timeout) |
| `RAWHID_APP_AI_CLIENT_STATE_RENDERER` | `n` | Statically declares that an AI state renderer is present |
| `RAWHID_APP_LAYER_STATE_REPORT` | `n` | LAYER_STATE uplink (current layer and mask) |
| `RAWHID_APP_BATTERY_REPORT` | `n` | BATTERY_STATUS uplink (Central/Self and peripheral levels) |
| `RAWHID_APP_HOST_ACTION` | `n` | HOST_ACTION uplink (`&host_action <id> <value>`) |
| `RAWHID_APP_KEY_STATS` | `n` | KEY_STATS uplink (uses `uint16_t * ZMK_KEYMAP_LEN` of RAM) |
| `RAWHID_APP_KEY_PRESS` | `n` | KEY_PRESS uplink (sends press/release events immediately) |
| `RAWHID_APP_CONFIG_RPC` | `n` | Enables the Config RPC ENCODER feature (edit + save to settings). Also exposes Combo feature queries when Combo runtime is enabled |
| `RAWHID_APP_COMBO_RUNTIME` | `n` | Keylink runtime Combo engine. Enable by setting `status = "disabled"` on `/combos` in an overlay |
| `RAWHID_APP_COMBO_SETTINGS` | `n` | Saves/loads the Combo Settings table to NVS. Requires Combo runtime and Config RPC |

If you enable `CONFIG_RAWHID_APP_COMBO_RUNTIME=y`, keep the Combo definitions themselves
in the keyboard's `.keymap` file as usual, and put only the `status = "disabled"` override
in an overlay (e.g. `<keyboard>_left.overlay` on the central side). Do not disable
`/combos` in `.keymap`.

```dts
// e.g. <keyboard>_left.overlay (central side)
/ {
    combos {
        status = "disabled";
    };
};
```

This prevents the build using Keylink runtime from generating the standard ZMK Combo
listener, avoiding a situation where both ZMK's standard Combo handling and Keylink
runtime process the same key events. Builds without the overlay keep `/combos` enabled.
Because the node is disabled via an overlay rather than deleted, the `/combos` children
remain in the devicetree and Keylink runtime can use them as the initial Combo
definitions.

---

## Using each feature

For the list of `CONFIG_RAWHID_APP_*` options that enable each feature and their default
values, see the "CONFIG" table above.

Headers live under the module's `include/` and are available as `<rawhid_app/...>` once
included in the build. The `time_sync.h` and `ai_usage.h` getters have inline stubs for
when their feature CONFIG is disabled. Other runtime APIs should only be used in builds
where the corresponding feature is enabled.

### Host action uplink

Add the behavior node to your keymap:

```dts
#include <behaviors/host_action.dtsi>
```

Add to `.conf`:

```ini
CONFIG_RAWHID_APP_HOST_ACTION=y
```

Use `&host_action <action_id> <value>` in the keymap. On split peripheral builds,
`RAWHID_APP_HOST_ACTION=n`, so it becomes a no-op and sends nothing.

Keymap example (assign to a key on any layer):

```dts
#include <behaviors/host_action.dtsi>

/ {
    keymap {
        compatible = "zmk,keymap";

        my_layer {
            bindings = <
                // send action_id = 1, value = 0 to the host
                &host_action 1 0
                // send action_id = 2, value = 5 (value is interpreted host-side)
                &host_action 2 5
            >;
        };
    };
};
```

The meaning of `action_id` (show window, pause monitoring, launch app, etc.) is defined
per device in the **host-side config allowlist**. The firmware just forwards `action_id`
and `value` as-is and assigns no meaning to them. Assign an action to the same number in
the [Keylink Studio](https://github.com/hrmt-lab/Keylink-Studio) app's **"Actions" screen**
(see the "Actions" section of `docs/manual-app-usage.md`). It only works once both sides
agree on the same `action_id`.

### ZMK Studio encoder override (CONFIG_RPC)

Add to `.conf`:

```ini
CONFIG_RAWHID_APP_CONFIG_RPC=y
```

If you assign mouse scroll to an encoder in Keylink Studio and want it to use the same
scroll amount and press timing as `.keymap`, also add the following bridge node to
`.keymap`:

```dts
keylink_encoder_runtime: keylink_encoder_runtime {
    compatible = "keylink,encoder-runtime";
    scroll-value = <ZMK_POINTING_DEFAULT_SCRL_VAL>;
    sensor-behavior = <&inc_dec_ms>;
};
```

`ZMK_POINTING_DEFAULT_SCRL_VAL` references the existing scroll amount defined in
`.keymap`, and `&inc_dec_ms` references the existing sensor behavior with `tap-ms` set.
You don't need to duplicate these values here. This node only passes the configured
values to rawhid-app for Keylink Studio's benefit — it doesn't replace the normal
`sensor-bindings`, and no changes to ZMK itself are needed.

Host Link v2's Config RPC lets you edit a runtime CW/CCW override per encoder. The
ENCODER feature supports `GET_INFO`, `GET_BINDINGS`, `SET_BINDINGS`, `GET_DIRTY`, `SAVE`,
`DISCARD`, and `CLEAR_OVERRIDE`. `SET_BINDINGS` only changes RAM state; `SAVE` persists it
to settings/NVS.

Overrides are keyed by ZMK Studio's `(Layer.id, encoder_id)` rather than layer index.
Deleting a layer in between does not shift the overrides of later layers. Records for a
deleted layer are kept as tombstones, and only the matching
`keylink/enc/v1/l<layer_id>/e<encoder_id>` is removed on SAVE. On DISCARD, a saved binding
is restored only if Studio's layer DISCARD restores the same Layer.id.

Settings records whose Layer.id currently doesn't exist are kept at boot too, and are
never applied to any layer or executed. `GET_DIRTY` reports these orphan records as dirty
so the next SAVE removes them. Encoder events with no override still fall through to
`.keymap`'s `sensor-bindings` as before.

### Querying the Combo runtime (CONFIG_RPC)

With `CONFIG_RAWHID_APP_COMBO_RUNTIME=y` and `CONFIG_RAWHID_APP_CONFIG_RPC=y` enabled, you
can query the runtime Combo table via the CONFIG_RPC COMBO feature (`feature=0x02`). It is
currently read-only, supporting only `GET_INFO=0x01` and `GET_COMBO=0x02`. `SET_COMBO`,
`GET_DIRTY`, `SAVE`, `DISCARD`, `DELETE_COMBO`, and `RESET_TO_KEYMAP` have reserved op
values but currently return `UNSUPPORTED_OP`.

To use the Combo persisted table, also enable `CONFIG_RAWHID_APP_COMBO_SETTINGS=y`. This
requires the NVS Settings backend and stores a fixed-length table at
`keylink/cmb/v1/table`. Combo runtime alone can still use the keymap-derived definitions
read-only.

### Battery level (BATTERY_STATUS uplink)

Add to `.conf`:

```ini
CONFIG_RAWHID_APP_BATTERY_REPORT=y
```

Receives ZMK's battery events and sends Central/Self and peripheral levels. It sends both
on level change and roughly every 5 minutes; when a level is unknown, unsupported, or the
split link is disconnected, it sends `0xFF` (unknown / not available / disconnected). No
keymap or C code changes needed.

`source=0` is Central/Self (the RawHID endpoint); `source=1..3` are Peripheral 1..3. On
builds where Central/Self's level can't be read (e.g. a USB-powered dongle), `source=0,
level=0xFF` is sent. The host can exclude sources with `0xFF` from its normal display, so
in that case it can show peripheral-only info like `P1:N%`.

While a peripheral is disconnected or powered off, the corresponding level on the firmware
side stays `0xFF`. This does not mean the packet failed to arrive — it means the
`BATTERY_STATUS` level is genuinely unknown. Once the peripheral connects and
`zmk_peripheral_battery_state_changed` fires, the real `0..100` level is sent.

### Key statistics (KEY_STATS uplink)

Add to `.conf`:

```ini
CONFIG_RAWHID_APP_KEY_STATS=y
```

Receives `position_state_changed`, counts presses per key position, and sends only the
**non-zero positions** roughly every 45 seconds, clearing them to zero afterward. Only
"position and count" is sent — never what was actually typed. It uses
`uint16_t * ZMK_KEYMAP_LEN` of RAM, so check usage before enabling on RAM-constrained
builds.

### Layer state reporting (LAYER_STATE uplink)

Add to `.conf`:

```ini
CONFIG_RAWHID_APP_LAYER_STATE_REPORT=y
```

Receives `layer_state_changed` and sends the topmost active layer and layer mask to the
host (roughly 50ms debounced). The host uses this for **display only** and does not echo
it back as APP_LAYER. No keymap or C code changes needed.

### Common uplink behavior

- After receiving `HOST_HELLO`, LAYER_STATE / BATTERY_STATUS initial state is pushed with
  a roughly 150ms delay (to avoid sending before capability registration and having the
  host discard it).
- Uplinks are best-effort. Packets sent while the host isn't reading (e.g. monitoring
  paused) are lost.
- Over BLE transport, the underlying `zmk-raw-hid` holds the send buffer until the notify
  completes, so uplinks must be serialized through a queue. `LAYER_STATE` and
  `BATTERY_STATUS` tend to occur back-to-back right after `DEVICE_HELLO`, so when using
  BLE, assume a notify queue implementation equivalent to `zmk-raw-hid`'s
  `custom/raw-hid-custom`.

### Layer control (APP_LAYER)

Add to `.conf`:

```ini
CONFIG_RAWHID_APP_LAYER_CONTROL=y
```

No keymap or C code changes needed. When the host sends an `APP_LAYER` packet, the
keyboard automatically calls `zmk_keymap_layer_activate()` /
`zmk_keymap_layer_deactivate()`.

- **SET**: Activates the specified layer. Any layer previously activated by the host is
  automatically deactivated.
- **CLEAR**: Deactivates only the layer the host activated. Manually activated layers are
  left untouched.

The host only ever manages one layer at a time. It works as long as the layer number sent
by the host (0..31) has a corresponding layer defined in the keymap.

### Time display (TIME_SYNC)

Add to `.conf`:

```ini
CONFIG_RAWHID_APP_TIME_SYNC=y
```

Using `unix_time_sec + tz_offset_min` from when the host sent the `TIME_SYNC` packet as a
reference point along with `k_uptime_get()`, the keyboard keeps advancing time on its own
afterward. Time keeps ticking even if RawHID disconnects.

Call the getter from your display drawing code:

```c
#include <rawhid_app/time_sync.h>

char buf[24];
if (rawhid_app_time_sync_format(buf, sizeof(buf))) {
    // buf now holds a time string -> draw it on screen
} else {
    // TIME_SYNC not received yet -> show "--:--" or similar
}

// Refresh every second for seconds-showing formats (HMS), every minute is enough otherwise
bool needs_fast_refresh = rawhid_app_time_sync_wants_seconds();
```

The output format switches automatically based on the host's `format_hint` /
`clock_mode`:

| Format | Example output |
|---|---|
| HM (default) | `14:30` |
| HMS | `14:30:05` |
| Y-M-D | `2026-06-06` |
| M-D | `06-06` |
| datetime | `2026-06-06 14:30` |
| weekday+HM | `Fri 14:30` |

### AI usage display (AI_USAGE)

Add to `.conf`:

```ini
CONFIG_RAWHID_APP_AI_USAGE=y
```

Every time the host sends an `AI_USAGE` packet, the latest state is overwritten per
provider (Codex / Claude Code). The getter returns a thread-safe copy.

Call the getter from your display drawing code:

```c
#include <rawhid_app/ai_usage.h>

struct rawhid_app_ai_usage_provider p;

// provider: 1=Codex, 2=Claude Code
if (rawhid_app_ai_usage_get(2 /* claude_code */, &p) && p.present) {

    // Usage is in basis points (10000 = 100.00%)
    uint16_t pct_5h = p.five_hour_used_bp;   // 5-hour window usage
    uint16_t pct_7d = p.seven_day_used_bp;   // 7-day window usage

    // Seconds remaining until reset (no TIME_SYNC required)
    int64_t elapsed_sec = (k_uptime_get() - p.received_uptime_ms) / 1000;
    int64_t remaining_5h = (int64_t)p.five_hour_reset_unix - (int64_t)p.updated_unix - elapsed_sec;
    int64_t remaining_7d = (int64_t)p.seven_day_reset_unix - (int64_t)p.updated_unix - elapsed_sec;

    // Error check
    if (p.flags & RAWHID_APP_AI_USAGE_FLAG_ERROR_PRESENT) {
        // Check p.error_code and show an error
    }
}
```

Key `flags` bits:

| Flag | Meaning |
|---|---|
| `RAWHID_APP_AI_USAGE_FLAG_FIVE_HOUR_VALID` (bit0) | 5-hour usage is valid |
| `RAWHID_APP_AI_USAGE_FLAG_SEVEN_DAY_VALID` (bit1) | 7-day usage is valid |
| `RAWHID_APP_AI_USAGE_FLAG_ESTIMATED` (bit2) | Estimated value (lower accuracy) |
| `RAWHID_APP_AI_USAGE_FLAG_STALE` (bit5) | Data is stale |
| `RAWHID_APP_AI_USAGE_FLAG_ERROR_PRESENT` (bit7) | Check `error_code` |

### About the CONFIG guards

All three features have `#else` stubs in their headers, so display-side code only needs an
`#if IS_ENABLED(...)` guard to always build:

```c
#if IS_ENABLED(CONFIG_RAWHID_APP_TIME_SYNC)
    // time display handling
#endif

#if IS_ENABLED(CONFIG_RAWHID_APP_AI_USAGE)
    // AI usage display handling
#endif
```

---

## Protocol specification (v2)

Host Link packets are a **fixed 64 bytes**, little-endian.

| offset | contents |
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
| 12..63 | payload (unused region past `payload_len` is 0) |

| Value | Name | Direction |
|---|---|---|
| `0x01` | HOST_HELLO | H→D |
| `0x02` | DEVICE_HELLO | D→H |
| `0x03` | ERROR | reserved |
| `0x04` | PING | reserved |
| `0x05` | PONG | reserved |
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

`0x40`–`0x80` are device → host uplinks. To send them, set the corresponding capability
bit in `DEVICE_HELLO` (the host discards types whose bit isn't set). `0x90` is the
host → device CONFIG_REQUEST, and `0x91` is the device → host CONFIG_RESPONSE.

Validation (`src/dispatch.c`): magic / version / known type / length==64 /
`payload_len<=52` / header reserved and unused payload region are 0.

### HELLO

HOST_HELLO has `payload_len=0`. It returns DEVICE_HELLO with the same header seq as the
HOST_HELLO it answers. DEVICE_HELLO has `payload_len=12`, with `capabilities u32 LE` and
`device_uid_hash u64 LE` in the payload.

### AI Client State (`0xA0`)

Uses `feature=0x0A`, `op=0`, and `flags=0`. The first six payload bytes contain
`client_type`, `client_variant`, `session_active`, `activity_state`, and `revision u16 LE`.
The legacy format for bit 10-only devices uses `payload_len=6` and defaults `work_phase`
to UNSPECIFIED. Devices advertising bit 11 `AI_CLIENT_WORK_PHASE` accept `payload_len=7`,
with `work_phase` at offset 6.

| work_phase | value | meaning |
|---|---:|---|
| UNSPECIFIED | `0x00` | Detail unavailable; falls back to the legacy WORKING display |
| THINKING | `0x01` | Reasoning or response generation |
| EXECUTING | `0x02` | Command or tool execution |
| SEARCHING | `0x03` | Web search |

Only UNSPECIFIED is valid outside WORKING. Unknown values are normalized to UNSPECIFIED
without dropping the base state and produce a diagnostic log. A work-phase-only change
with the same revision publishes a new state event, while an identical heartbeat does not.
The last semantically valid packet wins regardless of revision ordering. The current state
expires after 15 seconds without another valid update.

### APP_LAYER (`0x30`)

`payload_len=2`. `payload[0]` action (1=set, 2=clear) / `payload[1]` layer (0..31).
Tracks a single host-activated layer (`src/layer_control.c`).

### TIME_SYNC (`0x20`)

`payload_len=9`. `payload[0..4]` unix_sec (u32) / `payload[4..6]` tz_offset_min (i16) /
`payload[6]` weekday (1=Mon..7=Sun) / `payload[7]` format_hint (0:HM 1:HMS 2:Y-M-D 3:M-D
4:datetime 5:weekday+HM) / `payload[8]` clock_mode (0:24h, 1:12h). Current time is
computed from the received timestamp plus `k_uptime` (`src/time_sync.c`).

### AI_USAGE (`0x10`)

`payload_len=19`. `payload[0]` provider (1=codex, 2=claude_code) / `payload[1]` flags /
`payload[2..4]` 5h_used_bp (u16) / `payload[4..6]` 7d_used_bp (u16) /
`payload[6..10]` 5h_reset (u32) / `payload[10..14]` 7d_reset (u32) /
`payload[14..18]` updated (u32) / `payload[18]` error_code.

bp is basis points (10000=100.00%, clamped to 0..10000 on receipt). Kept per provider
(`src/ai_usage.c`).

flags bits: `0` 5h_valid / `1` 7d_valid / `2` estimated / `3` local_history / `4`
quota_source / `5` stale / `6` fallback_limit / `7` error_present.

error_code: `0` none / `1` source_disabled / `2` missing_credentials / `3`
expired_credentials / `4` auth_failed / `5` rate_limited / `6` fetch_failed / `7`
parse_failed / `8` no_usage_data / `9` missing_limit.

### BATTERY_STATUS (`0x40`, D→H)

`payload_len=1+count*2`. `payload[0]` count (1..4) /
`payload[1+2i]` source[i] (0=Central/Self, 1=Peripheral 1, 2=Peripheral 2, 3=Peripheral 3)
/ `payload[2+2i]` level[i] (0..100, `0xFF`=unknown/not available/disconnected). Header seq
is fixed at 0 for MVP. `src/battery_report.c`.

`source=0` always represents Central/Self. On builds where Central/Self's level can't be
read, `source=0, level=0xFF` is sent. `source=1..3` don't indicate left/right — they are
ZMK's peripheral slot, 1-based.

Example: a USB-powered dongle with one peripheral sends `source=0, level=0xFF` and
`source=1, level=0..100`. The host can exclude the `0xFF` source from its normal display
and show peripheral-only info like `P1:N%`.

### HOST_ACTION (`0x50`, D→H)

`payload_len=2`. `payload[0]` action_id / `payload[1]` value. The meaning of `action_id` /
`value` is defined by host-side config. `src/behaviors/behavior_host_action.c`.

### KEY_STATS (`0x60`, D→H)

`payload_len=4+count*3`. `payload[0]` entry_count (1..8) /
`payload[1]` flags (bit0=MORE_FOLLOWS) / `payload[2..4]` reserved /
`payload[4+3i]` position[i] / `payload[5+3i..7+3i]` delta[i] (u16 LE, 0 is never sent).
Sends only non-zero positions periodically and clears them to zero. More than 8 entries
are split across multiple packets, with MORE_FOLLOWS set on all but the last.
`src/key_stats.c`.

### LAYER_STATE (`0x70`, D→H)

`payload_len=8`. `payload[0]` active_layer (0..31) / `payload[1..4]` reserved /
`payload[4..8]` layer_mask (u32 LE, bit i = layer i active). The host uses this for
display only. `src/layer_state_report.c`.

### KEY_PRESS (`0x80`, D→H)

`payload_len=2`. `payload[0]` position (u8) / `payload[1]` flags.

Flags bit0 is `1` for press, `0` for release. While `CONFIG_RAWHID_APP_KEY_PRESS=y`, a
press/release is sent immediately for every `zmk_position_state_changed`. Sends while
monitoring is paused or the host is disconnected are subject to the same RawHID send
constraints as other uplinks.

### CONFIG_REQUEST / CONFIG_RESPONSE (`0x90` / `0x91`)

When `CONFIG_RAWHID_APP_CONFIG_RPC=y`, the ENCODER feature (`feature=0x01`) is handled.
Ops are `GET_INFO=0x01`, `GET_BINDINGS=0x02`, `SET_BINDINGS=0x03`, `GET_DIRTY=0x04`,
`SAVE=0x05`, `DISCARD=0x06`, `CLEAR_OVERRIDE=0x07`. Layer.id is a u32 LE in the payload,
encoder_id is a u8.

When `CONFIG_RAWHID_APP_COMBO_RUNTIME=y` is also enabled, the COMBO feature
(`feature=0x02`) handles `GET_INFO=0x01` and `GET_COMBO=0x02`. Other Combo ops are
reserved for future use and currently return `UNSUPPORTED_OP`.

Config RPC's header and op/payload formats are fixed by Host Link v2. SAVE performs
settings operations on a workqueue and returns a response once complete. For the host's
timeout retries, the same response is resent from a short-lived response cache.

---

## Implementation structure (porting reference)

```
include/rawhid_app/
  packet.h      … packet types/structs/enums, handler declarations
  time_sync.h   … getter + #else stub
  ai_usage.h    … flags/struct/getter + #else stub
  identity.h    … device_uid_hash / capabilities getter declarations
  uplink.h      … uplink common helper (prepare/seq/send/initial push) declarations
  encoder_runtime.h … encoder override runtime API
  behavior_identity.h … behavior identity hash for settings records
  combo_runtime.h … read-only Combo runtime API
src/
  dispatch.c    … raw_hid_received_event receive/validate/dispatch/HELLO response
  layer_control.c
  time_sync.c
  ai_usage.c
  identity.c            … device_uid_hash / capabilities generation
  studio_identity.c     … overrides ZMK Studio's get_device_info().serial_number with UID hex
  uplink.c              … uplink common helper, initial push after HELLO
  battery_report.c      … BATTERY_STATUS uplink
  key_stats.c           … KEY_STATS uplink
  key_press.c           … KEY_PRESS uplink
  layer_state_report.c  … LAYER_STATE uplink
  encoder_runtime.c     … Config RPC ENCODER override, settings persistence, sensor event interception
  behavior_identity.c   … behavior identity hash implementation
  combo_runtime.c       … Combo runtime, Settings table loading
  combo_runtime_contract.c … configuration check for /combos and Combo runtime
  behaviors/
    behavior_host_action.c … &host_action behavior + HOST_ACTION uplink
dts/
  behaviors/host_action.dtsi             … &host_action node
  bindings/behaviors/zmk,behavior-host-action.yaml
```

Key points:
- **Loose coupling**: the time/AI-usage getters expose state guarded by a `K_MUTEX` and
  can be called via inline stubs even when their feature CONFIG is disabled. Other runtime
  APIs should only be used in builds where the corresponding feature is enabled.
- **Extensibility**: to add a custom packet type, add a case to the `switch` in
  `dispatch.c`, or set up a callback registration point in the future.

New keyboards only need west.yml / build.yaml / `.conf` settings and keymap bindings — no
C code required.

## Per-device identity (`device_uid_hash` + `capabilities`)

DEVICE_HELLO carries `capabilities (u32)` and `device_uid_hash (u64)`. RawHID-Host uses
these to distinguish multiple devices individually and apply per-device settings.

**DEVICE_HELLO format:**
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

**Related files:**

- `include/rawhid_app/identity.h` — declarations for `rawhid_app_identity_get_uid_hash()` /
  `rawhid_app_identity_get_capabilities()`
- `src/identity.c` — implementation

**`device_uid_hash` generation logic:**

```
1. CONFIG_HWINFO enabled and hwinfo_get_device_id() succeeds
   -> use a FNV-1a 64-bit hash of the hardware ID
   -> stays the same hash even if the USB port changes

2. hwinfo unavailable (CONFIG_SETTINGS enabled)
   -> read "raw_hid/identity_seed" (128-bit) from NVS
   -> if absent, generate randomly at boot, save it, and reuse the same value afterward

3. Both unavailable
   -> device_uid_hash = 0 (identity unavailable)
```

The raw hardware ID is never sent to the host; only a value hashed with FNV-1a 64-bit
using the namespace `"zmk-raw-hid-device-uid-v1"` and the seed is sent. A hash result of 0
is corrected to 1.

**`capabilities` generation logic:**

Auto-generated from the existing Kconfig.

| bit | feature | corresponding CONFIG |
|---|---|---|
| 0 | APP_LAYER | `RAWHID_APP_LAYER_CONTROL` |
| 1 | TIME_SYNC | `RAWHID_APP_TIME_SYNC` |
| 2 | AI_USAGE | `RAWHID_APP_AI_USAGE` |
| 3 | THEME | not implemented (always 0) |
| 4 | BATTERY_STATUS | `RAWHID_APP_BATTERY_REPORT` |
| 5 | HOST_ACTION | `RAWHID_APP_HOST_ACTION` |
| 6 | KEY_STATS | `RAWHID_APP_KEY_STATS` |
| 7 | LAYER_STATE | `RAWHID_APP_LAYER_STATE_REPORT` |
| 8 | KEY_PRESS | `RAWHID_APP_KEY_PRESS` |
| 9 | CONFIG_RPC | `RAWHID_APP_CONFIG_RPC` |
| 10 | AI_CLIENT_STATE | both `RAWHID_APP_AI_CLIENT_STATE` and `RAWHID_APP_AI_CLIENT_STATE_RENDERER` |
| 11 | AI_CLIENT_WORK_PHASE | same condition as bit 10; always advertised together with bit 10 |

The host side can use this bit to skip sending packets to unsupported devices.

## ZMK Studio identity (`get_device_info().serial_number`)

RawHID-Host wants to link a device connected via Host Link with the same device connected
via **ZMK Studio** as "the same physical device" (e.g. for tying key-tester/heatmap stats
together).

Upstream ZMK puts the **raw bytes** of `hwinfo_get_device_id()` into
`get_device_info().serial_number`, so (1) it's represented differently from Host Link's
`device_uid_hash` and doesn't match it, and (2) it tends to be **empty** on devices without
hwinfo or over a BLE connection.

To address this, when `CONFIG_RAWHID_APP` + `CONFIG_ZMK_STUDIO_RPC` are both set,
`src/studio_identity.c` overrides the `core/get_device_info` handler and puts
**the same u64 as `device_uid_hash`, formatted as a 16-digit lowercase hex string**
(equivalent to `%016llx`, most significant nibble first) into `serial_number`. This makes
it always non-empty, stable across reboots, and distinguishable even when multiple
keyboards share the same product name.

```
serial_number(ASCII, UTF-8) = lower_hex(device_uid_hash_u64, 16 digits, most significant nibble first)
```

- Even when `device_uid_hash == 0` (identity unavailable), `"0000000000000000"` is
  returned rather than leaving it empty, but this collides across multiple devices, so a
  `LOG_WRN` warning is logged. Normally `rawhid_app_identity_get_uid_hash()` is expected to
  return a non-zero, stable UID.
- Neither the proto (`serial_number` is `bytes`) nor `device_uid_hash`'s generation rule
  has been changed.

**Host-side matching:** you can determine it's the same physical device by directly
comparing `DEVICE_HELLO`'s `device_uid_hash` (u64 LE), formatted the same way as `%016x`
(lowercase, most significant nibble first), against `get_device_info().serial_number`
(UTF-8).

**How the override works (note):** ZMK places RPC handlers in an iterable section sorted
by symbol name (linker `SORT_BY_NAME`), and calls the **first** one whose
`request_choice` matches. The handler variable name in `src/studio_identity.c`,
`core_subsystem_handler_00_get_device_info`, is deliberately named to stay within the core
block via the `core_` prefix while sorting before upstream's
`core_subsystem_handler_get_device_info` (do not casually rename this — the override
depends on this exact name).

---

## License

MIT License
