#!/usr/bin/env bash
# Phase 8 demo 7 — Novelty 1: the same query, constraint_aware off then on.
#
#   ./scripts/demo_constraint_aware.sh
#
# Query: a COUNT over Purchases joined to Shoppers. The foreign key guarantees
# exactly one Shoppers row per purchase and the query selects nothing from
# Shoppers, so rule R3 may delete the join — and with it a 10,000,000-row
# estimate and one whole table scan.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=./build/sthirdb
EV=docs/evidence
mkdir -p "$EV"

say() { printf '%s\n' "$*"; }
metric() { awk -F'|' -v key="$1" '$0 ~ key { gsub(/^ +| +$/, "", $3); print $3 }'; }

main() {
  say "=== SthirDB demo: Novelty 1 — the constraint-aware optimizer ==="
  make -s all

  rm -rf ./data
  mkdir -p ./data
  "$BIN" sql scripts/retail_schema.sql > /dev/null
  "$BIN" retail-gen > /dev/null

  q="SELECT COUNT(*) FROM Purchases JOIN Shoppers ON Purchases.ShopperID = Shoppers.ShopperID"
  say
  say "QUERY: $q"

  off_explain=$("$BIN" explain --constraint-aware off "$q")
  on_explain=$("$BIN" explain --constraint-aware on "$q")

  say
  say "---- SET constraint_aware = off (baseline: the join is executed)"
  printf '%s\n' "$off_explain"

  say
  say "---- SET constraint_aware = on (rules allowed to fire)"
  printf '%s\n' "$on_explain"

  off_pages=$("$BIN" optimizer --constraint-aware off "$q" | metric pages_read)
  on_pages=$("$BIN" optimizer --constraint-aware on "$q" | metric pages_read)
  off_ms=$("$BIN" optimizer --constraint-aware off "$q" | metric actual_time_ms)
  on_ms=$("$BIN" optimizer --constraint-aware on "$q" | metric actual_time_ms)

  "$BIN" run --constraint-aware off "$q" > /tmp/demo_ca_off.txt 2>&1
  "$BIN" run --constraint-aware on "$q" > /tmp/demo_ca_on.txt 2>&1

  say
  say "| setting | join in the plan | pages read | time (ms) |"
  say "|---|---|---:|---:|"
  if printf '%s' "$off_explain" | grep -q "HashJoin"; then off_join="yes"; else off_join="no"; fi
  if printf '%s' "$on_explain" | grep -q "HashJoin"; then on_join="yes"; else on_join="no"; fi
  say "| constraint_aware = off | $off_join | $off_pages | $off_ms |"
  say "| constraint_aware = on | $on_join | $on_pages | $on_ms |"

  say
  if diff -q /tmp/demo_ca_off.txt /tmp/demo_ca_on.txt > /dev/null; then
    say "row equivalence: PASS — both settings returned identical rows"
    same=1
  else
    say "row equivalence: FAIL — the two settings disagreed"
    diff /tmp/demo_ca_off.txt /tmp/demo_ca_on.txt || true
    same=0
  fi

  rules_ok=0
  if printf '%s' "$on_explain" | grep -q "R3"; then rules_ok=1; fi

  say
  say "R3 fired (join eliminated): $([ "$rules_ok" -eq 1 ] && echo yes || echo no)"
  if [ "$same" -eq 1 ] && [ "$rules_ok" -eq 1 ] && [ "$off_join" = "yes" ] && [ "$on_join" = "no" ]; then
    say "RESULT: PASS — the rule removed the join without changing a single row."
    return 0
  fi
  say "RESULT: FAIL — rule did not fire, or the rows differ."
  return 1
}

main 2>&1 | tee "$EV/phase8_demo_constraint_aware.txt"
exit "${PIPESTATUS[0]}"
