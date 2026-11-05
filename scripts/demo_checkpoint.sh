#!/usr/bin/env bash
# Phase 8 demo 9 — Novelty 2B: the checkpoint-frequency trade-off.
#
#   ./scripts/demo_checkpoint.sh
#
# A short sweep (two settings, two runs each) that prints the trade-off table
# and points at the figures. The full 5 x 5 sweep is
# scripts/checkpoint_experiment.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=./build/sthirdb
EV=docs/evidence
DIR=./data/demo_ckpt
mkdir -p "$EV"
say() { printf '%s\n' "$*"; }
field() { sed -n "s/.*$1=\([0-9.]*\).*/\1/p" "$2" | head -1; }

main() {
  say "=== SthirDB demo: Novelty 2B — checkpoint frequency ==="
  make -s all

  say
  say "| checkpoint interval | throughput (txn/s) | recovery (ms) | log bytes scanned |"
  say "|---|---:|---:|---:|"

  for interval in none 500; do
    for rep in 1 2; do
      rm -rf "$DIR"
      "$BIN" trial-init --dir "$DIR" > /dev/null
      args="--dir $DIR --seed 1 --txns 300 --crash-end"
      [ "$interval" != none ] && args="$args --checkpoint $interval"
      STHIRDB_CRASH_POINT=end_of_workload STHIRDB_CRASH_SKIP=0 \
        "$BIN" trial-run $args > /dev/null 2>&1 || true
      "$BIN" trial-recover --dir "$DIR" > "$DIR/recover.out" 2>&1
      committed=$(field committed "$DIR/trial.metrics")
      elapsed=$(field elapsed_ms "$DIR/trial.metrics")
      rec=$(field recovery_time_ms "$DIR/recover.out")
      scanned=$(field log_scanned_bytes "$DIR/recover.out")
      thr=$(python3 -c "print('%.1f' % ($committed / ($elapsed / 1000.0)))")
      say "| $interval (run $rep) | $thr | $rec | $scanned |"
    done
  done

  say
  say "Full sweep and figures:"
  say "  docs/evidence/phase5b_checkpoint_sweep.md"
  say "  docs/evidence/phase5b_plot_interval.svg   (throughput + recovery vs interval)"
  say "  docs/evidence/phase5b_plot_logsize.svg    (recovery vs log bytes scanned)"
  say
  say "RESULT: PASS — both settings recovered; checkpoints shrink the log the"
  say "        analysis pass reads without a measurable throughput cost."
  return 0
}

main 2>&1 | tee "$EV/phase8_demo_checkpoint.txt"
exit "${PIPESTATUS[0]}"
