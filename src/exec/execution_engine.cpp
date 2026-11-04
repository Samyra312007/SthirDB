#include "exec/execution_engine.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "exec/executors.h"
#include "exec/constraint_checker.h"
#include "index/bplus_tree.h"
#include "index/index_builder.h"
#include "index/index_scan.h"
#include "optimizer/statistics.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "recovery/recovery_manager.h"
#include "wal/wal_manager.h"
#include "concurrency/lock_manager.h"

namespace fs = std::filesystem;

namespace sthir {

namespace {

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string FormatCost(double value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", value);
  return buf;
}

// Literal expression for one value (used to build cascade delete predicates).
std::unique_ptr<Expr> ValueToExpr(const Value& v) {
  if (v.is_null()) return LiteralExpr::MakeNull();
  switch (v.type()) {
    case DataType::INT:
      return LiteralExpr::MakeInt(v.as_int());
    case DataType::DECIMAL:
      return LiteralExpr::MakeDecimal(v.as_decimal());
    case DataType::VARCHAR:
      return LiteralExpr::MakeString(v.as_string());
  }
  return LiteralExpr::MakeNull();
}

// `SET constraints_enabled = off` (also 0/false/no) turns the checker into a
// no-op; that single flag is the whole constrained-vs-unconstrained switch.
bool ConstraintsFlagOff(const std::string& value) {
  return value == "off" || value == "0" || value == "false" || value == "no";
}

}  // namespace

void ExecutionResult::PrintTable(std::ostream& os) const {
  if (!success) {
    os << "Error: " << error_message << "\n";
    return;
  }

  if (!is_query) {
    if (rows_affected > 0 || !column_names.empty()) {
      os << "Query OK, " << rows_affected << " row(s) affected\n";
    } else {
      os << "Query OK\n";
    }
    return;
  }

  if (column_names.empty()) {
    os << "(empty result set)\n";
    return;
  }

  // Calculate column widths
  std::vector<size_t> widths(column_names.size(), 0);
  for (size_t i = 0; i < column_names.size(); ++i) {
    widths[i] = std::max(widths[i], column_names[i].size());
  }

  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }

  // Helper for border line: +-----+-----+
  auto print_border = [&]() {
    os << "+";
    for (size_t w : widths) {
      os << std::string(w + 2, '-') << "+";
    }
    os << "\n";
  };

  print_border();

  // Print header
  os << "|";
  for (size_t i = 0; i < column_names.size(); ++i) {
    os << " " << std::left << std::setw(static_cast<int>(widths[i])) << column_names[i] << " |";
  }
  os << "\n";

  print_border();

  // Print data rows
  for (const auto& row : rows) {
    os << "|";
    for (size_t i = 0; i < column_names.size(); ++i) {
      std::string val = (i < row.size()) ? row[i] : "";
      os << " " << std::left << std::setw(static_cast<int>(widths[i])) << val << " |";
    }
    os << "\n";
  }

  print_border();
  os << "(" << rows.size() << " row(s))\n";
}

ExecutionEngine::ExecutionEngine(Catalog* catalog, std::string data_dir)
    : catalog_(catalog), data_dir_(std::move(data_dir)) {
  std::string wal_path = data_dir_ + "/wal.log";
  wal_ = std::make_unique<WalManager>(wal_path);
  recovery_ = std::make_unique<RecoveryManager>(wal_.get(), nullptr);
  lock_manager_ = std::make_unique<LockManager>();
  constraint_checker_ = std::make_unique<ConstraintChecker>(
      catalog_,
      [this](const std::string& table, const std::vector<std::string>& columns,
             std::unordered_map<std::string, uint32_t>* out, std::string* err) {
        return LoadKeySet(table, columns, out, err);
      },
      [this](const std::string& table, const std::vector<std::string>& columns,
             const std::vector<Value>& key, uint32_t txn_id, std::string* err) {
        return DeleteRowsByKey(table, columns, key, txn_id, err);
      });
}

ExecutionEngine::~ExecutionEngine() {
  for (auto& kv : storage_map_) {
    if (kv.second && kv.second->bp) {
      kv.second->bp->flush_all();
    }
  }
}

RecoveryReport ExecutionEngine::Recover() {
  if (catalog_) {
    catalog_->Load();
    for (const auto& name : catalog_->GetTableNames()) {
      StorageContext* sc = GetStorage(name);
      if (sc && sc->bp) {
        recovery_->SetBufferPool(sc->bp.get());
      }
    }
  }
  // Recovery rewrites pages behind the checker's back: re-read on next check.
  if (constraint_checker_) constraint_checker_->InvalidateAll();
  return recovery_->Recover();
}

void ExecutionEngine::SetSessionFlag(const std::string& key, const std::string& val) {
  session_flags_[ToLower(key)] = val;
}

std::string ExecutionEngine::GetSessionFlag(const std::string& key) const {
  auto it = session_flags_.find(ToLower(key));
  if (it != session_flags_.end()) return it->second;
  return "";
}

void ExecutionEngine::SetPoolFrames(std::size_t frames) {
  if (frames == 0) return;
  std::lock_guard<std::mutex> lock(engine_latch_);
  if (!storage_map_.empty()) {
    // Storage contexts bake the frame count in at creation time; changing it
    // mid-flight would leave mixed pools. Callers set it before the first
    // statement (the benchmark does).
    return;
  }
  pool_frames_ = frames;
}

uint32_t ExecutionEngine::GetCurrentTxnId() const {
  std::lock_guard<std::mutex> lock(engine_latch_);
  auto it = thread_txns_.find(std::this_thread::get_id());
  if (it != thread_txns_.end()) return it->second;
  return 0;
}

void ExecutionEngine::SetCurrentTxnId(uint32_t tid) {
  std::lock_guard<std::mutex> lock(engine_latch_);
  if (tid == 0) {
    thread_txns_.erase(std::this_thread::get_id());
  } else {
    thread_txns_[std::this_thread::get_id()] = tid;
  }
}

ExecutionEngine::StorageContext* ExecutionEngine::GetStorage(const std::string& table_name) {
  std::lock_guard<std::mutex> lock(engine_latch_);
  std::string lower = ToLower(table_name);
  auto it = storage_map_.find(lower);
  if (it != storage_map_.end()) return it->second.get();

  const TableSchema* schema = catalog_ ? catalog_->GetTable(table_name) : nullptr;
  std::string file_path = schema ? schema->data_file_path : (data_dir_ + "/" + lower + ".dat");

  auto sc = std::make_unique<StorageContext>();
  sc->dm = std::make_unique<DiskManager>(file_path);
  sc->bp = std::make_unique<BufferPool>(pool_frames_, sc->dm.get());
  if (wal_) {
    sc->bp->set_wal_manager(wal_.get());
  }
  if (recovery_) {
    recovery_->SetBufferPool(sc->bp.get());
  }

  auto* ptr = sc.get();
  storage_map_[lower] = std::move(sc);
  return ptr;
}

ExecutionEngine::StorageContext* ExecutionEngine::GetIndexStorage(const std::string& path) {
  std::lock_guard<std::mutex> lock(engine_latch_);
  const std::string key = "file:" + path;
  auto it = storage_map_.find(key);
  if (it != storage_map_.end()) return it->second.get();

  auto sc = std::make_unique<StorageContext>();
  sc->dm = std::make_unique<DiskManager>(path);
  sc->bp = std::make_unique<BufferPool>(pool_frames_, sc->dm.get());

  auto* ptr = sc.get();
  storage_map_[key] = std::move(sc);
  return ptr;
}

bool ExecutionEngine::LoadKeySet(const std::string& table,
                                 const std::vector<std::string>& columns,
                                 std::unordered_map<std::string, uint32_t>* out,
                                 std::string* err) {
  const TableSchema* schema = catalog_ ? catalog_->GetTable(table) : nullptr;
  if (schema == nullptr) {
    if (err) *err = "table '" + table + "' does not exist";
    return false;
  }
  StorageContext* sc = GetStorage(schema->table_name);
  if (sc == nullptr) {
    if (err) *err = "storage for table '" + table + "' unavailable";
    return false;
  }
  // Same scan semantics as every other reader (deleted rows excluded).
  SeqScanExecutor scan(schema->table_name, *schema, sc->bp.get(), sc->dm.get());
  scan.init();
  while (true) {
    auto tuple = scan.next();
    if (!tuple.has_value()) break;
    std::string key;
    if (ConstraintChecker::KeyOf(*schema, columns, *tuple, &key)) ++(*out)[std::move(key)];
  }
  scan.close();
  return true;
}

size_t ExecutionEngine::DeleteRowsByKey(const std::string& table,
                                        const std::vector<std::string>& columns,
                                        const std::vector<Value>& key, uint32_t txn_id,
                                        std::string* err) {
  if (cascade_depth_ >= 8) {
    if (err) *err = "ON DELETE CASCADE depth limit exceeded";
    return 0;
  }
  const TableSchema* schema = catalog_ ? catalog_->GetTable(table) : nullptr;
  if (schema == nullptr) {
    if (err) *err = "cascade target table '" + table + "' does not exist";
    return 0;
  }
  if (columns.size() != key.size()) {
    if (err) *err = "cascade key arity mismatch for table '" + table + "'";
    return 0;
  }

  // Predicate: col1 = v1 AND col2 = v2 ...
  std::unique_ptr<Expr> pred;
  for (size_t i = 0; i < columns.size(); ++i) {
    std::unique_ptr<Expr> eq = std::make_unique<BinaryExpr>(
        BinaryOp::EQ, std::make_unique<ColumnRefExpr>("", columns[i]), ValueToExpr(key[i]));
    pred = pred ? std::make_unique<BinaryExpr>(BinaryOp::AND, std::move(pred), std::move(eq))
                : std::move(eq);
  }
  auto scan = std::make_unique<SeqScanPlan>(schema->table_name, *schema);
  auto filter = std::make_unique<FilterPlan>(std::move(pred), std::move(scan));
  DeletePlan dp(schema->table_name, *schema, std::move(filter));

  StorageContext* sc = GetStorage(schema->table_name);
  if (sc == nullptr) {
    if (err) *err = "storage for table '" + table + "' unavailable";
    return 0;
  }
  recovery_->SetBufferPool(sc->bp.get());
  auto exec = BuildExecutorTree(static_cast<const DeletePlan*>(&dp)->child());
  if (!exec) {
    if (err) *err = "failed to build cascade executor for table '" + table + "'";
    return 0;
  }

  ++cascade_depth_;  // recursion guard for chains of cascading deletes
  const size_t deleted = ExecuteDelete(dp, std::move(exec), sc->bp.get(), sc->dm.get(), err,
                                       recovery_.get(), txn_id, lock_manager_.get(),
                                       constraint_checker_.get());
  --cascade_depth_;
  return deleted;
}

std::unique_ptr<Executor> ExecutionEngine::BuildExecutorTree(const PlanNode* plan) {
  if (!plan) return nullptr;

  switch (plan->type()) {
    case PlanNodeType::EMPTY_RESULT: {
      return std::make_unique<EmptyExecutor>(plan->output_schema());
    }
    case PlanNodeType::SEQ_SCAN: {
      const auto* scan = static_cast<const SeqScanPlan*>(plan);
      StorageContext* sc = GetStorage(scan->table_name());
      return std::make_unique<SeqScanExecutor>(
          scan->table_name(), scan->output_schema(), sc->bp.get(), sc->dm.get());
    }
    case PlanNodeType::INDEX_SCAN: {
      const auto* idx_scan = static_cast<const IndexScanPlan*>(plan);
      StorageContext* table = GetStorage(idx_scan->table_name());
      const std::string path = IndexFilePath(idx_scan->output_schema(), idx_scan->index().index_name);
      StorageContext* index = GetIndexStorage(path);
      auto tree = std::make_unique<BPlusTree>(index->dm.get(), index->bp.get(),
                                              idx_scan->index().root_page_id,
                                              idx_scan->index().height);
      return std::make_unique<IndexScanExecutor>(
          idx_scan->table_name(), idx_scan->output_schema(), table->bp.get(), table->dm.get(),
          std::move(tree), idx_scan->range());
    }
    case PlanNodeType::FILTER: {
      const auto* f = static_cast<const FilterPlan*>(plan);
      auto child = BuildExecutorTree(f->child());
      auto pred = f->predicate() ? f->predicate()->Clone() : nullptr;
      return std::make_unique<FilterExecutor>(std::move(child), std::move(pred));
    }
    case PlanNodeType::PROJECT: {
      const auto* p = static_cast<const ProjectPlan*>(plan);
      auto child = BuildExecutorTree(p->child());
      std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projs;
      for (const auto& item : p->projections()) {
        projs.push_back({item.first ? item.first->Clone() : nullptr, item.second});
      }
      return std::make_unique<ProjectExecutor>(std::move(child), p->output_schema(), std::move(projs));
    }
    case PlanNodeType::AGGREGATE: {
      const auto* a = static_cast<const AggregatePlan*>(plan);
      auto child = BuildExecutorTree(a->child());
      std::vector<AggEntry> aggs;
      for (const auto& item : a->aggregates()) {
        AggEntry entry;
        entry.func = item.func;
        entry.is_star = item.is_star;
        entry.arg = item.arg ? item.arg->Clone() : nullptr;
        entry.output_name = item.output_name;
        aggs.push_back(std::move(entry));
      }
      return std::make_unique<AggregateExecutor>(
          std::move(child), std::move(aggs), a->group_by_cols(), a->output_schema());
    }
    case PlanNodeType::SORT: {
      const auto* s = static_cast<const SortPlan*>(plan);
      auto child = BuildExecutorTree(s->child());
      return std::make_unique<SortExecutor>(std::move(child), s->order_by_items(), s->output_schema());
    }
    case PlanNodeType::NESTED_LOOP_JOIN: {
      const auto* j = static_cast<const NestedLoopJoinPlan*>(plan);
      auto left = BuildExecutorTree(j->left());
      auto right = BuildExecutorTree(j->right());
      auto pred = j->join_predicate() ? j->join_predicate()->Clone() : nullptr;
      return std::make_unique<NestedLoopJoinExecutor>(
          std::move(left), std::move(right), std::move(pred), j->output_schema());
    }
    case PlanNodeType::HASH_JOIN: {
      const auto* j = static_cast<const HashJoinPlan*>(plan);
      auto left = BuildExecutorTree(j->left());
      auto right = BuildExecutorTree(j->right());
      auto pred = j->join_predicate() ? j->join_predicate()->Clone() : nullptr;
      std::vector<std::unique_ptr<Expr>> left_keys;
      std::vector<std::unique_ptr<Expr>> right_keys;
      for (const auto& expr : j->left_keys()) left_keys.push_back(expr->Clone());
      for (const auto& expr : j->right_keys()) right_keys.push_back(expr->Clone());
      return std::make_unique<HashJoinExecutor>(
          std::move(left), std::move(right), std::move(pred),
          std::move(left_keys), std::move(right_keys), j->build_on_right(),
          j->output_schema());
    }
    default:
      break;
  }
  return nullptr;
}

ExecutionResult ExecutionEngine::Execute(const std::string& sql) {
  const uint64_t before = DiskManager::TotalReads();
  ExecutionResult res = ExecuteStatement(sql);
  res.pages_read = static_cast<size_t>(DiskManager::TotalReads() - before);
  return res;
}

ExecutionResult ExecutionEngine::ExecuteStatement(const std::string& sql) {
  try {
    return ExecuteInner(sql);
  } catch (const std::exception& e) {
    ExecutionResult res;
    res.success = false;
    res.error_message = e.what();
    return res;
  } catch (...) {
    ExecutionResult res;
    res.success = false;
    res.error_message = "unknown error while executing statement";
    return res;
  }
}

ExecutionResult ExecutionEngine::ExecuteInner(const std::string& sql) {
  ExecutionResult res;

  Lexer lexer(sql);
  auto tokens = lexer.Tokenize();
  if (lexer.has_error()) {
    res.success = false;
    res.error_message = lexer.error_message();
    return res;
  }

  Parser parser(std::move(tokens));
  auto stmt = parser.ParseStatement();
  if (parser.has_error() || !stmt) {
    res.success = false;
    res.error_message = parser.has_error() ? parser.error_message() : "empty statement";
    return res;
  }

  // 1. DDL: CREATE TABLE
  if (stmt->type() == StmtType::CREATE_TABLE) {
    auto* ct = static_cast<CreateTableStmt*>(stmt.get());
    TableSchema schema;
    schema.table_name = ct->table_name;
    schema.columns = ct->columns;
    schema.primary_key_columns = ct->primary_key_columns;
    schema.unique_constraints = ct->unique_constraints;
    schema.foreign_keys = ct->foreign_keys;
    schema.check_constraints = ct->check_constraints;
    schema.data_file_path = data_dir_ + "/" + ToLower(ct->table_name) + ".dat";

    std::string err;
    if (!catalog_->CreateTable(schema, &err)) {
      res.success = false;
      res.error_message = err;
      return res;
    }
    // The catalog changed: drop cached rules so the next check re-reads them.
    if (constraint_checker_) constraint_checker_->InvalidateAll();

    // Initialize first page of data file
    StorageContext* sc = GetStorage(ct->table_name);
    if (sc && sc->dm->num_pages() == 0) {
      uint32_t pid = sc->dm->allocate_page();
      Page* p = sc->bp->fetch_page(pid);
      if (p) {
        p->Reset(pid);
        sc->bp->unpin_page(pid, true);
        sc->bp->flush_all();
      }
    }

    res.success = true;
    res.is_query = false;
    return res;
  }

  // 2. DDL: CREATE INDEX
  if (stmt->type() == StmtType::CREATE_INDEX) {
    auto* ci = static_cast<CreateIndexStmt*>(stmt.get());
    const TableSchema* schema = catalog_ ? catalog_->GetTable(ci->table_name) : nullptr;
    if (schema == nullptr) {
      res.success = false;
      res.error_message = "table '" + ci->table_name + "' does not exist";
      return res;
    }
    const int col_idx = schema->FindColumnIndex(ci->column_name);
    if (col_idx < 0) {
      res.success = false;
      res.error_message = "column '" + ci->column_name + "' not found in table '" +
                          schema->table_name + "'";
      return res;
    }
    for (const auto& existing : schema->indexes) {
      if (ToLower(existing.index_name) == ToLower(ci->index_name)) {
        res.success = false;
        res.error_message = "index '" + ci->index_name + "' already exists on table '" +
                            schema->table_name + "'";
        return res;
      }
    }

    StorageContext* sc = GetStorage(schema->table_name);
    std::string build_err;
    IndexBuildResult built;
    if (!BuildIndex(*schema, ci->index_name, schema->columns[col_idx].name, sc->dm.get(),
                    sc->bp.get(), &built, &build_err)) {
      res.success = false;
      res.error_message = build_err;
      return res;
    }

    IndexMetadata meta;
    meta.index_name = ci->index_name;
    meta.column_name = schema->columns[col_idx].name;
    meta.root_page_id = built.root_page_id;
    meta.height = built.height;
    meta.order = kIndexOrder;
    std::string catalog_err;
    if (!catalog_->AddIndex(schema->table_name, meta, &catalog_err)) {
      res.success = false;
      res.error_message = catalog_err;
      return res;
    }

    // Phase 4: an index build also refreshes the statistics of its column, so
    // the optimizer can estimate selectivities without a separate ANALYZE.
    ColumnStats stats;
    std::string stats_err;
    if (!ComputeColumnStats(*schema, meta.column_name, sc->dm.get(), sc->bp.get(), &stats,
                            &stats_err)) {
      res.success = false;
      res.error_message = stats_err;
      return res;
    }
    if (!catalog_->SetColumnStats(schema->table_name, stats, &catalog_err)) {
      res.success = false;
      res.error_message = catalog_err;
      return res;
    }

    res.success = true;
    res.is_query = false;
    res.rows_affected = built.entries;
    return res;
  }

  // 3. Control statements
  if (stmt->type() == StmtType::TRANSACTION) {
    auto* tx = static_cast<TransactionStmt*>(stmt.get());
    res.success = true;
    res.is_query = false;
    res.rows_affected = 0;
    if (tx->action() == TransactionStmt::Action::BEGIN) {
      SetCurrentTxnId(recovery_->Begin());
    } else if (tx->action() == TransactionStmt::Action::COMMIT) {
      uint32_t tid = GetCurrentTxnId();
      if (tid != 0) {
        recovery_->Commit(tid);
        lock_manager_->ReleaseAll(tid);
        SetCurrentTxnId(0);
      }
    } else if (tx->action() == TransactionStmt::Action::ROLLBACK) {
      uint32_t tid = GetCurrentTxnId();
      if (tid != 0) {
        recovery_->Abort(tid);
        lock_manager_->ReleaseAll(tid);
        SetCurrentTxnId(0);
      }
      // Rolled-back writes must not leave their keys in the constraint caches.
      if (constraint_checker_) constraint_checker_->InvalidateAll();
    } else if (tx->action() == TransactionStmt::Action::CHECKPOINT) {
      recovery_->Checkpoint(true);
    }
    return res;
  }

  if (stmt->type() == StmtType::ANALYZE) {
    auto* an = static_cast<AnalyzeStmt*>(stmt.get());
    const TableSchema* schema = catalog_ ? catalog_->GetTable(an->table_name()) : nullptr;
    if (schema == nullptr) {
      res.success = false;
      res.error_message = "table '" + an->table_name() + "' does not exist";
      return res;
    }

    StorageContext* sc = GetStorage(schema->table_name);
    std::vector<ColumnStats> stats;
    std::string stats_err;
    if (!ComputeTableStats(*schema, sc->dm.get(), sc->bp.get(), &stats, &stats_err)) {
      res.success = false;
      res.error_message = stats_err;
      return res;
    }
    for (const auto& column_stats : stats) {
      std::string catalog_err;
      if (!catalog_->SetColumnStats(schema->table_name, column_stats, &catalog_err)) {
        res.success = false;
        res.error_message = catalog_err;
        return res;
      }
    }

    res.success = true;
    res.is_query = false;
    res.rows_affected = stats.size();  // columns analyzed
    return res;
  }

  if (stmt->type() == StmtType::SET) {
    auto* st = static_cast<SetStmt*>(stmt.get());
    SetSessionFlag(st->flag(), st->value());
    if (ToLower(st->flag()) == "checkpoint_interval") {
      if (st->value() == "none" || st->value() == "off" || st->value() == "0") {
        recovery_->SetCheckpointInterval(0);
      } else {
        try {
          recovery_->SetCheckpointInterval(std::stoul(st->value()));
        } catch (...) {}
      }
    }
    res.success = true;
    res.is_query = false;
    return res;
  }

  // 4. EXPLAIN
  if (stmt->type() == StmtType::EXPLAIN) {
    auto* exp = static_cast<ExplainStmt*>(stmt.get());
    Planner planner(*catalog_, GetSessionFlag("enable_index_scan") != "off",
                    GetSessionFlag("enable_seq_scan") != "off",
                    GetSessionFlag("constraint_aware") == "on",
                    !ConstraintsFlagOff(GetSessionFlag("constraints_enabled")));
    std::string err;
    auto plan = planner.CreatePlan(*exp->stmt(), &err);
    if (!plan) {
      res.success = false;
      res.error_message = err;
      return res;
    }
    std::string text;
    if (!plan->access_path().empty()) {
      text += "Chosen access path: " + plan->access_path() + "\n";
    }
    text += plan->ToString();
    if (!plan->alternatives().empty()) {
      text += "Rejected alternatives:\n";
      for (const auto& alternative : plan->alternatives()) {
        text += "  " + alternative.name + "  cost=" + FormatCost(alternative.cost) +
                "  rows=" + FormatCost(alternative.rows) + "\n";
      }
    }
    // Phase 4B: list every constraint-aware rule that fired while planning.
    if (GetSessionFlag("constraint_aware") == "on") {
      res.rules_line = "Applied rules: ";
      if (plan->applied_rules().empty()) {
        res.rules_line += "(none)";
      } else {
        for (size_t i = 0; i < plan->applied_rules().size(); ++i) {
          if (i > 0) res.rules_line += ", ";
          res.rules_line += plan->applied_rules()[i];
        }
      }
      text += res.rules_line + "\n";
    }

    res.success = true;
    res.is_query = true;
    res.column_names = {"QUERY PLAN"};
    res.rows.push_back({text});
    res.chosen_path = plan->access_path();
    res.estimated_cost = plan->estimated_cost();
    res.rows_estimated = plan->estimated_rows();
    res.alternatives = plan->alternatives();
    res.plan_text = plan->ToString();
    res.has_plan_info = true;
    return res;
  }

  // 5. DML & Queries via Planner
  // Phase 7: re-read the schema flag so `SET constraints_enabled = on|off`
  // decides enforcement for this statement (and nothing else).
  if (constraint_checker_) {
    constraint_checker_->SetEnabled(!ConstraintsFlagOff(GetSessionFlag("constraints_enabled")));
  }
  Planner planner(*catalog_, GetSessionFlag("enable_index_scan") != "off",
                  GetSessionFlag("enable_seq_scan") != "off",
                  GetSessionFlag("constraint_aware") == "on",
                  !ConstraintsFlagOff(GetSessionFlag("constraints_enabled")));
  std::string plan_err;
  auto plan = planner.CreatePlan(*stmt, &plan_err);
  if (!plan) {
    res.success = false;
    res.error_message = plan_err;
    return res;
  }

  if (plan->type() == PlanNodeType::INSERT) {
    const auto* ip = static_cast<const InsertPlan*>(plan.get());
    StorageContext* sc = GetStorage(ip->table_name());
    recovery_->SetBufferPool(sc->bp.get());
    uint32_t active_tid = GetCurrentTxnId();
    bool auto_commit = false;
    if (active_tid == 0) {
      active_tid = recovery_->Begin();
      auto_commit = true;
    }
    std::string exec_err;
    size_t count = ExecuteInsert(*ip, sc->bp.get(), sc->dm.get(), &exec_err,
                                 recovery_.get(), active_tid, lock_manager_.get(),
                                 constraint_checker_.get());
    if (!exec_err.empty()) {
      // Partial writes are rolled back below; the caches must forget any key
      // they recorded for those rows.
      if (count > 0) constraint_checker_->InvalidateAll();
      if (auto_commit) {
        recovery_->Abort(active_tid);
        lock_manager_->ReleaseAll(active_tid);
      }
      res.success = false;
      res.error_message = exec_err;
      return res;
    }
    if (auto_commit) {
      recovery_->Commit(active_tid);
      lock_manager_->ReleaseAll(active_tid);
    }
    res.success = true;
    res.is_query = false;
    res.rows_affected = count;
    return res;
  }

  if (plan->type() == PlanNodeType::UPDATE) {
    const auto* up = static_cast<const UpdatePlan*>(plan.get());
    StorageContext* sc = GetStorage(up->table_name());
    recovery_->SetBufferPool(sc->bp.get());
    uint32_t active_tid = GetCurrentTxnId();
    bool auto_commit = false;
    if (active_tid == 0) {
      active_tid = recovery_->Begin();
      auto_commit = true;
    }
    auto child_exec = BuildExecutorTree(up->child());
    std::string exec_err;
    size_t count = ExecuteUpdate(*up, std::move(child_exec), sc->bp.get(), sc->dm.get(),
                                 &exec_err, recovery_.get(), active_tid, lock_manager_.get(),
                                 constraint_checker_.get());
    if (!exec_err.empty()) {
      if (count > 0) constraint_checker_->InvalidateAll();
      if (auto_commit) {
        recovery_->Abort(active_tid);
        lock_manager_->ReleaseAll(active_tid);
      }
      res.success = false;
      res.error_message = exec_err;
      return res;
    }
    if (auto_commit) {
      recovery_->Commit(active_tid);
      lock_manager_->ReleaseAll(active_tid);
    }
    res.success = true;
    res.is_query = false;
    res.rows_affected = count;
    return res;
  }

  if (plan->type() == PlanNodeType::DELETE_NODE) {
    const auto* dp = static_cast<const DeletePlan*>(plan.get());
    StorageContext* sc = GetStorage(dp->table_name());
    recovery_->SetBufferPool(sc->bp.get());
    uint32_t active_tid = GetCurrentTxnId();
    bool auto_commit = false;
    if (active_tid == 0) {
      active_tid = recovery_->Begin();
      auto_commit = true;
    }
    auto child_exec = BuildExecutorTree(dp->child());
    std::string exec_err;
    size_t count = ExecuteDelete(*dp, std::move(child_exec), sc->bp.get(), sc->dm.get(),
                                 &exec_err, recovery_.get(), active_tid, lock_manager_.get(),
                                 constraint_checker_.get());
    if (!exec_err.empty()) {
      if (count > 0) constraint_checker_->InvalidateAll();
      if (auto_commit) {
        recovery_->Abort(active_tid);
        lock_manager_->ReleaseAll(active_tid);
      }
      res.success = false;
      res.error_message = exec_err;
      return res;
    }
    if (auto_commit) {
      recovery_->Commit(active_tid);
      lock_manager_->ReleaseAll(active_tid);
    }
    res.success = true;
    res.is_query = false;
    res.rows_affected = count;
    return res;
  }

  // SELECT execution
  auto exec = BuildExecutorTree(plan.get());
  if (!exec) {
    res.success = false;
    res.error_message = "failed to build executor for plan";
    return res;
  }

  const auto start_time = std::chrono::steady_clock::now();
  exec->init();
  res.success = true;
  res.is_query = true;

  for (const auto& col : exec->output_schema().columns) {
    res.column_names.push_back(col.name);
  }

  while (true) {
    auto opt = exec->next();
    if (!opt.has_value()) break;
    std::vector<std::string> row_strings;
    for (size_t i = 0; i < opt->size(); ++i) {
      row_strings.push_back(opt->GetValue(i).ToString());
    }
    res.rows.push_back(std::move(row_strings));
  }
  exec->close();

  const auto end_time = std::chrono::steady_clock::now();
  res.actual_time_ms =
      std::chrono::duration<double, std::milli>(end_time - start_time).count();
  res.rows_actual = res.rows.size();
  res.chosen_path = plan->access_path();
  res.estimated_cost = plan->estimated_cost();
  res.rows_estimated = plan->estimated_rows();
  res.alternatives = plan->alternatives();
  res.plan_text = plan->ToString();
  if (GetSessionFlag("constraint_aware") == "on") {
    res.rules_line = "Applied rules: ";
    if (plan->applied_rules().empty()) {
      res.rules_line += "(none)";
    } else {
      for (size_t i = 0; i < plan->applied_rules().size(); ++i) {
        if (i > 0) res.rules_line += ", ";
        res.rules_line += plan->applied_rules()[i];
      }
    }
  }
  res.has_plan_info = true;

  return res;
}

}  // namespace sthir
