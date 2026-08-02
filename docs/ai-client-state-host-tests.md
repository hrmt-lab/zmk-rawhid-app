# AI Client State ホストテスト

`tests/` と `tools/` は生成物ではない。AI Client StateのPayload decode、状態モデル、
ZMK event発行を、Zephyr実機ビルドより短いフィードバックで回帰確認するための
ホスト側Cテストである。

## 実行方法

リポジトリのルートで次を実行する。

```sh
tools/test-ai-client-state-core.sh
tools/test-ai-client-state-model.sh
tools/test-ai-client-state-packet.sh
```

各スクリプトは `/tmp` に一時バイナリを作成し、`cc -std=c11 -Wall -Wextra -Werror` で
コンパイルして実行する。リポジトリ内へ生成物は残さない。

## 対象範囲

- `test-ai-client-state-core.sh`: state更新、ZMK event、15秒timeoutを検証する。
  同revisionのphase-only変更ではeventを1件発行し、同一heartbeatではeventを増やさない。
  Zephyr依存は`tests/host_shim/`で最小限に置き換える。
- `test-ai-client-state-model.sh`: validation、到着順LWW、generation、phase-only更新、
  heartbeat、timeout後のUNSPECIFIED初期化を検証する。
- `test-ai-client-state-packet.sh`: 6 byte legacyと7 byte詳細Payload、全既知phase、
  未知phaseのUNSPECIFIED正規化、base state保持、長さと組み合わせ不正のrejectを検証する。

AI Client Stateのwire／state契約は
[`ai-client-work-phase.md`](ai-client-work-phase.md)を参照する。変更時は3スクリプトを
すべて実行し、`git diff --check`と対象Firmwareのfresh buildも行う。
