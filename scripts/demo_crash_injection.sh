#!/usr/bin/env bash
# Phase 8 demo 8 — Novelty 2A: the crash-injection harness.
#
#   ./scripts/demo_crash_injection.sh [trials]
#
# A short campaign (70 trials, 10 per crash point) that finishes in seconds.
# The full 1,050-trial campaign is scripts/crash_injection.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=./build/sthirdb
EV=docs/evidence
TRIALS=${1:-70}
mkdir -p "$EV"
say() { printf '%s\n' "$*"; }

main() {
  say "=== SthirDB demo: Novelty 2A — randomized crash injection ==="
  make -s all

  rm -rf ./data/demo_crash
  "$BIN" crash-harness --trials "$TRIALS" --txns 8 --seed 1 \
    --dir ./data/demo_crash --csv "$EV/phase8_demo_crash_trials.csv"

  passed=$(awk -F, 'NR>1 && $6==1' "$EV/phase8_demo_crash_trials.csv" | wc -l)
  total=$(awk -F, 'NR>1' "$EV/phase8_demo_crash_trials.csv" | wc -l)
  points=$(awk -F, 'NR>1 { print $3 }' "$EV/phase8_demo_crash_trials.csv" | sort -u | wc -l)

  say
  say "crash points covered: $points / 7"
  say "trials passed       : $passed / $total"
  if [ "$passed" -eq "$total" ] && [ "$points" -eq 7 ]; then
    say "RESULT: PASS — every crash recovered to a correct state."
    return 0
  fi
  say "RESULT: FAIL — see $EV/phase8_demo_crash_trials.csv"
  return 1
}

main 2>&1 | tee "$EV/phase8_demo_crash_injection.txt"
exit "${PIPESTATUS[0]}"
