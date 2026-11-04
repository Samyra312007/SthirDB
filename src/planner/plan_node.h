#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/catalog.h"
#include "index/index_scan.h"
#include "parser/ast.h"

namespace sthir {

enum class PlanNodeType {
  SEQ_SCAN,
  INDEX_SCAN,
  FILTER,
  PROJECT,
  AGGREGATE,
  SORT,
  NESTED_LOOP_JOIN,
  HASH_JOIN,
  EMPTY_RESULT,
  INSERT,
  UPDATE,
  DELETE_NODE
};

// A candidate access path that was costed and rejected by the optimizer.
struct PlanAlternative {
  std::string name;
  double cost = 0.0;
  double rows = 0.0;
};

class PlanNode {
 public:
  virtual ~PlanNode() = default;
  virtual PlanNodeType type() const = 0;
  virtual const TableSchema& output_schema() const = 0;
  virtual std::vector<const PlanNode*> children() const = 0;

  // One-line label for EXPLAIN, e.g. "Filter" or "IndexScan(Items using ...)".
  virtual std::string NodeLabel() const = 0;

  // Indented tree with `[rows=..., cost=...]` on every node that was costed.
  std::string ToString(int indent = 0) const {
    std::string s = IndentStr(indent) + NodeLabel();
    if (has_estimates_) {
      s += "  [rows=" + FormatMetric(estimated_rows_) + ", cost=" + FormatMetric(estimated_cost_) + "]";
    }
    s += "\n";
    for (const PlanNode* child : children()) {
      if (child != nullptr) s += child->ToString(indent + 1);
    }
    return s;
  }

  // Phase 4 optimizer metadata.
  void set_estimates(double rows, double cost) {
    estimated_rows_ = rows;
    estimated_cost_ = cost;
    has_estimates_ = true;
  }
  double estimated_rows() const { return estimated_rows_; }
  double estimated_cost() const { return estimated_cost_; }
  bool has_estimates() const { return has_estimates_; }

  // Access path chosen for this plan (e.g. "IndexScan(idx_price)+Filter") and
  // the candidates it beat. Set on the root by the planner.
  void set_access_path(std::string name) { access_path_ = std::move(name); }
  const std::string& access_path() const { return access_path_; }
  void set_alternatives(std::vector<PlanAlternative> alternatives) {
    alternatives_ = std::move(alternatives);
  }
  const std::vector<PlanAlternative>& alternatives() const { return alternatives_; }

  void add_applied_rule(std::string rule) {
    if (!rule.empty()) applied_rules_.push_back(std::move(rule));
  }
  const std::vector<std::string>& applied_rules() const { return applied_rules_; }

  // Constraint-derived hard cardinality override. Used by R1/R4 before
  // annotate_plan() and preserved through cost-based annotation.
  void set_forced_rows(double rows) {
    forced_rows_ = rows;
    estimated_rows_ = rows;
    has_estimates_ = true;
  }
  double forced_rows() const { return forced_rows_; }

 protected:
  static std::string IndentStr(int indent) { return std::string(indent * 2, ' '); }

  static std::string FormatMetric(double value) {
    char buf[32];
    if (value >= 0.0 && value < 1e15 && value == static_cast<double>(static_cast<long long>(value))) {
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    } else {
      std::snprintf(buf, sizeof(buf), "%.1f", value);
    }
    return buf;
  }

  double estimated_rows_ = 0.0;
  double estimated_cost_ = 0.0;
  bool has_estimates_ = false;
  double forced_rows_ = -1.0;
  std::string access_path_;
  std::vector<PlanAlternative> alternatives_;
  std::vector<std::string> applied_rules_;
};

class SeqScanPlan : public PlanNode {
 public:
  SeqScanPlan(std::string table_name, TableSchema schema)
      : table_name_(std::move(table_name)), schema_(std::move(schema)) {}

  PlanNodeType type() const override { return PlanNodeType::SEQ_SCAN; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::string& table_name() const { return table_name_; }

  std::vector<const PlanNode*> children() const override { return {}; }
  std::string NodeLabel() const override { return "SeqScan(" + table_name_ + ")"; }

 private:
  std::string table_name_;
  TableSchema schema_;
};

// Index access path: fetches the RIDs inside `range` from the B+tree described
// by `index`. The predicate is kept for estimation and EXPLAIN only — the
// Filter node above the scan owns result correctness.
class IndexScanPlan : public PlanNode {
 public:
  IndexScanPlan(std::string table_name, TableSchema schema, IndexMetadata index,
                size_t key_column, KeyRange range, std::unique_ptr<Expr> predicate)
      : table_name_(std::move(table_name)),
        schema_(std::move(schema)),
        index_(std::move(index)),
        key_column_(key_column),
        range_(range),
        predicate_(std::move(predicate)) {}

  PlanNodeType type() const override { return PlanNodeType::INDEX_SCAN; }
  const TableSchema& output_schema() const override { return schema_; }

  const std::string& table_name() const { return table_name_; }
  const IndexMetadata& index() const { return index_; }
  size_t key_column() const { return key_column_; }
  const KeyRange& range() const { return range_; }
  const Expr* predicate() const { return predicate_.get(); }

  std::vector<const PlanNode*> children() const override { return {}; }
  std::string NodeLabel() const override {
    std::string s = "IndexScan(" + table_name_ + " using " + index_.index_name + " on " +
                    index_.column_name + ", keys ";
    s += range_.has_lo ? "[" + std::to_string(range_.lo) : "(-inf";
    s += range_.has_hi ? ", " + std::to_string(range_.hi) + "]" : ", +inf)";
    return s + ")";
  }

 private:
  std::string table_name_;
  TableSchema schema_;
  IndexMetadata index_;
  size_t key_column_ = 0;
  KeyRange range_;
  std::unique_ptr<Expr> predicate_;
};

class FilterPlan : public PlanNode {
 public:
  FilterPlan(std::unique_ptr<Expr> predicate, std::unique_ptr<PlanNode> child)
      : predicate_(std::move(predicate)), child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::FILTER; }
  const TableSchema& output_schema() const override { return child_->output_schema(); }
  const Expr* predicate() const { return predicate_.get(); }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string NodeLabel() const override { return "Filter"; }

 private:
  std::unique_ptr<Expr> predicate_;
  std::unique_ptr<PlanNode> child_;
};

class ProjectPlan : public PlanNode {
 public:
  ProjectPlan(std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projections,
              TableSchema schema, std::unique_ptr<PlanNode> child)
      : projections_(std::move(projections)), schema_(std::move(schema)), child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::PROJECT; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::vector<std::pair<std::unique_ptr<Expr>, std::string>>& projections() const {
    return projections_;
  }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string NodeLabel() const override {
    std::string s = "Project(";
    for (size_t i = 0; i < projections_.size(); ++i) {
      s += projections_[i].second;
      if (i + 1 < projections_.size()) s += ", ";
    }
    return s + ")";
  }

 private:
  std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projections_;
  TableSchema schema_;
  std::unique_ptr<PlanNode> child_;
};

struct AggEntry {
  AggFunc func;
  bool is_star = false;
  std::unique_ptr<Expr> arg;
  std::string output_name;
};

class AggregatePlan : public PlanNode {
 public:
  AggregatePlan(std::vector<AggEntry> aggregates,
                std::vector<std::string> group_by_cols,
                TableSchema schema,
                std::unique_ptr<PlanNode> child)
      : aggregates_(std::move(aggregates)),
        group_by_cols_(std::move(group_by_cols)),
        schema_(std::move(schema)),
        child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::AGGREGATE; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::vector<AggEntry>& aggregates() const { return aggregates_; }
  const std::vector<std::string>& group_by_cols() const { return group_by_cols_; }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string NodeLabel() const override {
    std::string s = "Aggregate(group_by=[";
    for (size_t i = 0; i < group_by_cols_.size(); ++i) {
      s += group_by_cols_[i];
      if (i + 1 < group_by_cols_.size()) s += ", ";
    }
    return s + "])";
  }

 private:
  std::vector<AggEntry> aggregates_;
  std::vector<std::string> group_by_cols_;
  TableSchema schema_;
  std::unique_ptr<PlanNode> child_;
};

class SortPlan : public PlanNode {
 public:
  SortPlan(std::vector<OrderByItem> order_by_items, std::unique_ptr<PlanNode> child)
      : order_by_items_(std::move(order_by_items)), child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::SORT; }
  const TableSchema& output_schema() const override { return child_->output_schema(); }
  const std::vector<OrderByItem>& order_by_items() const { return order_by_items_; }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string NodeLabel() const override {
    std::string s = "Sort(";
    for (size_t i = 0; i < order_by_items_.size(); ++i) {
      s += order_by_items_[i].column_name + (order_by_items_[i].is_desc ? " DESC" : " ASC");
      if (i + 1 < order_by_items_.size()) s += ", ";
    }
    return s + ")";
  }

 private:
  std::vector<OrderByItem> order_by_items_;
  std::unique_ptr<PlanNode> child_;
};

class NestedLoopJoinPlan : public PlanNode {
 public:
  NestedLoopJoinPlan(std::unique_ptr<Expr> join_predicate,
                     std::unique_ptr<PlanNode> left,
                     std::unique_ptr<PlanNode> right,
                     TableSchema schema)
      : join_predicate_(std::move(join_predicate)),
        left_(std::move(left)),
        right_(std::move(right)),
        schema_(std::move(schema)) {}

  PlanNodeType type() const override { return PlanNodeType::NESTED_LOOP_JOIN; }
  const TableSchema& output_schema() const override { return schema_; }
  const Expr* join_predicate() const { return join_predicate_.get(); }
  const PlanNode* left() const { return left_.get(); }
  const PlanNode* right() const { return right_.get(); }
  std::unique_ptr<PlanNode>& left() { return left_; }
  std::unique_ptr<PlanNode>& right() { return right_; }

  std::vector<const PlanNode*> children() const override { return {left_.get(), right_.get()}; }
  std::string NodeLabel() const override { return "NestedLoopJoin"; }

 private:
  std::unique_ptr<Expr> join_predicate_;
  std::unique_ptr<PlanNode> left_;
  std::unique_ptr<PlanNode> right_;
  TableSchema schema_;
};

// Equi-join: hash one child and probe it with the other. `build_on_right`
// names the child that is materialized into the hash table; the planner picks
// the cheaper (smaller) side. `left_keys` / `right_keys` are the per-side key
// expressions of the equalities extracted from the join predicate — every
// candidate pair still has the full predicate re-checked, so the result set is
// identical to NestedLoopJoin for the same inputs (verified by tests).
//
// Added in Phase 7: the Yuan workload joins 100k transactions to 300k line
// items, which the quadratic NestedLoopJoin cannot execute in reasonable time.
class HashJoinPlan : public PlanNode {
 public:
  HashJoinPlan(std::unique_ptr<Expr> join_predicate,
               std::unique_ptr<PlanNode> left,
               std::unique_ptr<PlanNode> right,
               TableSchema schema,
               std::vector<std::unique_ptr<Expr>> left_keys,
               std::vector<std::unique_ptr<Expr>> right_keys,
               bool build_on_right)
      : join_predicate_(std::move(join_predicate)),
        left_(std::move(left)),
        right_(std::move(right)),
        schema_(std::move(schema)),
        left_keys_(std::move(left_keys)),
        right_keys_(std::move(right_keys)),
        build_on_right_(build_on_right) {}

  PlanNodeType type() const override { return PlanNodeType::HASH_JOIN; }
  const TableSchema& output_schema() const override { return schema_; }
  const Expr* join_predicate() const { return join_predicate_.get(); }
  const std::vector<std::unique_ptr<Expr>>& left_keys() const { return left_keys_; }
  const std::vector<std::unique_ptr<Expr>>& right_keys() const { return right_keys_; }
  bool build_on_right() const { return build_on_right_; }
  const PlanNode* left() const { return left_.get(); }
  const PlanNode* right() const { return right_.get(); }
  std::unique_ptr<PlanNode>& left() { return left_; }
  std::unique_ptr<PlanNode>& right() { return right_; }

  std::vector<const PlanNode*> children() const override { return {left_.get(), right_.get()}; }
  std::string NodeLabel() const override {
    return std::string("HashJoin(build=") + (build_on_right_ ? "right" : "left") + ")";
  }

 private:
  std::unique_ptr<Expr> join_predicate_;
  std::unique_ptr<PlanNode> left_;
  std::unique_ptr<PlanNode> right_;
  TableSchema schema_;
  std::vector<std::unique_ptr<Expr>> left_keys_;
  std::vector<std::unique_ptr<Expr>> right_keys_;
  bool build_on_right_ = true;
};

class EmptyResultPlan : public PlanNode {
 public:
  explicit EmptyResultPlan(TableSchema schema) : schema_(std::move(schema)) {}

  PlanNodeType type() const override { return PlanNodeType::EMPTY_RESULT; }
  const TableSchema& output_schema() const override { return schema_; }

  std::vector<const PlanNode*> children() const override { return {}; }
  std::string NodeLabel() const override { return "EmptyResult"; }

 private:
  TableSchema schema_;
};

class InsertPlan : public PlanNode {
 public:
  InsertPlan(std::string table_name,
             TableSchema schema,
             std::vector<std::string> target_columns,
             std::vector<std::vector<std::unique_ptr<Expr>>> values_matrix)
      : table_name_(std::move(table_name)),
        schema_(std::move(schema)),
        target_columns_(std::move(target_columns)),
        values_matrix_(std::move(values_matrix)) {}

  PlanNodeType type() const override { return PlanNodeType::INSERT; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::string& table_name() const { return table_name_; }
  const std::vector<std::string>& target_columns() const { return target_columns_; }
  const std::vector<std::vector<std::unique_ptr<Expr>>>& values_matrix() const {
    return values_matrix_;
  }

  std::vector<const PlanNode*> children() const override { return {}; }
  std::string NodeLabel() const override { return "InsertInto(" + table_name_ + ")"; }

 private:
  std::string table_name_;
  TableSchema schema_;
  std::vector<std::string> target_columns_;
  std::vector<std::vector<std::unique_ptr<Expr>>> values_matrix_;
};

class UpdatePlan : public PlanNode {
 public:
  UpdatePlan(std::string table_name,
             TableSchema schema,
             std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments,
             std::unique_ptr<PlanNode> child)
      : table_name_(std::move(table_name)),
        schema_(std::move(schema)),
        assignments_(std::move(assignments)),
        child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::UPDATE; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::string& table_name() const { return table_name_; }
  const std::vector<std::pair<std::string, std::unique_ptr<Expr>>>& assignments() const {
    return assignments_;
  }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string NodeLabel() const override { return "Update(" + table_name_ + ")"; }

 private:
  std::string table_name_;
  TableSchema schema_;
  std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments_;
  std::unique_ptr<PlanNode> child_;
};

class DeletePlan : public PlanNode {
 public:
  DeletePlan(std::string table_name, TableSchema schema, std::unique_ptr<PlanNode> child)
      : table_name_(std::move(table_name)), schema_(std::move(schema)), child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::DELETE_NODE; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::string& table_name() const { return table_name_; }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string NodeLabel() const override { return "Delete(" + table_name_ + ")"; }

 private:
  std::string table_name_;
  TableSchema schema_;
  std::unique_ptr<PlanNode> child_;
};

}  // namespace sthir
