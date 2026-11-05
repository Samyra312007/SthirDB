# SthirDB

**SthirDB** is a disk-resident relational database engine written from scratch in C++17: 8 KB slotted pages, a buffer pool with steal/no-force policies, a B+tree index, a Volcano executor, a cost-based and **constraint-aware** optimizer, strict 2PL row locking, and **ARIES write-ahead logging** with three-pass crash recovery. It is evaluated on a constraint-driven retail workload and by a **1,050-trial randomized crash-injection campaign**.

The full write-up is [`docs/report.md`](docs/report.md). Raw experiment output is in [`docs/evidence/`](docs/evidence/).

## Requirements

- C++17 compiler (`g++` or `clang++`)
- GNU Make (`make` or `mingw32-make`)
- POSIX environment (Linux, macOS, WSL) or Windows via MinGW / MSYS2
- Python 3 (only for the experiment scripts that format tables and draw SVG plots)

## Building

```bash
make          # -> ./build/sthirdb
make test     # build and run the full test suite (27 programs)
make clean
```

## Quick start

```bash
mkdir -p ./data
./build/sthirdb sql scripts/retail_schema.sql   # 8 retail tables with constraints
./build/sthirdb retail-gen                      # deterministic data, seed 42
./build/sthirdb run "SELECT ItemName FROM Items WHERE ItemID = 4242"
./build/sthirdb explain "SELECT COUNT(*) FROM Purchases JOIN Shoppers ON Purchases.ShopperID = Shoppers.ShopperID"
```

Every experiment is seeded, so the data and the crash trials replay exactly; wall-clock numbers vary with machine load.

## Demos — one command each

| Script | Shows |
|---|---|
| `./scripts/demo_storage.sh` | create the schema, bulk-load 5,000 rows, count them back |
| `./scripts/demo_query.sh` | joins, `IS NULL`, aggregates and `GROUP BY`, every answer asserted |
| `./scripts/demo_index.sh` | the same query through `IndexScan` and `SeqScan`, pages read side by side |
| `./scripts/demo_optimizer.sh` | the planner picking an index for a selective query and a full scan for a broad one |
| `./scripts/demo_recovery.sh` | ARIES Tests A–H with a visible pass/fail verdict |
| `./scripts/demo_benchmark.sh` | anomaly interception (constrained vs unconstrained) and concurrent latency |
| `./scripts/demo_constraint_aware.sh` | Novelty 1: plan diff and applied rules with `constraint_aware` off then on |
| `./scripts/demo_crash_injection.sh` | Novelty 2A: a 70-trial crash campaign (10 per hazard point) |
| `./scripts/demo_checkpoint.sh` | Novelty 2B: the checkpoint-interval trade-off table |

Demo output is saved under `docs/evidence/phase8_demo_*.txt`.

## Full experiments

```bash
./scripts/benchmark.sh                  # ~6 min: constrained + unconstrained retail runs
./scripts/constraint_aware_experiment.sh # Novelty 1: A-vs-B, row equivalence, write overhead
./scripts/crash_injection.sh            # Novelty 2A: 1,050 trials + two mutation builds
./scripts/checkpoint_experiment.sh      # Novelty 2B: 5 intervals x 5 runs + both plots
```

Each script regenerates its own evidence files in `docs/evidence/` from a clean `./data/`.

## CLI

```
sthirdb run "<sql>"                 execute one statement
sthirdb explain [--constraint-aware on|off] "<sql>"
                                    plan tree, estimates, rejected paths, rules applied
sthirdb optimizer [--seq|--index] [--constraint-aware on|off] "<sql>"
                                    chosen plan plus actual time, rows and pages read
sthirdb sql [--constraints on|off] <file>
                                    run every statement in a file
sthirdb recover                     run ARIES restart recovery
sthirdb retail-gen [--seed N] [--constraints on|off]
                                    deterministically load the retail dataset
sthirdb inject [--constraints on|off] [--extended] [--rate 1|5|10]
                                    anomaly injector; prints interception tables
sthirdb bench [--threads 100,300,500] [--iters N] [--constraints on|off]
                                    concurrent reporting-query benchmark
sthirdb trial-init|trial-run|trial-recover|trial-verify --dir <path>
                                    one crash trial (init, workload, recover, verify)
sthirdb crash-harness [--trials N] [--csv <path>]
                                    randomized subprocess crash-injection campaign
```

`--version` prints the version; `sthirdb` with no arguments prints the full usage.

### Supported SQL

- **DDL:** `CREATE TABLE` with `INT`, `VARCHAR(n)`, `DECIMAL`, `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `CHECK (...)`, `FOREIGN KEY ... REFERENCES ...`, `ON DELETE RESTRICT | CASCADE`.
- **DML:** `INSERT`, `UPDATE ... SET ... [WHERE ...]`, `DELETE FROM ... [WHERE ...]`.
- **Queries:** `SELECT` with `JOIN ... ON`, `WHERE`, `GROUP BY`, `ORDER BY`, aggregates (`COUNT`, `SUM`, `MIN`, `MAX`, `AVG`), `IS NULL` / `IS NOT NULL`.
- **Control:** `BEGIN`, `COMMIT`, `ABORT` / `ROLLBACK`, `ANALYZE t`, `SET <flag> = <value>`, `EXPLAIN <sql>`.

## Data scale

`retail-gen` produces 1,000 shoppers, 500 items, 100,000 purchases, ~300,000 line items and the supporting invoice/catalog rows (516,150 rows total, seed 42). The crash trials use a much smaller two-table database (`Stock` + `Ledger`) so that a trial — including its recovery — stays under a second.

## Repository layout

```
src/common/     configuration, crash-injection hooks
src/storage/    disk manager, slotted pages, buffer pool, B+tree
src/catalog/    persistent schema catalog (tables, columns, constraints, table ids)
src/parser/     lexer, parser, AST
src/planner/    plan nodes, cost model, constraint-aware rules
src/optimizer/  plan enumeration and costing
src/exec/       Volcano executors, execution engine, lock manager
src/wal/        log records, WAL manager
src/recovery/   ARIES analysis / redo / undo
src/workload/   retail generator, anomaly injector, benchmark, crash trials
test/           27 test programs (`make test`)
scripts/        demos and reproducible experiments
docs/           report.md, evidence/, scope.md, design notes
```

## Notes and limits

Single process, single node; the WAL grows for the life of the data directory (no truncation); page writes assume the OS does not tear them (torn pages are detected only by the header-ordering rule described in report §8.6). `docs/report.md` §11 states every limitation and the next steps.
