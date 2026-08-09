# AI Client State ホストテスト

`tests/` と `tools/` は生成物ではない。AI Client StateのPayload decode、状態モデル、
ZMK event発行を、Zephyr実機ビルドより短いフィードバックで回帰確認するための
ホスト側Cテストである。

## 実行方法

リポジトリのルートで次を実行する。

```sh
tools/test-ai-client-contract.sh
tools/test-ai-client-state-core.sh
tools/test-ai-client-state-model.sh
tools/test-ai-client-state-packet.sh
```

各スクリプトは `/tmp` に一時バイナリを作成し、`cc -std=c11 -Wall -Wextra -Werror` で
コンパイルして実行する。リポジトリ内へ生成物は残さない。

## 対象範囲

- `test-ai-client-contract.sh`: `src/ai_client_contract.h`の純粋関数を検証する。
  既知client type（`CODEX` / `CLAUDE_CODE`）の判定と、Core／Renderer／Claude Code Renderer／
  Display Slot Renderer宣言の組み合わせごとのcapability bit 10／11／12／13を検証する。
  bit 12とbit 13が単独広告されないこと、bit 13が必ずbit 10とbit 11を伴うこと、
  slot Rendererを宣言しないdeviceの広告が従来と同じであることも含む。
- `test-ai-client-state-core.sh`: state更新、ZMK event、15秒timeoutを検証する。
  同revisionのphase-only変更ではeventを1件発行し、同一heartbeatではeventを増やさない。
  Zephyr依存は`tests/host_shim/`で最小限に置き換える。
  このスクリプトは同じテストを2回ビルドして実行する。1回目は既定の1 slot構成で、
  6／7 byte payloadがslot 0へ従来どおり届くことと、存在しないslotが無視されることを見る。
  2回目は`-DCONFIG_RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_COUNT=2`で、slot 0とslot 1の
  state／revision／generation／event／timeoutが互いに干渉しないことを見る。
- `test-ai-client-state-model.sh`: validation、到着順LWW、generation、phase-only更新、
  heartbeat、timeout後のUNSPECIFIED初期化を検証する。モデル自体はslot非依存である。
- `test-ai-client-state-packet.sh`: 6 byte legacy、7 byte詳細、8 byte slot付きPayload、
  全既知phase、未知phaseのUNSPECIFIED正規化、base state保持、
  `display_slot 0..=7`のdecodeと`8`以上のreject、長さと組み合わせ不正のrejectを検証する。

AI Client Stateのwire／state契約は
[`ai-client-work-phase.md`](ai-client-work-phase.md)と
[`ai-client-display-slot.md`](ai-client-display-slot.md)を参照する。変更時は4スクリプトを
すべて実行し、`git diff --check`と対象Firmwareのfresh buildも行う。
