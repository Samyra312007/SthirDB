#include "optimizer/constraint_rules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_set>

namespace sthir {

namespace {

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool SameName(const std::string& a, const std::string& b) {
  return ToLower(a) == ToLower(b);
}

// Reads `col OP literal` (or the mirrored `literal OP col`) into an interval on
// `col`. Returns false for anything else.
bool ComparisonInterval(const Expr* expr, const TableSchema& schema, std::string* column,
                        ColumnInterval* out) {
  if (expr == nullptr || expr->type() != ExprType::BINARY_OP) return false;
  const auto* bin = static_cast<const BinaryExpr*>(expr);
  const BinaryOp op = bin->op();
  if (op != BinaryOp::EQ && op != BinaryOp::NEQ && op != BinaryOp::LT &&
      op != BinaryOp::LTE && op != BinaryOp::GT && op != BinaryOp::GTE) {
    return false;
  }

  auto as_column = [&](const Expr* e) -> const ColumnRefExpr* {
    if (e == nullptr || e->type() != ExprType::COLUMN_REF) return nullptr;
    const auto* ref = static_cast<const ColumnRefExpr*>(e);
    if (!ref->table_name().empty() && !SameName(ref->table_name(), schema.table_name)) {
      return nullptr;  // qualified for another table
    }
    if (schema.FindColumnIndex(ref->column_name()) < 0) return nullptr;
    return ref;
  };
  auto as_number = [](const Expr* e, double* v) -> bool {
    if (e == nullptr || e->type() != ExprType::LITERAL) return false;
    const auto* lit = static_cast<const LiteralExpr*>(e);
    if (lit->lit_type() == LiteralExpr::LitType::INT) {
      *v = static_cast<double>(lit->int_val());
      return true;
    }
    if (lit->lit_type() == LiteralExpr::LitType::DECIMAL) {
      *v = lit->decimal_val();
      return true;
    }
    return false;
  };

  const ColumnRefExpr* col = as_column(bin->left());
  double value = 0.0;
  BinaryOp effective = op;
  if (col != nullptr && as_number(bin->right(), &value)) {
    // as written
  } else if ((col = as_column(bin->right())) != nullptr && as_number(bin->left(), &value)) {
    // Mirror the operator: `5 < StockLevel` is `StockLevel > 5`.
    switch (op) {
      case BinaryOp::LT: effective = BinaryOp::GT; break;
      case BinaryOp::LTE: effective = BinaryOp::GTE; break;
      case BinaryOp::GT: effective = BinaryOp::LT; break;
      case BinaryOp::GTE: effective = BinaryOp::LTE; break;
      default: break;
    }
  } else {
    return false;
  }

  const std::string& col_name = schema.columns[static_cast<size_t>(
      schema.FindColumnIndex(col->column_name()))].name;
  switch (effective) {
    case BinaryOp::EQ:
      out->has_lo = out->has_hi = true;
      out->lo = out->hi = value;
      out->lo_closed = out->hi_closed = true;
      *column = col_name;
      return true;
    case BinaryOp::GT:
      out->has_lo = true;
      out->lo = value;
      out->lo_closed = false;
      *column = col_name;
      return true;
    case BinaryOp::GTE:
      out->has_lo = true;
      out->lo = value;
      out->lo_closed = true;
      *column = col_name;
      return true;
    case BinaryOp::LT:
      out->has_hi = true;
      out->hi = value;
      out->hi_closed = false;
      *column = col_name;
      return true;
    case BinaryOp::LTE:
      out->has_hi = true;
      out->hi = value;
      out->hi_closed = true;
      *column = col_name;
      return true;
    default:
      return false;  // NEQ does not yield one interval
  }
}

ColumnInterval ToInterval(const Catalog::CheckRange& check) {
  ColumnInterval interval;
  interval.has_lo = check.has_lo;
  interval.lo = check.lo;
  interval.lo_closed = check.lo_closed;
  interval.has_hi = check.has_hi;
  interval.hi = check.hi;
  interval.hi_closed = check.hi_closed;
  return interval;
}

void MergeInto(ColumnInterval* into, const ColumnInterval& add) {
  if (add.has_lo) {
    if (!into->has_lo || add.lo > into->lo ||
        (add.lo == into->lo && !add.lo_closed)) {
      into->has_lo = true;
      into->lo = add.lo;
      into->lo_closed = add.lo_closed;
    }
  }
  if (add.has_hi) {
    if (!into->has_hi || add.hi < into->hi ||
        (add.hi == into->hi && !add.hi_closed)) {
      into->has_hi = true;
      into->hi = add.hi;
      into->hi_closed = add.hi_closed;
    }
  }
}

// Recursively ANDs the comparisons on one column into a single interval.
bool IntervalRec(const Expr* expr, const TableSchema& schema, std::string* column,
                 ColumnInterval* out) {
  if (expr == nullptr) return false;
  if (expr->type() == ExprType::BINARY_OP &&
      static_cast<const BinaryExpr*>(expr)->op() == BinaryOp::AND) {
    const auto* bin = static_cast<const BinaryExpr*>(expr);
    std::string left_col;
    std::string right_col;
    ColumnInterval left;
    ColumnInterval right;
    const bool has_left = IntervalRec(bin->left(), schema, &left_col, &left);
    const bool has_right = IntervalRec(bin->right(), schema, &right_col, &right);

    if (has_left && has_right) {
      if (!SameName(left_col, right_col)) return false;  // two columns: no fact
      *out = left;
      MergeInto(out, right);
      *column = left_col;
      return true;
    }
    if (has_left) {
      *out = left;
      *column = left_col;
      return true;
    }
    if (has_right) {
      *out = right;
      *column = right_col;
      return true;
    }
    return false;
  }
  std::string col;
  ColumnInterval interval;
  if (!ComparisonInterval(expr, schema, &col, &interval)) return false;
  *out = interval;
  *column = col;
  return true;
}

// Column qualifiers/names referenced by an expression.
void CollectRefs(const Expr* expr, std::vector<const ColumnRefExpr*>* out) {
  if (expr == nullptr) return;
  switch (expr->type()) {
    case ExprType::COLUMN_REF:
      out->push_back(static_cast<const ColumnRefExpr*>(expr));
      break;
    case ExprType::UNARY_OP:
      CollectRefs(static_cast<const UnaryExpr*>(expr)->expr(), out);
      break;
    case ExprType::BINARY_OP: {
      const auto* bin = static_cast<const BinaryExpr*>(expr);
      CollectRefs(bin->left(), out);
      CollectRefs(bin->right(), out);
      break;
    }
    case ExprType::IS_NULL_EXPR:
      CollectRefs(static_cast<const IsNullExpr*>(expr)->expr(), out);
      break;
    case ExprType::AGGREGATE:
      CollectRefs(static_cast<const AggregateExpr*>(expr)->arg(), out);
      break;
    default:
      break;
  }
}

const std::string* ScanTable(const PlanNode* node) {
  if (node == nullptr) return nullptr;
  if (node->type() == PlanNodeType::SEQ_SCAN) {
    return &static_cast<const SeqScanPlan*>(node)->table_name();
  }
  if (node->type() == PlanNodeType::INDEX_SCAN) {
    return &static_cast<const IndexScanPlan*>(node)->table_name();
  }
  return nullptr;
}

bool IsBareScanOf(const PlanNode* node, const std::string& table) {
  const std::string* name = ScanTable(node);
  return name != nullptr && SameName(*name, table);
}

// Does this subtree mention a column of `parent`? Stops at `skip` (the side
// being removed) and ignores the predicate of `ignore_predicate_at` (the join
// being eliminated, whose own ON clause necessarily names the parent).
bool MentionsTable(const PlanNode* node, const TableSchema& parent, const PlanNode* skip,
                   const PlanNode* ignore_predicate_at) {
  if (node == nullptr || node == skip) return false;

  std::vector<const ColumnRefExpr*> refs;
  if (node != ignore_predicate_at) {
    switch (node->type()) {
      case PlanNodeType::FILTER:
        CollectRefs(static_cast<const FilterPlan*>(node)->predicate(), &refs);
        break;
      case PlanNodeType::PROJECT: {
        for (const auto& proj : static_cast<const ProjectPlan*>(node)->projections()) {
          CollectRefs(proj.first.get(), &refs);
        }
        break;
      }
      case PlanNodeType::NESTED_LOOP_JOIN:
        CollectRefs(static_cast<const NestedLoopJoinPlan*>(node)->join_predicate(), &refs);
        break;
      case PlanNodeType::HASH_JOIN:
        CollectRefs(static_cast<const HashJoinPlan*>(node)->join_predicate(), &refs);
        break;
      case PlanNodeType::AGGREGATE: {
        const auto* agg = static_cast<const AggregatePlan*>(node);
        for (const auto& entry : agg->aggregates()) {
          CollectRefs(entry.arg.get(), &refs);
        }
        for (const auto& col : agg->group_by_cols()) {
          if (parent.HasColumn(col)) return true;
        }
        break;
      }
      case PlanNodeType::SORT: {
        for (const auto& item : static_cast<const SortPlan*>(node)->order_by_items()) {
          if (parent.HasColumn(item.column_name)) return true;
        }
        break;
      }
      default:
        break;
    }
  }

  // A qualified ref names the parent explicitly. An unqualified ref may be the
  // parent's column whenever the parent has a column of that name, so treat
  // that as a mention (conservative: never eliminate on a guess).
  for (const ColumnRefExpr* ref : refs) {
    if (ref == nullptr) continue;
    if (!ref->table_name().empty()) {
      if (SameName(ref->table_name(), parent.table_name)) return true;
    } else if (parent.HasColumn(ref->column_name())) {
      return true;
    }
  }

  for (const PlanNode* child : node->children()) {
    if (MentionsTable(child, parent, skip, ignore_predicate_at)) return true;
  }
  return false;
}

// The single equality in `join_predicate` that maps `child.col` to `parent.col`.
bool HasFkEquality(const Expr* join_predicate, const std::string& child_table,
                   const std::string& child_col, const std::string& parent_table,
                   const std::string& parent_col) {
  if (join_predicate == nullptr || join_predicate->type() != ExprType::BINARY_OP) return false;
  const auto* bin = static_cast<const BinaryExpr*>(join_predicate);
  if (bin->op() == BinaryOp::AND) {
    return HasFkEquality(bin->left(), child_table, child_col, parent_table, parent_col) ||
           HasFkEquality(bin->right(), child_table, child_col, parent_table, parent_col);
  }
  if (bin->op() != BinaryOp::EQ) return false;

  auto ref_is = [](const Expr* expr, const std::string& table, const std::string& col) {
    if (expr == nullptr || expr->type() != ExprType::COLUMN_REF) return false;
    const auto* ref = static_cast<const ColumnRefExpr*>(expr);
    return SameName(ref->table_name(), table) && SameName(ref->column_name(), col);
  };
  return (ref_is(bin->left(), child_table, child_col) &&
          ref_is(bin->right(), parent_table, parent_col)) ||
         (ref_is(bin->right(), child_table, child_col) &&
          ref_is(bin->left(), parent_table, parent_col));
}

// Finds the FK edge (from any table in `child_side`) that this join implements,
// naming the parent side as the bare scan of `parent_table`.
struct JoinFkMatch {
  bool found = false;
  ForeignKeyInfo edge;
  bool parent_is_left = false;
};

JoinFkMatch MatchFkJoin(const Catalog& catalog, const PlanNode* left, const PlanNode* right,
                        const Expr* join_predicate) {
  JoinFkMatch match;
  const std::string* left_table = ScanTable(left);
  const std::string* right_table = ScanTable(right);
  if (left_table == nullptr || right_table == nullptr) return match;

  // The parent side must be a bare scan; try left and right in turn.
  for (int side = 0; side < 2; ++side) {
    const std::string& parent = side == 0 ? *left_table : *right_table;
    const std::string& child = side == 0 ? *right_table : *left_table;
    for (const auto& edge : catalog.GetForeignKeyInfo()) {
      if (!SameName(edge.parent_table, parent)) continue;
      if (!SameName(edge.child_table, child)) continue;
      if (edge.parent_columns.size() != 1 || edge.child_columns.size() != 1) continue;

      const std::string& child_col = edge.child_columns[0];
      if (!HasFkEquality(join_predicate, child, child_col, parent, edge.parent_columns[0])) continue;

      match.found = true;
      match.edge = edge;
      match.parent_is_left = side == 0;
      return match;
    }
  }
  return match;
}

}  // namespace

bool ExtractColumnInterval(const Expr* predicate, const TableSchema& schema,
                           std::string* column, ColumnInterval* out) {
  if (predicate == nullptr || column == nullptr || out == nullptr) return false;
  ColumnInterval interval;
  std::string col;
  if (!IntervalRec(predicate, schema, &col, &interval)) return false;
  *out = interval;
  *column = col;
  return true;
}

bool IntervalIsEmpty(const ColumnInterval& a, const ColumnInterval& b) {
  if (a.has_hi && b.has_lo) {
    if (a.hi < b.lo) return true;
    if (a.hi == b.lo && !(a.hi_closed && b.lo_closed)) return true;
  }
  if (b.has_hi && a.has_lo) {
    if (b.hi < a.lo) return true;
    if (b.hi == a.lo && !(b.hi_closed && a.lo_closed)) return true;
  }
  return false;
}

bool IntervalCovers(const ColumnInterval& check, const ColumnInterval& predicate) {
  // A bound on the predicate that the CHECK does not have excludes values the
  // CHECK allows, so it cannot be a cover (`StockLevel < 0` never covers
  // `StockLevel >= 0`).
  if (!check.has_lo && predicate.has_lo) return false;
  if (!check.has_hi && predicate.has_hi) return false;

  if (check.has_lo) {
    if (!predicate.has_lo) return false;
    if (predicate.lo > check.lo) return false;
    if (predicate.lo == check.lo && !predicate.lo_closed && check.lo_closed) return false;
  }
  if (check.has_hi) {
    if (!predicate.has_hi) return false;
    if (predicate.hi < check.hi) return false;
    if (predicate.hi == check.hi && !predicate.hi_closed && check.hi_closed) return false;
  }
  return true;
}

PredicateFinding AnalyzeConstraintPredicate(const Catalog& catalog,
                                            const TableSchema& schema,
                                            const Expr* predicate) {
  PredicateFinding finding;
  if (predicate == nullptr) return finding;

  // R1 — equality on a declared-unique column set. Checked across every
  // conjunct so `ItemID = 7 AND ...` still triggers.
  std::vector<std::unique_ptr<Expr>> conjuncts;
  std::function<void(const Expr*)> split = [&](const Expr* expr) {
    if (expr == nullptr) return;
    if (expr->type() == ExprType::BINARY_OP &&
        static_cast<const BinaryExpr*>(expr)->op() == BinaryOp::AND) {
      split(static_cast<const BinaryExpr*>(expr)->left());
      split(static_cast<const BinaryExpr*>(expr)->right());
      return;
    }
    conjuncts.push_back(expr->Clone());
  };
  split(predicate);

  for (const auto& conjunct : conjuncts) {
    if (conjunct->type() != ExprType::BINARY_OP) continue;
    const auto* bin = static_cast<const BinaryExpr*>(conjunct.get());
    if (bin->op() != BinaryOp::EQ) continue;
    for (int side = 0; side < 2; ++side) {
      const Expr* col_side = side == 0 ? bin->left() : bin->right();
      const Expr* lit_side = side == 0 ? bin->right() : bin->left();
      if (col_side == nullptr || col_side->type() != ExprType::COLUMN_REF) continue;
      if (lit_side == nullptr || lit_side->type() != ExprType::LITERAL) continue;
      const auto* ref = static_cast<const ColumnRefExpr*>(col_side);
      if (!ref->table_name().empty() && !SameName(ref->table_name(), schema.table_name)) continue;
      const int col_idx = schema.FindColumnIndex(ref->column_name());
      if (col_idx < 0) continue;
      const std::string& col_name = schema.columns[static_cast<size_t>(col_idx)].name;
      if (!catalog.IsUnique(schema.table_name, {col_name})) continue;

      finding.rewrite = PredicateRewrite::UNIQUE_LOOKUP;
      finding.column = col_name;
      finding.detail = "R1 (unique lookup on " + schema.table_name + "." + col_name +
                       " -> 1 row)";
      return finding;
    }
  }

  // R2 / R2b: one conjunct at a time, so `ItemID = 5 AND StockLevel < 0` still
  // finds the contradiction on StockLevel. The conjunct must name a single
  // NOT NULL column with a declared CHECK range.
  for (const auto& conjunct : conjuncts) {
    std::string column;
    ColumnInterval predicate_interval;
    if (!ExtractColumnInterval(conjunct.get(), schema, &column, &predicate_interval)) continue;

    const int col_idx = schema.FindColumnIndex(column);
    if (col_idx < 0) continue;
    const Column& col = schema.columns[static_cast<size_t>(col_idx)];
    if (!col.not_null) continue;  // a CHECK passes NULL; the fact is weaker

    const auto check = catalog.GetCheckRange(schema.table_name, column);
    if (!check.has_value()) continue;
    const ColumnInterval check_interval = ToInterval(*check);

    if (IntervalIsEmpty(check_interval, predicate_interval)) {
      finding.rewrite = PredicateRewrite::CONTRADICTION;
      finding.column = column;
      finding.detail = "R2 (contradiction: predicate on " + schema.table_name + "." + column +
                       " is disjoint from its CHECK range)";
      return finding;
    }
    if (IntervalCovers(check_interval, predicate_interval)) {
      finding.rewrite = PredicateRewrite::REDUNDANT;
      finding.column = column;
      finding.detail = "R2b (redundant predicate on " + schema.table_name + "." + column +
                       " implied by its CHECK range)";
      return finding;
    }
  }
  return finding;
}

std::unique_ptr<Expr> StripImpliedConjuncts(const Catalog& catalog,
                                            const TableSchema& schema,
                                            std::unique_ptr<Expr> predicate,
                                            std::vector<std::string>* fired) {
  if (predicate == nullptr) return nullptr;

  std::vector<std::unique_ptr<Expr>> conjuncts;
  std::function<void(std::unique_ptr<Expr>)> split = [&](std::unique_ptr<Expr> expr) {
    if (expr == nullptr) return;
    if (expr->type() == ExprType::BINARY_OP &&
        static_cast<const BinaryExpr*>(expr.get())->op() == BinaryOp::AND) {
      auto* bin = static_cast<BinaryExpr*>(expr.get());
      split(std::move(bin->left()));
      split(std::move(bin->right()));
      return;
    }
    conjuncts.push_back(std::move(expr));
  };
  split(std::move(predicate));

  std::vector<std::unique_ptr<Expr>> kept;
  for (auto& conjunct : conjuncts) {
    const PredicateFinding finding =
        AnalyzeConstraintPredicate(catalog, schema, conjunct.get());
    if (finding.rewrite == PredicateRewrite::REDUNDANT) {
      if (fired != nullptr) fired->push_back(finding.detail);
      continue;
    }
    kept.push_back(std::move(conjunct));
  }

  if (kept.empty()) return nullptr;
  std::unique_ptr<Expr> out = std::move(kept[0]);
  for (size_t i = 1; i < kept.size(); ++i) {
    out = std::make_unique<BinaryExpr>(BinaryOp::AND, std::move(out), std::move(kept[i]));
  }
  return out;
}

namespace {

// `root` is the top of the whole plan: R3's "no parent column referenced
// anywhere above the join" test has to see ancestors, not just the join's own
// subtree.
std::unique_ptr<PlanNode> EliminateRec(std::unique_ptr<PlanNode> node, PlanNode* root,
                                       const Catalog& catalog,
                                       std::vector<std::string>* fired) {
  if (node == nullptr) return node;

  // Recurse first: a join deeper in the tree may disappear before we look at
  // this node.
  switch (node->type()) {
    case PlanNodeType::FILTER:
      static_cast<FilterPlan*>(node.get())->child() =
          EliminateRec(std::move(static_cast<FilterPlan*>(node.get())->child()), root, catalog,
                       fired);
      break;
    case PlanNodeType::PROJECT:
      static_cast<ProjectPlan*>(node.get())->child() =
          EliminateRec(std::move(static_cast<ProjectPlan*>(node.get())->child()), root, catalog,
                       fired);
      break;
    case PlanNodeType::AGGREGATE:
      static_cast<AggregatePlan*>(node.get())->child() =
          EliminateRec(std::move(static_cast<AggregatePlan*>(node.get())->child()), root, catalog,
                       fired);
      break;
    case PlanNodeType::SORT:
      static_cast<SortPlan*>(node.get())->child() =
          EliminateRec(std::move(static_cast<SortPlan*>(node.get())->child()), root, catalog,
                       fired);
      break;
    case PlanNodeType::NESTED_LOOP_JOIN: {
      auto* join = static_cast<NestedLoopJoinPlan*>(node.get());
      join->left() = EliminateRec(std::move(join->left()), root, catalog, fired);
      join->right() = EliminateRec(std::move(join->right()), root, catalog, fired);
      break;
    }
    case PlanNodeType::HASH_JOIN: {
      auto* join = static_cast<HashJoinPlan*>(node.get());
      join->left() = EliminateRec(std::move(join->left()), root, catalog, fired);
      join->right() = EliminateRec(std::move(join->right()), root, catalog, fired);
      break;
    }
    default:
      break;
  }

  if (node->type() != PlanNodeType::HASH_JOIN &&
      node->type() != PlanNodeType::NESTED_LOOP_JOIN) {
    return node;
  }

  const PlanNode* left = node->children()[0];
  const PlanNode* right = node->children()[1];
  const Expr* join_predicate =
      node->type() == PlanNodeType::HASH_JOIN
          ? static_cast<HashJoinPlan*>(node.get())->join_predicate()
          : static_cast<NestedLoopJoinPlan*>(node.get())->join_predicate();

  const JoinFkMatch match = MatchFkJoin(catalog, left, right, join_predicate);
  if (!match.found) return node;

  // Safety conditions for R3.
  if (!match.edge.child_not_null) return node;   // FK may be NULL -> no match
  if (!match.edge.parent_unique) return node;    // parent side is not a key
  const std::string& parent = match.edge.parent_table;
  const std::string& child = match.edge.child_table;

  const PlanNode* parent_side = match.parent_is_left ? left : right;

  // The parent side must be a plain scan: a filter there would change which
  // child rows survive, so elimination would be wrong.
  if (!IsBareScanOf(parent_side, parent)) return node;

  // No column of the parent may be referenced anywhere else in the tree.
  const TableSchema* parent_schema = catalog.GetTable(parent);
  if (parent_schema == nullptr) return node;
  if (MentionsTable(root, *parent_schema, parent_side, node.get())) return node;

  // The parent table must not be scanned twice.
  int parent_scans = 0;
  std::function<void(const PlanNode*)> count_scans = [&](const PlanNode* n) {
    if (n == nullptr) return;
    if (IsBareScanOf(n, parent)) ++parent_scans;
    for (const PlanNode* c : n->children()) count_scans(c);
  };
  count_scans(node.get());
  if (parent_scans != 1) return node;

  if (fired != nullptr) {
    fired->push_back("R3 (join elimination on FK " + child + "." + match.edge.child_columns[0] +
                     " -> " + parent + "." + match.edge.parent_columns[0] + ")");
  }

  // Keep the child side; it carries the rows the FK guarantees.
  std::unique_ptr<PlanNode> kept;
  const bool parent_left = match.parent_is_left;
  if (node->type() == PlanNodeType::HASH_JOIN) {
    auto* join = static_cast<HashJoinPlan*>(node.get());
    kept = parent_left ? std::move(join->right()) : std::move(join->left());
  } else {
    auto* join = static_cast<NestedLoopJoinPlan*>(node.get());
    kept = parent_left ? std::move(join->right()) : std::move(join->left());
  }
  return kept;
}

}  // namespace

std::unique_ptr<PlanNode> EliminateRedundantJoins(std::unique_ptr<PlanNode> plan,
                                                  const Catalog& catalog,
                                                  std::vector<std::string>* fired) {
  PlanNode* root = plan.get();
  return EliminateRec(std::move(plan), root, catalog, fired);
}

bool FkJoinCardinality(const Catalog& catalog, const PlanNode* left, const PlanNode* right,
                       const Expr* join_predicate, double* rows_estimated,
                       std::string* detail) {
  const JoinFkMatch match = MatchFkJoin(catalog, left, right, join_predicate);
  if (!match.found || !match.edge.parent_unique) return false;

  const PlanNode* child_side = match.parent_is_left ? right : left;
  if (rows_estimated != nullptr) *rows_estimated = child_side->estimated_rows();
  if (detail != nullptr) {
    *detail = "R4 (FK join cardinality: " + match.edge.child_table + " join " +
              match.edge.parent_table + " -> rows(child))";
  }
  return true;
}

}  // namespace sthir
