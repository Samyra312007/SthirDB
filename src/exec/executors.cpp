#include "exec/executors.h"
#include "exec/constraint_checker.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "recovery/recovery_manager.h"
#include "concurrency/lock_manager.h"

namespace sthir {

// -------------------------------------------------------------
// SeqScanExecutor
// -------------------------------------------------------------

SeqScanExecutor::SeqScanExecutor(std::string table_name,
                                 TableSchema schema,
                                 BufferPool* bp,
                                 DiskManager* dm)
    : table_name_(std::move(table_name)),
      schema_(std::move(schema)),
      bp_(bp),
      dm_(dm) {}

SeqScanExecutor::~SeqScanExecutor() {
  close();
}

void SeqScanExecutor::init() {
  close();
  cur_page_id_ = 0;
  cur_slot_ = 0;
  cur_page_ = nullptr;
  total_pages_ = dm_ ? dm_->num_pages() : 0;
}

std::optional<Tuple> SeqScanExecutor::next() {
  if (!dm_ || !bp_) return std::nullopt;

  while (cur_page_id_ < total_pages_) {
    if (cur_page_ == nullptr) {
      cur_page_ = bp_->fetch_page(cur_page_id_);
      if (cur_page_ == nullptr) {
        // Every frame pinned: ending the scan here would silently truncate the
        // result (a wrong answer). Surface it instead; ExecuteStatement turns
        // the exception into a failed ExecutionResult.
        throw std::runtime_error("buffer pool exhausted while scanning table '" +
                                 table_name_ + "'");
      }
      cur_slot_ = 0;
    }

    while (cur_slot_ < cur_page_->slot_count()) {
      uint16_t slot = cur_slot_++;
      if (!cur_page_->tuple_alive(slot)) continue;
      uint16_t len = cur_page_->tuple_length(slot);
      if (len == 0) continue;

      std::vector<char> buf(len);
      int bytes_read = cur_page_->ReadTuple(slot, buf.data());
      if (bytes_read <= 0) continue;

      Tuple t = Tuple::Deserialize(buf.data(), static_cast<uint16_t>(bytes_read), schema_);
      if (t.is_deleted()) continue;

      t.set_rid(RID{cur_page_id_, slot, true});
      return t;
    }

    // Done with this page
    bp_->unpin_page(cur_page_id_, false);
    cur_page_ = nullptr;
    ++cur_page_id_;
  }

  return std::nullopt;
}

void SeqScanExecutor::close() {
  if (cur_page_ != nullptr && bp_ != nullptr) {
    bp_->unpin_page(cur_page_id_, false);
    cur_page_ = nullptr;
  }
}

// -------------------------------------------------------------
// IndexScanExecutor
// -------------------------------------------------------------

IndexScanExecutor::IndexScanExecutor(std::string table_name,
                                     TableSchema schema,
                                     BufferPool* bp,
                                     DiskManager* dm,
                                     std::unique_ptr<BPlusTree> tree,
                                     KeyRange range)
    : table_name_(std::move(table_name)),
      schema_(std::move(schema)),
      bp_(bp),
      dm_(dm),
      tree_(std::move(tree)),
      range_(range) {}

void IndexScanExecutor::init() {
  rids_.clear();
  rid_idx_ = 0;
  if (!tree_ || range_.IsEmpty()) return;
  const int64_t lo = range_.has_lo ? range_.lo : std::numeric_limits<int64_t>::min();
  const int64_t hi = range_.has_hi ? range_.hi : std::numeric_limits<int64_t>::max();
  rids_ = tree_->RangeScan(lo, hi);
}

std::optional<Tuple> IndexScanExecutor::next() {
  if (!dm_ || !bp_) return std::nullopt;

  while (rid_idx_ < rids_.size()) {
    const IndexRid rid = rids_[rid_idx_++];
    if (rid.page_id >= dm_->num_pages()) continue;  // table replaced under the index

    Page* page = bp_->fetch_page(rid.page_id);
    if (page == nullptr) {
      throw std::runtime_error("buffer pool exhausted while scanning table '" +
                               table_name_ + "'");
    }
    if (rid.slot_id >= page->slot_count() || !page->tuple_alive(rid.slot_id)) {
      bp_->unpin_page(rid.page_id, false);
      continue;  // tombstoned after the index was built
    }

    std::vector<char> buf(page->tuple_length(rid.slot_id));
    const int bytes = buf.empty() ? 0 : page->ReadTuple(rid.slot_id, buf.data());
    if (bytes <= 0) {
      bp_->unpin_page(rid.page_id, false);
      continue;
    }
    Tuple tuple = Tuple::Deserialize(buf.data(), static_cast<uint16_t>(bytes), schema_);
    bp_->unpin_page(rid.page_id, false);
    if (tuple.is_deleted()) continue;

    tuple.set_rid(RID{rid.page_id, rid.slot_id, true});
    return tuple;
  }
  return std::nullopt;
}

void IndexScanExecutor::close() {}

// -------------------------------------------------------------
// FilterExecutor
// -------------------------------------------------------------

FilterExecutor::FilterExecutor(std::unique_ptr<Executor> child, std::unique_ptr<Expr> predicate)
    : child_(std::move(child)), predicate_(std::move(predicate)) {}

void FilterExecutor::init() {
  if (child_) child_->init();
}

std::optional<Tuple> FilterExecutor::next() {
  if (!child_) return std::nullopt;

  while (true) {
    auto opt = child_->next();
    if (!opt.has_value()) return std::nullopt;

    if (!predicate_) return opt;

    Tribool tb = EvaluatePredicate(predicate_.get(), *opt, child_->output_schema());
    if (tb == Tribool::True) {
      return opt;
    }
    // Unknown and False are filtered out in SQL WHERE
  }
}

void FilterExecutor::close() {
  if (child_) child_->close();
}

// -------------------------------------------------------------
// ProjectExecutor
// -------------------------------------------------------------

ProjectExecutor::ProjectExecutor(std::unique_ptr<Executor> child,
                                 TableSchema output_schema,
                                 std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projections)
    : child_(std::move(child)),
      output_schema_(std::move(output_schema)),
      projections_(std::move(projections)) {}

void ProjectExecutor::init() {
  if (child_) child_->init();
}

std::optional<Tuple> ProjectExecutor::next() {
  if (!child_) return std::nullopt;

  auto opt = child_->next();
  if (!opt.has_value()) return std::nullopt;

  std::vector<Value> projected_values;
  projected_values.reserve(projections_.size());

  for (const auto& p : projections_) {
    projected_values.push_back(EvaluateExpr(p.first.get(), *opt, child_->output_schema()));
  }

  Tuple t(std::move(projected_values));
  t.set_rid(opt->rid());
  return t;
}

void ProjectExecutor::close() {
  if (child_) child_->close();
}

// -------------------------------------------------------------
// AggregateExecutor
// -------------------------------------------------------------

AggregateExecutor::AggregateExecutor(std::unique_ptr<Executor> child,
                                     std::vector<AggEntry> aggregates,
                                     std::vector<std::string> group_by_cols,
                                     TableSchema output_schema)
    : child_(std::move(child)),
      aggregates_(std::move(aggregates)),
      group_by_cols_(std::move(group_by_cols)),
      output_schema_(std::move(output_schema)) {}

void AggregateExecutor::init() {
  groups_.clear();
  emit_idx_ = 0;
  if (!child_) return;

  child_->init();
  std::unordered_map<std::string, size_t> key_to_group;
  bool had_any_row = false;

  while (true) {
    auto opt = child_->next();
    if (!opt.has_value()) break;
    had_any_row = true;

    // Extract group key
    std::vector<Value> keys;
    std::string key_str;
    for (const auto& col_name : group_by_cols_) {
      int idx = child_->output_schema().FindColumnIndex(col_name);
      Value v = (idx >= 0) ? opt->GetValue(static_cast<size_t>(idx)) : Value::MakeNull();
      key_str += v.ToString() + "|";
      keys.push_back(v);
    }

    size_t g_idx;
    auto it = key_to_group.find(key_str);
    if (it == key_to_group.end()) {
      g_idx = groups_.size();
      key_to_group[key_str] = g_idx;

      GroupState gs;
      gs.group_keys = std::move(keys);
      gs.counts.assign(aggregates_.size(), 0);
      gs.sums.assign(aggregates_.size(), 0.0);
      gs.mins.assign(aggregates_.size(), Value::MakeNull());
      gs.maxs.assign(aggregates_.size(), Value::MakeNull());
      gs.non_null_counts.assign(aggregates_.size(), 0);
      groups_.push_back(std::move(gs));
    } else {
      g_idx = it->second;
    }

    GroupState& gs = groups_[g_idx];

    for (size_t i = 0; i < aggregates_.size(); ++i) {
      const auto& agg = aggregates_[i];
      if (agg.is_star) {
        ++gs.counts[i];
      } else {
        Value val = EvaluateExpr(agg.arg.get(), *opt, child_->output_schema());
        if (!val.is_null()) {
          ++gs.non_null_counts[i];
          if (agg.func == AggFunc::COUNT) {
            ++gs.counts[i];
          } else if (agg.func == AggFunc::SUM || agg.func == AggFunc::AVG) {
            gs.sums[i] += val.as_decimal();
          } else if (agg.func == AggFunc::MIN) {
            if (gs.mins[i].is_null() || val.Compare(BinaryOp::LT, gs.mins[i]) == Tribool::True) {
              gs.mins[i] = val;
            }
          } else if (agg.func == AggFunc::MAX) {
            if (gs.maxs[i].is_null() || val.Compare(BinaryOp::GT, gs.maxs[i]) == Tribool::True) {
              gs.maxs[i] = val;
            }
          }
        }
      }
    }
  }

  // Scalar aggregate over empty table produces 1 row with 0 count / null sums
  if (!had_any_row && group_by_cols_.empty()) {
    GroupState gs;
    gs.counts.assign(aggregates_.size(), 0);
    gs.sums.assign(aggregates_.size(), 0.0);
    gs.mins.assign(aggregates_.size(), Value::MakeNull());
    gs.maxs.assign(aggregates_.size(), Value::MakeNull());
    gs.non_null_counts.assign(aggregates_.size(), 0);
    groups_.push_back(std::move(gs));
  }

  child_->close();
}

std::optional<Tuple> AggregateExecutor::next() {
  if (emit_idx_ >= groups_.size()) return std::nullopt;

  const GroupState& gs = groups_[emit_idx_++];
  std::vector<Value> row_vals;
  row_vals.reserve(gs.group_keys.size() + aggregates_.size());

  // First group keys
  for (const auto& k : gs.group_keys) {
    row_vals.push_back(k);
  }

  // Next aggregates
  for (size_t i = 0; i < aggregates_.size(); ++i) {
    const auto& agg = aggregates_[i];
    switch (agg.func) {
      case AggFunc::COUNT:
        row_vals.push_back(Value::MakeInt(static_cast<int32_t>(gs.counts[i])));
        break;
      case AggFunc::SUM:
        if (gs.non_null_counts[i] == 0) {
          row_vals.push_back(Value::MakeNull(DataType::DECIMAL));
        } else {
          row_vals.push_back(Value::MakeDecimal(gs.sums[i]));
        }
        break;
      case AggFunc::AVG:
        if (gs.non_null_counts[i] == 0) {
          row_vals.push_back(Value::MakeNull(DataType::DECIMAL));
        } else {
          row_vals.push_back(Value::MakeDecimal(gs.sums[i] / static_cast<double>(gs.non_null_counts[i])));
        }
        break;
      case AggFunc::MIN:
        row_vals.push_back(gs.mins[i]);
        break;
      case AggFunc::MAX:
        row_vals.push_back(gs.maxs[i]);
        break;
    }
  }

  return Tuple(std::move(row_vals));
}

void AggregateExecutor::close() {
  groups_.clear();
  emit_idx_ = 0;
  if (child_) child_->close();
}

// -------------------------------------------------------------
// SortExecutor
// -------------------------------------------------------------

SortExecutor::SortExecutor(std::unique_ptr<Executor> child,
                           std::vector<OrderByItem> order_by_items,
                           TableSchema schema)
    : child_(std::move(child)),
      order_by_items_(std::move(order_by_items)),
      schema_(std::move(schema)) {}

void SortExecutor::init() {
  tuples_.clear();
  cur_idx_ = 0;
  if (!child_) return;

  child_->init();
  while (true) {
    auto opt = child_->next();
    if (!opt.has_value()) break;
    tuples_.push_back(std::move(*opt));
  }
  child_->close();

  std::sort(tuples_.begin(), tuples_.end(), [this](const Tuple& a, const Tuple& b) {
    for (const auto& item : order_by_items_) {
      int idx = schema_.FindColumnIndex(item.column_name);
      if (idx < 0) continue;
      const Value& va = a.GetValue(static_cast<size_t>(idx));
      const Value& vb = b.GetValue(static_cast<size_t>(idx));

      if (va.is_null() && vb.is_null()) continue;
      if (va.is_null()) return item.is_desc ? false : true;
      if (vb.is_null()) return item.is_desc ? true : false;

      if (va.Compare(BinaryOp::EQ, vb) == Tribool::True) continue;

      Tribool lt = va.Compare(BinaryOp::LT, vb);
      if (lt == Tribool::True) return !item.is_desc;
      return item.is_desc;
    }
    return false;
  });
}

std::optional<Tuple> SortExecutor::next() {
  if (cur_idx_ < tuples_.size()) {
    return tuples_[cur_idx_++];
  }
  return std::nullopt;
}

void SortExecutor::close() {
  tuples_.clear();
  cur_idx_ = 0;
  if (child_) child_->close();
}

// -------------------------------------------------------------
// NestedLoopJoinExecutor
// -------------------------------------------------------------

NestedLoopJoinExecutor::NestedLoopJoinExecutor(std::unique_ptr<Executor> left,
                                               std::unique_ptr<Executor> right,
                                               std::unique_ptr<Expr> join_predicate,
                                               TableSchema schema)
    : left_(std::move(left)),
      right_(std::move(right)),
      join_predicate_(std::move(join_predicate)),
      schema_(std::move(schema)) {}

void NestedLoopJoinExecutor::init() {
  if (left_) left_->init();
  right_tuples_.clear();
  right_idx_ = 0;

  if (right_) {
    right_->init();
    while (true) {
      auto opt = right_->next();
      if (!opt.has_value()) break;
      right_tuples_.push_back(std::move(*opt));
    }
    right_->close();
  }

  cur_left_ = left_ ? left_->next() : std::nullopt;
}

std::optional<Tuple> NestedLoopJoinExecutor::next() {
  while (cur_left_.has_value()) {
    while (right_idx_ < right_tuples_.size()) {
      const Tuple& r = right_tuples_[right_idx_++];

      std::vector<Value> joined_values;
      joined_values.reserve(cur_left_->size() + r.size());
      for (const auto& v : cur_left_->values()) joined_values.push_back(v);
      for (const auto& v : r.values()) joined_values.push_back(v);

      Tuple joined(std::move(joined_values));

      if (!join_predicate_) {
        return joined;
      }

      Tribool tb = EvaluatePredicate(join_predicate_.get(), joined, schema_);
      if (tb == Tribool::True) {
        return joined;
      }
    }

    // Advance outer
    cur_left_ = left_ ? left_->next() : std::nullopt;
    right_idx_ = 0;
  }

  return std::nullopt;
}

void NestedLoopJoinExecutor::close() {
  if (left_) left_->close();
  right_tuples_.clear();
  right_idx_ = 0;
  cur_left_ = std::nullopt;
}

// -------------------------------------------------------------
// HashJoinExecutor (Phase 7)
// -------------------------------------------------------------

HashJoinExecutor::HashJoinExecutor(std::unique_ptr<Executor> left,
                                   std::unique_ptr<Executor> right,
                                   std::unique_ptr<Expr> join_predicate,
                                   std::vector<std::unique_ptr<Expr>> left_keys,
                                   std::vector<std::unique_ptr<Expr>> right_keys,
                                   bool build_on_right,
                                   TableSchema schema)
    : left_(std::move(left)),
      right_(std::move(right)),
      join_predicate_(std::move(join_predicate)),
      left_keys_(std::move(left_keys)),
      right_keys_(std::move(right_keys)),
      build_on_right_(build_on_right),
      schema_(std::move(schema)) {}

std::vector<Value> HashJoinExecutor::MakeKey(
    const Tuple& tuple, const std::vector<std::unique_ptr<Expr>>& keys,
    const TableSchema& schema) {
  std::vector<Value> key;
  key.reserve(keys.size());
  for (const auto& expr : keys) {
    key.push_back(EvaluateExpr(expr.get(), tuple, schema));
  }
  return key;
}

size_t HashJoinExecutor::HashKey(const Key& key) {
  size_t h = 14695981039346656037ull;  // FNV-1a offset basis
  for (const auto& value : key) {
    h ^= value.Hash();
    h *= 1099511628211ull;
  }
  return h;
}

void HashJoinExecutor::BuildTable(Executor* side,
                                  const std::vector<std::unique_ptr<Expr>>& keys) {
  const TableSchema& side_schema = side->output_schema();
  side->init();
  while (true) {
    auto opt = side->next();
    if (!opt.has_value()) break;
    Key key = MakeKey(*opt, keys, side_schema);
    bool null_key = false;
    for (const auto& value : key) {
      if (value.is_null()) { null_key = true; break; }
    }
    if (null_key) continue;  // NULL never equals anything in a join (3VL)

    auto& bucket = table_[HashKey(key)];
    bool placed = false;
    for (auto& entry : bucket) {
      if (entry.first.size() != key.size()) continue;
      bool equal = true;
      for (size_t i = 0; i < key.size(); ++i) {
        if (!(entry.first[i] == key[i])) { equal = false; break; }
      }
      if (!equal) continue;
      entry.second.push_back(std::move(*opt));
      placed = true;
      break;
    }
    if (!placed) {
      std::vector<Tuple> rows;
      rows.push_back(std::move(*opt));
      bucket.push_back({std::move(key), std::move(rows)});
    }
    ++build_rows_;
  }
  side->close();
}

void HashJoinExecutor::init() {
  table_.clear();
  build_rows_ = 0;
  if (build_on_right_) {
    BuildTable(right_.get(), right_keys_);
    stream_left_ = true;
    left_->init();
  } else {
    BuildTable(left_.get(), left_keys_);
    stream_left_ = false;
    right_->init();
  }
}

std::optional<Tuple> HashJoinExecutor::next() {
  Executor* stream = stream_left_ ? left_.get() : right_.get();
  const auto& stream_keys = stream_left_ ? left_keys_ : right_keys_;
  const TableSchema& stream_schema = stream->output_schema();

  while (true) {
    // Resume the candidate list of the stream tuple we are still matching.
    if (cur_stream_.has_value()) {
      while (cur_matches_idx_ < cur_matches_.size()) {
        const Tuple& build_tuple = cur_matches_[cur_matches_idx_++];
        std::vector<Value> joined_values;
        joined_values.reserve(cur_stream_->size() + build_tuple.size());
        if (stream_left_) {
          for (const auto& v : cur_stream_->values()) joined_values.push_back(v);
          for (const auto& v : build_tuple.values()) joined_values.push_back(v);
        } else {
          for (const auto& v : build_tuple.values()) joined_values.push_back(v);
          for (const auto& v : cur_stream_->values()) joined_values.push_back(v);
        }
        Tuple joined(std::move(joined_values));
        // Re-check the full predicate: residual conjuncts and NULL semantics
        // stay exactly what NestedLoopJoin would evaluate.
        if (EvaluatePredicate(join_predicate_.get(), joined, schema_) == Tribool::True) {
          return joined;
        }
      }
      cur_stream_.reset();
      cur_matches_.clear();
      cur_matches_idx_ = 0;
    }

    auto opt = stream->next();
    if (!opt.has_value()) return std::nullopt;
    cur_stream_ = std::move(*opt);

    const Key key = MakeKey(*cur_stream_, stream_keys, stream_schema);
    bool null_key = false;
    for (const auto& value : key) {
      if (value.is_null()) { null_key = true; break; }
    }
    if (null_key) {
      cur_stream_.reset();
      continue;
    }

    auto bucket_it = table_.find(HashKey(key));
    if (bucket_it == table_.end()) continue;
    for (auto& entry : bucket_it->second) {
      if (entry.first.size() != key.size()) continue;
      bool equal = true;
      for (size_t i = 0; i < key.size(); ++i) {
        if (!(entry.first[i] == key[i])) { equal = false; break; }
      }
      if (!equal) continue;
      cur_matches_ = entry.second;  // candidates for this stream tuple
      cur_matches_idx_ = 0;
      break;
    }
  }
}

void HashJoinExecutor::close() {
  table_.clear();
  build_rows_ = 0;
  cur_stream_.reset();
  cur_matches_.clear();
  cur_matches_idx_ = 0;
  if (left_) left_->close();
  if (right_) right_->close();
}

// -------------------------------------------------------------
// DML operations
// -------------------------------------------------------------

size_t ExecuteInsert(const InsertPlan& plan, BufferPool* bp, DiskManager* dm, std::string* err,
                     RecoveryManager* rm, uint32_t txn_id, LockManager* lm,
                     ConstraintChecker* checker) {
  if (!bp || !dm) {
    if (err) *err = "storage uninitialized";
    return 0;
  }

  const TableSchema& schema = plan.output_schema();

  // Phase 1: materialize every tuple from the VALUES matrix.
  std::vector<Tuple> tuples;
  tuples.reserve(plan.values_matrix().size());
  for (const auto& val_row : plan.values_matrix()) {
    std::vector<Value> tuple_vals;
    tuple_vals.reserve(schema.columns.size());

    for (size_t col_i = 0; col_i < schema.columns.size(); ++col_i) {
      const auto& col = schema.columns[col_i];
      // Find index in target columns
      int target_idx = -1;
      for (size_t ti = 0; ti < plan.target_columns().size(); ++ti) {
        if (plan.target_columns()[ti] == col.name) {
          target_idx = static_cast<int>(ti);
          break;
        }
      }

      if (target_idx >= 0 && target_idx < static_cast<int>(val_row.size())) {
        Value v = EvaluateExpr(val_row[static_cast<size_t>(target_idx)].get(), Tuple{}, schema);
        tuple_vals.push_back(v.CastTo(col.type));
      } else {
        tuple_vals.push_back(Value::MakeNull(col.type));
      }
    }

    tuples.emplace_back(std::move(tuple_vals));
  }

  // Phase 2 (Phase 7): every constraint check runs before the first write, so
  // a rejected statement never touches the table — no undo is ever needed.
  if (checker != nullptr && !checker->CheckInsert(schema, tuples, err)) return 0;

  uint32_t cur_pid = 0;
  if (dm->num_pages() == 0) {
    cur_pid = dm->allocate_page();
  } else {
    cur_pid = dm->num_pages() - 1;
  }

  Page* page = bp->fetch_page(cur_pid);
  if (!page) {
    if (err) *err = "buffer pool exhausted";
    return 0;
  }
  if (page->free_space_ptr() < kPageHeaderSize) page->Reset(cur_pid);

  size_t inserted_count = 0;
  for (Tuple& tuple : tuples) {
    std::vector<char> bytes = tuple.Serialize(schema);

    int slot = page->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size()));
    if (slot < 0) {
      // Page is full, allocate next page
      bp->unpin_page(cur_pid, true);
      cur_pid = dm->allocate_page();
      page = bp->fetch_page(cur_pid);
      if (!page) {
        if (err) *err = "buffer pool exhausted";
        return inserted_count;
      }
      page->Reset(cur_pid);
      slot = page->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size()));
      if (slot < 0) {
        bp->unpin_page(cur_pid, false);
        if (err) *err = "tuple size exceeds page capacity";
        return inserted_count;
      }
    }

    RID rid{cur_pid, static_cast<uint16_t>(slot), true};
    if (lm && txn_id != 0) {
      if (!lm->AcquireExclusive(txn_id, plan.table_name(), rid)) {
        page->DeleteTuple(static_cast<uint16_t>(slot));
        bp->unpin_page(cur_pid, true);
        if (err) *err = "lock acquisition timeout";
        return inserted_count;
      }
    }

    if (rm && txn_id != 0) {
      uint64_t lsn = rm->LogUpdate(txn_id, schema.table_id, cur_pid,
                                   static_cast<uint16_t>(slot), 0, {}, bytes);
      page->set_page_lsn(lsn);
    }
    if (checker != nullptr) checker->RecordInsert(schema, tuple);
    ++inserted_count;
  }

  bp->unpin_page(cur_pid, true);
  if (!rm) {
    bp->flush_all();
  }
  return inserted_count;
}

size_t ExecuteUpdate(const UpdatePlan& plan, std::unique_ptr<Executor> child,
                     BufferPool* bp, DiskManager* dm, std::string* err,
                     RecoveryManager* rm, uint32_t txn_id, LockManager* lm,
                     ConstraintChecker* checker) {
  if (!child || !bp || !dm) {
    if (err) *err = "storage or child plan uninitialized";
    return 0;
  }

  const TableSchema& schema = plan.output_schema();
  child->init();

  std::vector<Tuple> matching_tuples;
  while (true) {
    auto opt = child->next();
    if (!opt.has_value()) break;
    matching_tuples.push_back(std::move(*opt));
  }
  child->close();

  if (lm && txn_id != 0) {
    std::vector<RID> rids_to_lock;
    for (const auto& tuple : matching_tuples) {
      if (tuple.rid().is_valid) {
        rids_to_lock.push_back(tuple.rid());
      }
    }
    if (!lm->AcquireExclusiveBatch(txn_id, plan.table_name(), std::move(rids_to_lock))) {
      if (err) *err = "lock acquisition timeout";
      return 0;
    }
  }

  size_t updated_count = 0;
  for (auto& tuple : matching_tuples) {
    if (!tuple.rid().is_valid) continue;

    Page* p = bp->fetch_page(tuple.rid().page_id);
    if (!p) continue;

    int old_len = p->tuple_length(tuple.rid().slot_id);
    if (old_len <= 0) {
      bp->unpin_page(tuple.rid().page_id, false);
      continue;
    }
    std::vector<char> old_bytes(old_len);
    p->ReadTuple(tuple.rid().slot_id, old_bytes.data());

    Tuple current_tuple = Tuple::Deserialize(old_bytes.data(), old_len, schema);
    current_tuple.set_rid(tuple.rid());

    // Apply assignments to create updated tuple using current under-lock state
    std::vector<Value> new_vals = current_tuple.values();
    for (const auto& assign : plan.assignments()) {
      int idx = schema.FindColumnIndex(assign.first);
      if (idx >= 0 && idx < static_cast<int>(new_vals.size())) {
        Value nv = EvaluateExpr(assign.second.get(), current_tuple, schema);
        new_vals[static_cast<size_t>(idx)] = nv.CastTo(schema.columns[idx].type);
      }
    }

    Tuple updated_tuple(std::move(new_vals));

    // Phase 7: NOT NULL / CHECK / changed-key uniqueness / changed-FK
    // existence on the resulting row. Nothing has been written for this row
    // yet, so an unpin(false) here leaves the page untouched; rows written
    // earlier in this statement are undone when the caller aborts.
    if (checker != nullptr &&
        !checker->CheckUpdate(schema, current_tuple, updated_tuple, err)) {
      bp->unpin_page(tuple.rid().page_id, false);
      return updated_count;
    }

    std::vector<char> bytes = updated_tuple.Serialize(schema);

    bool written = p->WriteTupleAt(tuple.rid().slot_id, bytes.data(), static_cast<uint16_t>(bytes.size()));
    if (written) {
      if (rm && txn_id != 0) {
        uint64_t lsn = rm->LogUpdate(txn_id, schema.table_id, tuple.rid().page_id,
                                     tuple.rid().slot_id, 0, std::move(old_bytes), bytes);
        p->set_page_lsn(lsn);
      }
      bp->unpin_page(tuple.rid().page_id, true);
    } else {
      // Slot grew beyond available space: delete and re-insert
      p->DeleteTuple(tuple.rid().slot_id);

      int new_slot = p->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size()));
      if (new_slot < 0) {
        bp->unpin_page(tuple.rid().page_id, true);
        // Append to last page
        uint32_t last_pid = dm->num_pages() - 1;
        Page* last_p = bp->fetch_page(last_pid);
        if (last_p && (new_slot = last_p->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size()))) >= 0) {
          if (lm && txn_id != 0) {
            lm->AcquireExclusive(txn_id, plan.table_name(), RID{last_pid, static_cast<uint16_t>(new_slot), true});
          }
          if (rm && txn_id != 0) {
            uint64_t lsn = rm->LogUpdate(txn_id, schema.table_id, last_pid,
                                         static_cast<uint16_t>(new_slot), 0,
                                         std::move(old_bytes), bytes);
            last_p->set_page_lsn(lsn);
          }
          bp->unpin_page(last_pid, true);
        } else {
          if (last_p) bp->unpin_page(last_pid, false);
          uint32_t new_pid = dm->allocate_page();
          Page* new_p = bp->fetch_page(new_pid);
          if (new_p) {
            new_p->Reset(new_pid);
            new_slot = new_p->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size()));
            if (new_slot >= 0) {
              if (lm && txn_id != 0) {
                lm->AcquireExclusive(txn_id, plan.table_name(), RID{new_pid, static_cast<uint16_t>(new_slot), true});
              }
              if (rm && txn_id != 0) {
                uint64_t lsn = rm->LogUpdate(txn_id, schema.table_id, new_pid,
                                             static_cast<uint16_t>(new_slot), 0,
                                             std::move(old_bytes), bytes);
                new_p->set_page_lsn(lsn);
              }
            }
            bp->unpin_page(new_pid, true);
          }
        }
      } else {
        if (rm && txn_id != 0) {
          uint64_t lsn = rm->LogUpdate(txn_id, schema.table_id, tuple.rid().page_id,
                                       static_cast<uint16_t>(new_slot), 0,
                                       std::move(old_bytes), bytes);
          p->set_page_lsn(lsn);
        }
        bp->unpin_page(tuple.rid().page_id, true);
      }
    }

    if (checker != nullptr) checker->RecordUpdate(schema, current_tuple, updated_tuple);
    ++updated_count;
  }

  if (!rm) {
    bp->flush_all();
  }
  return updated_count;
}

size_t ExecuteDelete(const DeletePlan& plan, std::unique_ptr<Executor> child,
                     BufferPool* bp, DiskManager* /*dm*/, std::string* err,
                     RecoveryManager* rm, uint32_t txn_id, LockManager* lm,
                     ConstraintChecker* checker) {
  if (!child || !bp) {
    if (err) *err = "storage or child plan uninitialized";
    return 0;
  }

  child->init();
  std::vector<RID> rids_to_delete;
  while (true) {
    auto opt = child->next();
    if (!opt.has_value()) break;
    if (opt->rid().is_valid) {
      rids_to_delete.push_back(opt->rid());
    }
  }
  child->close();

  // Phase 7: ON DELETE RESTRICT — read every victim and reject the statement
  // before the first write, so a restricted delete leaves the table untouched.
  // `victims[i]` mirrors rids_to_delete[i]; an empty tuple marks a row that
  // could not be read (the write loop skips it anyway).
  const TableSchema& schema = plan.output_schema();
  std::vector<Tuple> victims;
  if (checker != nullptr && !rids_to_delete.empty()) {
    victims.reserve(rids_to_delete.size());
    for (const auto& rid : rids_to_delete) {
      Page* p = bp->fetch_page(rid.page_id);
      if (!p) {
        victims.emplace_back();
        continue;
      }
      Tuple victim;
      const int len = p->tuple_length(rid.slot_id);
      if (len > 0) {
        std::vector<char> buf(static_cast<size_t>(len));
        p->ReadTuple(rid.slot_id, buf.data());
        victim = Tuple::Deserialize(buf.data(), static_cast<uint16_t>(len), schema);
        victim.set_rid(rid);
      }
      bp->unpin_page(rid.page_id, false);
      if (victim.size() > 0 && !checker->CheckDelete(schema, victim, err)) return 0;
      victims.push_back(std::move(victim));
    }
  }

  if (lm && txn_id != 0) {
    if (!lm->AcquireExclusiveBatch(txn_id, plan.table_name(), rids_to_delete)) {
      if (err) *err = "lock acquisition timeout";
      return 0;
    }
  }

  size_t deleted_count = 0;
  for (size_t i = 0; i < rids_to_delete.size(); ++i) {
    const RID& rid = rids_to_delete[i];
    Page* p = bp->fetch_page(rid.page_id);
    if (!p) continue;

    std::vector<char> old_bytes;
    int old_len = p->tuple_length(rid.slot_id);
    if (old_len > 0) {
      old_bytes.resize(old_len);
      p->ReadTuple(rid.slot_id, old_bytes.data());
    }

    if (p->DeleteTuple(rid.slot_id)) {
      if (rm && txn_id != 0) {
        uint64_t lsn = rm->LogUpdate(txn_id, schema.table_id, rid.page_id, rid.slot_id, 0,
                                     std::move(old_bytes), {});
        p->set_page_lsn(lsn);
      }
      ++deleted_count;
      bp->unpin_page(rid.page_id, true);

      if (checker != nullptr && i < victims.size() && victims[i].size() > 0) {
        checker->RecordDelete(schema, victims[i]);
        // ON DELETE CASCADE: remove the referencing child rows in the same
        // transaction (recursive; the engine guards the depth).
        std::string cascade_err;
        if (!checker->CascadeDelete(schema, victims[i], txn_id, &cascade_err)) {
          if (err) *err = cascade_err;
          return deleted_count;
        }
      }
    } else {
      bp->unpin_page(rid.page_id, false);
    }
  }

  if (!rm) {
    bp->flush_all();
  }
  return deleted_count;
}

}  // namespace sthir

