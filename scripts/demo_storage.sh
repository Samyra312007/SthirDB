#!/usr/bin/env bash
# Phase 8 demo 1 — storage layer: create the schema, bulk-load rows, count them.
#
#   ./scripts/demo_storage.sh
#
# Proves the bottom of the stack on its own: a page-formatted table file can be
# created, filled through the disk manager and buffer pool, and read back.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=./build/sthirdb
EV=docs/evidence
mkdir -p "$EV"

say() { printf '%s\n' "$*"; }

main() {
  say "=== SthirDB demo: storage (create -> load -> count) ==="
  make -s all

  rm -rf ./data
  mkdir -p ./data

  say
  say "-- 1. create ./data/retail and the Items table"
  "$BIN" init_schema

  say
  say "-- 2. bulk-insert 5,000 rows through the buffer pool"
  "$BIN" load_data --rows 5000

  say
  say "-- 3. count the live rows"
  count=$("$BIN" run "SELECT COUNT(*) FROM Items" | awk 'NR==4 { gsub(/[^0-9]/, ""); print }')

  say
  say "-- 4. sample rows"
  "$BIN" run "SELECT * FROM Items ORDER BY id LIMIT 3" 2>/dev/null \
    || "$BIN" run "SELECT * FROM Items ORDER BY id" | sed -n '1,8p'

  say
  say "rows loaded : 5000"
  say "rows counted: ${count:-?}"
  if [ "${count:-0}" = "5000" ]; then
    say "RESULT: PASS — every row written survived the round trip to disk."
    return 0
  fi
  say "RESULT: FAIL — expected 5000 rows, counted ${count:-?}."
  return 1
}

main 2>&1 | tee "$EV/phase8_demo_storage.txt"
exit "${PIPESTATUS[0]}"
