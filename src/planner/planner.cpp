#include "planner/planner.h"

#include <algorithm>
#include <cctype>

#include "index/index_scan.h"
#include "optimizer/constraint_rules.h"
#include "optimizer/optimizer.h"

namespace sthir {

namespace {

std::unique_ptr<Expr> CloneExpr(const Expr* expr) {
  if (!expr) return nullptr;
  switch (expr->type()) {
    case ExprType::LITERAL: {
      const auto* lit = static_cast<const LiteralExpr*>(expr);
      switch (lit->lit_type()) {
        case LiteralExpr::LitType::INT: return LiteralExpr::MakeInt(lit->int_val());
        case LiteralExpr::LitType::DECIMAL: return LiteralExpr::MakeDecimal(lit->decimal_val());
        case LiteralExpr::LitType::STRING: return LiteralExpr::MakeString(lit->str_val());
        case LiteralExpr::LitType::NULL_VAL: return LiteralExpr::MakeNull();
      }
      break;
    }
    case ExprType::COLUMN_REF: {
      const auto* cr = static_cast<const ColumnRefExpr*>(expr);
      return std::make_unique<ColumnRefExpr>(cr->table_name(), cr->column_name());
    }
    case ExprType::UNARY_OP: {
      const auto* u = static_cast<const UnaryExpr*>(expr);
      return std::make_unique<UnaryExpr>(u->op(), CloneExpr(u->expr()));
    }
    case ExprType::BINARY_OP: {
      const auto* b = static_cast<const BinaryExpr*>(expr);
      return std::make_unique<BinaryExpr>(b->op(), CloneExpr(b->left()), CloneExpr(b->right()));
    }
    case ExprType::IS_NULL_EXPR: {
      const auto* in = static_cast<const IsNullExpr*>(expr);
      return std::make_unique<IsNullExpr>(CloneExpr(in->expr()), in->is_not());
    }
    case ExprType::AGGREGATE: {
      const auto* agg = static_cast<const AggregateExpr*>(expr);
      return std::make_unique<AggregateExpr>(agg->func(), agg->is_star(), CloneExpr(agg->arg()));
    }
  }
  return nullptr;
}

bool ContainsAggregate(const Expr* expr) {
  if (!expr) return false;
  if (expr->type() == ExprType::AGGREGATE) return true;
  if (expr->type() == ExprType::UNARY_OP) {
    return ContainsAggregate(static_cast<const UnaryExpr*>(expr)->expr());
  }
  if (expr->type() == ExprType::BINARY_OP) {
    const auto* b = static_cast<const BinaryExpr*>(expr);
    return ContainsAggregate(b->left()) || ContainsAggregate(b->right());
  }
  if (expr->type() == ExprType::IS_NULL_EXPR) {
    return ContainsAggregate(static_cast<const IsNullExpr*>(expr)->expr());
  }
  return false;
}

std::string GetExprName(const Expr* expr, const std::string& alias) {
  if (!alias.empty()) return alias;
  if (!expr) return "?";
  if (expr->type() == ExprType::COLUMN_REF) {
    return static_cast<const ColumnRefExpr*>(expr)->column_name();
  }
  if (expr->type() == ExprType::AGGREGATE) {
    const auto* agg = static_cast<const AggregateExpr*>(expr);
    std::string fn = "COUNT";
    if (agg->func() == AggFunc::SUM) fn = "SUM";
    else if (agg->func() == AggFunc::MIN) fn = "MIN";
    else if (agg->func() == AggFunc::MAX) fn = "MAX";
    else if (agg->func() == AggFunc::AVG) fn = "AVG";
    if (agg->is_star()) return fn + "(*)";
    return fn + "(" + GetExprName(agg->arg(), "") + ")";
  }
  return "expr";
}

DataType DeduceExprType(const Expr* expr, const TableSchema& schema) {
  if (!expr) return DataType::INT;
  if (expr->type() == ExprType::LITERAL) {
    const auto* lit = static_cast<const LiteralExpr*>(expr);
    if (lit->lit_type() == LiteralExpr::LitType::DECIMAL) return DataType::DECIMAL;
    if (lit->lit_type() == LiteralExpr::LitType::STRING) return DataType::VARCHAR;
    return DataType::INT;
  }
  if (expr->type() == ExprType::COLUMN_REF) {
    const auto* cr = static_cast<const ColumnRefExpr*>(expr);
    const Column* col = schema.GetColumn(cr->column_name());
    if (col) return col->type;
    return DataType::VARCHAR;
  }
  if (expr->type() == ExprType::AGGREGATE) {
    const auto* agg = static_cast<const AggregateExpr*>(expr);
    if (agg->func() == AggFunc::COUNT) return DataType::INT;
    if (agg->func() == AggFunc::AVG) return DataType::DECIMAL;
    if (agg->arg()) return DeduceExprType(agg->arg(), schema);
    return DataType::DECIMAL;
  }
  if (expr->type() == ExprType::BINARY_OP) {
    const auto* b = static_cast<const BinaryExpr*>(expr);
    if (b->op() == BinaryOp::AND || b->op() == BinaryOp::OR ||
        b->op() == BinaryOp::EQ || b->op() == BinaryOp::NEQ ||
        b->op() == BinaryOp::LT || b->op() == BinaryOp::LTE ||
        b->op() == BinaryOp::GT || b->op() == BinaryOp::GTE) {
      return DataType::INT;
    }
    DataType lt = DeduceExprType(b->left(), schema);
    DataType rt = DeduceExprType(b->right(), schema);
    if (lt == DataType::DECIMAL || rt == DataType::DECIMAL) return DataType::DECIMAL;
    return DataType::INT;
  }
  return DataType::INT;
}

bool ValidateExprColumns(const Expr* expr, const TableSchema& schema, std::string* err) {
  if (!expr) return true;
  if (expr->type() == ExprType::COLUMN_REF) {
    const auto* cr = static_cast<const ColumnRefExpr*>(expr);
    if (schema.FindColumnIndex(cr->column_name()) < 0) {
      if (err) *err = "unknown column '" + cr->column_name() + "'";
      return false;
    }
    return true;
  }
  if (expr->type() == ExprType::UNARY_OP) {
    return ValidateExprColumns(static_cast<const UnaryExpr*>(expr)->expr(), schema, err);
  }
  if (expr->type() == ExprType::BINARY_OP) {
    const auto* b = static_cast<const BinaryExpr*>(expr);
    return ValidateExprColumns(b->left(), schema, err) &&
           ValidateExprColumns(b->right(), schema, err);
  }
  if (expr->type() == ExprType::IS_NULL_EXPR) {
    return ValidateExprColumns(static_cast<const IsNullExpr*>(expr)->expr(), schema, err);
  }
  if (expr->type() == ExprType::AGGREGATE) {
    const auto* agg = static_cast<const AggregateExpr*>(expr);
    if (!agg->is_star() && agg->arg()) {
      return ValidateExprColumns(agg->arg(), schema, err);
    }
    return true;
  }
  return true;
}

// One FROM/JOIN input: its schema and the WHERE conjuncts that were pushed
// onto it (Phase 7 pushdown — lets a join query still choose an index path).
struct JoinInput {
  const TableSchema* schema = nullptr;
  std::unique_ptr<Expr> pushed;
};

// Splits a predicate into its top-level AND conjuncts (cloned).
void SplitConjuncts(const Expr* expr, std::vector<std::unique_ptr<Expr>>* out) {
  if (!expr) return;
  if (expr->type() == ExprType::BINARY_OP &&
      static_cast<const BinaryExpr*>(expr)->op() == BinaryOp::AND) {
    const auto* b = static_cast<const BinaryExpr*>(expr);
    SplitConjuncts(b->left(), out);
    SplitConjuncts(b->right(), out);
    return;
  }
  out->push_back(expr->Clone());
}

void CollectColumnRefs(const Expr* expr, std::vector<const ColumnRefExpr*>* out) {
  if (!expr) return;
  switch (expr->type()) {
    case ExprType::COLUMN_REF:
      out->push_back(static_cast<const ColumnRefExpr*>(expr));
      break;
    case ExprType::UNARY_OP:
      CollectColumnRefs(static_cast<const UnaryExpr*>(expr)->expr(), out);
      break;
    case ExprType::BINARY_OP: {
      const auto* b = static_cast<const BinaryExpr*>(expr);
      CollectColumnRefs(b->left(), out);
      CollectColumnRefs(b->right(), out);
      break;
    }
    case ExprType::IS_NULL_EXPR:
      CollectColumnRefs(static_cast<const IsNullExpr*>(expr)->expr(), out);
      break;
    default:
      break;
  }
}

bool EqualIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Index of the single input a WHERE conjunct belongs to, or -1 when the
// conjunct cannot be pushed below the joins (it spans several inputs, names
// an unknown qualifier, or carries no columns at all). Only single-table
// conjuncts are pushed; everything else stays above the join as before.
int FindOwningInput(const Expr& pred, const std::vector<JoinInput>& inputs,
                    const std::vector<std::string>& input_names) {
  std::vector<const ColumnRefExpr*> refs;
  CollectColumnRefs(&pred, &refs);
  if (refs.empty()) return -1;

  int owner = -1;
  for (const ColumnRefExpr* ref : refs) {
    int match = -1;
    if (!ref->table_name().empty()) {
      for (size_t i = 0; i < inputs.size(); ++i) {
        if (EqualIgnoreCase(ref->table_name(), input_names[i]) &&
            inputs[i].schema->HasColumn(ref->column_name())) {
          match = static_cast<int>(i);
          break;
        }
      }
      if (match < 0) return -1;  // unknown qualifier: leave it alone
    } else {
      for (size_t i = 0; i < inputs.size(); ++i) {
        if (!inputs[i].schema->HasColumn(ref->column_name())) continue;
        if (match >= 0) return -1;  // ambiguous unqualified column
        match = static_cast<int>(i);
      }
      if (match < 0) return -1;
    }
    if (owner < 0) owner = match;
    else if (owner != match) return -1;  // spans two inputs
  }
  return owner;
}

// Merges the right input's columns into the accumulated join schema,
// prefixing duplicate column names with the right table's name.
TableSchema MergeJoinSchemas(const TableSchema& left, const TableSchema& right) {
  TableSchema merged;
  merged.table_name = left.table_name + "_" + right.table_name;
  for (const auto& c : left.columns) merged.columns.push_back(c);
  for (const auto& c : right.columns) {
    Column col = c;
    if (merged.HasColumn(col.name)) col.name = right.table_name + "." + col.name;
    merged.columns.push_back(col);
  }
  return merged;
}

// Collects the usable hash keys of one JOIN's ON predicate. A conjunct
// `a = b` is usable when one side belongs to the left input chain (owner
// index < join_index) and the other side is the input being joined (owner
// index == join_index); the pair is then normalized to (left, right).
// Everything else is left for NestedLoopJoin (it is re-checked at runtime).
size_t ExtractEquiJoinKeys(const Expr& on, size_t join_index,
                           const std::vector<JoinInput>& inputs,
                           const std::vector<std::string>& input_names,
                           std::vector<std::unique_ptr<Expr>>* left_keys,
                           std::vector<std::unique_ptr<Expr>>* right_keys) {
  std::vector<std::unique_ptr<Expr>> conjuncts;
  SplitConjuncts(&on, &conjuncts);
  for (auto& conjunct : conjuncts) {
    if (conjunct->type() != ExprType::BINARY_OP) continue;
    auto* binary = static_cast<BinaryExpr*>(conjunct.get());
    if (binary->op() != BinaryOp::EQ) continue;
    const int left_owner = FindOwningInput(*binary->left(), inputs, input_names);
    const int right_owner = FindOwningInput(*binary->right(), inputs, input_names);
    if (left_owner < 0 || right_owner < 0) continue;
    const int join_idx = static_cast<int>(join_index);
    if (left_owner < join_idx && right_owner == join_idx) {
      left_keys->push_back(binary->left()->Clone());
      right_keys->push_back(binary->right()->Clone());
    } else if (right_owner < join_idx && left_owner == join_idx) {
      left_keys->push_back(binary->right()->Clone());
      right_keys->push_back(binary->left()->Clone());
    }
  }
  return left_keys->size();
}

}  // namespace

std::unique_ptr<PlanNode> Planner::CreatePlan(const SQLStatement& stmt, std::string* err) {
  switch (stmt.type()) {
    case StmtType::SELECT:
      return PlanSelect(static_cast<const SelectStmt&>(stmt), err);
    case StmtType::INSERT:
      return PlanInsert(static_cast<const InsertStmt&>(stmt), err);
    case StmtType::UPDATE:
      return PlanUpdate(static_cast<const UpdateStmt&>(stmt), err);
    case StmtType::DELETE:
      return PlanDelete(static_cast<const DeleteStmt&>(stmt), err);
    default:
      if (err) *err = "statement type does not produce an execution plan";
      return nullptr;
  }
}

std::unique_ptr<PlanNode> Planner::PlanSelect(const SelectStmt& stmt, std::string* err) {
  const TableSchema* from_schema = catalog_.GetTable(stmt.from_table);
  if (!from_schema) {
    if (err) *err = "table '" + stmt.from_table + "' does not exist";
    return nullptr;
  }

  // Inputs: the FROM table plus one entry per JOIN clause.
  std::vector<JoinInput> inputs;
  std::vector<std::string> input_names;
  inputs.push_back({from_schema, nullptr});
  input_names.push_back(stmt.from_table);
  for (const auto& clause : stmt.joins) {
    const TableSchema* join_schema = catalog_.GetTable(clause.table_name);
    if (!join_schema) {
      if (err) *err = "joined table '" + clause.table_name + "' does not exist";
      return nullptr;
    }
    inputs.push_back({join_schema, nullptr});
    input_names.push_back(clause.table_name);
  }

  // Predicate pushdown: every WHERE conjunct that belongs to exactly one input
  // moves below the joins, so that input gets its own access-path choice.
  std::vector<std::unique_ptr<Expr>> residual_where;
  if (stmt.where_clause) {
    std::vector<std::unique_ptr<Expr>> conjuncts;
    SplitConjuncts(stmt.where_clause.get(), &conjuncts);
    for (auto& conjunct : conjuncts) {
      const int owner = FindOwningInput(*conjunct, inputs, input_names);
      if (owner < 0) {
        residual_where.push_back(std::move(conjunct));
        continue;
      }
      if (!ValidateExprColumns(conjunct.get(), *inputs[owner].schema, err)) {
        return nullptr;
      }
      if (!inputs[owner].pushed) {
        inputs[owner].pushed = std::move(conjunct);
      } else {
        inputs[owner].pushed = std::make_unique<BinaryExpr>(
            BinaryOp::AND, std::move(inputs[owner].pushed), std::move(conjunct));
      }
    }
  }

  // Merge schemas left to right, validating every ON clause as its join is
  // added (the ON predicate may reference anything seen so far).
  TableSchema merged_schema = *inputs[0].schema;
  for (size_t i = 1; i < inputs.size(); ++i) {
    TableSchema next = MergeJoinSchemas(merged_schema, *inputs[i].schema);
    if (stmt.joins[i - 1].on &&
        !ValidateExprColumns(stmt.joins[i - 1].on.get(), next, err)) {
      return nullptr;
    }
    merged_schema = std::move(next);
  }

  // Validate the SELECT list and any WHERE conjunct that could not be pushed.
  if (!stmt.is_select_all) {
    for (const auto& item : stmt.select_list) {
      if (!ValidateExprColumns(item.expr.get(), merged_schema, err)) {
        return nullptr;
      }
    }
  }
  for (const auto& conjunct : residual_where) {
    if (!ValidateExprColumns(conjunct.get(), merged_schema, err)) {
      return nullptr;
    }
  }

  // Phase 4B: constraint-aware rewrite of the pushed predicates, run before
  // any plan is costed so the access-path choice sees the rewritten predicate.
  // Rules fire only when both flags are on (see constraint_rules.h).
  const bool rules_active = constraint_aware_ && constraints_enforced_;
  std::vector<std::string> fired;
  std::vector<bool> input_empty(inputs.size(), false);
  std::vector<bool> unique_lookup(inputs.size(), false);
  if (rules_active) {
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (!inputs[i].pushed) continue;
      const TableSchema& input_schema = *inputs[i].schema;
      // R2b drops conjuncts the CHECK makes tautological.
      inputs[i].pushed = StripImpliedConjuncts(catalog_, input_schema,
                                               std::move(inputs[i].pushed), &fired);
      if (!inputs[i].pushed) continue;
      const PredicateFinding finding =
          AnalyzeConstraintPredicate(catalog_, input_schema, inputs[i].pushed.get());
      if (finding.rewrite == PredicateRewrite::CONTRADICTION) {
        input_empty[i] = true;
        fired.push_back(finding.detail);
      } else if (finding.rewrite == PredicateRewrite::UNIQUE_LOOKUP) {
        unique_lookup[i] = true;
        fired.push_back(finding.detail);
      }
    }
  }

  // Build the input plans (each with its pushed filter and access-path choice)
  // and chain one join per JOIN clause.
  std::unique_ptr<PlanNode> current_plan;
  std::string chosen_path;
  std::vector<PlanAlternative> rejected_paths;
  if (input_empty[0]) {
    current_plan = std::make_unique<EmptyResultPlan>(*inputs[0].schema);
    chosen_path = "EmptyResult (0 pages)";
  } else {
    AccessPathChoice first =
        ChooseAccessPath(*inputs[0].schema, inputs[0].pushed.get(),
                         allow_index_scan_, allow_seq_scan_, unique_lookup[0]);
    current_plan = std::move(first.plan);
    chosen_path = first.name;
    rejected_paths = first.rejected;
  }
  if (unique_lookup[0] && !input_empty[0]) current_plan->set_forced_rows(1.0);
  TableSchema current_schema = *inputs[0].schema;

  for (size_t i = 1; i < inputs.size(); ++i) {
    std::unique_ptr<PlanNode> right_plan;
    if (input_empty[i]) {
      right_plan = std::make_unique<EmptyResultPlan>(*inputs[i].schema);
    } else {
      AccessPathChoice right =
          ChooseAccessPath(*inputs[i].schema, inputs[i].pushed.get(),
                           allow_index_scan_, allow_seq_scan_, unique_lookup[i]);
      right_plan = std::move(right.plan);
      if (unique_lookup[i]) right_plan->set_forced_rows(1.0);
    }
    TableSchema join_schema = MergeJoinSchemas(current_schema, *inputs[i].schema);
    const JoinClause& clause = stmt.joins[i - 1];

    // Equi-join predicates become a HashJoin (Phase 7: the Yuan workload
    // joins 100k transactions to 300k line items, which nested loops cannot
    // finish in reasonable time). The smaller estimated side is built.
    std::vector<std::unique_ptr<Expr>> left_keys;
    std::vector<std::unique_ptr<Expr>> right_keys;
    const size_t key_count =
        ExtractEquiJoinKeys(*clause.on, i, inputs, input_names, &left_keys, &right_keys);
    if (key_count > 0) {
      AnnotatePlan(current_plan.get(), rules_active ? &catalog_ : nullptr, rules_active);
      AnnotatePlan(right_plan.get(), rules_active ? &catalog_ : nullptr, rules_active);
      const bool build_on_right =
          right_plan->estimated_rows() <= current_plan->estimated_rows();
      current_plan = std::make_unique<HashJoinPlan>(
          CloneExpr(clause.on.get()),
          std::move(current_plan),
          std::move(right_plan),
          join_schema,
          std::move(left_keys),
          std::move(right_keys),
          build_on_right);
    } else {
      current_plan = std::make_unique<NestedLoopJoinPlan>(
          CloneExpr(clause.on.get()),
          std::move(current_plan),
          std::move(right_plan),
          join_schema);
    }
    current_schema = std::move(join_schema);
  }

  // Conjuncts that span several inputs stay above the join chain.
  if (!residual_where.empty()) {
    std::unique_ptr<Expr> pred = std::move(residual_where[0]);
    for (size_t i = 1; i < residual_where.size(); ++i) {
      pred = std::make_unique<BinaryExpr>(BinaryOp::AND, std::move(pred),
                                          std::move(residual_where[i]));
    }
    current_plan = std::make_unique<FilterPlan>(std::move(pred), std::move(current_plan));
  }

  // Check if aggregation is needed (either explicit GROUP BY or aggregate functions in select list)
  bool has_aggregates = !stmt.group_by_columns.empty();
  if (!has_aggregates && !stmt.is_select_all) {
    for (const auto& item : stmt.select_list) {
      if (ContainsAggregate(item.expr.get())) {
        has_aggregates = true;
        break;
      }
    }
  }

  if (has_aggregates) {
    std::vector<AggEntry> agg_entries;
    TableSchema agg_schema;
    agg_schema.table_name = current_schema.table_name;

    // First group-by columns in schema
    for (const auto& gb_col : stmt.group_by_columns) {
      const Column* c = current_schema.GetColumn(gb_col);
      if (!c) {
        if (err) *err = "group by column '" + gb_col + "' not found";
        return nullptr;
      }
      agg_schema.columns.push_back(*c);
    }

    // Next aggregate functions
    for (const auto& item : stmt.select_list) {
      if (item.expr && item.expr->type() == ExprType::AGGREGATE) {
        const auto* agg = static_cast<const AggregateExpr*>(item.expr.get());
        std::string out_name = GetExprName(item.expr.get(), item.alias);
        AggEntry entry;
        entry.func = agg->func();
        entry.is_star = agg->is_star();
        entry.arg = CloneExpr(agg->arg());
        entry.output_name = out_name;
        agg_entries.push_back(std::move(entry));

        Column out_col;
        out_col.name = out_name;
        out_col.type = DeduceExprType(item.expr.get(), current_schema);
        agg_schema.columns.push_back(out_col);
      }
    }

    current_plan = std::make_unique<AggregatePlan>(
        std::move(agg_entries),
        stmt.group_by_columns,
        agg_schema,
        std::move(current_plan));
    current_schema = agg_schema;
  }

  // Handle ORDER BY
  if (!stmt.order_by.empty()) {
    current_plan = std::make_unique<SortPlan>(stmt.order_by, std::move(current_plan));
  }

  // Handle Projection (if not SELECT *)
  if (!stmt.is_select_all) {
    std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projs;
    TableSchema proj_schema;
    proj_schema.table_name = current_schema.table_name;

    for (const auto& item : stmt.select_list) {
      std::string out_name = GetExprName(item.expr.get(), item.alias);
      Column out_col;
      out_col.name = out_name;
      out_col.type = DeduceExprType(item.expr.get(), current_schema);
      proj_schema.columns.push_back(out_col);
      projs.push_back({CloneExpr(item.expr.get()), out_name});
    }

    current_plan = std::make_unique<ProjectPlan>(
        std::move(projs),
        proj_schema,
        std::move(current_plan));
  }

  // Phase 4B rule R3: drop an inner join to a parent table whose FK child
  // column is NOT NULL and none of whose columns are used above the join.
  if (rules_active) {
    current_plan = EliminateRedundantJoins(std::move(current_plan), catalog_, &fired);
  }

  // Phase 4: cost every node and record the access-path decision on the root
  // so EXPLAIN can print the chosen plan next to the rejected candidates.
  AnnotatePlan(current_plan.get(), rules_active ? &catalog_ : nullptr, rules_active,
               &fired);
  current_plan->set_access_path(chosen_path);
  current_plan->set_alternatives(std::move(rejected_paths));
  for (const auto& rule : fired) current_plan->add_applied_rule(rule);
  return current_plan;
}

std::unique_ptr<PlanNode> Planner::PlanInsert(const InsertStmt& stmt, std::string* err) {
  const TableSchema* schema = catalog_.GetTable(stmt.table_name);
  if (!schema) {
    if (err) *err = "table '" + stmt.table_name + "' does not exist";
    return nullptr;
  }

  std::vector<std::string> target_cols;
  if (stmt.column_names.empty()) {
    for (const auto& c : schema->columns) {
      target_cols.push_back(c.name);
    }
  } else {
    for (const auto& cn : stmt.column_names) {
      if (!schema->HasColumn(cn)) {
        if (err) *err = "column '" + cn + "' not found in table '" + stmt.table_name + "'";
        return nullptr;
      }
      target_cols.push_back(cn);
    }
  }

  // Validate values matrix dimensions
  for (const auto& row : stmt.values_matrix) {
    if (row.size() != target_cols.size()) {
      if (err) *err = "INSERT has " + std::to_string(target_cols.size()) +
                      " target columns but VALUES has " + std::to_string(row.size()) + " values";
      return nullptr;
    }
  }

  std::vector<std::vector<std::unique_ptr<Expr>>> cloned_matrix;
  for (const auto& row : stmt.values_matrix) {
    std::vector<std::unique_ptr<Expr>> cloned_row;
    for (const auto& expr : row) {
      cloned_row.push_back(CloneExpr(expr.get()));
    }
    cloned_matrix.push_back(std::move(cloned_row));
  }

  return std::make_unique<InsertPlan>(stmt.table_name, *schema, target_cols, std::move(cloned_matrix));
}

std::unique_ptr<PlanNode> Planner::PlanUpdate(const UpdateStmt& stmt, std::string* err) {
  const TableSchema* schema = catalog_.GetTable(stmt.table_name);
  if (!schema) {
    if (err) *err = "table '" + stmt.table_name + "' does not exist";
    return nullptr;
  }

  for (const auto& assign : stmt.assignments) {
    if (!schema->HasColumn(assign.first)) {
      if (err) *err = "column '" + assign.first + "' not found in table '" + stmt.table_name + "'";
      return nullptr;
    }
  }

  std::unique_ptr<PlanNode> scan = std::make_unique<SeqScanPlan>(stmt.table_name, *schema);
  if (stmt.where_clause) {
    scan = std::make_unique<FilterPlan>(CloneExpr(stmt.where_clause.get()), std::move(scan));
  }

  std::vector<std::pair<std::string, std::unique_ptr<Expr>>> cloned_assignments;
  for (const auto& a : stmt.assignments) {
    cloned_assignments.push_back({a.first, CloneExpr(a.second.get())});
  }

  AnnotatePlan(scan.get());
  return std::make_unique<UpdatePlan>(stmt.table_name, *schema, std::move(cloned_assignments), std::move(scan));
}

std::unique_ptr<PlanNode> Planner::PlanDelete(const DeleteStmt& stmt, std::string* err) {
  const TableSchema* schema = catalog_.GetTable(stmt.table_name);
  if (!schema) {
    if (err) *err = "table '" + stmt.table_name + "' does not exist";
    return nullptr;
  }

  std::unique_ptr<PlanNode> scan = std::make_unique<SeqScanPlan>(stmt.table_name, *schema);
  if (stmt.where_clause) {
    scan = std::make_unique<FilterPlan>(CloneExpr(stmt.where_clause.get()), std::move(scan));
  }

  AnnotatePlan(scan.get());
  return std::make_unique<DeletePlan>(stmt.table_name, *schema, std::move(scan));
}

}  // namespace sthir
