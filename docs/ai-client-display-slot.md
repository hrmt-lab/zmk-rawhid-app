# AI Client Display Slot契約

## 目的

1台のキーボードに複数のScreenKeyがある構成で、Hostが選んだ論理表示slotごとのAI状態を、
対応する物理ScreenKeyへ独立して表示する。Host Link version、packet type `0xA0`、
feature `0x0A`、op／flags `0x00`は変更しない。互換性はcapability bitとPayload長だけで保つ。

Host側のslot割当（Auto／固定、slot数変更、`cycle_ai_session`）はKeylink Studioの責務であり、
Firmwareはwireで届いた`display_slot`をそのまま物理画面へ写像するだけである。

## CapabilityとPayload

- bit 10 `CAP_AI_CLIENT_STATE`: AI Client Stateを受信できる。
- bit 11 `CAP_AI_CLIENT_WORK_PHASE`: 末尾`work_phase`を受信できる。
- bit 13 `CAP_AI_CLIENT_DISPLAY_SLOT`: 末尾`display_slot`付きの8 byte payloadを受信し、
  slotごとに別の物理Rendererへ表示できる。
- bit 13は`RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_RENDERER`が有効なときだけ広告し、
  必ずbit 10とbit 11も同時に広告する。単独では広告しない。
- bit 13を広告しないdeviceへ、Hostはslot 0だけを従来の6／7 byte形式で送る。

| offset | size | field | bit 10のみ | bit 11対応 | bit 13対応 |
|---:|---:|---|---|---|---|
| 0 | 1 | `client_type` | 同じ | 同じ | 同じ |
| 1 | 1 | `client_variant` | 同じ | 同じ | 同じ |
| 2 | 1 | `session_active` | 同じ | 同じ | 同じ |
| 3 | 1 | `activity_state` | 同じ | 同じ | 同じ |
| 4 | 2 | `revision`（u16 LE） | 同じ | 同じ | 同じ |
| 6 | 1 | `work_phase` | なし | 追加 | 同じ |
| 7 | 1 | `display_slot` | なし | なし | 追加 |

- `payload_len=6` と `payload_len=7` は従来どおりdecodeし、`display_slot = 0`として扱う。
- `payload_len=8` の末尾1 byteが`display_slot`である。
- 有効な`display_slot`は`0..=7`。`8`以上はpacket全体をrejectする。
  これは`work_phase`の未知値正規化とは異なり、宛先が特定できない状態を推測しないためである。
- 上記以外のpayload長はrejectする。`payload_len`より後ろのpayload byteが非zeroの場合も
  従来どおりrejectする。
- `display_slot`はscreen ID、thread ID、session IDではない。wireへsession IDやthread IDは載せない。

## slotと物理ScreenKeyの対応

`display_slot n` を物理ScreenKey `n` へ写像する。キーボードごとの個別割当はしない。
複数キーボードが接続されている場合、Hostは同じslotの状態を各キーボードへ同報してよい。

物理ScreenKey数はKconfigで宣言する。

| Kconfig | 意味 |
|---|---|
| `RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_RENDERER` | slotごとに別Rendererを持つ。bit 13の広告条件 |
| `RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_COUNT` | Coreが保持するslot数（`1..8`、既定`1`） |

既定は`1`なので、単一画面の既存targetのRAMと挙動は変わらない。
`RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_COUNT`より大きいslot宛の更新は、
CoreがLOG_DBGを残して破棄する。保持中のどのslotのstate、revision、timeoutにも影響しない。
Renderer側も、自分に対応する画面が無いslotのeventを無視する。

## slot別のstateとrevision

- Coreはslotごとに独立した`rawhid_app_ai_client_state_model`を持つ。
  `client_type`、`client_variant`、`session_active`、`activity_state`、`work_phase`、`revision`、
  そしてgenerationはすべてslot単位である。
- slot 1の更新はslot 0のstate、revision、generation、event、表示を変えない。逆も同様である。
- arrival-order last-write-wins、heartbeat抑制、同revision更新の扱い、未知work phaseの正規化は
  slot内で従来どおり動作する。詳細は[`ai-client-work-phase.md`](ai-client-work-phase.md)を参照する。
- ZMK event `rawhid_app_ai_client_state_changed` は`display_slot`を持つ。
  Rendererはこの値で対象画面を選ぶ。
- 15秒のHost timeoutはslotごとに独立している。slot 1が期限切れになってもslot 0のstateは
  有効なまま残り、slot 1のtimeout eventだけが発行される。
- `session_active=false`または`activity_state=NONE`は、該当slotだけを従来どおり
  消去／待機表示へ戻す。

## API

```c
bool rawhid_app_ai_client_state_get_slot(uint8_t display_slot,
                                         struct rawhid_app_ai_client_state *state,
                                         uint32_t *state_generation);

/* slot 0のshorthand。単一画面Renderer向けに従来のまま残す。 */
bool rawhid_app_ai_client_state_get(struct rawhid_app_ai_client_state *state,
                                    uint32_t *state_generation);
```

存在しないslotを渡した場合は`false`を返す。

## HOST_ACTION

`HOST_ACTION`のpacket形式は変更しない。`&host_action <action_id> <value>`の`value`は
Host側で対象slot番号（`0..=7`）として解釈される。`value=0`は従来どおりslot 0である。
Firmware Coreに変更は不要である。

## 実機確認

2枚のScreenKeyを持つ実機（`zmk-config-multiscreenkey`）で次を確認済みである。

- `DEVICE_HELLO`のcapabilityが`0x3FF7`となり、bit 13を bit 10／11 と同時に広告すること。
- Hostが8-byte payloadでslot 0とslot 1を送り分け、Coreがslot別に受理すること。
- 2画面が同時に点灯し、slotごとに別の内容を描画できること。

次は**未実施**である。

- 実データでのslot 0／slot 1の独立表示と非干渉（固定パターンでの確認のみ）
- slot別`cycle_ai_session`
- 複数キーボードへの同報
- SPI共有時の描画レート
- LVGL 2画面分のRAM実測

ホストCテストの実行方法と検証分担は
[`ai-client-state-host-tests.md`](ai-client-state-host-tests.md)を参照する。
