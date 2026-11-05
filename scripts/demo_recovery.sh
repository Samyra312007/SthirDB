#!/usr/bin/env bash
# Phase 8 demo 5 — ARIES recovery, Tests A-H, with a visible verdict.
#
#   ./scripts/demo_recovery.sh
set -euo pipefail
cd "$(dirname "$0")/.."
EV=docs/evidence
mkdir -p "$EV"
say() { printf '%s\n' "$*"; }

main() {
  say "=== SthirDB demo: ARIES recovery (Tests A-H) ==="
  make -s build/test_recovery

  set +e
  output=$(./build/test_recovery 2>&1)
  status=$?
  set -e
  printf '%s\n' "$output"

  pass=$(printf '%s\n' "$output" | grep -c '^PASS' || true)
  fail=$(printf '%s\n' "$output" | grep -c '^FAIL' || true)
  say
  say "tests passed: $pass"
  say "tests failed: $fail"
  if [ "$status" -eq 0 ] && [ "$fail" -eq 0 ]; then
    say "RESULT: PASS — every ARIES verification test recovered the expected state."
    return 0
  fi
  say "RESULT: FAIL — recovery tests reported $fail failure(s) (exit $status)."
  return 1
}

main 2>&1 | tee "$EV/phase8_demo_recovery.txt"
exit "${PIPESTATUS[0]}"
