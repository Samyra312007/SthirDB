#!/usr/bin/env bash
# Phase 4B — constraint-aware optimizer (Novelty 1) experiment.
#
# Reproduces every number in docs/report.md §6 from a clean ./data/:
#   1. constrained schema + deterministic retail data (seed 42)
#   2. for each rule-triggering query: EXPLAIN + actual metrics with
#      `constraint_aware` OFF (baseline) and ON (rules active), on the SAME
#      engine and the SAME data
#   3. an on/off row-equivalence check (`diff` of the result tables)
#   4. two control queries no rule may touch
#   5. write-overhead: 10,000 INSERTs inside ONE transaction (so the timing
#      is check cost, not per-statement log flushing) with enforcement on vs off
#
# Raw output lands in docs/evidence/phase4b_*.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="./build/sthirdb"
EV="docs/evidence"
mkdir -p "$EV"

R1_QUERY="SELECT ItemName FROM Items WHERE ItemID = 4242"
R1_UNIQUE_QUERY="SELECT Total FROM Invoices WHERE PurchaseID = 50000"
R1_VARCHAR_QUERY="SELECT Name FROM Shoppers WHERE Email = 'shopper537@retail.test'"
R2_QUERY="SELECT COUNT(*) FROM Items WHERE StockLevel < 0"
R2B_QUERY="SELECT COUNT(*) FROM Items WHERE StockLevel >= 0"
R3_QUERY="SELECT COUNT(*) FROM Purchases JOIN Shoppers ON Purchases.ShopperID = Shoppers.ShopperID"
R4_QUERY="SELECT Shoppers.Name FROM Purchases JOIN Shoppers ON Purchases.ShopperID = Shoppers.ShopperID"
CONTROL_1="SELECT COUNT(*) FROM Items WHERE RetailPrice > 100"
CHAIN_QUERY="SELECT COUNT(*) FROM Purchase_Line_Items JOIN Items ON Purchase_Line_Items.ItemID = Items.ItemID WHERE Items.StockLevel >= 0"
CONTROL_2="SELECT COUNT(*) FROM Purchases JOIN Items ON Purchases.Total = Items.RetailPrice"

# The ten queries the report calls the Novelty-1 suite: eight rule triggers
# (R1 x3, R2, R2b, R3, R4, and one R2b->R3 chain) plus two controls.
QUERIES=(
  "R1-pk|$R1_QUERY"
  "R1-unique|$R1_UNIQUE_QUERY"
  "R1-varchar|$R1_VARCHAR_QUERY"
  "R2-contradiction|$R2_QUERY"
  "R2b-redundant|$R2B_QUERY"
  "R3-join-elimination|$R3_QUERY"
  "R4-fk-cardinality|$R4_QUERY"
  "R2b-plus-R3-chain|$CHAIN_QUERY"
  "control-1|$CONTROL_1"
  "control-2|$CONTROL_2"
)

echo "== building"
make -s all

echo "== clean ./data/ and load the constrained retail schema (seed 42)"
rm -rf ./data
mkdir -p ./data
"$BIN" sql scripts/retail_schema.sql > /dev/null
"$BIN" retail-gen > "$EV/phase4b_retail_gen.txt"
tail -4 "$EV/phase4b_retail_gen.txt"

echo "== rule-triggering queries, constraint_aware off vs on"
{
  echo "# Phase 4B — constraint-aware optimizer A-vs-B"
  echo "# Same engine, same data, one flag apart: constraint_aware = off | on"
  echo "# (constraints_enabled stays on in both runs)"
  echo
  for entry in "${QUERIES[@]}"; do
    name="${entry%%|*}"
    q="${entry#*|}"
    echo "=============================================================="
    echo "QUERY ($name): $q"
    echo
    for mode in off on; do
      echo "---- constraint_aware = $mode"
      echo "-- EXPLAIN"
      "$BIN" explain --constraint-aware "$mode" "$q"
      echo "-- metrics"
      "$BIN" optimizer --constraint-aware "$mode" "$q"
      echo
    done
  done
} > "$EV/phase4b_avb.txt"

echo "== on/off row equivalence (results must be identical)"
{
  echo "# Row equivalence: constraint_aware off vs on for every suite query"
  echo "# Identical output is required; a difference is a correctness bug."
  echo
  failures=0
  for entry in "${QUERIES[@]}"; do
    name="${entry%%|*}"
    q="${entry#*|}"
    "$BIN" run --constraint-aware off "$q" > /tmp/phase4b_off.txt 2>&1
    "$BIN" run --constraint-aware on "$q" > /tmp/phase4b_on.txt 2>&1
    if diff -q /tmp/phase4b_off.txt /tmp/phase4b_on.txt > /dev/null; then
      printf 'PASS  %-22s identical rows with the flag off and on\n' "$name"
    else
      printf 'FAIL  %-22s RESULTS DIFFER\n' "$name"
      diff /tmp/phase4b_off.txt /tmp/phase4b_on.txt || true
      failures=$((failures + 1))
    fi
  done
  echo
  if [ "$failures" -eq 0 ]; then
    echo "equivalence: 10/10 queries returned identical rows"
  else
    echo "equivalence: $failures query/queries returned DIFFERENT rows"
    exit 1
  fi
} | tee "$EV/phase4b_equivalence.txt"

echo "== write-overhead measurement (10,000 INSERTs, one transaction)"
{
  echo "# Write overhead of constraint enforcement"
  echo "# 2,000 single-row INSERTs into Purchases (valid shopper ids), one"
  echo "# process per run, best-of-3 wall time. Same statements in both modes."
  echo
  echo "| enforcement | run 1 ms | run 2 ms | run 3 ms | best ms | us/insert |"
  echo "|---|---:|---:|---:|---:|---:|"
} > "$EV/phase4b_write_overhead.md"

python3 - <<'PY'
# 1,000 shoppers + 1 staff member, so the Purchases rows satisfy the FK while
# staying far away from the retail generator's id ranges.
setup = ["INSERT INTO Staff VALUES (1, 'staff_1', 'staff1@retail.test');"]
for i in range(1, 1001):
    setup.append(
        "INSERT INTO Shoppers VALUES (%d, 'shopper_%d', 'shopper%d@retail.test', 'City%d', 20250101);"
        % (i, i, i, i % 20)
    )
rows = []
for i in range(1, 10001):
    shopper = (i % 1000) + 1
    rows.append(
        "INSERT INTO Purchases VALUES (%d, %d, 1, 20251101, 12.50);" % (900000 + i, shopper)
    )
open("/tmp/phase4b_setup.sql", "w").write("\n".join(setup) + "\n")
open("/tmp/phase4b_inserts.sql", "w").write("BEGIN;\n" + "\n".join(rows) + "\nCOMMIT;\n")
PY

# Each repetition starts from the same tiny database: the 2,000 statements are
# identical every time, so the only variable is enforcement.
measure() {
  local mode="$1"
  local times=()
  for rep in 1 2 3; do
    local start end ms
    rm -rf ./data
    mkdir -p ./data
    "$BIN" sql scripts/retail_schema.sql > /dev/null
    "$BIN" sql --constraints "$mode" /tmp/phase4b_setup.sql > /dev/null
    start=$(date +%s%N)
    "$BIN" sql --constraints "$mode" /tmp/phase4b_inserts.sql > /dev/null
    end=$(date +%s%N)
    ms=$(( (end - start) / 1000000 ))
    times+=("$ms")
  done
  local best=${times[0]}
  for t in "${times[@]}"; do [ "$t" -lt "$best" ] && best=$t; done
  local us
  us=$(python3 -c "print('%.2f' % (($best * 1000) / 10000))")
  echo "| $mode | ${times[0]} | ${times[1]} | ${times[2]} | $best | $us |" \
    >> "$EV/phase4b_write_overhead.md"
}

for mode in on off; do
  measure "$mode"
done

echo "== recreating the clean constrained dataset for later steps"
rm -rf ./data
mkdir -p ./data
"$BIN" sql scripts/retail_schema.sql > /dev/null
"$BIN" retail-gen > /dev/null

echo
echo "constraint_aware_experiment.sh: done. Evidence:"
ls -1 "$EV" | grep phase4b | sed 's/^/  /'
