#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "common/config.h"
#include "exec/execution_engine.h"
#include "parser/parser.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "storage/row.h"
#include "workload/anomaly_injector.h"
#include "workload/benchmark.h"
#include "workload/crash_trial.h"
#include "workload/retail_gen.h"

namespace fs = std::filesystem;

namespace {

void PrintUsage() {
  std::printf(
      "SthirDB %s — disk-resident relational engine\n"
      "\n"
      "Usage:\n"
      "  sthirdb                     Print this help and exit (status 0).\n"
      "  sthirdb --version           Print version and exit.\n"
      "  sthirdb run \"<sql>\"         Execute a SQL query, DDL, or DML statement.\n"
      "  sthirdb explain [--constraint-aware on|off] \"<sql>\"\n"
      "                             Plan tree, estimated rows/cost, rejected paths,\n"
      "                             and every constraint-aware rule that fired.\n"
      "  sthirdb optimizer [--seq|--index] [--constraint-aware on|off] \"<sql>\"\n"
      "                             Chosen plan, estimated cost/rows, actual time.\n"
      "                             --seq / --index force one access path (measurement).\n"
      "                             --constraints on|off selects the schema mode.\n"

      "  sthirdb analyze <table>     Refresh and persist column statistics.\n"
      "  sthirdb init_schema         Create ./data/retail and the Items table file.\n"
      "  sthirdb load_data --rows N  Bulk-insert N rows into Items.\n"
      "  sthirdb count               Count live rows in Items.\n"
      "  sthirdb recover             Run ARIES restart recovery over the WAL.\n"
      "  sthirdb sql [--constraints on|off] <file>\n"
      "                             Execute every statement in <file> in order.\n"
      "  sthirdb retail-gen [--shoppers N] [--items N] [--purchases N]\n"
      "                      [--lpp N] [--seed N] [--constraints on|off]\n"
      "                      [--no-index] [--no-analyze]\n"
      "                             Deterministically load the retail dataset.\n"
      "  sthirdb inject [--constraints on|off] [--count N] [--rate 1|5|10]\n"
      "                 [--extended] [--classes name=N,...]\n"
      "                             Run the anomaly injector; prints a markdown\n"
      "                             per-class interception table.\n"
      "  sthirdb bench [--threads 100,300,500] [--iters N] [--warmup N]\n"
      "                [--constraints on|off] [--frames N] [--query <sql>]\n"
      "                             Run the concurrent reporting-query benchmark;\n"
      "                             prints avg/P99 latency and failures.\n"
      "  sthirdb trial-init --dir <path>\n"
      "                             Create the crash-trial schema (Stock+Ledger).\n"
      "  sthirdb trial-run --dir <path> --seed N [--txns N] [--checkpoint N]\n"
      "                    [--crash-end]\n"
      "                             Run one deterministic trial workload; appends\n"
      "                             acknowledged commits to <dir>/oracle.log.\n"
      "  sthirdb trial-recover --dir <path>\n"
      "                             Run ARIES recovery and print its metrics.\n"
      "  sthirdb trial-verify --dir <path> [--oracle <file>]\n"
      "                             Check durability, atomicity, consistency and\n"
      "                             golden-state invariants; prints PASS/FAIL.\n"
      "  sthirdb crash-harness [--trials N] [--bin <path>] [--csv <path>]\n"
      "                        [--dir <path>] [--txns N]\n"
      "                             Randomized subprocess crash-injection harness.\n",
      sthir::kVersion);
}

int CmdInitSchema() {
  std::error_code ec;
  fs::create_directories(sthir::kDataDir, ec);
  if (ec) {
    std::fprintf(stderr, "init_schema: cannot create directory %s: %s\n", sthir::kDataDir,
                 ec.message().c_str());
    return 1;
  }

  // Ensure catalog has Items entry
  sthir::Catalog catalog(sthir::kCatalogPath);
  catalog.Load();
  if (!catalog.HasTable("items")) {
    sthir::TableSchema schema;
    schema.table_name = "Items";
    schema.data_file_path = sthir::kItemsFilePath;
    schema.columns = {
        {"id", sthir::DataType::INT, 4, true, true, false},
        {"name", sthir::DataType::VARCHAR, 50, false, false, false},
        {"price", sthir::DataType::DECIMAL, 8, false, false, false}};
    schema.primary_key_columns = {"id"};
    catalog.CreateTable(schema);
  }

  sthir::DiskManager dm(sthir::kItemsFilePath);
  sthir::BufferPool bp(sthir::kDefaultPoolFrames, &dm);
  bool created_first_page = false;
  if (dm.num_pages() == 0) {
    const uint32_t pid = dm.allocate_page();
    sthir::Page* page = bp.fetch_page(pid);
    if (page == nullptr) {
      std::fprintf(stderr, "init_schema: buffer pool exhausted\n");
      return 1;
    }
    page->Reset(pid);
    bp.unpin_page(pid, true);
    bp.flush_all();
    created_first_page = true;
  }
  std::printf("init_schema: table Items ready at %s (%u page(s)%s)\n", sthir::kItemsFilePath,
              dm.num_pages(), created_first_page ? ", first page created" : ", already initialized");
  return 0;
}

int CmdLoadData(size_t rows) {
  sthir::DiskManager dm(sthir::kItemsFilePath);
  if (dm.num_pages() == 0) {
    std::fprintf(stderr, "load_data: no table file at %s; run 'sthirdb init_schema' first\n",
                 sthir::kItemsFilePath);
    return 1;
  }
  sthir::BufferPool bp(sthir::kDefaultPoolFrames, &dm);

  uint32_t cur = dm.num_pages() - 1;  // append onto the last page
  sthir::Page* page = bp.fetch_page(cur);
  if (page == nullptr) {
    std::fprintf(stderr, "load_data: buffer pool exhausted\n");
    return 1;
  }
  if (page->free_space_ptr() < sthir::kPageHeaderSize) page->Reset(cur);  // never-initialized page

  size_t inserted = 0;
  while (inserted < rows) {
    const size_t n = inserted + 1;
    const sthir::Row row = sthir::make_row(static_cast<int32_t>(n), "item_" + std::to_string(n),
                                           1.0 + 0.01 * static_cast<double>(inserted));
    char buf[sthir::kRowSize];
    sthir::serialize_row(row, buf);
    if (page->InsertTuple(buf, sthir::kRowSize) < 0) {
      // Current page is full: release it (it was modified), grow the file.
      bp.unpin_page(cur, true);
      cur = dm.allocate_page();
      page = bp.fetch_page(cur);
      if (page == nullptr) {
        std::fprintf(stderr, "load_data: buffer pool exhausted\n");
        return 1;
      }
      page->Reset(cur);
      continue;
    }
    ++inserted;
  }
  bp.unpin_page(cur, true);
  bp.flush_all();

  std::printf("load_data: inserted %zu row(s) into Items; file now %u page(s)\n", inserted,
              dm.num_pages());
  return 0;
}

int CmdCount() {
  sthir::DiskManager dm(sthir::kItemsFilePath);
  if (dm.num_pages() == 0) {
    std::fprintf(stderr, "count: no table file at %s; run 'sthirdb init_schema' first\n",
                 sthir::kItemsFilePath);
    return 1;
  }
  sthir::BufferPool bp(sthir::kDefaultPoolFrames, &dm);

  size_t live = 0;
  size_t physical = 0;
  for (uint32_t pid = 0; pid < dm.num_pages(); ++pid) {
    sthir::Page* page = bp.fetch_page(pid);
    if (page == nullptr) {
      std::fprintf(stderr, "count: buffer pool exhausted\n");
      return 1;
    }
    for (uint16_t slot = 0; slot < page->slot_count(); ++slot) {
      if (!page->tuple_alive(slot)) continue;
      char out[sthir::kRowSize];
      const int len = page->ReadTuple(slot, out);
      if (len != static_cast<int>(sthir::kRowSize)) continue;
      ++physical;
      const sthir::Row row = sthir::deserialize_row(out);
      if (!row.deleted) ++live;
    }
    bp.unpin_page(pid, false);
  }
  std::printf("count: %zu live row(s) in Items (%zu physical tuple(s), %u page(s))\n", live,
              physical, dm.num_pages());
  return 0;
}

// Per-process session flags. A `SET` statement works inside one `sql` run, but
// each CLI invocation is a fresh session, so the shell demos pass the same
// flags on the command line.
struct SessionOpts {
  std::string constraints;        // "on" | "off" | "" (leave default)
  std::string constraint_aware;   // "on" | "off" | "" (leave default)
};

int RunSql(const std::string& sql, const SessionOpts& opts = SessionOpts()) {
  sthir::Catalog catalog(sthir::kCatalogPath);
  std::string err;
  if (!catalog.Load(&err)) {
    std::fprintf(stderr, "run: cannot load catalog: %s\n", err.c_str());
    return 1;
  }
  sthir::ExecutionEngine engine(&catalog, sthir::kDataDir);
  if (!opts.constraints.empty()) engine.SetSessionFlag("constraints_enabled", opts.constraints);
  if (!opts.constraint_aware.empty()) {
    engine.SetSessionFlag("constraint_aware", opts.constraint_aware);
  }
  auto res = engine.Execute(sql);
  res.PrintTable(std::cout);
  return res.success ? 0 : 1;
}

// Splits `--flag value` pairs (on|off only) from the trailing SQL argument.
bool ParseOnOffFlag(int argc, char** argv, const char* name, std::string* out,
                    std::string* sql) {
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == name) {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      if (*out != "on" && *out != "off") return false;
    } else {
      *sql = a;
    }
  }
  return !sql->empty();
}

int CmdRun(int argc, char** argv) {
  SessionOpts opts;
  std::string sql;
  if (!ParseOnOffFlag(argc, argv, "--constraints", &opts.constraints, &sql) ||
      !ParseOnOffFlag(argc, argv, "--constraint-aware", &opts.constraint_aware, &sql)) {
    std::fprintf(stderr, "run: expected 'run [--constraints on|off]"
                         " [--constraint-aware on|off] \"<sql>\"'\n");
    return 2;
  }
  return RunSql(sql, opts);
}

// EXPLAIN "<sql>": plan tree, predicted rows/cost per node, rejected candidates.
int CmdExplain(int argc, char** argv) {
  SessionOpts opts;
  std::string sql;
  if (!ParseOnOffFlag(argc, argv, "--constraints", &opts.constraints, &sql) ||
      !ParseOnOffFlag(argc, argv, "--constraint-aware", &opts.constraint_aware, &sql)) {
    std::fprintf(stderr, "explain: expected 'explain [--constraints on|off]"
                         " [--constraint-aware on|off] \"<sql>\"'\n");
    return 2;
  }
  if (sql.size() >= 7 && sql.compare(0, 7, "EXPLAIN") == 0) return RunSql(sql, opts);
  return RunSql("EXPLAIN " + sql, opts);
}

// ANALYZE <table>: refresh and persist column statistics.
int CmdAnalyze(const std::string& table) {
  if (table.empty()) {
    std::fprintf(stderr, "analyze: expected 'sthirdb analyze <table>'\n");
    return 2;
  }
  return RunSql("ANALYZE " + table);
}

// optimizer "<sql>": chosen plan, estimated cost/rows, actual time and rows.
int CmdOptimizer(int argc, char** argv, const SessionOpts& opts, bool force_seq = false,
                 bool force_index = false) {
  std::string sql;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--seq" || a == "--index" || a == "--constraints" ||
        a == "--constraint-aware") {
      if (a == "--constraints" || a == "--constraint-aware") ++i;  // value consumed
      continue;
    }
    sql = a;
  }
  if (sql.empty()) {
    std::fprintf(stderr, "optimizer: expected 'optimizer [--seq|--index]"
                         " [--constraints on|off] [--constraint-aware on|off] \"<sql>\"'\n");
    return 2;
  }
  sthir::Catalog catalog(sthir::kCatalogPath);
  std::string err;
  if (!catalog.Load(&err)) {
    std::fprintf(stderr, "optimizer: cannot load catalog: %s\n", err.c_str());
    return 1;
  }
  sthir::ExecutionEngine engine(&catalog, sthir::kDataDir);
  if (!opts.constraints.empty()) engine.SetSessionFlag("constraints_enabled", opts.constraints);
  if (!opts.constraint_aware.empty()) {
    engine.SetSessionFlag("constraint_aware", opts.constraint_aware);
  }
  // Measurement hooks: run the same query on a forced path to compare runtimes.
  if (force_seq) engine.SetSessionFlag("enable_index_scan", "off");
  if (force_index) engine.SetSessionFlag("enable_seq_scan", "off");
  auto res = engine.Execute(sql);
  if (!res.success) {
    res.PrintTable(std::cout);
    return 1;
  }
  if (!res.has_plan_info) {
    std::fprintf(stderr, "optimizer: statement produced no plan\n");
    return 1;
  }

  std::printf("Chosen plan (%s):\n%s\n", res.chosen_path.c_str(), res.plan_text.c_str());
  if (!res.rules_line.empty()) std::printf("%s\n", res.rules_line.c_str());

  sthir::ExecutionResult summary;
  summary.is_query = true;
  summary.column_names = {"METRIC", "VALUE"};
  auto add = [&summary](const std::string& metric, const std::string& value) {
    summary.rows.push_back({metric, value});
  };
  char buf[64];
  add("chosen_path", res.chosen_path);
  std::snprintf(buf, sizeof(buf), "%.1f", res.estimated_cost);
  add("estimated_cost", buf);
  std::snprintf(buf, sizeof(buf), "%.0f", res.rows_estimated);
  add("rows_estimated", buf);
  std::snprintf(buf, sizeof(buf), "%.3f", res.actual_time_ms);
  add("actual_time_ms", buf);
  add("rows_actual", std::to_string(res.rows_actual));
  std::snprintf(buf, sizeof(buf), "%zu", res.pages_read);
  add("pages_read", buf);
  for (const auto& alternative : res.alternatives) {
    std::snprintf(buf, sizeof(buf), "cost=%.1f rows=%.0f", alternative.cost, alternative.rows);
    add("rejected " + alternative.name, buf);
  }
  if (res.rows_estimated > 0.0) {
    std::snprintf(buf, sizeof(buf), "%.2fx",
                  static_cast<double>(res.rows_actual) / res.rows_estimated);
    add("estimation_error", buf);
  }
  summary.PrintTable(std::cout);
  return 0;
}

bool ParseRowsArg(int argc, char** argv, size_t* out_rows) {
  if (argc != 4 || std::string(argv[2]) != "--rows") return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(argv[3], &end, 10);
  if (errno != 0 || end == argv[3] || *end != '\0' || v == 0) return false;
  *out_rows = static_cast<size_t>(v);
  return true;
}

// sql [--constraints on|off] <file>: run a whole SQL script in one session.
int CmdSql(int argc, char** argv) {
  std::string constraints_flag = "";
  const char* path = nullptr;
  if (argc == 3) {
    path = argv[2];
  } else if (argc == 5 && std::string(argv[2]) == "--constraints") {
    constraints_flag = argv[3];
    path = argv[4];
  }
  if (path == nullptr || (constraints_flag != "" && constraints_flag != "on" &&
                          constraints_flag != "off")) {
    std::fprintf(stderr, "sql: expected 'sthirdb sql [--constraints on|off] <file>'\n");
    return 2;
  }

  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    std::fprintf(stderr, "sql: cannot read %s: %s\n", path, ec.message().c_str());
    return 1;
  }
  std::string script(size, '\0');
  {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "sql: cannot open %s\n", path);
      return 1;
    }
    const size_t got = std::fread(script.data(), 1, size, f);
    std::fclose(f);
    script.resize(got);
  }

  sthir::Catalog catalog(sthir::kCatalogPath);
  std::string err;
  if (!catalog.Load(&err)) {
    std::fprintf(stderr, "sql: cannot load catalog: %s\n", err.c_str());
    return 1;
  }
  sthir::ExecutionEngine engine(&catalog, sthir::kDataDir);
  if (!constraints_flag.empty()) {
    engine.SetSessionFlag("constraints_enabled", constraints_flag);
  }

  const std::vector<std::string> statements = sthir::SplitSqlScript(script);
  size_t executed = 0;
  for (const auto& stmt : statements) {
    auto res = engine.Execute(stmt);
    if (!res.success) {
      std::fprintf(stderr, "sql: statement %zu failed: %s\n  in: %s\n", executed + 1,
                   res.error_message.c_str(), stmt.c_str());
      return 1;
    }
    ++executed;
  }
  std::printf("sql: %zu statement(s) executed from %s%s%s\n", executed, path,
              constraints_flag.empty() ? "" : " (constraints_enabled=",
              constraints_flag.empty() ? "" : (constraints_flag + ")").c_str());
  return 0;
}

}  // namespace

// retail-gen [--shoppers N] [--items N] [--purchases N] [--lpp N] [--seed N]
//            [--constraints on|off] [--no-index] [--no-analyze]
int CmdRetailGen(int argc, char** argv) {
  sthir::RetailGenConfig cfg;
  std::string constraints_flag;

  auto next_u64 = [&](int* i, uint64_t* out) -> bool {
    if (*i + 1 >= argc) return false;
    errno = 0;
    char* end = nullptr;
    const std::string v = argv[++(*i)];
    const unsigned long long parsed = std::strtoull(v.c_str(), &end, 10);
    if (errno != 0 || end == v.c_str() || *end != '\0' || parsed == 0) return false;
    *out = static_cast<uint64_t>(parsed);
    return true;
  };

  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    uint64_t v = 0;
    if (a == "--shoppers" && next_u64(&i, &v)) {
      cfg.shoppers = static_cast<uint32_t>(v);
    } else if (a == "--items" && next_u64(&i, &v)) {
      cfg.items = static_cast<uint32_t>(v);
    } else if (a == "--purchases" && next_u64(&i, &v)) {
      cfg.purchases = static_cast<uint32_t>(v);
    } else if (a == "--lpp" && next_u64(&i, &v)) {
      cfg.line_items_per_purchase = static_cast<uint32_t>(v);
    } else if (a == "--seed" && next_u64(&i, &v)) {
      cfg.seed = v;
    } else if (a == "--constraints" && i + 1 < argc) {
      constraints_flag = argv[++i];
      if (constraints_flag != "on" && constraints_flag != "off") {
        std::fprintf(stderr, "retail-gen: --constraints expects on|off\n");
        return 2;
      }
    } else if (a == "--no-index") {
      cfg.create_indexes = false;
    } else if (a == "--no-analyze") {
      cfg.analyze = false;
    } else {
      std::fprintf(stderr, "retail-gen: unknown or incomplete argument '%s'\n", a.c_str());
      return 2;
    }
  }

  sthir::Catalog catalog(sthir::kCatalogPath);
  std::string err;
  if (!catalog.Load(&err)) {
    std::fprintf(stderr, "retail-gen: cannot load catalog: %s\n", err.c_str());
    return 1;
  }
  sthir::ExecutionEngine engine(&catalog, sthir::kDataDir);
  if (!constraints_flag.empty()) engine.SetSessionFlag("constraints_enabled", constraints_flag);

  sthir::RetailGenResult result;
  if (!sthir::GenerateRetailData(&engine, cfg, &result, &err)) {
    std::fprintf(stderr, "retail-gen: failed: %s\n", err.c_str());
    return 1;
  }

  std::printf("retail-gen: seed=%llu, constraints=%s\n",
              static_cast<unsigned long long>(cfg.seed),
              constraints_flag.empty() ? "on" : constraints_flag.c_str());
  for (const auto& [table, rows] : result.rows_by_table) {
    std::printf("  %-24s %8zu\n", table.c_str(), rows);
  }
  std::printf("  %-24s %8zu rows in %.1f ms\n", "TOTAL", result.total_rows,
              result.elapsed_ms);
  std::printf("  constraint-backed indexes created: %zu\n", result.indexes_created);
  return 0;
}

// inject [--constraints on|off] [--count N] [--rate 1|5|10] [--extended]
//        [--classes name=N,...]
int CmdInject(int argc, char** argv) {
  sthir::InjectConfig cfg;
  std::string constraints_flag = "on";
  std::string classes_label;

  auto next_u64 = [&](int* i, uint64_t* out) -> bool {
    if (*i + 1 >= argc) return false;
    errno = 0;
    char* end = nullptr;
    const std::string v = argv[++(*i)];
    const unsigned long long parsed = std::strtoull(v.c_str(), &end, 10);
    if (errno != 0 || end == v.c_str() || *end != '\0' || parsed == 0) return false;
    *out = static_cast<uint64_t>(parsed);
    return true;
  };

  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    uint64_t v = 0;
    if (a == "--constraints" && i + 1 < argc) {
      constraints_flag = argv[++i];
      if (constraints_flag != "on" && constraints_flag != "off") {
        std::fprintf(stderr, "inject: --constraints expects on|off\n");
        return 2;
      }
    } else if (a == "--count" && next_u64(&i, &v)) {
      cfg.default_count = static_cast<size_t>(v);
    } else if (a == "--rate" && next_u64(&i, &v)) {
      if (v != 1 && v != 5 && v != 10) {
        std::fprintf(stderr, "inject: --rate expects 1, 5 or 10\n");
        return 2;
      }
      cfg.rate_percent = static_cast<int>(v);
    } else if (a == "--extended") {
      cfg.extended = true;
    } else if (a == "--classes" && i + 1 < argc) {
      const std::string spec = argv[++i];
      classes_label = spec;
      size_t pos = 0;
      while (pos < spec.size()) {
        const size_t comma = spec.find(',', pos);
        const std::string item = spec.substr(pos, comma - pos);
        const size_t eq = item.find('=');
        if (eq == std::string::npos || eq + 1 >= item.size()) {
          std::fprintf(stderr, "inject: bad --classes entry '%s' (want name=N)\n", item.c_str());
          return 2;
        }
        sthir::InjectionClassDef def;
        def.name = item.substr(0, eq);
        def.count = std::strtoul(item.c_str() + eq + 1, nullptr, 10);
        cfg.classes.push_back(std::move(def));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    } else {
      std::fprintf(stderr, "inject: unknown or incomplete argument '%s'\n", a.c_str());
      return 2;
    }
  }

  sthir::Catalog catalog(sthir::kCatalogPath);
  std::string err;
  if (!catalog.Load(&err)) {
    std::fprintf(stderr, "inject: cannot load catalog: %s\n", err.c_str());
    return 1;
  }
  sthir::ExecutionEngine engine(&catalog, sthir::kDataDir);
  engine.SetSessionFlag("constraints_enabled", constraints_flag);

  sthir::InjectResult result;
  if (!sthir::RunAnomalyInjector(&engine, cfg, &result, &err)) {
    std::fprintf(stderr, "inject: failed: %s\n", err.c_str());
    return 1;
  }

  std::string classes_desc;
  if (!cfg.classes.empty()) {
    classes_desc = classes_label;
  } else {
    classes_desc = std::to_string(cfg.extended ? 8 : 3) + " classes x " +
                   std::to_string(cfg.default_count);
  }
  if (cfg.rate_percent > 0) {
    classes_desc += " (rate " + std::to_string(cfg.rate_percent) +
                    "% of each target table)";
  }
  const std::string mode =
      (constraints_flag == "off" ? "unconstrained" : "constrained") +
      std::string(", ") + classes_desc;
  std::printf("%s", sthir::FormatInjectionTable(result, mode).c_str());
  return 0;
}

// bench [--threads a,b,c] [--iters N] [--warmup N] [--constraints on|off]
//       [--frames N] [--query <sql>]
int CmdBench(int argc, char** argv) {
  sthir::BenchConfig cfg;
  std::string constraints_flag = "on";
  std::size_t frames = 8192;  // 500 threads x ~1 pin each cannot fit in 64 frames

  auto next_u64 = [&](int* i, uint64_t* out) -> bool {
    if (*i + 1 >= argc) return false;
    errno = 0;
    char* end = nullptr;
    const std::string v = argv[++(*i)];
    const unsigned long long parsed = std::strtoull(v.c_str(), &end, 10);
    if (errno != 0 || end == v.c_str() || *end != '\0' || parsed == 0) return false;
    *out = static_cast<uint64_t>(parsed);
    return true;
  };

  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    uint64_t v = 0;
    if (a == "--threads" && i + 1 < argc) {
      const std::string list = argv[++i];
      cfg.thread_counts.clear();
      size_t pos = 0;
      while (pos < list.size()) {
        const size_t comma = list.find(',', pos);
        const std::string item = list.substr(pos, comma - pos);
        const unsigned long n = std::strtoul(item.c_str(), nullptr, 10);
        if (item.empty() || n == 0) {
          std::fprintf(stderr, "bench: bad --threads entry '%s'\n", item.c_str());
          return 2;
        }
        cfg.thread_counts.push_back(static_cast<size_t>(n));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    } else if (a == "--iters" && next_u64(&i, &v)) {
      cfg.iters_per_thread = static_cast<size_t>(v);
    } else if (a == "--warmup" && next_u64(&i, &v)) {
      cfg.warmup = static_cast<size_t>(v);
    } else if (a == "--frames" && next_u64(&i, &v)) {
      frames = static_cast<std::size_t>(v);
    } else if (a == "--constraints" && i + 1 < argc) {
      constraints_flag = argv[++i];
      if (constraints_flag != "on" && constraints_flag != "off") {
        std::fprintf(stderr, "bench: --constraints expects on|off\n");
        return 2;
      }
    } else if (a == "--query" && i + 1 < argc) {
      cfg.query = argv[++i];
    } else {
      std::fprintf(stderr, "bench: unknown or incomplete argument '%s'\n", a.c_str());
      return 2;
    }
  }

  sthir::Catalog catalog(sthir::kCatalogPath);
  std::string err;
  if (!catalog.Load(&err)) {
    std::fprintf(stderr, "bench: cannot load catalog: %s\n", err.c_str());
    return 1;
  }
  sthir::ExecutionEngine engine(&catalog, sthir::kDataDir);
  engine.SetSessionFlag("constraints_enabled", constraints_flag);
  engine.SetPoolFrames(frames);

  sthir::BenchResult result;
  const bool ok = sthir::RunBenchmark(
      &engine, cfg, &result, &err, [](const sthir::BenchRow& row) {
        std::fprintf(stderr, "bench: %zu threads done (queries=%zu failures=%zu avg=%.1f ms)\n",
                     row.threads, row.queries, row.failures, row.avg_ms);
      });
  if (!ok) {
    std::fprintf(stderr, "bench: failed: %s\n", err.c_str());
    return 1;
  }

  const std::string mode =
      (constraints_flag == "off" ? "unconstrained" : "constrained") +
      std::string(", ") + std::to_string(cfg.iters_per_thread) +
      " iteration(s) per thread, pool frames=" + std::to_string(frames);
  std::printf("%s", sthir::FormatBenchTable(result, mode).c_str());
  return 0;
}

// ----------------------------------------------------------------
// Novelty 2A: crash-injection trial commands
// ----------------------------------------------------------------

std::string ArgValue(int argc, char** argv, const char* name, const std::string& fallback) {
  for (int i = 2; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == name) return argv[i + 1];
  }
  return fallback;
}

bool HasFlag(int argc, char** argv, const char* name) {
  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]) == name) return true;
  }
  return false;
}

int CmdTrialInit(int argc, char** argv) {
  const std::string dir = ArgValue(argc, argv, "--dir", "./data/trial");
  std::string err;
  if (!sthir::TrialInit(dir, &err)) {
    std::fprintf(stderr, "trial-init: %s\n", err.c_str());
    return 1;
  }
  std::printf("trial-init: %s ready\n", dir.c_str());
  return 0;
}

int CmdTrialRun(int argc, char** argv) {
  sthir::TrialConfig cfg;
  cfg.dir = ArgValue(argc, argv, "--dir", cfg.dir);
  cfg.seed = std::strtoull(ArgValue(argc, argv, "--seed", "1").c_str(), nullptr, 10);
  cfg.txns = static_cast<size_t>(
      std::strtoull(ArgValue(argc, argv, "--txns", "8").c_str(), nullptr, 10));
  cfg.checkpoint_interval = static_cast<size_t>(
      std::strtoull(ArgValue(argc, argv, "--checkpoint", "0").c_str(), nullptr, 10));
  cfg.crash_at_end = HasFlag(argc, argv, "--crash-end");

  std::string err;
  sthir::TrialRunResult result;
  if (!sthir::TrialRun(cfg, &result, &err)) {
    std::fprintf(stderr, "trial-run: %s\n", err.c_str());
    return 1;
  }
  std::printf("trial-run: seed=%llu committed=%zu elapsed_ms=%.3f wal_bytes=%llu\n",
              static_cast<unsigned long long>(cfg.seed), result.committed, result.elapsed_ms,
              static_cast<unsigned long long>(result.wal_bytes));
  return 0;
}

int CmdTrialRecover(int argc, char** argv) {
  const std::string dir = ArgValue(argc, argv, "--dir", "./data/trial");
  std::string err;
  std::string out;
  if (!sthir::TrialRecover(dir, &out, &err)) {
    std::fprintf(stderr, "trial-recover: %s\n", err.c_str());
    return 1;
  }
  std::printf("%s", out.c_str());
  return 0;
}

int CmdTrialVerify(int argc, char** argv) {
  const std::string dir = ArgValue(argc, argv, "--dir", "./data/trial");
  const std::string oracle = ArgValue(argc, argv, "--oracle", dir + "/oracle.log");
  std::string err;
  std::string out;
  bool ok = false;
  if (!sthir::TrialVerify(dir, oracle, &out, &ok, &err)) {
    std::fprintf(stderr, "trial-verify: %s\n", err.c_str());
    return 1;
  }
  std::printf("%s", out.c_str());
  return ok ? 0 : 1;
}

int CmdCrashHarness(int argc, char** argv) {
  sthir::HarnessConfig cfg;
  cfg.trials = static_cast<size_t>(
      std::strtoull(ArgValue(argc, argv, "--trials", "1000").c_str(), nullptr, 10));
  cfg.txns = static_cast<size_t>(
      std::strtoull(ArgValue(argc, argv, "--txns", "8").c_str(), nullptr, 10));
  cfg.seed_base = std::strtoull(ArgValue(argc, argv, "--seed", "1").c_str(), nullptr, 10);
  cfg.bin = ArgValue(argc, argv, "--bin", cfg.bin);
  cfg.dir = ArgValue(argc, argv, "--dir", cfg.dir);
  cfg.csv_path = ArgValue(argc, argv, "--csv", cfg.csv_path);

  std::string err;
  if (!sthir::RunCrashHarness(cfg, &err)) {
    std::fprintf(stderr, "crash-harness: %s\n", err.c_str());
    return 1;
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 0;
  }
  const std::string cmd = argv[1];

  if (cmd == "--version" || cmd == "-v") {
    std::printf("SthirDB %s\n", sthir::kVersion);
    return 0;
  }
  if (cmd == "run") {
    if (argc < 3) {
      std::fprintf(stderr, "run: expected 'sthirdb run \"<sql>\"'\n");
      return 2;
    }
    return CmdRun(argc, argv);
  }
  if (cmd == "explain") {
    return CmdExplain(argc, argv);
  }
  if (cmd == "optimizer") {
    bool force_seq = false;
    bool force_index = false;
    SessionOpts opts;
    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--seq") {
        force_seq = true;
      } else if (a == "--index") {
        force_index = true;
      } else if (a == "--constraints" && i + 1 < argc) {
        opts.constraints = argv[++i];
        if (opts.constraints != "on" && opts.constraints != "off") {
          std::fprintf(stderr, "optimizer: --constraints expects on|off\n");
          return 2;
        }
      } else if (a == "--constraint-aware" && i + 1 < argc) {
        opts.constraint_aware = argv[++i];
        if (opts.constraint_aware != "on" && opts.constraint_aware != "off") {
          std::fprintf(stderr, "optimizer: --constraint-aware expects on|off\n");
          return 2;
        }
      }
    }
    return CmdOptimizer(argc, argv, opts, force_seq, force_index);
  }
  if (cmd == "analyze") {
    if (argc != 3) {
      std::fprintf(stderr, "analyze: expected 'sthirdb analyze <table>'\n");
      return 2;
    }
    return CmdAnalyze(argv[2]);
  }
  if (cmd == "init_schema") {
    if (argc != 2) {
      std::fprintf(stderr, "init_schema: takes no arguments\n");
      return 2;
    }
    return CmdInitSchema();
  }
  if (cmd == "load_data") {
    size_t rows = 0;
    if (!ParseRowsArg(argc, argv, &rows)) {
      std::fprintf(stderr, "load_data: expected 'load_data --rows N' with N a positive integer\n");
      return 2;
    }
    return CmdLoadData(rows);
  }
  if (cmd == "count") {
    if (argc != 2) {
      std::fprintf(stderr, "count: takes no arguments\n");
      return 2;
    }
    return CmdCount();
  }
  if (cmd == "recover") {
    sthir::Catalog catalog(sthir::kCatalogPath);
    catalog.Load();
    sthir::ExecutionEngine engine(&catalog, sthir::kDataDir);
    auto report = engine.Recover();
    std::printf("Recovery completed:\n");
    std::printf("  Analysis scanned : %zu records\n", report.analysis_records_scanned);
    std::printf("  Redo applied     : %zu records\n", report.redo_records_applied);
    std::printf("  Redo skipped     : %zu records\n", report.redo_records_skipped);
    std::printf("  Undo applied     : %zu records\n", report.undo_records_applied);
    std::printf("  Loser txns       : %zu\n", report.loser_txns.size());
    std::printf("  Recovery time    : %.2f ms\n", report.recovery_time_ms);
    return 0;
  }
  if (cmd == "sql") {
    return CmdSql(argc, argv);
  }
  if (cmd == "retail-gen") {
    return CmdRetailGen(argc, argv);
  }
  if (cmd == "inject") {
    return CmdInject(argc, argv);
  }
  if (cmd == "bench") {
    return CmdBench(argc, argv);
  }
  if (cmd == "trial-init") return CmdTrialInit(argc, argv);
  if (cmd == "trial-run") return CmdTrialRun(argc, argv);
  if (cmd == "trial-recover") return CmdTrialRecover(argc, argv);
  if (cmd == "trial-verify") return CmdTrialVerify(argc, argv);
  if (cmd == "crash-harness") return CmdCrashHarness(argc, argv);

  std::fprintf(stderr, "sthirdb: unknown command '%s'\n", cmd.c_str());
  PrintUsage();
  return 2;
}
