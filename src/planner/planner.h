#pragma once

#include <memory>
#include <string>

#include "catalog/catalog.h"
#include "parser/ast.h"
#include "planner/plan_node.h"

namespace sthir {

class Planner {
 public:
  // `allow_index_scan` / `allow_seq_scan` expose the access-path choice to the
  // CLI (SET enable_index_scan / enable_seq_scan) for measurement; both default
  // to on, which is the normal optimizer behaviour.
  // `constraint_aware` is the SET flag governing the Phase 4B rewrite rules;
  // those rules additionally require `constraints_enforced`, because the facts
  // they rely on only hold when the schema is actually enforcing its DDL.
  explicit Planner(const Catalog& catalog, bool allow_index_scan = true,
                   bool allow_seq_scan = true, bool constraint_aware = false,
                   bool constraints_enforced = true)
      : catalog_(catalog),
        allow_index_scan_(allow_index_scan),
        allow_seq_scan_(allow_seq_scan),
        constraint_aware_(constraint_aware),
        constraints_enforced_(constraints_enforced) {}

  std::unique_ptr<PlanNode> CreatePlan(const SQLStatement& stmt, std::string* err);

 private:
  std::unique_ptr<PlanNode> PlanSelect(const SelectStmt& stmt, std::string* err);
  std::unique_ptr<PlanNode> PlanInsert(const InsertStmt& stmt, std::string* err);
  std::unique_ptr<PlanNode> PlanUpdate(const UpdateStmt& stmt, std::string* err);
  std::unique_ptr<PlanNode> PlanDelete(const DeleteStmt& stmt, std::string* err);

  const Catalog& catalog_;
  bool allow_index_scan_ = true;
  bool allow_seq_scan_ = true;
  bool constraint_aware_ = false;
  bool constraints_enforced_ = true;
};

}  // namespace sthir
