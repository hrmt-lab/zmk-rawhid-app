#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/rawhid-app-ai-client-state-packet-test"

mkdir -p "$build_dir"
cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/include" \
  -I"$repo_root/src" \
  "$repo_root/src/ai_client_state_packet.c" \
  "$repo_root/tests/ai_client_state_packet_test.c" \
  -o "$build_dir/ai_client_state_packet_test"
"$build_dir/ai_client_state_packet_test"

printf 'AI client state packet test passed.\n'
