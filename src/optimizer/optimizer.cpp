#include "optimizer/optimizer.h"

#include <algorithm>
#include <filesystem>

#include "common/config.h"
#include "index/index_scan.h"
#include "optimizer/constraint_rules.h"
#include "optimizer/cost_model.h"
#include "optimizer/selectivity.h"

namespace sthir {

namespace {

uint32_t TablePages(const TableSchema& schema) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(schema.data_file_path, ec);
  if (ec) return 0;
  return static_cast<uint32_t>((size + kPageSize - 1) / kPageSize);
}

// Rows per page fallback when a table was never analyzed: assume the page is
// 90% full of fixed-width tuples.
double FallbackRows(const TableSummary& summary, const TableSchema& schema) {
  if (summary.pages == 0) return 0.0;
  const double usable = static_cast<double>(kPageSize) * 0.9;
  return static_cast<double>(summary.pages) * usable / EstimatedTupleBytes(schema);
}

// Rows the index range is expected to touch, using the statistics of the
// indexed column. Bounds are widened by one key unit, so an equality estimate
// can be one row generous — harmless because the Filter rechecks the predicate.
double IndexRangeRows(const TableSchema& schema, size_t column, const KeyRange& range,
                      double fallback_selectivity, double base_rows) {
  const ColumnStats* stats = schema.GetColumnStats(schema.columns[column].name);
  if (stats == nullptr || !stats->valid) {
    return static_cast<double>(EstimateRows(fallback_selectivity, static_cast<uint64_t>(base_rows)));
  }
  const double scale = static_cast<double>(IndexKeyScale(schema.columns[column].type));
  const double lo = range.has_lo ? static_cast<double>(range.lo) / scale : 0.0;
  const double hi = range.has_hi ? static_cast<double>(range.hi) / scale : 0.0;
  const double selectivity = RangeSelectivity(*stats, range.has_lo, lo, range.has_hi, hi);
  return static_cast<double>(EstimateRows(selectivity, static_cast<uint64_t>(base_rows)));
}

double GroupEstimate(const PlanNode* child, const std::vector<std::string>& group_by) {
  if (group_by.empty()) return 1.0;  // one-row aggregate
  double groups = 1.0;
  bool known = true;
  for (const auto& column_name : group_by) {
    const ColumnStats* stats = child->output_schema().GetColumnStats(column_name);
    if (stats == nullptr || !stats->valid || stats->ndv == 0) {
      known = false;
      break;
    }
    groups *= static_cast<double>(stats->ndv);
  }
  const double input_rows = child->estimated_rows();
  if (!known) return std::min(input_rows, 100.0);  // unanalyzed: conventional cap
  return std::min(groups, input_rows);
}

void AnnotateRec(PlanNode* node, const Catalog* catalog, bool constraint_rules,
                 std::vector<std::string>* fired_rules) {
  if (node == nullptr) return;

  switch (node->type()) {
    case PlanNodeType::SEQ_SCAN: {
      const TableSummary summary = SummarizeTable(node->output_schema());
      node->set_estimates(summary.rows, SeqScanCost(static_cast<double>(summary.pages)));
      break;
    }
    case PlanNodeType::INDEX_SCAN: {
      const auto* scan = static_cast<const IndexScanPlan*>(node);
      const TableSummary summary = SummarizeTable(scan->output_schema());
      const double selectivity = EstimateSelectivity(scan->predicate(), scan->output_schema());
      const double rows =
          static_cast<double>(EstimateRows(selectivity, static_cast<uint64_t>(summary.rows)));
      node->set_estimates(rows, IndexScanCost(static_cast<double>(scan->index().height), rows,
                                              summary.rows,
                                              static_cast<double>(summary.pages)));
      break;
    }
    case PlanNodeType::FILTER: {
      auto* filter = static_cast<FilterPlan*>(node);
      AnnotateRec(filter->child().get(), catalog, constraint_rules, fired_rules);
      const PlanNode* child = filter->child().get();
      // A filter above an index scan rechecks the predicate the range already
      // encoded, so it is estimated to pass everything the scan produced.
      // Residual conjuncts would make that optimistic (see report §5).
      double rows = child->estimated_rows();
      if (child->type() != PlanNodeType::INDEX_SCAN) {
        rows *= EstimateSelectivity(filter->predicate(), child->output_schema());
      }
      node->set_estimates(rows, FilterCost(child->estimated_cost(), child->estimated_rows()));
      break;
    }
    case PlanNodeType::PROJECT: {
      auto* project = static_cast<ProjectPlan*>(node);
      AnnotateRec(project->child().get(), catalog, constraint_rules, fired_rules);
      node->set_estimates(project->child().get()->estimated_rows(), project->child().get()->estimated_cost());
      break;
    }
    case PlanNodeType::SORT: {
      auto* sort = static_cast<SortPlan*>(node);
      AnnotateRec(sort->child().get(), catalog, constraint_rules, fired_rules);
      const double rows = sort->child().get()->estimated_rows();
      node->set_estimates(rows, sort->child().get()->estimated_cost() +
                                    SortCost(rows, EstimatedTupleBytes(node->output_schema())));
      break;
    }
    case PlanNodeType::AGGREGATE: {
      auto* aggregate = static_cast<AggregatePlan*>(node);
      AnnotateRec(aggregate->child().get(), catalog, constraint_rules, fired_rules);
      const double rows = GroupEstimate(aggregate->child().get(), aggregate->group_by_cols());
      node->set_estimates(rows, AggregateCost(aggregate->child().get()->estimated_cost(),
                                              aggregate->child().get()->estimated_rows()));
      break;
    }
    case PlanNodeType::NESTED_LOOP_JOIN: {
      auto* join = static_cast<NestedLoopJoinPlan*>(node);
      AnnotateRec(join->left().get(), catalog, constraint_rules, fired_rules);
      AnnotateRec(join->right().get(), catalog, constraint_rules, fired_rules);
      const double outer_rows = join->left().get()->estimated_rows();
      const double inner_rows = join->right().get()->estimated_rows();
      // No column statistics for join predicates yet: conventional 10% default.
      // R4 overrides it when the join is a declared FK child->parent edge.
      double rows = outer_rows * inner_rows * 0.1;
      if (constraint_rules && catalog != nullptr) {
        double fk_rows = 0.0;
        std::string detail;
        if (FkJoinCardinality(*catalog, join->left().get(), join->right().get(),
                              join->join_predicate(), &fk_rows, &detail)) {
          rows = fk_rows;
          if (fired_rules != nullptr) fired_rules->push_back(detail);
        }
      }
      const double cost = join->left().get()->estimated_cost() + join->right().get()->estimated_cost() +
                          NestedLoopJoinCost(join->left().get()->estimated_cost(), outer_rows,
                                             join->right().get()->estimated_cost());
      node->set_estimates(rows, cost);
      break;
    }
    case PlanNodeType::HASH_JOIN: {
      auto* join = static_cast<HashJoinPlan*>(node);
      AnnotateRec(join->left().get(), catalog, constraint_rules, fired_rules);
      AnnotateRec(join->right().get(), catalog, constraint_rules, fired_rules);
      const double outer_rows = join->left().get()->estimated_rows();
      const double inner_rows = join->right().get()->estimated_rows();
      // Same conventional 10% join selectivity as the nested-loop case. Each
      // input is read once (already priced by the children) and the hash
      // table lives in memory, so the join itself adds no page I/O.
      double rows = outer_rows * inner_rows * 0.1;
      if (constraint_rules && catalog != nullptr) {
        double fk_rows = 0.0;
        std::string detail;
        if (FkJoinCardinality(*catalog, join->left().get(), join->right().get(),
                              join->join_predicate(), &fk_rows, &detail)) {
          rows = fk_rows;
          if (fired_rules != nullptr) fired_rules->push_back(detail);
        }
      }
      const double cost = join->left().get()->estimated_cost() +
                          join->right().get()->estimated_cost();
      node->set_estimates(rows, cost);
      break;
    }
    case PlanNodeType::EMPTY_RESULT: {
      node->set_estimates(0.0, 0.0);
      break;
    }
    case PlanNodeType::INSERT: {
      node->set_estimates(1.0, 0.0);
      break;
    }
    case PlanNodeType::UPDATE:
    case PlanNodeType::DELETE_NODE: {
      PlanNode* child = node->type() == PlanNodeType::UPDATE
                            ? static_cast<UpdatePlan*>(node)->child().get()
                            : static_cast<DeletePlan*>(node)->child().get();
      AnnotateRec(child, catalog, constraint_rules, fired_rules);
      node->set_estimates(child->estimated_rows(), child->estimated_cost());
      break;
    }
  }

  if (node->forced_rows() >= 0.0) {
    node->set_estimates(node->forced_rows(), node->estimated_cost());
  }
}

}  // namespace

double EstimatedTupleBytes(const TableSchema& schema) {
  double bytes = 0.0;
  for (const auto& column : schema.columns) {
    switch (column.type) {
      case DataType::INT: bytes += 4.0; break;
      case DataType::DECIMAL: bytes += 8.0; break;
      case DataType::VARCHAR: bytes += 4.0 + static_cast<double>(column.length); break;
    }
  }
  return bytes > 0.0 ? bytes : 1.0;
}

TableSummary SummarizeTable(const TableSchema& schema) {
  TableSummary summary;
  summary.pages = TablePages(schema);

  double analyzed_rows = -1.0;
  for (const auto& stats : schema.column_stats) {
    if (!stats.valid) continue;
    const double rows = static_cast<double>(stats.row_count + stats.null_count);
    analyzed_rows = std::max(analyzed_rows, rows);
  }
  summary.rows = analyzed_rows >= 0.0 ? analyzed_rows : FallbackRows(summary, schema);
  return summary;
}

AccessPathChoice ChooseAccessPath(const TableSchema& schema, const Expr* where,
                                 bool allow_index, bool allow_seq, bool prefer_index) {
  const TableSummary table = SummarizeTable(schema);
  const double selectivity = where != nullptr ? EstimateSelectivity(where, schema) : 1.0;
  const double output_rows =
      static_cast<double>(EstimateRows(selectivity, static_cast<uint64_t>(table.rows)));

  // Candidate 1: SeqScan -> Filter.
  std::unique_ptr<PlanNode> seq_plan =
      std::make_unique<SeqScanPlan>(schema.table_name, schema);
  if (where != nullptr) {
    seq_plan = std::make_unique<FilterPlan>(where->Clone(), std::move(seq_plan));
  }
  const double seq_cost = SeqScanCost(static_cast<double>(table.pages));
  const std::string seq_name = where != nullptr ? "SeqScan+Filter" : "SeqScan";

  // Candidate 2: IndexScan -> Filter, only when an index can serve the predicate.
  std::unique_ptr<PlanNode> index_plan;
  std::string index_name;
  double index_cost = 0.0;
  double index_output_rows = output_rows;
  if (where != nullptr && allow_index) {
    for (const auto& index : schema.indexes) {
      const int column = schema.FindColumnIndex(index.column_name);
      if (column < 0) continue;
      KeyRange range;
      if (!ExtractKeyRange(where, schema, static_cast<size_t>(column), &range)) continue;
      if (range.IsEmpty()) continue;

      const double range_rows = IndexRangeRows(schema, static_cast<size_t>(column), range,
                                               selectivity, table.rows);
      auto scan = std::make_unique<IndexScanPlan>(schema.table_name, schema, index,
                                                  static_cast<size_t>(column), range,
                                                  where->Clone());
      scan->set_estimates(range_rows, 0.0);
      index_plan = std::make_unique<FilterPlan>(where->Clone(), std::move(scan));
      index_name = "IndexScan(" + index.index_name + ")+Filter";
      index_cost = IndexScanCost(static_cast<double>(index.height), range_rows, table.rows,
                                 static_cast<double>(table.pages));
      index_output_rows = output_rows;
      break;
    }
  }

  AccessPathChoice choice;
  choice.rows = output_rows;
  bool use_index = false;
  if (index_plan != nullptr) {
    if (!allow_seq || prefer_index) {
      use_index = true;  // forced by a measurement hook or by rule R1
    } else if (allow_index && index_cost < seq_cost) {
      use_index = true;
    }
  }
  if (use_index) {
    choice.plan = std::move(index_plan);
    choice.name = index_name;
    choice.cost = index_cost;
    choice.rejected.push_back({seq_name, seq_cost, index_output_rows});
  } else {
    choice.plan = std::move(seq_plan);
    choice.name = seq_name;
    choice.cost = seq_cost;
    if (index_plan != nullptr) {
      choice.rejected.push_back({index_name, index_cost, index_output_rows});
    }
  }
  return choice;
}

void AnnotatePlan(PlanNode* plan, const Catalog* catalog, bool constraint_rules,
                  std::vector<std::string>* fired_rules) {
  AnnotateRec(plan, catalog, constraint_rules, fired_rules);
}

}  // namespace sthir
