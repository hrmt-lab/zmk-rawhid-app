#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/rawhid-app-ai-client-state-core-test"

mkdir -p "$build_dir"

# Built twice: once with the default single display slot to cover the legacy
# single screen behaviour, and once with two slots to cover the per-slot
# isolation a multi screen renderer depends on.
run_variant() {
  local name="$1"
  shift
  cc -std=c11 -Wall -Wextra -Werror \
    -I"$repo_root/tests/host_shim" \
    -I"$repo_root/include" \
    -I"$repo_root/src" \
    "$@" \
    "$repo_root/tests/ai_client_state_core_test.c" \
    -o "$build_dir/$name"
  "$build_dir/$name"
}

run_variant ai_client_state_core_test
run_variant ai_client_state_core_slots_test \
  -DCONFIG_RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_COUNT=2

printf 'AI client state core test passed.\n'
