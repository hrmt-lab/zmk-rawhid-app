# AI Client Work Phase契約

## 目的

Host Link v2の`AI_CLIENT / STATE_UPDATE (0xA0)`で、既存の上位`activity_state`を維持したまま
`WORKING`中の処理種別をFirmwareへ渡す。旧deviceとの互換性はcapabilityとPayload長で保つ。

## CapabilityとPayload

- bit 10 `CAP_AI_CLIENT_STATE`: AI Client Stateを受信できる。
- bit 11 `CAP_AI_CLIENT_WORK_PHASE`: 末尾`work_phase`を受信できる。
- bit 11はAI Client State CoreとRenderer宣言がともに有効な場合だけ、bit 10と同時に広告する。
- Host Link version、packet type `0xA0`、feature `0x0A`、op／flags `0x00`は変更しない。

| offset | size | field | bit 10のみ | bit 11対応 |
|---:|---:|---|---|---|
| 0 | 1 | `client_type` | 同じ | 同じ |
| 1 | 1 | `client_variant` | 同じ | 同じ |
| 2 | 1 | `session_active` | 同じ | 同じ |
| 3 | 1 | `activity_state` | 同じ | 同じ |
| 4 | 2 | `revision`（u16 LE） | 同じ | 同じ |
| 6 | 1 | `work_phase` | なし | 追加 |

- bit 10のみは`payload_len=6`で、decode時に`UNSPECIFIED`を補完する。
- bit 11対応は`payload_len=7`とする。同じdeviceへ6 byteと7 byteを二重送信しない。

## Work phase

| 値 | 名称 | 意味 |
|---:|---|---|
| `0x00` | `UNSPECIFIED` | 詳細不明。旧WORKING表示へfallbackする |
| `0x01` | `THINKING` | 推論・plan・応答生成中 |
| `0x02` | `EXECUTING` | command、tool、file変更などの実行中 |
| `0x03` | `SEARCHING` | 構造化eventで明示されたWeb検索中 |

`work_phase`が有効なのは`activity_state=WORKING`だけである。それ以外の上位状態では
`UNSPECIFIED`だけを受理する。未知phaseはbase stateを捨てず`UNSPECIFIED`へ正規化し、
呼び出し側が診断logを残す。既知の不正な組み合わせと、従来から不正な
`session_active`／`activity_state`の組み合わせはpacket全体をrejectする。

## State、revision、event

- state equalityには`work_phase`を含める。
- 同revisionでwork phaseだけが変わったpacketは新しいstateとして受理し、generationを進めて
  ZMK eventを発行する。base stateの同revision変更とは別結果として扱い、revision警告を出さない。
- client、session、activity、revision、work phaseがすべて同じpacketはheartbeatとして扱い、
  generationもevent数も増やさない。Renderer animationは再開始しない。
- 15秒timeoutではstateを無効化し、公開eventのstateを全zero、work phaseを
  `UNSPECIFIED`へ戻す。

ホストCテストの実行方法と検証分担は
[`ai-client-state-host-tests.md`](ai-client-state-host-tests.md)を参照する。
