#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "parser/ast.h"
#include "planner/plan_node.h"

namespace sthir {

// What the costs are computed over: pages from the table file size, rows from
// the analyzed statistics (or a fallback derived from page count and row width).
struct TableSummary {
  uint32_t pages = 0;
  double rows = 0.0;
};

TableSummary SummarizeTable(const TableSchema& schema);

// The chosen access path plus every candidate that lost.
struct AccessPathChoice {
  std::unique_ptr<PlanNode> plan;
  std::string name;
  double cost = 0.0;
  double rows = 0.0;
  std::vector<PlanAlternative> rejected;
};

// Enumerates SeqScan+Filter and, when an index can serve the predicate,
// IndexScan+Filter; returns the cheaper one (SeqScan wins ties).
//
// `allow_index` / `allow_seq` exist for measurement and debugging (the CLI's
// `optimizer --seq|--index` and the SET enable_* flags): disabling a path drops
// its candidate; if both are disabled SeqScan is used as the fallback.
// `prefer_index` forces the index candidate when one exists (rule R1: a unique
// equality is a seek, never a scan). It is ignored when no index can serve the
// predicate.
AccessPathChoice ChooseAccessPath(const TableSchema& schema, const Expr* where,
                                 bool allow_index = true, bool allow_seq = true,
                                 bool prefer_index = false);

// Bottom-up pass that stamps estimated rows and cost on every plan node.
//
// When `constraint_rules` is true (SET constraint_aware = on with constraints
// enforced) rule R4 replaces the conventional 10% join selectivity with the
// exact FK-join cardinality rows(child); `catalog` supplies the FK edges.
void AnnotatePlan(PlanNode* plan, const Catalog* catalog = nullptr,
                  bool constraint_rules = false,
                  std::vector<std::string>* fired_rules = nullptr);

// Estimated tuple width in bytes, used by the sort/aggregate cost estimates.
double EstimatedTupleBytes(const TableSchema& schema);

}  // namespace sthir
