#!/usr/bin/env bash
# Phase 5B, Part A — randomized crash-injection harness.
#
# Runs >= 1,000 crash trials across all seven ARIES hazard points, then proves
# the harness is not passing vacuously by rebuilding the engine with two
# deliberate bugs (mutations) and showing that the same harness catches them.
#
#   ./scripts/crash_injection.sh
#
# Evidence written (regenerated every run, never edited by hand):
#   docs/evidence/phase5b_trials.csv      one row per trial
#   docs/evidence/phase5b_trials.txt      harness summary + per-crash-point table
#   docs/evidence/phase5b_mutation.txt    mutation-test result

set -u -o pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BIN=./build/sthirdb
EVID=docs/evidence
TRIALS=${TRIALS:-1050}          # 150 per crash point
MUT_TRIALS=${MUT_TRIALS:-140}   # 20 per crash point, enough to catch a bug
BASE_CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -pthread -Isrc -MMD -MP"

mkdir -p "$EVID"

echo "== building (normal) =="
make -s all || { echo "build failed"; exit 1; }

# ---------------------------------------------------------------------------
# 1. Full randomized run
# ---------------------------------------------------------------------------
rm -rf ./data/crash
echo "== crash-injection harness: $TRIALS trials (7 crash points) =="
if ! "$BIN" crash-harness --trials "$TRIALS" --txns 8 --seed 1 \
        --dir ./data/crash --csv "$EVID/phase5b_trials.csv" \
        > "$EVID/phase5b_trials.txt" 2>&1; then
  echo "harness invocation failed; see $EVID/phase5b_trials.txt"
  exit 1
fi
cat "$EVID/phase5b_trials.txt"

TOTAL=$(awk -F, 'NR>1' "$EVID/phase5b_trials.csv" | wc -l)
PASSED=$(awk -F, 'NR>1 && $6==1' "$EVID/phase5b_trials.csv" | wc -l)
CRASHED=$(awk -F, 'NR>1 && $5==1' "$EVID/phase5b_trials.csv" | wc -l)
POINTS=$(awk -F, 'NR>1 {print $3}' "$EVID/phase5b_trials.csv" | sort -u | wc -l)

echo
echo "trials=$TOTAL passed=$PASSED crashed_mid_run=$CRASHED crash_points_covered=$POINTS"

# ---------------------------------------------------------------------------
# 2. Mutation testing — the harness must catch a deliberately broken engine
# ---------------------------------------------------------------------------
# Each mutation is a compile-time flag that disables one ARIES guarantee:
#   STHIRDB_MUTATION_SKIP_COMMIT_FLUSH  commit returns before the log is forced
#                                       (breaks WAL durability)
#   STHIRDB_MUTATION_SKIP_REDO          recovery never repeats history
#                                       (breaks no-force / redo)
# Neither flag is ever set in a normal build.
mutate() {
  local name="$1" define="$2" bdir="build/mut_${name}"
  echo
  echo "== mutation '$name' (-D${define}) =="
  rm -rf "$bdir"
  if ! make -s BUILD="$bdir" CXXFLAGS="$BASE_CXXFLAGS -D$define" "$bdir/sthirdb"; then
    echo "mutation build failed"
    return 1
  fi
  rm -rf "./data/mut_${name}"
  # --bin is essential: the harness spawns the trials in subprocesses, so the
  # mutated binary must be the one those subprocesses run.
  "$bdir/sthirdb" crash-harness --trials "$MUT_TRIALS" --txns 8 --seed 1 \
      --bin "$bdir/sthirdb" --dir "./data/mut_${name}" \
      --csv "/tmp/mut_${name}.csv" > "/tmp/mut_${name}.out" 2>&1
  tail -15 "/tmp/mut_${name}.out"
}

MUT_OUT="$EVID/phase5b_mutation.txt"
{
  echo "# Phase 5B mutation test — a deliberately broken engine must be caught."
  echo "# Each mutation is one compile-time flag; the normal build never sets it."
  echo
} > "$MUT_OUT"

MUT_FAILURES=0
for spec in "skip_commit_flush:STHIRDB_MUTATION_SKIP_COMMIT_FLUSH" \
            "skip_redo:STHIRDB_MUTATION_SKIP_REDO"; do
  name="${spec%%:*}"
  define="${spec##*:}"
  mutate "$name" "$define"
  run_out=$(cat "/tmp/mut_${name}.out")
  caught_failed=$(echo "$run_out" | awk '/^ +failed +:/{print $3}')
  caught_passed=$(echo "$run_out" | awk '/^ +passed +:/{print $3}')
  {
    echo "## $name  (-D$define)"
    echo '```'
    echo "$run_out" | sed -n '1,60p'
    echo '```'
    if [ "${caught_failed:-0}" -gt 0 ]; then
      echo "Result: CAUGHT — $caught_passed passed, $caught_failed failed, as required."
    else
      echo "Result: NOT CAUGHT — the harness passed a broken engine (harness bug)."
    fi
    echo
  } >> "$MUT_OUT"
  if [ "${caught_failed:-0}" -le 0 ]; then
    MUT_FAILURES=$((MUT_FAILURES + 1))
  fi
done

echo
if [ "$MUT_FAILURES" -ne 0 ]; then
  echo "MUTATION TEST FAILED: the harness did not catch $MUT_FAILURES broken build(s)."
  exit 1
fi
echo "Mutation test: both broken builds caught. See $MUT_OUT"

# Leave the normal binary in place.
make -s all

if [ "$PASSED" -ne "$TOTAL" ]; then
  echo "FAILURE: $((TOTAL - PASSED)) of $TOTAL trials failed. See $EVID/phase5b_trials.csv"
  exit 1
fi
if [ "$POINTS" -ne 7 ]; then
  echo "FAILURE: only $POINTS of 7 crash points were exercised."
  exit 1
fi
echo "OK: $PASSED/$TOTAL trials passed across $POINTS crash points."
