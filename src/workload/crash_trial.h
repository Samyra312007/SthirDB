#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sthir {

// Novelty 2A — randomized crash-injection trials.
//
// A trial is a deterministic retail workload (stock transfers plus a ledger
// insert, one transaction each) run in a subprocess with a named crash point
// armed. The harness then restarts the engine, runs recovery twice, and checks
// five invariants against an oracle of acknowledged commits:
//
//   durability   every acknowledged commit is present
//   atomicity    no partially applied transaction survives
//   consistency  stock conserved, no negative stock, every FK valid
//   idempotency  recovering twice equals recovering once
//   golden       the recovered state equals the committed-transaction state
//
// The oracle is written by the trial process after COMMIT returns, and crashes
// only fire at named hooks inside the engine, so the oracle can never be more
// or less than the set of transactions the engine actually committed.

struct TrialConfig {
  std::string dir = "./data/trial";
  uint64_t seed = 1;
  size_t txns = 8;
  size_t checkpoint_interval = 0;  // 0 = no periodic checkpoints
  bool crash_at_end = false;       // crash after the last commit (sweep runs)
};

struct TrialRunResult {
  size_t committed = 0;
  double elapsed_ms = 0.0;
  uint64_t wal_bytes = 0;
  int64_t total_amount = 0;
};

// Creates the trial schema (Stock + Ledger) in `dir` if it is missing. Run
// outside any crash window so a crash always meets a valid database.
bool TrialInit(const std::string& dir, std::string* err);

// Runs the deterministic workload and appends one oracle line per acknowledged
// commit. Prints nothing; writes `dir/trial.metrics`.
bool TrialRun(const TrialConfig& cfg, TrialRunResult* out, std::string* err);

// Runs ARIES recovery and prints one `key=value` line per metric.
bool TrialRecover(const std::string& dir, std::string* out, std::string* err);

// Checks the five invariants against the oracle. Prints one line per invariant
// plus a canonical `STATE ...` line used for the double-recovery comparison.
bool TrialVerify(const std::string& dir, const std::string& oracle_path,
                 std::string* out, bool* ok, std::string* err);

struct HarnessConfig {
  size_t trials = 1000;
  size_t txns = 8;
  uint64_t seed_base = 1;
  std::string bin = "./build/sthirdb";
  std::string dir = "./data/trial";
  std::string csv_path = "docs/evidence/phase5b_trials.csv";
  bool quiet_progress = false;
};

// Spawns txns/trials subprocesses, asserts every invariant, and prints a
// summary table. Returns false only when the harness itself could not run.
bool RunCrashHarness(const HarnessConfig& cfg, std::string* err);

}  // namespace sthir
