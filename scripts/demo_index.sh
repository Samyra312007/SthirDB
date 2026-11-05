#!/usr/bin/env bash
# Phase 8 demo 3 — the same query through SeqScan and through IndexScan.
#
#   ./scripts/demo_index.sh
#
# `optimizer` reports pages_read for the chosen path; `--seq` and `--index`
# force one path so the two can be compared on identical data.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=./build/sthirdb
EV=docs/evidence
mkdir -p "$EV"

metric() { awk -F'|' -v key="$1" '$0 ~ key { gsub(/^ +| +$/, "", $3); print $3 }'; }
say() { printf '%s\n' "$*"; }

main() {
  say "=== SthirDB demo: index scan vs sequential scan ==="
  make -s all

  say
  say "-- building the retail dataset (seed 42, deterministic) ..."
  rm -rf ./data
  mkdir -p ./data
  "$BIN" sql scripts/retail_schema.sql > /dev/null
  "$BIN" retail-gen > /dev/null

  q="SELECT ItemName FROM Items WHERE ItemID = 4242"

  say
  say "QUERY: $q"
  say
  say "-- IndexScan (the primary-key index created by the constraint)"
  "$BIN" optimizer --index "$q"
  say
  say "-- SeqScan (the same rows, every page of Items read)"
  "$BIN" optimizer --seq "$q"

  idx_pages=$("$BIN" optimizer --index "$q" | metric pages_read)
  seq_pages=$("$BIN" optimizer --seq "$q" | metric pages_read)
  idx_ms=$("$BIN" optimizer --index "$q" | metric actual_time_ms)
  seq_ms=$("$BIN" optimizer --seq "$q" | metric actual_time_ms)
  idx_rows=$("$BIN" optimizer --index "$q" | metric rows_actual)
  seq_rows=$("$BIN" optimizer --seq "$q" | metric rows_actual)

  say
  say "| path | pages read | time (ms) | rows |"
  say "|---|---:|---:|---:|"
  say "| IndexScan(idx_items_pk) | $idx_pages | $idx_ms | $idx_rows |"
  say "| SeqScan(Items) | $seq_pages | $seq_ms | $seq_rows |"
  say
  if [ "$idx_rows" = "$seq_rows" ] && [ "${idx_pages:-0}" -lt "${seq_pages:-0}" ]; then
    say "RESULT: PASS — identical rows ($idx_rows), $idx_pages vs $seq_pages pages read."
    return 0
  fi
  say "RESULT: FAIL — index scan returned $idx_rows rows over $idx_pages pages; sequential returned $seq_rows over $seq_pages."
  return 1
}

main 2>&1 | tee "$EV/phase8_demo_index.txt"
exit "${PIPESTATUS[0]}"
