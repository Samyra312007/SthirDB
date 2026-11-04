#pragma once

#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "parser/ast.h"
#include "planner/plan_node.h"

namespace sthir {

// Phase 4B — constraint-aware optimizer (Novelty 1).
//
// The five rules use *declared* constraints (which are guaranteed facts) as
// rewrite licenses. They run only when both `constraint_aware = on` and
// `constraints_enabled = on`: on the unconstrained baseline the facts are no
// longer true, so no rule may fire.
//
//   R1  unique lookup      equality on a PRIMARY KEY / UNIQUE set -> 1 row,
//                          index seek when an index exists
//   R2  contradiction      query range disjoint from a CHECK range -> empty
//   R2b redundant predicate predicate implied by a CHECK range -> dropped
//   R3  join elimination   inner join to a parent whose columns are unused
//                          and whose FK child column is NOT NULL -> removed
//   R4  FK join cardinality child-join-parent has exactly rows(child) rows

// One column's value interval, with open/closed bounds so that strict
// comparisons stay exact (treating `> 0` as `>= 0` would let R2b drop a
// predicate that a zero-valued row fails).
struct ColumnInterval {
  bool has_lo = false;
  double lo = 0.0;
  bool lo_closed = true;
  bool has_hi = false;
  double hi = 0.0;
  bool hi_closed = true;
};

// Extracts the interval a conjunct imposes on a single column. Returns false
// when the predicate is not a conjunction of `column OP literal` comparisons
// on one column (OR, other columns, arithmetic: no fact, no rule).
bool ExtractColumnInterval(const Expr* predicate, const TableSchema& schema,
                           std::string* column, ColumnInterval* out);

// True when no value can satisfy both intervals.
bool IntervalIsEmpty(const ColumnInterval& a, const ColumnInterval& b);

// True when every value allowed by `check` satisfies `predicate`.
bool IntervalCovers(const ColumnInterval& check, const ColumnInterval& predicate);

// The predicate outcome R2/R2b/R1 are derived from.
enum class PredicateRewrite {
  NONE,          // no rule applies
  CONTRADICTION, // R2: predicate is unsatisfiable given the CHECK
  REDUNDANT,     // R2b: predicate is always true given the CHECK
  UNIQUE_LOOKUP  // R1: equality on a declared-unique column set
};

struct PredicateFinding {
  PredicateRewrite rewrite = PredicateRewrite::NONE;
  std::string column;
  std::string detail;  // human-readable reason for EXPLAIN
};

// Orders R2 before R2b before R1 for one pushed predicate.
PredicateFinding AnalyzeConstraintPredicate(const Catalog& catalog,
                                            const TableSchema& schema,
                                            const Expr* predicate);

// Drops every top-level conjunct that a CHECK makes tautological (R2b) and
// records one rule line per dropped conjunct. Returns null when every
// conjunct is implied (the caller then has no Filter node).
std::unique_ptr<Expr> StripImpliedConjuncts(const Catalog& catalog,
                                            const TableSchema& schema,
                                            std::unique_ptr<Expr> predicate,
                                            std::vector<std::string>* fired);

// R3: removes inner joins whose parent side is unused above the join and whose
// FK child column is NOT NULL, so the FK already guarantees a match. Walks the
// tree bottom-up and returns the (possibly new) root.
std::unique_ptr<PlanNode> EliminateRedundantJoins(std::unique_ptr<PlanNode> plan,
                                                  const Catalog& catalog,
                                                  std::vector<std::string>* fired);

// R4: when `left`/`right` are a child-join-parent pair on a declared FK whose
// parent columns are unique, the join yields exactly rows(child). Returns
// false when the pair is not such a join.
bool FkJoinCardinality(const Catalog& catalog, const PlanNode* left,
                       const PlanNode* right, const Expr* join_predicate,
                       double* rows_estimated, std::string* detail);

}  // namespace sthir
