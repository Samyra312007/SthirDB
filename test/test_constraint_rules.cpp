// Phase 4B — constraint-aware optimizer tests.
//
// Three things are checked here:
//   1. the constraint catalog APIs read the right facts out of the DDL;
//   2. every rule fires where it should (R1, R2, R2b, R3, R4) and reports it;
//   3. every rule-triggering query returns IDENTICAL rows with
//      `constraint_aware` on and off. A rule that changes an answer is a hard
//      bug, so this is the most important assertion in the file.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "common/config.h"
#include "exec/execution_engine.h"
#include "optimizer/constraint_rules.h"
#include "test_macros.h"

namespace fs = std::filesystem;

namespace {

const std::string kDir = "build/test_constraint_rules_env";

struct Env {
  sthir::Catalog catalog{kDir + "/catalog.json"};
  std::unique_ptr<sthir::ExecutionEngine> engine;

  Env() {
    std::string err;
    catalog.Load(&err);
    engine = std::make_unique<sthir::ExecutionEngine>(&catalog, kDir);
  }
};

sthir::ExecutionResult Run(Env& env, const std::string& sql) {
  auto res = env.engine->Execute(sql);
  if (!res.success) {
    std::fprintf(stderr, "SQL failed: %s -> %s\n", sql.c_str(), res.error_message.c_str());
  }
  CHECK(res.success);
  return res;
}

// A small retail-shaped schema exercising all four rules.
void BuildSchema(Env& env) {
  Run(env, "CREATE TABLE Items (ItemID INT PRIMARY KEY, ItemName VARCHAR(50), "
           "RetailPrice DECIMAL, StockLevel INT NOT NULL CHECK (StockLevel >= 0))");
  Run(env, "CREATE TABLE Shoppers (ShopperID INT PRIMARY KEY, Name VARCHAR(50), "
           "Email VARCHAR(50) UNIQUE)");
  Run(env, "CREATE TABLE Purchases (PurchaseID INT PRIMARY KEY, "
           "ShopperID INT NOT NULL REFERENCES Shoppers (ShopperID) ON DELETE RESTRICT, "
           "Total DECIMAL)");

  for (int i = 1; i <= 40; ++i) {
    char sql[256];
    std::snprintf(sql, sizeof(sql), "INSERT INTO Items VALUES (%d, 'item_%d', %d.50, %d)", i, i,
                  i, i);  // every StockLevel >= 0
    Run(env, sql);
  }
  // 100 shoppers but only 30 purchases: the conventional 10% join selectivity
  // estimates 30 * 100 * 0.1 = 300 rows where the answer has 30.
  for (int i = 1; i <= 100; ++i) {
    char sql[256];
    std::snprintf(sql, sizeof(sql), "INSERT INTO Shoppers VALUES (%d, 'shopper_%d', 's%d@x.test')",
                  i, i, i);
    Run(env, sql);
  }
  for (int i = 1; i <= 30; ++i) {
    char sql[256];
    std::snprintf(sql, sizeof(sql), "INSERT INTO Purchases VALUES (%d, %d, %d.00)", i,
                  (i % 10) + 1, i);
    Run(env, sql);
  }

  // The PK index R1 seeks.
  Run(env, "CREATE INDEX idx_items_pk ON Items (ItemID)");
  Run(env, "ANALYZE Items");
  Run(env, "ANALYZE Shoppers");
  Run(env, "ANALYZE Purchases");
}

// Runs `sql` in a fresh engine so buffer-pool state cannot hide page reads.
sthir::ExecutionResult RunFresh(const std::string& sql, const std::string& constraint_aware,
                                const std::string& constraints_enabled = "on") {
  Env env;
  env.engine->SetSessionFlag("constraint_aware", constraint_aware);
  env.engine->SetSessionFlag("constraints_enabled", constraints_enabled);
  auto res = env.engine->Execute(sql);
  if (!res.success) {
    std::fprintf(stderr, "SQL failed: %s -> %s\n", sql.c_str(), res.error_message.c_str());
  }
  CHECK(res.success);
  return res;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

void CheckIdenticalRows(const std::string& sql) {
  const auto off = RunFresh(sql, "off");
  const auto on = RunFresh(sql, "on");
  CHECK_EQ(off.rows.size(), on.rows.size());
  for (size_t i = 0; i < off.rows.size(); ++i) {
    CHECK_EQ(off.rows[i].size(), on.rows[i].size());
    for (size_t c = 0; c < off.rows[i].size(); ++c) {
      if (off.rows[i][c] != on.rows[i][c]) {
        std::fprintf(stderr, "row mismatch for %s: '%s' vs '%s'\n", sql.c_str(),
                     off.rows[i][c].c_str(), on.rows[i][c].c_str());
      }
      CHECK(off.rows[i][c] == on.rows[i][c]);
    }
  }
}

void test_catalog_constraint_facts() {
  fs::remove_all(kDir);
  fs::create_directories(kDir);
  Env env;
  BuildSchema(env);

  const auto& catalog = env.catalog;
  CHECK(catalog.IsUnique("Items", {"ItemID"}));
  CHECK(catalog.IsUnique("Shoppers", {"Email"}));
  CHECK(!catalog.IsUnique("Shoppers", {"Name"}));
  CHECK(!catalog.IsUnique("Items", {"ItemID", "ItemName"}));  // wrong column set

  const auto range = catalog.GetCheckRange("Items", "StockLevel");
  CHECK(range.has_value());
  CHECK(range->has_lo);
  CHECK(range->lo_closed);
  CHECK_EQ(range->lo, 0.0);
  CHECK(!range->has_hi);
  CHECK(!catalog.GetCheckRange("Items", "RetailPrice").has_value());

  const auto fk = catalog.GetForeignKeyFrom("Purchases", "ShopperID");
  CHECK(fk.has_value());
  CHECK_EQ(fk->parent_table, "Shoppers");
  CHECK(fk->child_not_null);
  CHECK(fk->parent_unique);
  CHECK(fk->on_delete == sthir::ForeignKey::OnDelete::RESTRICT);

  // A nullable FK column must not satisfy R3's safety condition.
  Run(env, "CREATE TABLE Notes (NoteID INT PRIMARY KEY, "
           "ShopperID INT REFERENCES Shoppers (ShopperID))");
  const auto nullable = catalog.GetForeignKeyFrom("Notes", "ShopperID");
  CHECK(nullable.has_value());
  CHECK(!nullable->child_not_null);
  test_passed("catalog_constraint_facts");
}

void test_interval_helpers() {
  // [0, inf) vs (-inf, 0): disjoint.
  sthir::ColumnInterval check;
  check.has_lo = true;
  check.lo = 0.0;
  check.lo_closed = true;

  sthir::ColumnInterval negative;
  negative.has_hi = true;
  negative.hi = 0.0;
  negative.hi_closed = false;
  CHECK(sthir::IntervalIsEmpty(check, negative));
  CHECK(!sthir::IntervalCovers(check, negative));

  // The CHECK range covers `StockLevel >= 0`, but not `StockLevel > 0`.
  sthir::ColumnInterval non_negative;
  non_negative.has_lo = true;
  non_negative.lo = 0.0;
  non_negative.lo_closed = true;
  CHECK(sthir::IntervalCovers(check, non_negative));

  sthir::ColumnInterval positive;
  positive.has_lo = true;
  positive.lo = 0.0;
  positive.lo_closed = false;
  CHECK(!sthir::IntervalCovers(check, positive));  // a row with 0 would fail it
  CHECK(!sthir::IntervalIsEmpty(check, positive));

  // Closed intervals that touch are not empty.
  sthir::ColumnInterval zero;
  zero.has_lo = zero.has_hi = true;
  zero.lo = zero.hi = 0.0;
  CHECK(!sthir::IntervalIsEmpty(check, zero));
  test_passed("interval_helpers");
}

void test_r1_unique_lookup() {
  const std::string sql = "SELECT ItemName FROM Items WHERE ItemID = 7";
  const auto off = RunFresh(sql, "off");
  const auto on = RunFresh(sql, "on");

  CHECK_EQ(off.rows.size(), 1u);
  CHECK_EQ(on.rows.size(), 1u);
  CHECK_EQ(off.rows[0][0], "item_7");
  CHECK_EQ(on.rows[0][0], "item_7");
  CHECK(Contains(off.chosen_path, "SeqScan"));
  CHECK(Contains(on.chosen_path, "IndexScan(idx_items_pk)"));
  CHECK_EQ(on.rows_estimated, 1.0);
  CHECK(Contains(on.rules_line, "R1"));
  CHECK(off.rules_line.empty());
  CheckIdenticalRows(sql);
  test_passed("r1_unique_lookup");
}

void test_r2_contradiction() {
  const std::string sql = "SELECT COUNT(*) FROM Items WHERE StockLevel < 0";
  const auto off = RunFresh(sql, "off");
  const auto on = RunFresh(sql, "on");

  CHECK_EQ(off.rows.size(), 1u);
  CHECK_EQ(off.rows[0][0], "0");
  CHECK_EQ(on.rows[0][0], "0");
  CHECK(off.pages_read > 0);   // the baseline still scans
  CHECK_EQ(on.pages_read, 0u); // EmptyResult reads nothing
  CHECK(Contains(on.chosen_path, "EmptyResult"));
  CHECK(Contains(on.plan_text, "EmptyResult"));
  CHECK(Contains(on.rules_line, "R2 ("));  // R2, not R2b
  CheckIdenticalRows(sql);
  test_passed("r2_contradiction");
}

void test_r2b_redundant_predicate() {
  const std::string sql = "SELECT COUNT(*) FROM Items WHERE StockLevel >= 0";
  const auto off = RunFresh(sql, "off");
  const auto on = RunFresh(sql, "on");

  CHECK_EQ(off.rows[0][0], "40");
  CHECK_EQ(on.rows[0][0], "40");
  CHECK(Contains(off.plan_text, "Filter"));
  CHECK(!Contains(on.plan_text, "Filter"));  // predicate dropped
  CHECK(Contains(on.rules_line, "R2b"));
  CheckIdenticalRows(sql);
  test_passed("r2b_redundant_predicate");
}

void test_r3_join_elimination() {
  const std::string sql =
      "SELECT COUNT(*) FROM Purchases JOIN Shoppers "
      "ON Purchases.ShopperID = Shoppers.ShopperID";
  const auto off = RunFresh(sql, "off");
  const auto on = RunFresh(sql, "on");

  CHECK_EQ(off.rows[0][0], "30");
  CHECK_EQ(on.rows[0][0], "30");
  CHECK(Contains(off.plan_text, "HashJoin"));
  CHECK(!Contains(on.plan_text, "HashJoin"));
  CHECK(Contains(on.rules_line, "R3"));
  CHECK(on.pages_read < off.pages_read);  // the parent is never touched
  CheckIdenticalRows(sql);
  test_passed("r3_join_elimination");
}

void test_r3_safety_conditions() {
  // 1. The parent's columns are projected: elimination would lose them.
  const std::string projected =
      "SELECT Shoppers.Name FROM Purchases JOIN Shoppers "
      "ON Purchases.ShopperID = Shoppers.ShopperID";
  const auto on = RunFresh(projected, "on");
  CHECK(Contains(on.plan_text, "HashJoin"));
  CHECK(!Contains(on.rules_line, "R3"));
  CHECK_EQ(on.rows.size(), 30u);
  CheckIdenticalRows(projected);

  // 2. A filter on the parent changes which rows match.
  const std::string filtered =
      "SELECT COUNT(*) FROM Purchases JOIN Shoppers "
      "ON Purchases.ShopperID = Shoppers.ShopperID WHERE Shoppers.Name = 'shopper_1'";
  const auto filtered_on = RunFresh(filtered, "on");
  CHECK(!Contains(filtered_on.rules_line, "R3"));
  CheckIdenticalRows(filtered);

  // 3. With enforcement off the fact is no longer guaranteed, even with
  //    constraint_aware on: no rule may fire.
  const std::string count_join =
      "SELECT COUNT(*) FROM Purchases JOIN Shoppers "
      "ON Purchases.ShopperID = Shoppers.ShopperID";
  const auto unenforced = RunFresh(count_join, "on", "off");
  CHECK(!Contains(unenforced.rules_line, "R3"));
  CHECK(Contains(unenforced.plan_text, "HashJoin"));
  test_passed("r3_safety_conditions");
}

void test_r4_fk_join_cardinality() {
  // The join must stay (Shoppers.Name is projected), so only R4 can fire.
  const std::string sql =
      "SELECT Shoppers.Name FROM Purchases JOIN Shoppers "
      "ON Purchases.ShopperID = Shoppers.ShopperID";
  const auto off = RunFresh(sql, "off");
  const auto on = RunFresh(sql, "on");

  CHECK_EQ(off.rows.size(), 30u);
  CHECK_EQ(on.rows.size(), 30u);
  // 10% of 30 x 100 = 300; the FK says exactly rows(child) = 30.
  CHECK_EQ(off.rows_estimated, 300.0);
  CHECK_EQ(on.rows_estimated, 30.0);
  CHECK(Contains(on.rules_line, "R4"));
  CheckIdenticalRows(sql);
  test_passed("r4_fk_join_cardinality");
}

void test_rule_equivalence_suite() {
  // The report's Novelty-1 query suite: 6 rule-triggering queries plus one
  // control the rules must leave alone.
  const std::vector<std::string> queries = {
      "SELECT ItemName FROM Items WHERE ItemID = 7",
      "SELECT COUNT(*) FROM Items WHERE StockLevel < 0",
      "SELECT COUNT(*) FROM Items WHERE StockLevel >= 0",
      "SELECT COUNT(*) FROM Purchases JOIN Shoppers "
      "ON Purchases.ShopperID = Shoppers.ShopperID",
      "SELECT Shoppers.Name FROM Purchases JOIN Shoppers "
      "ON Purchases.ShopperID = Shoppers.ShopperID",
      "SELECT COUNT(*) FROM Items WHERE StockLevel >= 0 AND ItemID = 3",
      "SELECT COUNT(*) FROM Items WHERE RetailPrice > 20",  // control: no rule
  };
  for (const auto& sql : queries) {
    CheckIdenticalRows(sql);
  }

  const auto control = RunFresh("SELECT COUNT(*) FROM Items WHERE RetailPrice > 20", "on");
  CHECK(!Contains(control.rules_line, "R1"));
  CHECK(!Contains(control.rules_line, "R2"));
  CHECK(!Contains(control.rules_line, "R3"));
  CHECK(!Contains(control.rules_line, "R4"));
  test_passed("rule_equivalence_suite");
}

void test_rules_off_by_default() {
  // Without `SET constraint_aware = on` no rule may fire, even on a fully
  // constrained schema.
  const auto res = RunFresh("SELECT COUNT(*) FROM Items WHERE StockLevel < 0", "");
  CHECK(!Contains(res.rules_line, "R2"));
  CHECK(res.pages_read > 0);
  test_passed("rules_off_by_default");
}

}  // namespace

int main() {
  test_catalog_constraint_facts();
  test_interval_helpers();
  test_r1_unique_lookup();
  test_r2_contradiction();
  test_r2b_redundant_predicate();
  test_r3_join_elimination();
  test_r3_safety_conditions();
  test_r4_fk_join_cardinality();
  test_rule_equivalence_suite();
  test_rules_off_by_default();
  std::printf("all constraint-aware rule tests passed\n");
  return 0;
}
