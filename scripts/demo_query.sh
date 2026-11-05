#!/usr/bin/env bash
# Phase 8 demo 2 — the SQL subset: joins, NULLs, aggregates and grouping.
#
#   ./scripts/demo_query.sh
#
# Loads a small hand-written dataset so every expected answer is visible in the
# script itself, then asserts each result.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=./build/sthirdb
EV=docs/evidence
mkdir -p "$EV"

fails=0
say() { printf '%s\n' "$*"; }

# check <label> <sql> <expected first-column value>
check() {
  local label="$1" sql="$2" want="$3" got
  got=$("$BIN" run "$sql" | awk -F'|' 'NR==4 { gsub(/^ +| +$/, "", $2); print $2 }')
  if [ "$got" = "$want" ]; then
    say "PASS  $label  ($want)"
  else
    say "FAIL  $label  expected '$want', got '$got'"
    fails=$((fails + 1))
  fi
}

show() {
  say
  say "-- $1"
  say "   $2"
  "$BIN" run "$2"
}

main() {
  say "=== SthirDB demo: SQL subset (joins, NULLs, aggregates) ==="
  make -s all

  rm -rf ./data
  mkdir -p ./data

  "$BIN" run "CREATE TABLE Dept (DeptID INT PRIMARY KEY, DeptName VARCHAR(20))" > /dev/null
  "$BIN" run "CREATE TABLE Emp (EmpID INT PRIMARY KEY, Name VARCHAR(20), DeptID INT, Salary INT)" > /dev/null
  "$BIN" run "INSERT INTO Dept VALUES (1, 'Engineering')" > /dev/null
  "$BIN" run "INSERT INTO Dept VALUES (2, 'Sales')" > /dev/null
  "$BIN" run "INSERT INTO Emp VALUES (10, 'Asha', 1, 900)" > /dev/null
  "$BIN" run "INSERT INTO Emp VALUES (11, 'Bilal', 1, 700)" > /dev/null
  "$BIN" run "INSERT INTO Emp VALUES (12, 'Chen', 2, 500)" > /dev/null
  "$BIN" run "INSERT INTO Emp VALUES (13, 'Dana', NULL, 400)" > /dev/null

  show "a plain projection with a predicate" "SELECT Name FROM Emp WHERE Salary > 500"
  check "predicate rows" "SELECT COUNT(*) FROM Emp WHERE Salary > 500" "2"

  show "an inner join across two tables" \
    "SELECT Emp.Name, Dept.DeptName FROM Emp JOIN Dept ON Emp.DeptID = Dept.DeptID WHERE Salary > 600"
  check "join rows" \
    "SELECT COUNT(*) FROM Emp JOIN Dept ON Emp.DeptID = Dept.DeptID" "3"

  show "IS NULL finds the row with no department" "SELECT Name FROM Emp WHERE DeptID IS NULL"
  check "null rows" "SELECT COUNT(*) FROM Emp WHERE DeptID IS NULL" "1"

  show "aggregates" "SELECT COUNT(*), SUM(Salary), MIN(Salary), MAX(Salary), AVG(Salary) FROM Emp"
  # SUM of an INT column comes back formatted as a decimal.
  check "sum of salaries" "SELECT SUM(Salary) FROM Emp" "2500.00"

  show "grouping with a join" \
    "SELECT Dept.DeptName, COUNT(*), SUM(Emp.Salary) FROM Emp JOIN Dept ON Emp.DeptID = Dept.DeptID GROUP BY Dept.DeptName ORDER BY Dept.DeptName"
  check "groups" \
    "SELECT COUNT(*) FROM Emp JOIN Dept ON Emp.DeptID = Dept.DeptID GROUP BY Dept.DeptName" "2"

  say
  if [ "$fails" -eq 0 ]; then
    say "RESULT: PASS — 5/5 asserted queries returned the expected answers."
    return 0
  fi
  say "RESULT: FAIL — $fails assertion(s) failed."
  return 1
}

main 2>&1 | tee "$EV/phase8_demo_query.txt"
exit "${PIPESTATUS[0]}"
