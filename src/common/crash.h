#pragma once

namespace sthir {

// Crash-injection hook for Novelty 2 (Phase 5B).
//
// No-op in normal runs. When the environment variable STHIRDB_CRASH_POINT is
// set to `name`, the process hard-exits (_exit, no flush, no destructors) so
// the on-disk state is left exactly as a real crash would leave it. Every page
// write and every log append routes through this hook via a single low-level
// function, so no call sites change when tests inject crashes.
//
// STHIRDB_CRASH_SKIP=N makes the first N occurrences of that name pass and the
// (N+1)-th fire, so a harness can place the crash anywhere inside a workload
// instead of always at its first hazard.
void crash_point(const char* name);

// True when `name` is the armed crash point, without consuming an occurrence.
// Used by the disk manager to split a page write in half (a torn write) only
// when the harness asks for one.
bool crash_armed(const char* name);

}  // namespace sthir
