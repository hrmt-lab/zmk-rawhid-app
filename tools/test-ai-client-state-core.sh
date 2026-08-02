#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/rawhid-app-ai-client-state-core-test"

mkdir -p "$build_dir"
cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/tests/host_shim" \
  -I"$repo_root/include" \
  -I"$repo_root/src" \
  "$repo_root/tests/ai_client_state_core_test.c" \
  -o "$build_dir/ai_client_state_core_test"
"$build_dir/ai_client_state_core_test"

printf 'AI client state core test passed.\n'
