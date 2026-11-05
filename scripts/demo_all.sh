#!/usr/bin/env bash
# scripts/demo_all.sh — Run all 9 Phase 8 demos in sequence
set -euo pipefail
cd "$(dirname "$0")/.."

echo "=========================================================="
echo "    SthirDB Phase 8: Consolidated Demonstration Suite     "
echo "=========================================================="

demos=(
  "scripts/demo_storage.sh"
  "scripts/demo_query.sh"
  "scripts/demo_index.sh"
  "scripts/demo_optimizer.sh"
  "scripts/demo_recovery.sh"
  "scripts/demo_benchmark.sh"
  "scripts/demo_constraint_aware.sh"
  "scripts/demo_crash_injection.sh"
  "scripts/demo_checkpoint.sh"
)

for d in "${demos[@]}"; do
  echo
  bash "$d"
done

echo
echo "=========================================================="
echo "   All 9 SthirDB Demonstrations Completed Successfully!   "
echo "=========================================================="
