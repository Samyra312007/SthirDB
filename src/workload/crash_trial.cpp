#include "workload/crash_trial.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "common/crash.h"
#include "exec/execution_engine.h"

namespace fs = std::filesystem;

namespace sthir {

namespace {

constexpr int64_t kInitialQty = 1000000;  // per stock row
constexpr int64_t kMaxTransfer = 50;

uint64_t SplitMix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

int TransferAmount(uint64_t seed, size_t txn) {
  const uint64_t h = SplitMix64(seed * 1000003ULL + txn);
  return 1 + static_cast<int>(h % static_cast<uint64_t>(kMaxTransfer));
}

std::string ReadFileOrEmpty(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int ExitCode(int status) {
  if (status == -1 || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

std::string StateLine(const std::string& text) {
  size_t pos = text.find("STATE ");
  if (pos == std::string::npos) return "";
  size_t end = text.find('\n', pos);
  return text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

int64_t ParseInt(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

bool ExecOk(ExecutionEngine* engine, const std::string& sql, std::string* err) {
  auto res = engine->Execute(sql);
  if (!res.success) {
    *err = "statement failed: " + sql + " -> " + res.error_message;
    return false;
  }
  return true;
}

}  // namespace

bool TrialInit(const std::string& dir, std::string* err) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    *err = "cannot create " + dir + ": " + ec.message();
    return false;
  }

  Catalog catalog(dir + "/catalog.json");
  if (!catalog.Load(err)) return false;
  if (catalog.HasTable("Stock")) return true;  // already initialized

  ExecutionEngine engine(&catalog, dir);
  const std::vector<std::string> ddl = {
      "CREATE TABLE Stock (ItemID INT PRIMARY KEY, Qty INT NOT NULL CHECK (Qty >= 0))",
      "CREATE TABLE Ledger (TxnID INT PRIMARY KEY, "
      "ItemID INT NOT NULL REFERENCES Stock (ItemID) ON DELETE RESTRICT, "
      "Amount INT NOT NULL)",
      "INSERT INTO Stock VALUES (1, 1000000)",
      "INSERT INTO Stock VALUES (2, 1000000)",
  };
  for (const auto& sql : ddl) {
    if (!ExecOk(&engine, sql, err)) return false;
  }
  return true;
}

bool TrialRun(const TrialConfig& cfg, TrialRunResult* out, std::string* err) {
  Catalog catalog(cfg.dir + "/catalog.json");
  if (!catalog.Load(err)) return false;
  if (!catalog.HasTable("Stock")) {
    *err = "trial database is not initialized (run trial-init first)";
    return false;
  }

  ExecutionEngine engine(&catalog, cfg.dir);
  if (cfg.checkpoint_interval > 0) {
    if (!ExecOk(&engine, "SET checkpoint_interval = " +
                             std::to_string(cfg.checkpoint_interval), err)) {
      return false;
    }
  }

  const std::string oracle_path = cfg.dir + "/oracle.log";
  std::FILE* oracle = std::fopen(oracle_path.c_str(), "w");
  if (oracle == nullptr) {
    *err = "cannot write oracle file " + oracle_path;
    return false;
  }

  TrialRunResult result;
  const auto start = std::chrono::steady_clock::now();

  for (size_t i = 1; i <= cfg.txns; ++i) {
    const int amount = TransferAmount(cfg.seed, i);
    if (!ExecOk(&engine, "BEGIN", err)) {
      std::fclose(oracle);
      return false;
    }
    if (!ExecOk(&engine, "UPDATE Stock SET Qty = Qty - " + std::to_string(amount) +
                             " WHERE ItemID = 1", err) ||
        !ExecOk(&engine, "UPDATE Stock SET Qty = Qty + " + std::to_string(amount) +
                             " WHERE ItemID = 2", err) ||
        !ExecOk(&engine, "INSERT INTO Ledger VALUES (" + std::to_string(i) +
                             ", 1, " + std::to_string(amount) + ")", err)) {
      std::fclose(oracle);
      return false;
    }

    // In-flight marker, durable before the commit request. A crash that lands
    // after the commit record became durable but before the acknowledgement
    // reached this client leaves exactly one transaction whose fate the oracle
    // cannot know; the marker names it so verification can demand atomicity
    // without demanding a particular outcome. Cleared once the commit is
    // acknowledged and the oracle line is on disk.
    const std::string inflight_path = cfg.dir + "/inflight";
    {
      std::FILE* mark = std::fopen(inflight_path.c_str(), "w");
      if (mark != nullptr) {
        std::fprintf(mark, "%zu\n", i);
        std::fflush(mark);
        ::fsync(::fileno(mark));
        std::fclose(mark);
      }
    }

    if (!ExecOk(&engine, "COMMIT", err)) {
      std::fclose(oracle);
      return false;
    }

    // Acknowledged commit: from here on the harness requires the row to survive.
    std::fprintf(oracle, "%zu %d\n", i, amount);
    std::fflush(oracle);
    ::fsync(::fileno(oracle));
    std::remove(inflight_path.c_str());

    result.committed = i;
    result.total_amount += amount;
  }

  const auto end = std::chrono::steady_clock::now();
  result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
  std::fclose(oracle);

  std::error_code ec;
  const auto wal_size = fs::file_size(cfg.dir + "/wal.log", ec);
  result.wal_bytes = ec ? 0 : static_cast<uint64_t>(wal_size);

  // Metrics survive the crash: they are written before the end-of-workload
  // crash point below.
  {
    std::ofstream metrics(cfg.dir + "/trial.metrics");
    metrics << "committed=" << result.committed << "\n"
            << "elapsed_ms=" << result.elapsed_ms << "\n"
            << "wal_bytes=" << result.wal_bytes << "\n"
            << "total_amount=" << result.total_amount << "\n";
  }

  if (out != nullptr) *out = result;

  if (cfg.crash_at_end) {
    crash_point("end_of_workload");  // armed only by the sweep harness
  }
  return true;
}

bool TrialRecover(const std::string& dir, std::string* out, std::string* err) {
  Catalog catalog(dir + "/catalog.json");
  if (!catalog.Load(err)) return false;
  ExecutionEngine engine(&catalog, dir);
  const RecoveryReport report = engine.Recover();

  std::ostringstream ss;
  ss << "analysis_scanned=" << report.analysis_records_scanned << "\n"
     << "redo_applied=" << report.redo_records_applied << "\n"
     << "redo_skipped=" << report.redo_records_skipped << "\n"
     << "undo_applied=" << report.undo_records_applied << "\n"
     << "losers=" << report.loser_txns.size() << "\n"
     << "checkpoint_lsn=" << report.checkpoint_lsn << "\n"
     << "redo_start_lsn=" << report.redo_start_lsn << "\n"
     << "wal_bytes=" << report.wal_size_bytes << "\n"
     << "log_scanned_bytes=" << report.log_scanned_bytes << "\n"
     << "recovery_time_ms=" << report.recovery_time_ms << "\n";
  if (out != nullptr) *out = ss.str();
  return true;
}

bool TrialVerify(const std::string& dir, const std::string& oracle_path,
                 std::string* out, bool* ok, std::string* err) {
  std::ifstream oracle(oracle_path);
  if (!oracle.is_open()) {
    *err = "cannot read oracle " + oracle_path;
    return false;
  }

  std::vector<std::pair<int64_t, int64_t>> committed;  // (txn id, amount)
  std::string line;
  while (std::getline(oracle, line)) {
    if (line.empty()) continue;
    std::istringstream ls(line);
    int64_t id = 0;
    int64_t amount = 0;
    if (!(ls >> id >> amount)) continue;
    committed.emplace_back(id, amount);
  }

  Catalog catalog(dir + "/catalog.json");
  if (!catalog.Load(err)) return false;
  ExecutionEngine engine(&catalog, dir);

  auto stock_res = engine.Execute("SELECT ItemID, Qty FROM Stock ORDER BY ItemID");
  if (!stock_res.success) {
    *err = "cannot read Stock: " + stock_res.error_message;
    return false;
  }
  auto ledger_res = engine.Execute("SELECT TxnID, ItemID, Amount FROM Ledger ORDER BY TxnID");
  if (!ledger_res.success) {
    *err = "cannot read Ledger: " + ledger_res.error_message;
    return false;
  }

  std::map<int64_t, int64_t> stock;  // ItemID -> Qty
  for (const auto& row : stock_res.rows) {
    if (row.size() < 2) continue;
    stock[ParseInt(row[0])] = ParseInt(row[1]);
  }
  std::map<int64_t, int64_t> ledger;  // TxnID -> Amount
  bool ledger_item_ok = true;
  for (const auto& row : ledger_res.rows) {
    if (row.size() < 3) continue;
    ledger[ParseInt(row[0])] = ParseInt(row[2]);
    if (ParseInt(row[1]) != 1) ledger_item_ok = false;
  }

  int64_t total = 0;
  for (const auto& [id, amount] : committed) total += amount;

  // One transaction may be in doubt: a crash between the moment its commit
  // record became durable and the moment the acknowledgement reached the
  // client. The oracle cannot say whether it committed, and neither can any
  // other external observer, so the invariants accept either outcome for that
  // single transaction provided it is all-or-nothing.
  int64_t in_doubt = 0;
  {
    std::ifstream f(dir + "/inflight");
    int64_t id = 0;
    if (f.is_open() && (f >> id) && id > 0) in_doubt = id;
  }
  auto acknowledged = [&](int64_t id) {
    for (const auto& entry : committed) {
      if (entry.first == id) return true;
    }
    return false;
  };
  if (in_doubt != 0 && acknowledged(in_doubt)) in_doubt = 0;

  // Durability: every acknowledged commit is in the ledger with its amount.
  bool durability = true;
  std::string durability_detail;
  for (const auto& [id, amount] : committed) {
    auto it = ledger.find(id);
    if (it == ledger.end()) {
      durability = false;
      durability_detail = "missing committed txn " + std::to_string(id);
      break;
    }
    if (it->second != amount) {
      durability = false;
      durability_detail = "amount mismatch for txn " + std::to_string(id);
      break;
    }
  }

  // Atomicity: nothing beyond the committed set except at most the one in-doubt
  // transaction, and never a half-applied transaction.
  bool atomicity = true;
  std::string atomicity_detail;
  int64_t doubt_rows = 0;
  for (const auto& [id, amount] : ledger) {
    (void)amount;
    if (acknowledged(id)) continue;
    if (id == in_doubt) {
      doubt_rows += 1;
      continue;
    }
    atomicity = false;
    atomicity_detail = "uncommitted txn " + std::to_string(id) + " survived";
    break;
  }
  if (atomicity && ledger.size() != committed.size() + static_cast<size_t>(doubt_rows)) {
    atomicity = false;
    atomicity_detail = "ledger has " + std::to_string(ledger.size()) + " rows, oracle " +
                       std::to_string(committed.size());
  }

  // Consistency: conservation, non-negative stock, valid FK.
  const int64_t s1 = stock.count(1) ? stock[1] : 0;
  const int64_t s2 = stock.count(2) ? stock[2] : 0;
  const bool consistent = ledger_item_ok && s1 >= 0 && s2 >= 0 &&
                          s1 + s2 == 2 * kInitialQty && stock.size() == 2;

  // Golden: the exact state the transactions reflected in the ledger describe.
  // Stock must equal the ledger's total transfer, and that total may exceed the
  // oracle's only by the in-doubt transaction.
  int64_t ledger_total = 0;
  for (const auto& [id, amount] : ledger) ledger_total += amount;
  int64_t expected_total = total;
  auto doubt_it = ledger.find(in_doubt);
  if (in_doubt != 0 && doubt_it != ledger.end()) expected_total += doubt_it->second;
  const bool golden = ledger_total == expected_total && s1 == kInitialQty - ledger_total &&
                      s2 == kInitialQty + ledger_total;

  std::ostringstream ss;
  ss << "INVARIANT durability " << (durability ? "PASS" : "FAIL")
     << (durability_detail.empty() ? "" : (" " + durability_detail)) << "\n";
  ss << "INVARIANT atomicity " << (atomicity ? "PASS" : "FAIL")
     << (atomicity_detail.empty() ? "" : (" " + atomicity_detail)) << "\n";
  ss << "INVARIANT consistency " << (consistent ? "PASS" : "FAIL")
     << " stock_sum=" << (s1 + s2) << " fk_ok=" << (ledger_item_ok ? 1 : 0) << "\n";
  ss << "INVARIANT golden " << (golden ? "PASS" : "FAIL") << " expected_s1="
     << (kInitialQty - expected_total) << " expected_s2=" << (kInitialQty + expected_total)
     << " in_doubt=" << in_doubt << "\n";
  ss << "STATE stock1=" << s1 << " stock2=" << s2 << " ledger=" << ledger.size()
     << " total=" << total << " in_doubt=" << (doubt_rows != 0 ? in_doubt : 0) << "\n";

  const bool all = durability && atomicity && consistent && golden;
  ss << "RESULT " << (all ? "PASS" : "FAIL") << "\n";

  if (out != nullptr) *out = ss.str();
  if (ok != nullptr) *ok = all;
  return true;
}

bool RunCrashHarness(const HarnessConfig& cfg, std::string* err) {
  // The seven ARIES hazard points from Phase 5B.
  const std::vector<std::string> points = {
      "after_log_before_page",
      "after_page_before_commit",
      "after_commit_before_page_flush",
      "mid_page_flush",
      "mid_checkpoint",
      "during_undo",
      "during_recovery",
  };

  std::error_code ec;
  fs::create_directories(cfg.dir, ec);

  std::ofstream csv(cfg.csv_path);
  if (!csv.is_open()) {
    *err = "cannot write csv " + cfg.csv_path;
    return false;
  }
  csv << "trial,seed,crash_point,crash_skip,crashed,pass,reason\n";

  std::map<std::string, std::pair<size_t, size_t>> per_point;  // pass, total
  size_t passed = 0;
  size_t failed = 0;
  size_t crashed_trials = 0;
  std::vector<std::string> first_failures;

  const auto harness_start = std::chrono::steady_clock::now();
  const std::string log = cfg.dir + "/run.log";
  const std::string rec1 = cfg.dir + "/recover1.txt";
  const std::string rec2 = cfg.dir + "/recover2.txt";
  const std::string ver1 = cfg.dir + "/verify1.txt";
  const std::string ver2 = cfg.dir + "/verify2.txt";

  for (size_t trial = 0; trial < cfg.trials; ++trial) {
    const std::string point = points[trial % points.size()];
    const size_t skip = (trial / points.size()) % 24;
    const uint64_t seed = cfg.seed_base + trial;

    fs::remove_all(cfg.dir, ec);
    fs::create_directories(cfg.dir, ec);

    std::string reason;
    bool pass = true;
    bool crashed = false;

    std::string cmd = cfg.bin + " trial-init --dir " + cfg.dir + " > " + log + " 2>&1";
    if (ExitCode(std::system(cmd.c_str())) != 0) {
      pass = false;
      reason = "trial-init failed";
    }

    if (pass) {
      cmd = "STHIRDB_CRASH_POINT=" + point + " STHIRDB_CRASH_SKIP=" + std::to_string(skip) +
            " " + cfg.bin + " trial-run --dir " + cfg.dir + " --seed " + std::to_string(seed) +
            " --txns " + std::to_string(cfg.txns) + " > " + log + " 2>&1";
      const int status = ExitCode(std::system(cmd.c_str()));
      crashed = status == 99;
      if (status != 0 && status != 99) {
        pass = false;
        reason = "trial-run exited " + std::to_string(status);
      }
    }

    bool verify1_ok = false;
    bool verify2_ok = false;
    std::string state1;
    std::string state2;

    if (pass) {
      cmd = cfg.bin + " trial-recover --dir " + cfg.dir + " > " + rec1 + " 2>&1";
      if (ExitCode(std::system(cmd.c_str())) != 0) {
        pass = false;
        reason = "first recovery failed";
      }
    }
    if (pass) {
      cmd = cfg.bin + " trial-verify --dir " + cfg.dir + " --oracle " + cfg.dir +
            "/oracle.log > " + ver1 + " 2>&1";
      verify1_ok = ExitCode(std::system(cmd.c_str())) == 0;
      state1 = StateLine(ReadFileOrEmpty(ver1));
    }
    if (pass) {
      cmd = cfg.bin + " trial-recover --dir " + cfg.dir + " > " + rec2 + " 2>&1";
      if (ExitCode(std::system(cmd.c_str())) != 0) {
        pass = false;
        reason = "second recovery failed";
      }
    }
    if (pass) {
      cmd = cfg.bin + " trial-verify --dir " + cfg.dir + " --oracle " + cfg.dir +
            "/oracle.log > " + ver2 + " 2>&1";
      verify2_ok = ExitCode(std::system(cmd.c_str())) == 0;
      state2 = StateLine(ReadFileOrEmpty(ver2));

      if (!verify1_ok) {
        pass = false;
        reason = "first verify failed: " + ReadFileOrEmpty(ver1).substr(0, 120);
      } else if (!verify2_ok) {
        pass = false;
        reason = "second verify failed: " + ReadFileOrEmpty(ver2).substr(0, 120);
      } else if (state1.empty() || state1 != state2) {
        pass = false;
        reason = "recovery not idempotent";
      }
    }

    auto& counter = per_point[point];
    counter.second += 1;
    if (pass) {
      counter.first += 1;
      ++passed;
    } else {
      ++failed;
      if (first_failures.size() < 10) {
        first_failures.push_back(point + " seed=" + std::to_string(seed) +
                                 " skip=" + std::to_string(skip) + ": " + reason);
      }
    }
    if (crashed) ++crashed_trials;

    std::string csv_reason = reason;
    std::replace(csv_reason.begin(), csv_reason.end(), ',', ';');
    csv << trial << "," << seed << "," << point << "," << skip << "," << (crashed ? 1 : 0)
        << "," << (pass ? 1 : 0) << "," << csv_reason << "\n";

    if (!cfg.quiet_progress && (trial + 1) % 100 == 0) {
      std::printf("  ... %zu/%zu trials (pass=%zu fail=%zu)\n", trial + 1, cfg.trials, passed,
                  failed);
      std::fflush(stdout);
    }
  }
  csv.close();

  const auto harness_end = std::chrono::steady_clock::now();
  const double seconds =
      std::chrono::duration<double>(harness_end - harness_start).count();

  std::printf("\nCrash-injection harness: %zu trials in %.1f s\n", cfg.trials, seconds);
  std::printf("  crashed mid-run : %zu\n", crashed_trials);
  std::printf("  passed          : %zu (%.2f%%)\n", passed,
              cfg.trials ? 100.0 * static_cast<double>(passed) / static_cast<double>(cfg.trials)
                         : 0.0);
  std::printf("  failed          : %zu\n", failed);
  std::printf("\n| crash point | trials | pass | pass rate |\n");
  std::printf("|---|---:|---:|---:|\n");
  for (const auto& point : points) {
    const auto it = per_point.find(point);
    const size_t total = it == per_point.end() ? 0 : it->second.second;
    const size_t good = it == per_point.end() ? 0 : it->second.first;
    std::printf("| %s | %zu | %zu | %.2f%% |\n", point.c_str(), total, good,
                total ? 100.0 * static_cast<double>(good) / static_cast<double>(total) : 0.0);
  }
  if (!first_failures.empty()) {
    std::printf("\nFirst failures (reproduce with the printed seed and crash point):\n");
    for (const auto& failure : first_failures) std::printf("  %s\n", failure.c_str());
  }
  std::printf("\nCSV: %s\n", cfg.csv_path.c_str());
  return true;
}

}  // namespace sthir
