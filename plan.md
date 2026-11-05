# SthirDB

**Project name:** **SthirDB** — A Disk-Resident Relational Engine with ARIES-Style Crash Recovery, Evaluated on a Constraint-Driven Retail Workload

*("Sthir" — स्थिर — steady, stable, unwavering; chosen because the project's defining property is durability: the engine that stays standing through a crash.)

---

### Final acceptance criteria (the project is "done" when all of these are true)
- [x] The engine can create the retail schema, load data, and run SQL-subset queries from a CLI.
- [x] The B+tree index supports insert, point lookup, and range scan over disk pages.
- [x] The optimizer chooses between SeqScan and IndexScan using a cost model and reports estimated vs. actual cost.
- [x] An intentional crash (SIGKILL) followed by restart leaves committed data intact and removes uncommitted data.
- [x] Constrained vs. unconstrained anomaly injection produces a reportable interception-rate comparison.
- [x] The constraint-aware optimizer (Novelty 1) rewrites queries via R1–R4; the report compares it against the baseline optimizer on the same engine (`SET constraint_aware = on/off`) and every rule-triggering query returns **identical rows** in both modes.
- [x] The crash-injection harness (Novelty 2A) runs 1,000+ randomized trials and reports a pass rate against durability, atomicity, consistency and idempotency invariants.
- [x] The checkpoint-frequency sweep (Novelty 2B) produces a throughput-vs-recovery-time trade-off graph with an identified sweet spot.
- [x] **One report, one file, end to end** (`docs/report.md`): a single research-paper-style document covering the whole project (Phases 0–8, both novelties inside it), written in plain understandable English, mapping every syllabus module to a delivered component and citing all references.

---

# Single-Report Policy (governs every phase — read before starting any phase)

**The project produces exactly ONE report for the whole project, prepared end to end.** Not one report per phase, not a draft plus a final, not a separate novelty write-up, not slides instead of a report. One file:

```
/docs/report.md       ← THE report; every phase writes into this same file
/docs/evidence/       ← raw numbers, tables, figures, logs pasted into the report (not reports themselves)
```

### What "one report, end to end" means
1. **One file for the entire project.** Phases 0–8 all contribute to `docs/report.md`. Forbidden: `report_v2.md`, `phase3_report.md`, `novelty_writeup.md`, per-member summaries. The two novelties (Phases 4B and 5B) are **sections inside** the same report, never standalone documents.
2. **Written while building, assembled at the end.** Each phase ends by drafting/updating *its own section* in the same file using real evidence from that phase. Phase 8 only **merges, de-duplicates, checks consistency, and formats** — it never starts report content from scratch.
3. **Research-paper style, one voice:**
   - Title block: title, team, abstract (~150 words), keywords.
   - Numbered sections (`1.`, `2.1`, …) with one consistent heading hierarchy.
   - Every claim backed by a figure, table, or measurement stored in `/docs/evidence/`.
   - Figures numbered (`Figure 1`) and tables numbered (`Table 1`), each referenced in the text at least once.
   - No code dumps in the body — pseudocode for the ARIES passes only; code lives in the repo / appendix.
   - One ACM/IEEE-style References section at the end; no citations hidden in footnotes.
4. **Easily understandable (a reader outside the team must follow it):**
   - Every technical term defined at first use in one plain sentence (e.g., "WAL — write-ahead logging: the log record reaches disk *before* the data page is overwritten").
   - Short sentences, active voice, one idea per paragraph.
   - Each section opens with a 2–3 sentence "what this is and why it exists" paragraph before any detail.
   - Prefer a diagram or table over a paragraph of prose.
   - End each major section with a **Plain-English summary** box (3–5 bullets).
   - Read-aloud test: if a sentence cannot be read in one breath, split it. Banned words without a number: "faster", "better", "efficient".
5. **Evidence, not adjectives.** Every experiment's raw table lives in `/docs/evidence/` and the *same* numbers appear in the report.
6. **Consistency pass before submission:** every number traced to an evidence file; every figure/table referenced; no section contradicts another; one terminology list used throughout.

### Report section ↔ owning phase (drafted where the work happens)
| Report section | Drafted during | Evidence source |
|---|---|---|
| 0 Title block + Abstract | Phase 8 (outline in Phase 0) | whole project |
| 1 Introduction | Phase 0 | `docs/scope.md` |
| 2 Background | Phase 0–1 | papers in `docs/papers/` |
| 3 System Overview | Phase 2 | architecture diagram |
| 4 Storage, Indexing & Query Execution | Phases 1–3 | stress/bench logs |
| 5 Optimizer | Phase 4 | `EXPLAIN` outputs, est-vs-actual tables |
| 6 Constraint-Aware Optimizer (Novelty 1) | Phase 4B | on/off A-vs-B tables |
| 7 ARIES Crash Recovery | Phase 5 | Tests A–H results |
| 8 Testing Methodology (Novelty 2) | Phase 5B | harness CSVs, sweep plots |
| 9 Benchmark & Evaluation | Phase 7 | interception + latency tables |
| 10 Syllabus Mapping | Phase 8 | module table |
| 11 Limitations & Future Work | Phase 8 (negative results logged continuously) | failure/negative-result log |
| 12 References | continuously | citation list below |

*Section numbers may flex by one if a section splits; the rule that may not flex: there is still only one file.*

---

# Phase 0 — Environment Setup
**Goal:** everyone agrees on scope, tooling, and the definition of done before a single line of engine code is written.

### Implementation detail
- Confirm the **language** (default C++17). Decide once; switching mid-project is the single biggest schedule risk.
- Confirm the **trim budget**: what gets dropped first if time runs out. Recommended drop order: (1) concurrency/2PL, (2) join-order optimization, (3) full 8-table schema → 4-table subset.
- Create the repo layout so later phases have obvious homes:
  ```
  /src        engine sources
  /src/storage, /src/index, /src/exec, /src/optimizer, /src/recovery, /src/wal
  /test       unit + integration tests
  /scripts    demo + benchmark shell scripts
  /data       runtime data dir (gitignored)
  /docs       design notes, ER diagram, report.md (THE single report), evidence/
  ```
- Set up the build so `make` (or `cmake --build`) produces `./build/sthirdb`.
- Download and store the anchor paper + Yuan paper in `/docs/papers/`.

### Checklist
- [x] Git repo created; `.gitignore` covers `/build`, `/data`, `*.log`, editor files.
- [x] Directory skeleton (`src`, `test`, `scripts`, `data`, `docs`) created with placeholder files.
- [x] Build system committed; a trivial `main()` prints `SthirDB` version + usage when `./build/sthirdb` runs.
- [x] Anchor paper (ARIES) PDF downloaded to `docs/papers/` and citation recorded in `docs/scope.md`.
- [x] Yuan 2026 paper PDF downloaded to `docs/papers/` (already in repo root as `itmconf_cfctcc2026_04015.pdf`).
- [x] **Single report started:** `docs/report.md` created with the locked section skeleton from the Single-Report Policy (§0–§12) plus an empty `docs/evidence/` directory; a repo-wide check finds **no other** report files.
- [x] Whiteboard/paged ER diagram of the retail domain sketched and photographed into `docs/` — *delivered as the Mermaid sketch `docs/er_diagram.md`*
- [x] **Verify:** fresh clone → `make` → `./build/sthirdb` prints usage, exit code 0.

---

# Phase 1 — Storage Layer: Pages, Buffer Pool, Row Format
**Goal:** durable page-level storage with an in-memory buffer pool; the foundation ARIES will later protect.

### Implementation detail
- **Page:** fixed 8192 bytes (8 KB). Page header fields: `page_id (uint32)`, `page_lsn (uint64)` (reserved now, filled in Phase 5), `slot_count (uint16)`, `free_space_ptr (uint16)`.
- **Page file:** one file per table (`./data/retail/<table>.dat`). Page *i* lives at offset `i * 8192`.
- **Buffer pool:** fixed frame count (start with 64 frames). Components: frame array, `page_id → frame_id` hash map, pin counts, dirty flags, a free list, and a **replacer** (LRU to start; ARC-light only as stretch).
- **Replacer contract:** `record_access(frame_id)`, `set_evictable(frame_id, bool)`, `evict() -> optional<frame_id>`, `remove(frame_id)`, `size()`.
- **Row format (single fixed table for now):** `[int32 id][char[50] name][double price][bool deleted]`. Provide `serialize(row) -> bytes` and `deserialize(bytes) -> row`. Slotted page layout: slot directory growing from the front, tuples from the back.
- **Disk manager API:** `read_page(page_id, buffer)`, `write_page(page_id, buffer)`, `allocate_page() -> page_id`.
- **Pin protocol (critical):** caller must `fetch_page` (increments pin) and `unpin_page(page_id, is_dirty)` (decrements pin; marks dirty). Eviction may only pick frames with pin count 0.
- **Page size is locked at 8 KB for the whole project.** (The constraint-aware guide suggests 4 KB; either value is fine, but changing it mid-project invalidates every offset and every test — decide at Phase 0 and never revisit.)
- **Crash-injection hook point:** route *every* page write and *every* log append through a single low-level function, so Novelty 2's `crash_point('name')` can be attached later without touching any call sites.
- **Verification:** a stress test that writes 5,000 rows across many pages with a buffer pool of only 64 frames (i.e., forced eviction cycles), then reads them all back and checks integrity.

### Checklist
- [x] `Page` class with header layout and 8 KB payload defined.
- [x] Page file read/write at correct byte offsets implemented and unit-tested.
- [x] Slot-directory (slotted page) insert/tombstone/compact operations implemented.
- [x] Row serialize/deserialize for the initial fixed schema implemented and round-trip tested.
- [x] Disk manager: `read_page`, `write_page`, `allocate_page` implemented and tested.
- [x] Buffer pool frame array + `page_id → frame_id` map implemented.
- [x] Pin/unpin counters implemented; `fetch_page`/`unpin_page` API exposed and thread-safe guarded by a latch.
- [x] Replacer interface implemented with an LRU policy.
- [x] Dirty-page flagging and writeback-on-evict implemented.
- [x] `init_schema` CLI command creates the table file and initializes the buffer pool.
- [x] `load_data --rows N` CLI command bulk-inserts N rows.
- [x] Stress test: 5,000 rows written with a 64-frame pool; all rows read back correctly.
- [x] **Verify:** `./build/sthirdb init_schema && ./build/sthirdb load_data --rows 5000` exits 0; a follow-up count query returns exactly 5000.
- [x] **Report contribution (single report, §4 first draft):** page format + buffer-pool design written into `docs/report.md`, with the 5,000-row/64-frame stress numbers saved to `docs/evidence/`.
- [x] Phase committed and `./data/` backed up.

---

# Phase 2 — Catalog, SQL-Subset Parser, Planner, and SeqScan Executor
**Goal:** turn SQL text into a relational-algebra plan tree and execute the simplest plan (full scan).

### Implementation detail
- **Catalog:** persist table metadata (name, column names, types, file path, root page of any index) in `./data/retail/catalog.json` (JSON is fine; a system table is stretch).
- **Supported SQL subset (lock this list and do not expand it casually):**
  - `CREATE TABLE t (col type, ...)` with types `INT`, `VARCHAR(n)`, `DECIMAL`.
  - `INSERT INTO t VALUES (...)` and `INSERT INTO t (col, ...) VALUES (...)`.
  - `SELECT col, ... | * FROM t [WHERE pred] [GROUP BY col] [ORDER BY col]`.
  - `UPDATE t SET col = val [WHERE pred]`, `DELETE FROM t [WHERE pred]`.
  - Aggregates: `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`.
  - Predicates: `=`, `<>`, `<`, `<=`, `>`, `>=`, `AND`, `OR`, `NOT`, `IS NULL`, `IS NOT NULL`.
  - `BEGIN`, `COMMIT`, `ABORT` / `ROLLBACK` (synonym) (parsed now, honored in Phase 5).
  - `ANALYZE t` (refresh statistics — consumed by Phase 4), `SET <flag> = <value>` (session flags — consumed by Phase 4B `constraint_aware` and Phase 5B `checkpoint_interval`), `EXPLAIN <sql>` (consumed by Phase 4).
  - **Constraint declarations in DDL:** `PRIMARY KEY`, `UNIQUE`, `CHECK (<expr>)`, `FOREIGN KEY ... REFERENCES ...`, `NOT NULL`, `ON DELETE RESTRICT | CASCADE`. *Parsed and recorded in the catalog now; enforced in Phase 7.* This declaration-vs-enforcement split is what lets the optimizer (Phase 4B) be built before the retail schema exists.
- **Parser:** hand-written recursive descent (recommended — no external dependency) or a small generator. Produce an AST.
- **Planner:** AST → relational-algebra tree. Node types for this phase: `SeqScan(table)`, `Filter(pred, child)`, `Project(cols, child)`, `Aggregate(funcs, group_by, child)`, `Sort(cols, child)`, `NestedLoopJoin(join_pred, left, right)`. The join node is **required** — the Yuan benchmark query (Phase 7C) joins four tables, and Novelty 1's rules R3/R4 rewrite joins.
- **Executor:** iterator/Volcano model. Every operator exposes `init()`, `next() -> optional<Row>`, `close()`. SeqScan walks pages via the buffer pool and deserializes live (non-deleted) tuples.
- **NULL semantics:** three-valued logic for comparisons in `Filter`; `COUNT(*)` counts rows, `COUNT(col)` skips NULLs.

### Checklist
- [x] Catalog persisted and loadable; `CREATE TABLE` writes an entry.
- [x] Tokenizer handles identifiers, numbers, strings, operators, keywords, comments.
- [x] Recursive-descent parser for the full locked SQL subset above.
- [x] AST node types defined and their parse tests written.
- [x] Planner converts AST → RA tree for SELECT / INSERT / UPDATE / DELETE.
- [x] Volcano iterator interface (`init`/`next`/`close`) defined.
- [x] `SeqScan` executor implemented over the buffer pool.
- [x] `Filter` executor with three-valued NULL logic implemented.
- [x] `Project` executor implemented.
- [x] `Aggregate` executor implemented (hash aggregation; `COUNT/SUM/MIN/MAX/AVG`).
- [x] `Sort` executor implemented (in-memory sort acceptable for this phase).
- [x] `NestedLoopJoin` executor implemented (block nested loop: outer pages streamed, inner re-scanned) — required by Phase 7C and by Novelty 1 rules R3/R4.
- [x] Catalog records **declared constraints** (unique column sets, CHECK ranges, FK parent info) — this is the constraint-aware optimizer's input.
- [x] DDL parses `PRIMARY KEY`, `UNIQUE`, `CHECK`, `FOREIGN KEY`, `NOT NULL`, `ON DELETE RESTRICT|CASCADE` and stores them (enforcement deferred to Phase 7).
- [x] `ANALYZE` and `SET <flag> = <value>` statements parsed and routed (statistics → Phase 4; flags → Phases 4B/5B).
- [x] `INSERT` / `UPDATE` / `DELETE` executors implemented against slotted pages.
- [x] `run "<sql>"` CLI command: parse → plan → execute → print a formatted result table.
- [x] Parse-error and type-error messages are human-readable (not asserts).
- [x] **Verify:** run each of these and confirm output correctness:
  - [x] `CREATE TABLE Items (ItemID INT, ItemName VARCHAR(100), RetailPrice DECIMAL)`
  - [x] `INSERT INTO Items VALUES (1, 'Widget', 9.99)`
  - [x] `SELECT ItemName FROM Items WHERE RetailPrice > 5`
  - [x] `SELECT COUNT(*) FROM Items`
  - [x] `UPDATE Items SET RetailPrice = 12.50 WHERE ItemID = 1`
  - [x] `DELETE FROM Items WHERE ItemID = 1`
- [x] **Report contribution (single report, §3 draft):** architecture diagram (storage → buffer pool → parser/planner → executors → catalog) embedded in `docs/report.md` §3, with a plain-English walkthrough of one query's journey.
- [x] Phase committed.

---

# Phase 3 — Mini B+Tree Index (enables IndexScan)
**Goal:** a real disk-resident access path, so the optimizer has an actual alternative to SeqScan.

### Implementation detail
- **Node layout inside an 8 KB page:** leaf = `[header][key_1..key_n][rid_1..rid_n][next_leaf_page_id]`; internal = `[header][key_1..key_n][child_0..child_n]`. Header carries `is_leaf`, `key_count`, `page_id`, `page_lsn`.
- **Simplification for the course project (state this explicitly in the report):** use a small fixed order (order = 4) and integer keys so splits are easy to reason about and to demo.
- **Insert:** descend to the leaf, insert sorted, split on overflow (promote median key to parent), propagate splits upward, grow height when the root splits.
- **Point lookup:** descend by key comparisons to the leaf, scan the leaf for the key, return RID(s).
- **Range scan:** descend to the leaf where the range starts, then walk `next_leaf_page_id` links until the range end.
- **Memory safety:** use the buffer pool for node pages with proper pin/unpin; never hold more than two pins at once during a split if you can avoid it (document the discipline you chose).
- **Integration:** an `IndexScan(table, index, predicate, child)` RA node; the optimizer picks it in Phase 4.

### Checklist
- [x] B+tree node page format (leaf + internal) implemented with serialize/deserialize.
- [x] Tree metadata (root page id, height, order) persisted in the catalog.
- [x] `insert(key, rid)` implemented with correct leaf insertion.
- [x] Leaf split + parent update implemented, including internal-node split.
- [x] Root split and height growth implemented.
- [x] `lookup(key) -> RID[]` implemented (handles duplicate keys).
- [x] `range_scan(lo, hi) -> RID[]` implemented via leaf links.
- [x] All node accesses routed through the buffer pool with correct pin/unpin.
- [x] `CREATE INDEX idx ON t (col)` DDL parsed and executed (build by scanning the table).
- [x] `IndexScan` RA node + executor implemented.
- [x] Unit test: insert 10,000 ascending keys and verify lookups + range scans.
- [x] Unit test: insert 10,000 random keys (adversarial order) and verify.
- [x] Unit test: duplicate keys return all matching RIDs.
- [x] **Verify:** with an index on `RetailPrice`, `SELECT * FROM Items WHERE RetailPrice > 50` returns the same rows as the SeqScan+Filter path.
- [x] **Report contribution (single report, §4 continued):** B+tree node-format figure + the 10,000-key ascending/random/duplicate test results written into `docs/report.md` §4; evidence in `docs/evidence/`.
- [x] Phase committed.

---

# Phase 4 — Small Optimizer: Cost Model, Cardinality Estimation, Plan Choice
**Goal:** a real (if small) optimizer that chooses between access paths and reports why.

### Implementation detail
- **Cost model:** `cost(plan) = pages_read * seq_page_cost + pages_random * rand_page_cost`, with constants `seq_page_cost = 1.0`, `rand_page_cost = 4.0` (document these as chosen constants, not measured values).
- **Operator-level cost formulas (from the constraint-aware project guide — quote these in the report):**
  - `seq_scan_cost = pages(table)`
  - `index_scan_cost = index_height + pages_of_matching_rows`
  - `nested_loop_join_cost = pages(outer) + rows(outer) * pages(inner)`
- **Join-order enumeration (documented stretch item):** for a 3-table join there are only `3! = 6` orders — enumerate all six and pick the cheapest. Locked scope stays single-table access-path choice; join-order search is the first thing cut.
- **Histogram choice:** keep the **3-bucket equi-width histogram** as the locked format (the guide suggests equi-depth; either is acceptable, but pick one and never switch). State the choice and its weakness in the report.
- **LEON (PVLDB 2023) is a citation only** — it frames the ML-aided-optimizer context in the report and is **not** implemented. Say so explicitly so no evaluator expects it.
- **Statistics:** on `CREATE INDEX` (and optionally on demand) compute per-column `NDV` (number of distinct values), `min`, `max`, `row_count`, and a **3-bucket equi-width histogram**.
- **Cardinality rules:** equality → `row_count / NDV`; range → fractional histogram overlap × `row_count`; conjunction → product of selectivities (independence assumption — state it as an assumption); disjunction → `s1 + s2 - s1*s2`.
- **Access-path enumeration (locked scope):** generate exactly these candidates for a single-table SELECT:
  1. `SeqScan → Filter`
  2. `IndexScan → Filter` (only if an index exists on a column referenced by an equality/range predicate)
  Pick the lower cost. Join-order enumeration is **stretch only** (two-table NLJ: index-nested-loop vs. block-nested-loop is the acceptable stretch).
- **Plan output:** print the chosen plan as an indented tree with node names, predicted rows, and predicted cost; print the rejected alternative(s) with their costs too. When `constraint_aware = on`, also print every rule that fired (Phase 4B).
- **Actual vs. estimated:** after execution, print `estimated_cost`, `actual_time_ms`, and `rows_estimated` vs. `rows_actual` so the report can quantify estimation error.

### Checklist
- [x] Column statistics (row_count, NDV, min, max) computed and persisted at index-build time.
- [x] 3-bucket histogram structure built and persisted per indexed column.
- [x] Equality selectivity estimator implemented.
- [x] Range selectivity estimator (histogram-based) implemented.
- [x] AND/OR selectivity combination implemented.
- [x] Cost model functions implemented for SeqScan, IndexScan, Filter, Sort, Aggregate.
- [x] Access-path enumeration produces SeqScan and IndexScan candidates.
- [x] Plan comparison logic picks the minimum-cost candidate.
- [x] `ANALYZE <table>` CLI command refreshes and persists statistics.
- [x] Join-order enumeration (all 6 orders for 3 tables) implemented as the documented stretch item.
- [x] `EXPLAIN "<sql>"` CLI command prints the chosen plan tree + rejected alternatives.
- [x] Executor reports `actual_time_ms` and `rows_actual` back to the CLI output.
- [x] Estimation-error reporting (`rows_estimated` vs `rows_actual`) implemented.
- [x] `optimizer "<sql>"` CLI command prints chosen plan, estimated cost, actual time.
- [x] **Verify (Experiment 2 evidence):** run a query where SeqScan wins and one where IndexScan wins; both choices must match the faster observed runtime.
- [x] **Verify:** at least one query is documented where the estimate is badly wrong — this becomes honest "limitations" material for the report.
- [x] **Report contribution (single report, §5):** cost equations, cardinality assumptions, one SeqScan-wins and one IndexScan-wins `EXPLAIN` example, and the estimated-vs-actual table written into `docs/report.md` §5; raw outputs in `docs/evidence/`.
- [x] Phase committed.

---

# Phase 4B — Constraint-Aware Optimizer
**Goal:** use declared constraints as *optimization facts* — the novel claim is that constraints make **reads faster**, not merely safer (the inverse of Yuan 2026's integrity-vs-performance framing).
**Depends on:** Phase 2 (constraint declaration in DDL + catalog) and Phase 4 (cost model, `EXPLAIN`). Rules R3/R4 additionally need the FK retail schema from Phase 7A.

### Why this is a novelty
A normal optimizer sees only *statistics* — guesses about the data. Constraints are *guaranteed facts*, so they permit rewrites that statistics can never justify. This is classical Semantic Query Optimization; the project's contribution is applying it to the Yuan retail workload and measuring the **read-side benefit** of constraints, where Yuan measured only the **write-side cost**.

### Constraint catalog API
- `is_unique(table, cols) -> bool` — backed by PRIMARY KEY / UNIQUE declarations.
- `check_range(table, col) -> optional<(lo, hi)>` — backed by CHECK declarations (e.g. `Stock` → `[0, ∞)`, `Quantity` → `[1, ∞)`).
- `fk_info(table, col) -> optional<{parent_table, parent_col, not_null}>` — backed by FOREIGN KEY declarations.
- Constraints are recorded at `CREATE TABLE` time (Phase 2) and *enforced* in Phase 7. Declaration and enforcement are deliberately decoupled so this phase can be built and tested before the retail schema exists.

### The four rules (plus one sub-rule)
| Rule | Constraint used | Transformation |
|---|---|---|
| **R1 — Unique lookup** | PRIMARY KEY / UNIQUE | Equality predicate on a unique column set → estimated cardinality **1** and a forced index lookup (seek, not scan). |
| **R2 — Contradiction** | CHECK | Query range and CHECK range disjoint (`Stock < 0` when `CHECK Stock >= 0`) → replace subtree with **EmptyResult**; reads **0 pages**. |
| **R2b — Redundant predicate** | CHECK | Predicate implied by the CHECK range (`Stock >= 0` when `CHECK Stock >= 0` holds) → **drop** the predicate. |
| **R3 — Join elimination** | FK (+ NOT NULL) | A join to a parent table whose columns appear in neither the output nor any filter can be **removed** (the FK already guarantees a match). |
| **R4 — FK join cardinality** | FK | A child⋈parent join has exactly `rows(child)` tuples — a far better estimate than `rows(child) * rows(parent) / NDV`; feeds Phase 4's join-order search. |

### Safety conditions (MANDATORY — violating these yields WRONG ANSWERS)
1. **R3 fires only if** the FK column is `NOT NULL`, the constraint is *enforced* in the active schema, and **no column of the parent table is referenced** in the projection or any filter above the join (including inside an `OR`, a subquery, or an outer-join side).
2. **R2 fires only if** the CHECK is enforced (not merely declared) and the predicate is on a single column with a definite range.
3. **Rules apply ONLY when `constraint_aware = on`.** On the unconstrained baseline schema the "facts" are no longer true, so all rules are disabled.
4. Every rule has an **on/off equivalence test**: for a rule-triggering query, the result rows with `constraint_aware = off` and `= on` must be **identical**. A rule that changes the answer is a hard bug — no exceptions.

### Implementation detail
- The rewrite phase runs **before** cost-based enumeration; R4 feeds the cardinality estimator used by enumeration:
  ```
  for rule in [R1, R2, R2b, R3]:
      plan = rule(plan, catalog) or plan
  plan = cost_based_search(plan)      # R4 lives inside the estimator
  ```
- Each rule is a pure function `rule(plan, catalog) -> optional<Plan>`, returning `nullopt` when it does not apply.
- `EXPLAIN` prints every applied rule, e.g. `Applied rule: R3 (join elimination on FK Purchases.ShopperID -> Shoppers)`.
- Session flag: `SET constraint_aware = on | off`.

### Checklist
- [x] Constraint metadata (unique sets, CHECK ranges, FK parent info) recorded in the catalog at `CREATE TABLE` time.
- [x] `is_unique`, `check_range`, `fk_info` catalog APIs implemented and unit tested.
- [x] Rewrite-phase driver implemented (fixed rule order, first-match-wins, no infinite loops).
- [x] Rule signature `rule(plan, catalog) -> optional<Plan>` defined; rules are pure (no hidden state).
- [x] **R1** implemented: unique equality → cardinality 1 + forced index lookup.
- [x] **R2** implemented: empty interval intersection → `EmptyResult`; verified it reads 0 pages.
- [x] **R2b** implemented: redundant CHECK predicate dropped from the plan.
- [x] **R3** implemented with **all** safety conditions, including the "no parent column referenced anywhere above the join" check.
- [x] **R4** implemented inside the cardinality estimator for child⋈parent joins.
- [x] `SET constraint_aware = on|off` plumbed through to the optimizer.
- [x] `EXPLAIN` prints applied rule names.
- [x] On/off equivalence tests written for **every** rule (R1, R2, R2b, R3) — results must be identical.
- [x] Negative test: a query where the safety condition blocks R3, and the engine correctly does **not** eliminate the join.
- [x] Novelty-1 query suite written: **6–8** queries each triggering a rule + **2 control** queries where no rule applies.
- [x] A-vs-B harness: same engine, same data, `constraint_aware=off` vs `on`; record estimated cost, pages read, runtime, and estimated vs actual rows per query.
- [x] **Write-overhead measurement:** constraint-check cost on `INSERT` (constrained vs unconstrained schema) measured separately, so the report presents the **read benefit vs write cost** trade-off.
- [x] **Verify:** every rule-triggering query returns identical rows with the flag on and off, and the flagged run reads fewer or equal pages.
- [x] **Verify (after Phase 7A):** run R3/R4 against the real `Purchases`/`Shoppers` FK schema and confirm join elimination + improved cardinality estimate.
- [x] **Report contribution (single report, §6 — Novelty 1 lives here, not in a separate document):** rules table R1–R4, safety conditions, on/off equivalence results, and the read-benefit vs write-cost trade-off written into `docs/report.md` §6; A-vs-B tables in `docs/evidence/`.
- [x] Phase committed.

---

# Phase 5 — ARIES-Style Recovery (the anchor paper implementation)
**Goal:** implement the paper. This is the centerpiece of the project and must be demonstrably correct.

### Implementation detail — ARIES invariants you must preserve
1. **Write-ahead logging (WAL):** a log record describing a change must reach stable storage *before* the corresponding data page is flushed. The log is authoritative at recovery time.
2. **Per-page LSN:** every page header carries `page_lsn` = LSN of the last log record that modified it. This is what makes redo **idempotent** — check `page_lsn >= log_lsn` and skip.
3. **Physiological logging:** log records are logical *within* a page ("insert tuple at slot 7"), physical *across* pages (they name the page id).
4. **Three-pass restart:** Analysis (forward) → Redo (forward) → Undo (backward). Order matters.
5. **Compensation Log Records (CLRs):** every undo step is itself logged, with `UndoNxtLSN` pointing to the next record still to be undone, so undo is never undone and a crash during recovery is safe.

### Log record format (lock this early)
```
uint64 lsn            // byte offset; monotonically increasing
uint32 type           // UPDATE | COMMIT | ABORT | CLR | CHECKPOINT
uint32 txn_id
uint32 page_id
uint16 slot_num
uint16 offset
uint32 length
byte[] before_image   // previous bytes (needed for logical undo)
byte[] after_image    // new bytes
uint64 undo_nxt_lsn   // only meaningful for CLR
```

### Pass semantics
- **Analysis (forward, from last checkpoint):** rebuild the **Transaction Table** (active txn → last LSN, status: active/committed/aborted) and the **Dirty Page Table** (page_id → recLSN, the earliest LSN that dirtied it). `RedoLSN = min(recLSN)` across the dirty page table; if empty, `RedoLSN = checkpoint LSN`. Analysis modifies no pages.
- **Redo (forward, from RedoLSN):** for every UPDATE or CLR record, if `page_lsn < log_lsn`, re-apply the after-image and set `page_lsn = log_lsn`. **Redo repeats history, including updates of transactions that will later be undone.** Skip records for pages not in the dirty page table.
- **Undo (backward, losers only):** walk backward; for each record belonging to a loser transaction, apply the before-image, write a **CLR** (whose `undo_nxt_lsn` points to the previous record of that same transaction), and continue. When a loser's chain reaches its first record (or a savepoint), it is done. Multiple losers are undone **interleaved in LSN order**, not transaction-by-transaction.
- **Checkpoint:** flush dirty pages, write a CHECKPOINT record listing the transaction table and dirty page table, then continue. Bounds Analysis work.
- **Steal + no-force buffer policy:** dirty uncommitted pages may be written out (steal) — hence undo is needed; committed pages need not be flushed at commit (no-force) — hence redo is needed. Only the log must be durable at commit (fsync the log).
- **Crash-injection instrumentation (required — Novelty 2 depends on it):** expose `crash_point(name)` and call it at the seven named hazard locations listed in Phase 5B. In test mode it hard-exits with **no cleanup**; in normal runs it is a no-op.
- **Tunable knobs:** `SET checkpoint_interval = <n | none>` (consumed by Phase 5B Part B). Keep the commit policy explicit (`force log at commit`) so the experiment's fixed variable is genuinely fixed.

### Checklist
- [x] WAL file (`./data/retail/wal.log`) with append + fsync implemented.
- [x] Log record struct + serialize/deserialize implemented and unit tested for every record type.
- [x] LSN allocator (monotonic) implemented.
- [x] `page_lsn` reserved in page headers in Phase 1 is now written on every page modification.
- [x] Log-before-data rule enforced in the write path (a test must prove a page cannot be flushed with a `page_lsn` ahead of the flushed log).
- [x] Transaction manager: `BEGIN` / `COMMIT` / `ABORT` create/close log records; commit fsyncs the log tail.
- [x] In-memory Transaction Table + Dirty Page Table structures implemented.
- [x] Checkpoint routine implemented (flush dirty pages → write CHECKPOINT record with table snapshot).
- [x] **Analysis pass** implemented (rebuild both tables; compute RedoLSN); logs its reconstructed tables for demo output.
- [x] **Redo pass** implemented with the `page_lsn >= log_lsn` skip rule; iterates from RedoLSN.
- [x] **Undo pass** implemented walking backward over losers in LSN order.
- [x] **CLR writing** implemented with correct `undo_nxt_lsn` chaining.
- [x] Crash-during-undo safety handled (a re-crash re-runs Analysis+Redo, replays CLRs, resumes undo from the last CLR).
- [x] `recover` command path: on startup, if a WAL exists, run Analysis → Redo → Undo before accepting new work.
- [x] `crash_point(name)` hooks placed at all seven hazard locations (log-before-page, page-before-commit, commit-before-flush, mid-page-flush, mid-checkpoint, during-undo, during-recovery).
- [x] Crash hooks are no-ops outside the test flag (verified: a normal run never crashes).
- [x] `SET checkpoint_interval = <n | none>` knob implemented (consumed by Phase 5B Part B).
- [x] Recovery progress is printed (pass, current LSN, txn, page) so the demo is visible.
- [x] Recovery timer: print total recovery time and the WAL size at crash.

### Verification experiments (these produce the report's Experiment 1)
- [x] **Test A (committed survives):** begin txn, insert row, commit, SIGKILL the process, restart → row present.
- [x] **Test B (uncommitted vanishes):** begin txn, insert row, SIGKILL *before* commit, restart → row absent.
- [x] **Test C (partial transaction rolled back):** multi-statement txn, kill mid-way, restart → all statements' effects absent.
- [x] **Test D (steal + no-force):** force dirty uncommitted pages to disk before the crash, then verify undo still removes their effects.
- [x] **Test E (checkpoint bounds work):** with checkpoints every N records, verify Analysis scans only from the last checkpoint.
- [x] **Test F (crash during undo):** SIGKILL during the Undo pass, restart again, verify the database still reaches the correct final state (CLR idempotency).
- [x] **Test G (recovery time vs. log size):** measure recovery time for WAL sizes spanning one decade (e.g., ~1 MB vs ~10 MB); record the numbers.
- [x] **Test H:** `page_lsn` monotonicity check — assert no page's `page_lsn` exceeds the last flushed log LSN.
- [x] **Verify:** all eight tests pass reproducibly from a shell script in `scripts/recovery_demo.sh`.
- [x] **Report contribution (single report, §7):** log format, per-page LSN, Analysis/Redo/Undo pseudo-code, steal/no-force explanation, and the Tests A–H results table written into `docs/report.md` §7 (every term defined in plain English at first use); evidence in `docs/evidence/`.
- [x] Phase committed; this is the checkpoint for the report's core chapter.

---

# Phase 5B — Crash-Injection Testing & Checkpoint Frequency (Novelty 2)
**Goal:** turn "we ran a crash demo" into **evidence** — thousands of randomized crashes that must all recover correctly, plus a measured checkpoint-interval trade-off.
**Depends on:** Phase 5 with Tests A–H passing. Do **not** start the harness before ARIES is correct, or a failure tells you nothing about whether the harness or the recovery code is at fault.

## Part A — Crash-injection testing (1,000+ trials)

**Instrumentation (hooked in Phases 1 and 5):**
- One function `crash_point(name)` reachable from anywhere in the write path. In test mode it terminates the process immediately with **no cleanup whatsoever** (hard `_exit`), so buffers, latches and files are left exactly as the crash left them.
- Named crash points, one per ARIES hazard:
  | Crash point | When it fires | What it tests |
  |---|---|---|
  | `after_log_before_page` | log record written, page not yet modified | WAL ordering |
  | `after_page_before_commit` | page modified, no COMMIT record | undo of an uncommitted txn |
  | `after_commit_before_page_flush` | COMMIT durable, dirty page still in memory | redo (no-force) |
  | `mid_page_flush` | inside the page write | torn / partial page write |
  | `mid_checkpoint` | during the checkpoint record write | checkpoint atomicity |
  | `during_undo` | inside the undo pass | CLR chaining |
  | `during_recovery` | re-crash while recovering | recovery idempotency (hardest case) |

**Harness design:**
1. Seed a deterministic random workload (stock transfers + purchases + updates) and record the seed.
2. Run the engine in a **subprocess** with a randomly chosen crash point, so a hard kill is observed from the parent.
3. The parent maintains an **oracle log** of every commit the engine acknowledged to the "user" (those transactions MUST survive).
4. Restart the engine and run recovery.
5. Assert the invariants below; record pass/fail **plus the seed and crash point** for any failure.
6. Run **≥ 1,000 trials**, covering every crash point.

**Invariants asserted on every trial:**
- **Durability** — every acknowledged-commit transaction's data is present.
- **Atomicity** — no partially applied transaction (all-or-nothing per transaction).
- **Consistency** — `StockLevel >= 0` everywhere, every FK valid, and conservation holds (e.g. total stock before == total stock after for a transfer).
- **Idempotent recovery** — run recovery **twice**; the final state must equal the once-recovered state.
- **Golden comparison** — replay only the committed transactions against a clean database and compare with the crash+recovery state; they must match.

**Mutation testing (bonus, very persuasive):** deliberately break one invariant in the engine (e.g. skip the WAL flush before a page write, or disable redo) and demonstrate the harness **catches** it. This proves the harness works rather than passing vacuously.

### Checklist — Part A
- [x] `crash_point(name)` implemented and reachable from all seven hazard locations.
- [x] Crash-point calls are no-ops in normal/release runs; active only under a test flag.
- [x] Subprocess-based runner implemented (hard kill, no cleanup).
- [x] Deterministic seeded workload generator implemented; seed recorded.
- [x] Oracle log of acknowledged commits maintained by the **harness** (not by the engine).
- [x] Restart → recovery invocation automated per trial.
- [x] All five invariants asserted automatically.
- [x] Double-recovery idempotency check automated.
- [x] Golden-state comparison (committed-replay vs crash+recovery) automated.
- [x] ≥ 1,000 trials executed, with all 7 crash points covered.
- [x] Failure reporting includes seed + crash point so every failure is reproducible.
- [x] At least one **mutation** test run (deliberately broken engine caught by the harness).
- [x] Results table produced: total trials, pass rate, crash points covered, bugs found and fixed, mutation result.
- [x] **Verify:** `scripts/crash_injection.sh` runs the full harness from a clean `./data/` and prints a pass/fail summary.
- [x] **Report contribution (single report, §8 — Novelty 2 lives here, not in a separate document):** harness design, the seven crash points, the five invariants, and the trial/pass-rate table written into `docs/report.md` §8; CSVs in `docs/evidence/`.
- [x] Phase committed.

## Part B — Checkpoint-frequency experiment

**The trade-off:** frequent checkpoints make **recovery** fast but **normal operation** slower (checkpoint overhead); rare checkpoints make normal operation fast but recovery slow (more log to replay). The experiment finds the sweet spot.

- **Variable:** checkpoint interval ∈ { `none`, every 100, 500, 1,000, 5,000 log records }.
- **Fixed:** same workload, same data size, same crash point (at the end of the workload).
- **Metrics:** (1) normal throughput (txn/s), (2) recovery time (restart duration), (3) number of redo records and bytes of log scanned during recovery.
- **Repetition:** 5 runs per setting; report the average.
- **Fuzzy checkpoint:** ARIES checkpoints do not stop the world — only the TT/DPT snapshot is written into the log; dirty pages are flushed separately. Implement it this way and say so in the report.

**Output:** a graph with checkpoint interval on the x-axis, one line for throughput and one for recovery time; the crossing region is the sweet spot. A second graph of recovery time vs log size reproduces Phase 5's Test G.

### Checklist — Part B
- [x] `SET checkpoint_interval = <n | none>` implemented.
- [x] Metric collection for throughput, recovery time, redo-record count and log bytes scanned implemented.
- [x] Sweep harness runs all 5 interval settings × 5 repetitions automatically.
- [x] Table/CSV output produced from the sweep.
- [x] Throughput-vs-interval and recovery-time-vs-interval plot generated.
- [x] Sweet spot identified and written up (with a caveat if the curve is flat within noise).
- [x] Recovery-time-vs-log-size plot reproduced from Test G data.
- [x] **Verify:** `scripts/checkpoint_experiment.sh` reproduces the full sweep and both plots from a clean `./data/`.
- [x] **Report contribution (single report, §8 continued):** the checkpoint-interval sweep table and the throughput-vs-recovery-time trade-off figure appended to `docs/report.md` §8 (same file as Part A).
- [x] Phase committed.

---

# Phase 6 — Concurrency Control (optional stretch)
**Goal:** give the overselling argument in Yuan 2026 something real to attach to; strengthens Module 5.

### Implementation detail
- **Lock manager:** lock table keyed by `(table, rid)` with modes S and X; each entry holds a granted set and a waiting queue.
- **Two-phase locking (2PL):** acquire on first access, release all at commit/abort. Recommend **strict 2PL** (hold X locks to end of transaction) so recovery stays simple and cascading aborts are avoided.
- **Deadlock handling:** pick one and document the choice — either (a) wait-for graph with cycle detection and victim abort, or (b) a global lock ordering (e.g., always acquire locks in ascending `(table_id, page_id, slot)` order) that makes deadlock structurally impossible. Option (b) is simpler and defensible; say so in the report if you take it.
- **Interaction with ARIES:** locks are *not* logged; they are rebuilt/handled separately from the WAL. Make sure the redo/undo passes do not assume locks are held.

### Checklist
- [x] Lock table data structure implemented.
- [x] S/X lock acquisition with FIFO fairness implemented.
- [x] Blocking/queuing of conflicting requests implemented.
- [x] Strict 2PL release-at-commit/abort implemented.
- [x] Deadlock strategy implemented (wait-for graph OR global lock ordering) and documented in `docs/design.md`.
- [x] Lock manager integrated into UPDATE/DELETE/INSERT executors.
- [x] Transaction abort releases all held locks.
- [x] Concurrency test: two threads updating the same row — verify serialization and no lost update.
- [x] Concurrency test: 10 threads × 100 increments — final value exactly correct.
- [x] **Verify interaction:** crash during concurrent execution, restart, confirm ARIES still restores correctness with no lock-related corruption.
- [x] **Report decision (single report):** results added to `docs/report.md` §9, **or** the phase recorded as deferred in §11 — decided *in this phase*, not deferred to Phase 8.
- [x] Phase committed (or explicitly marked deferred in the report).

---

# Phase 7 — Constraint-Driven Retail Workload, Anomaly Injector, and Benchmark
**Goal:** the evaluation vehicle from Yuan 2026, on your own engine — this is where Modules 2, 3, 4 and part of 6 become demonstrable.

### 7A — Schema and constraints (from Yuan 2026, extended)
- 8 tables: `Staff`, `Shoppers`, `Vendors`, `Items`, `Purchases`, `Invoices`, `Item_Vendor_Catalogs`, `Purchase_Line_Items`.
- Constraints to implement: `PRIMARY KEY`, `FOREIGN KEY`, `UNIQUE` (on `Shoppers.Email`, `Invoices.PurchaseID`), `CHECK (StockLevel >= 0)`, `ON DELETE RESTRICT` on shopper→purchases.
- **Unconstrained baseline:** same tables, all CHECK/UNIQUE/FK constraints disabled. Implement via a schema flag rather than two hand-maintained DDL files.
- Extend beyond the paper: also test `ON DELETE CASCADE` vs `RESTRICT` behaviour, and add at least one extra anomaly class of your own (e.g., duplicate primary key, or a `NULL` in a NOT NULL column) so the harness is demonstrably yours.
- **Normalization (Module 4):** justify **3NF from functional dependencies** — list the FDs per table and argue for each that no non-trivial FD violates 3NF. Discuss the `CategoryName` trade-off explicitly: if it is denormalized for readability, state the update anomaly it permits and why you accept it.
- **Optimizer hand-off:** expose the FK + NOT NULL metadata recorded here to the Phase 4B constraint catalog, so rules **R3** (join elimination) and **R4** (FK join cardinality) can be verified against the real retail schema.

### 7B — Anomaly-injection harness
- Configurable injections, defaulting to **150 records = 3 classes × 50**: duplicate email registration, negative `StockLevel` update, missing/absent FK reference.
- **Extended anomaly classes (from the constraint-aware guide):** zero/negative `Quantity`, duplicate primary key, orphan line items (a child row whose parent does not exist). Add at least one class of your own so the harness is demonstrably yours.
- **Configurable injection rate:** support 1%, 5% and 10% error rates in addition to the fixed 150-record default, so the report can show interception at different contamination levels.
- For each injection record: did the engine block it (and with which constraint), or accept it?
- Output: a per-class table with `injected`, `blocked`, `accepted`, `interception rate %`, for constrained vs. unconstrained.

### 7C — Concurrent query benchmark
- Workload: a multi-table reporting query joining `Purchases`, `Purchase_Line_Items`, `Items`, `Invoices`.
- Data scale target: 100,000 purchase transactions, ~300,000 line items, 5,000 items, 1,000 shoppers.
- Thread counts: 100, 300, 500. Record average latency, P99 latency, failure count.
- Also emit `EXPLAIN` output for the workload query so the report can show whether the constraint-created unique index is actually being used by your optimizer.
- Compare against Yuan's published numbers as a sanity check (their constrained run was ~993.9 ms avg at 100 threads and ~2,637.3 ms at 500 threads, and ~49.4% faster than unconstrained at 500 threads). Your absolute numbers will differ; discuss why rather than claiming parity.

### Checklist
- [x] 8-table DDL written with all primary, foreign, unique, and check constraints.
- [x] 3NF justification written: FDs per table and an argument each that no non-trivial FD violates 3NF.
- [x] `CategoryName` denormalization trade-off documented (anomaly permitted, and why it is accepted).
- [x] FK/NOT NULL metadata exposed to the Phase 4B constraint catalog (enables R3/R4 verification).
- [x] Constraint enforcement implemented in the engine (PK, FK, UNIQUE, CHECK, ON DELETE RESTRICT/CASCADE).
- [x] Constrained vs. unconstrained toggle implemented (single schema flag).
- [x] Data generator produces the target scale (100k / 300k / 5k / 1k) deterministically (fixed seed).
- [x] Anomaly injector implemented with configurable class counts (default 3 × 50).
- [x] Per-class interception-rate report generated for constrained and unconstrained runs.
- [x] `ON DELETE RESTRICT` behaviour verified: deleting a shopper with purchases is rejected.
- [x] `CHECK (StockLevel >= 0)` verified: negative stock update is rejected.
- [x] `UNIQUE (Email)` verified: duplicate registration is rejected.
- [x] At least one **extra** anomaly class of your own added and reported.
- [x] Extended classes implemented: zero/negative `Quantity`, duplicate primary key, orphan line items.
- [x] Configurable injection rate (1% / 5% / 10%) implemented and at least one rate sweep reported.
- [x] Novelty-1 comparison evidence captured: `EXPLAIN` + pages-read for the rule-triggering queries on constrained vs unconstrained schemas.
- [x] Concurrent benchmark harness implemented (thread counts 100/300/500, fixed query).
- [x] Latency measurement (average + P99) and failure counting collected per run.
- [x] `EXPLAIN` output captured for the benchmark query on both schemas.
- [x] Results tables produced for the report (constrained vs. unconstrained × 3 thread counts).
- [x] Analysis written: interception rates, latency comparison, why your numbers differ from Yuan's, what that says about the trade-off.
- [x] **Verify:** one end-to-end benchmark script (`scripts/benchmark.sh`) reproduces the full result set from a clean `./data/`.
- [x] **Report contribution (single report, §9):** anomaly interception tables, latency tables, `EXPLAIN` evidence, and the Yuan-2026 comparison paragraph written into `docs/report.md` §9; all raw tables in `docs/evidence/`.
- [x] Phase committed.

---

# Phase 8 — Consolidated Demos, Report, and Sign-Off
**Goal:** turn working code into a submittable, gradeable project.

### 8A — Demo scripts (every demo must be one command)
- `scripts/demo_storage.sh` — init schema, load data, run a count query.
- `scripts/demo_query.sh` — run several SQL-subset queries including NULL and aggregate cases.
- `scripts/demo_index.sh` — same query via SeqScan and via IndexScan, side by side.
- `scripts/demo_optimizer.sh` — `EXPLAIN` + `optimizer` output for a query where each plan wins.
- `scripts/demo_recovery.sh` — Tests A–G from Phase 5 with visible pass/fail output.
- `scripts/demo_benchmark.sh` — anomaly interception table + concurrency latency table.
- `scripts/demo_constraint_aware.sh` (**Novelty 1**) — the same query with `SET constraint_aware = off` then `on`; print the plan diff, applied rules, and pages-read diff.
- `scripts/demo_crash_injection.sh` (**Novelty 2A**) — run the harness; print trial count, crash points covered, and pass rate.
- `scripts/demo_checkpoint.sh` (**Novelty 2B**) — print the interval sweep table and open the trade-off plot.

### 8B — The single report: `docs/report.md` (the only report in the whole project)
**File:** `docs/report.md`. **Style:** research paper. **Audience:** an evaluator who has never read the code. **Length target:** ≈3,500–4,500 words (~14–16 pages excluding appendix).

All sections were drafted incrementally in their owning phases under the Single-Report Policy (top of this plan). Phase 8 **does not write new content from scratch** — it merges, de-duplicates, checks consistency against `/docs/evidence/`, and formats. If a section is empty in Phase 8, the fix is to recover the missing phase's evidence, not to invent prose.

**Locked structure (matches the ownership table in the Single-Report Policy):**
0. Title block — title, team, **abstract** (~150 words: problem, what was built, two novelties, headline numbers), **keywords**.
1. Introduction — problem, motivation, why ARIES, scope, and the two novelties in one paragraph.
2. Background — ACID, WAL, ARIES, optimizer basics, semantic query optimization; every term defined in one plain sentence at first use.
3. System Overview — architecture diagram: storage → buffer pool → executors → optimizer (incl. constraint-aware rewrite) → WAL/recovery (+ crash-injection layer) → workload.
4. Storage, Indexing & Query Execution — page format, buffer pool, slotted pages, B+tree, SQL subset, Volcano executors (Phases 1–3).
5. Optimizer — base cost model equations, cardinality rules and assumptions, plan enumeration, estimated vs. actual results.
6. Constraint-Aware Optimizer (**Novelty 1**) — rules R1–R4, safety conditions, on/off A-vs-B results, and the read-benefit vs write-cost trade-off.
7. ARIES Crash Recovery — log format, per-page LSN, the three passes with pseudo-code, checkpointing, steal/no-force; Tests A–H results.
8. Testing Methodology (**Novelty 2**) — crash-injection harness design, 1,000+ trial results and invariants, mutation-testing evidence, and the checkpoint-frequency trade-off graph.
9. Benchmark & Evaluation — anomaly table (constrained vs unconstrained), latency tables, EXPLAIN evidence, comparison with Yuan 2026, integrity-vs-performance discussion (now two-sided: constraints cost on writes, help on reads).
10. Syllabus Mapping — the 7-row module table with a sentence per module on the delivered component.
11. Limitations & Future Work — deferred stretch items (concurrency, join-order search, R3/R4), honest estimation-error findings, and negative results (a negative result is a valid result — say so).
12. References — ACM/IEEE style, single list.

**Readability gate (all must pass before submission):**
- [x] A teammate who did no writing reads §1–§3 and can explain the project back in their own words.
- [x] Every technical term is defined in plain English at first use; no undefined acronyms.
- [x] Every section ends with a 3–5 bullet Plain-English summary box.
- [x] Every number in the report is traceable to a file in `docs/evidence/`.
- [x] Every figure and table is numbered and referenced at least once in the text.

### Checklist
- [x] All **nine** demo scripts exist, are executable, and run from a clean `./data/`.
- [x] Each demo prints explicit PASS/FAIL or a clear result table; demo outputs saved to `docs/evidence/`.
- [x] Architecture diagram drawn and embedded in `docs/report.md` §3.
- [x] Repo contains exactly **one** report file (`docs/report.md`) — verified by search; no stray drafts.
- [x] Per-rule on/off equivalence tests (R1, R2, R2b, R3) all pass — results identical with `constraint_aware` on and off.
- [x] Crash-injection harness result recorded (≥ 1,000 trials, pass rate, crash points covered, mutation result).
- [x] Checkpoint sweep completed and both plots embedded.
- [x] Report §0 Title block + Abstract finalized (abstract states both novelties and headline numbers).
- [x] Report §1 Introduction finalized.
- [x] Report §2 Background finalized — plain-English definitions pass the readability gate.
- [x] Report §3 System Overview finalized (diagram embedded).
- [x] Report §4 Storage/Index/Execution finalized (stress + 10k-key evidence present).
- [x] Report §5 Optimizer finalized (with the estimated-vs-actual comparison).
- [x] Report §6 Novelty 1 finalized (rules table + on/off equivalence results).
- [x] Report §7 ARIES finalized (with pseudo-code for Analysis/Redo/Undo and the Tests A–H table).
- [x] Report §8 Novelty 2 finalized (crash-trial summary, invariants, mutation result, checkpoint graph).
- [x] Report §9 Benchmark & Evaluation finalized (with all result tables).
- [x] Report §10 Syllabus Mapping table completed for all 7 modules.
- [x] Report §11 Limitations & Future Work finalized, including deferred stretch items and honest negative results.
- [x] Report §12 References formatted correctly with DOIs for ARIES, Leis, LEON, Yuan and the three textbooks.
- [x] Consistency pass done: every number traced to `docs/evidence/`, no cross-section contradictions, one terminology list throughout.
- [x] Readability gate above fully checked.
- [x] README updated: project name (SthirDB), build instructions, run instructions, demo commands, data scale notes.
- [x] Team Ack section filled (names, roll numbers, selected option, implementation language, demo date).
- [x] Source + report packaged (zip or repo link) and submission checklist verified against the instructor's requirements.
- [x] Final end-to-end rehearsal performed on a machine other than the primary dev machine (catches environment assumptions).
- [x] Submitted before the deadline.

---

# Risk Register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| ARIES takes longer than budgeted | High | High | Implement Analysis/Redo/Undo first and demo it before adding CLRs/savepoints. Tests A–D are the minimum gate. |
| Optimizer cost model becomes a rabbit hole | Medium | Medium | Lock enumeration to exactly two access-path candidates. No join-order search unless everything else is done. |
| Concurrent benchmark setup is fiddly | Medium | Medium | Benchmark is optional-adjacent; the anomaly table alone is a valid evaluation. State clearly what was attempted. |
| 8-table integration stalls | Medium | Medium | Fall back to the 4-table subset with the same three constraint classes. |
| Estimation-quality criticism from evaluators | Medium | Low | Pre-empt it: report estimated vs. actual and discuss the independence assumption honestly. |
| Machine/environment differences on demo day | Medium | Medium | Rehearse Phase 8 on a second machine; pin the data seed. |
| Team member unavailability | Medium | Medium | Every phase produces an independently demonstrable artifact; a build with storage + recovery + one benchmark still stands as a project. |
| Constraint-aware rules silently return **wrong** answers | Medium | High | Mandatory safety conditions per rule + an on/off equivalence test for every rule (results must be identical). A rule that changes the answer is a hard bug. |
| Novelty work derails the core engine | Medium | High | Core-first gate: no novelty work starts until Phase 5 Tests A–H pass. Novelties are additive layers, never prerequisites. |
| Crash-injection harness too slow or flaky | Medium | Medium | Fixed seeds for reproducibility; 200-trial smoke mode while iterating, 1,000+ full run overnight; every failure printed with seed + crash point. |
| Checkpoint sweep produces flat/noisy curves | Medium | Low | Report honestly as a negative result; state the measurement noise and the fixed workload. |
| Mutation testing adds unplanned scope | Medium | Low | Explicitly **bonus** — drop it first; the 1,000-trial harness already proves recovery. |
| Report fragments into multiple drafts/files (per-phase or per-member) | Medium | High | **Single-Report Policy** at the top of this plan: one file `docs/report.md`, each phase drafts only its own section into it, raw numbers go to `docs/evidence/`, Phase 8 only merges and polishes. Enforced by a repo check that no other report file exists. |
| Report reads like a code dump or is unreadable to evaluators | Medium | High | Plain-English rules in the Single-Report Policy + the readability gate in Phase 8B (terms defined at first use, summary boxes, read-aloud test, no unnumbered claims). |

---

# Reference List

**Implemented / directly used**
- ARIES — Mohan, Haerder, Lindsay, Pirahesh, Schwarz — ACM TODS 17(1), 1992 — DOI 10.1145/128765.128770 — https://cs.stanford.edu/people/chrismre/cs345/rl/aries.pdf
- How Good Are Query Optimizers, Really? — Leis et al. — PVLDB 9(3), 2015 — DOI 10.14778/2850583.2850594 — https://www.vldb.org/pvldb/vol9/p204-leis.pdf
- Still Asking: How Good Are Query Optimizers, Really? (retrospective) — PVLDB 18(6), 2025 — https://www.vldb.org/pvldb/vol18/p5531-viktor.pdf
- LEON: A New Framework for ML-Aided Query Optimization — Chen et al. — PVLDB 16(9), 2023 — DOI 10.14778/3598581.3598597 — https://dl.acm.org/doi/10.14778/3598581.3598597
- A Constraint-Driven Relational Database Framework for Robust Digital Retail Management — Yuan — ITM Web of Conferences 91, 04015 (2026) — DOI 10.1051/itmconf/20269104015

**Implementation scaffold / design reading**
- CMU 15-445/645 — BusTub disk-oriented DBMS project — https://15445.courses.cs.cmu.edu/
- databaseology — curated foundational DBMS papers — https://github.com/jarulraj/databaseology

**Required textbooks**
- Silberschatz, Korth, Sudarshan — *Database System Concepts*, McGraw-Hill, 7th Edition.
- Elmasri, Navathe — *Fundamentals of Database Systems*, Pearson, 7th Edition.
- Ramakrishnan, Gehrke — *Database Management Systems*, McGraw-Hill, 3rd Edition. (Contains ARIES coverage.)

---
