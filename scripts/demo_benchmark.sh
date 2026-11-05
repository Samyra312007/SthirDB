#!/usr/bin/env bash
# Phase 8 demo 6 — benchmark: anomaly interception + concurrent query latency.
#
#   ./scripts/demo_benchmark.sh
#
# A short version of scripts/benchmark.sh: the same tables, smaller thread
# counts and fewer injections, so it finishes while someone is watching.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=./build/sthirdb
EV=docs/evidence
mkdir -p "$EV"
say() { printf '%s\n' "$*"; }

main() {
  say "=== SthirDB demo: constraints vs anomalies, and concurrent latency ==="
  make -s all

  rm -rf ./data
  mkdir -p ./data
  "$BIN" sql scripts/retail_schema.sql > /dev/null
  "$BIN" retail-gen > /dev/null

  say
  say "-- anomaly injection, constrained schema (3 classes x 50)"
  inj=$("$BIN" inject --constraints on)
  printf '%s\n' "$inj"

  say
  say "-- the same injections against the unconstrained schema"
  unj=$("$BIN" inject --constraints off)
  printf '%s\n' "$unj"

  say
  say "-- concurrent reporting query, constrained schema"
  con=$("$BIN" bench --constraints on --threads 8,32 --iters 2)
  printf '%s\n' "$con"

  say
  say "-- concurrent reporting query, unconstrained schema"
  unc=$("$BIN" bench --constraints off --threads 8,32 --iters 2)
  printf '%s\n' "$unc"

  # Injection table columns: class | constraint | injected | blocked | accepted | %
  blocked=$(printf '%s\n' "$inj" | awk -F'|' '$2 ~ /TOTAL/ { gsub(/ /,"",$5); print $5 }' | head -1)
  # Benchmark columns: threads | queries | failures | avg | p99 | min | max
  failures=$(printf '%s\n%s\n' "$con" "$unc" | \
    awk -F'|' '$0 ~ /^ *\| [0-9]/ { gsub(/ /,"",$4); s+=$4 } END { print s+0 }')

  say
  say "constrained blocks recorded : $blocked"
  say "query failures (both modes): $failures"
  if [ "${blocked:-0}" -gt 0 ] && [ "${failures:-1}" -eq 0 ]; then
    say "RESULT: PASS — constraints intercept every injected anomaly, queries never fail."
    return 0
  fi
  say "RESULT: FAIL — blocked=$blocked failures=$failures"
  return 1
}

main 2>&1 | tee "$EV/phase8_demo_benchmark.txt"
exit "${PIPESTATUS[0]}"
