#!/usr/bin/env bash
# Phase 8 demo 4 — the optimizer choosing different plans for different queries.
#
#   ./scripts/demo_optimizer.sh
#
# One query is cheap by index (a single key), one is cheap by full scan (a
# predicate that keeps most rows). The demo shows EXPLAIN for both, plus the
# estimate-versus-actual comparison the report discusses in section 5.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=./build/sthirdb
EV=docs/evidence
mkdir -p "$EV"

say() { printf '%s\n' "$*"; }
chosen() { awk -F'|' '$0 ~ /chosen_path/ { gsub(/^ +| +$/, "", $3); print $3 }'; }

main() {
  say "=== SthirDB demo: the optimizer picks the cheaper plan ==="
  make -s all

  rm -rf ./data
  mkdir -p ./data
  "$BIN" sql scripts/retail_schema.sql > /dev/null
  "$BIN" retail-gen > /dev/null

  selective="SELECT ItemName FROM Items WHERE ItemID = 4242"
  scanning="SELECT COUNT(*) FROM Items WHERE RetailPrice > 100"

  say
  say "---- Query A: one row out of 5,000 (a selective predicate)"
  say "QUERY: $selective"
  "$BIN" explain "$selective"
  "$BIN" optimizer "$selective"

  say
  say "---- Query B: almost every row (a non-selective predicate)"
  say "QUERY: $scanning"
  "$BIN" explain "$scanning"
  "$BIN" optimizer "$scanning"

  a=$("$BIN" optimizer "$selective" | chosen)
  b=$("$BIN" optimizer "$scanning" | chosen)

  say
  say "chosen for A: $a"
  say "chosen for B: $b"
  say
  if [ "${a#IndexScan}" != "$a" ] && [ "${b#SeqScan}" != "$b" ]; then
    say "RESULT: PASS — index for the selective query, sequential scan for the broad one."
    return 0
  fi
  say "RESULT: FAIL — expected IndexScan for A and SeqScan for B."
  return 1
}

main 2>&1 | tee "$EV/phase8_demo_optimizer.txt"
exit "${PIPESTATUS[0]}"
