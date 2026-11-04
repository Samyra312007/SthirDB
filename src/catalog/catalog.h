#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

namespace sthir {

enum class DataType {
  INT,
  VARCHAR,
  DECIMAL
};

std::string DataTypeToString(DataType type);
std::optional<DataType> StringToDataType(const std::string& str);

struct Column {
  std::string name;
  DataType type = DataType::INT;
  uint32_t length = 4;  // for VARCHAR(n), n bytes; for INT, 4; for DECIMAL, 8
  bool not_null = false;
  bool primary_key = false;
  bool unique = false;
};

struct ForeignKey {
  std::string name;
  std::vector<std::string> column_names;
  std::string ref_table_name;
  std::vector<std::string> ref_column_names;
  enum class OnDelete { RESTRICT, CASCADE } on_delete = OnDelete::RESTRICT;
};

struct CheckConstraint {
  std::string name;
  std::string expression;
};

// Phase 4B constraint-catalog view of one foreign-key edge, with the two
// facts the constraint-aware rules need:
//   * child_not_null — safety condition of R3 (join elimination): an FK that
//     cannot be NULL guarantees every child row has a parent, so a join whose
//     parent columns are unused can be dropped.
//   * parent_unique  — input of R4 (FK join cardinality): when the parent
//     columns are a declared key, child⋈parent yields exactly rows(child).
// Whether the edge is *enforced* in the running session is the engine's
// `constraints_enabled` session flag, read separately by the rules.
struct ForeignKeyInfo {
  std::string child_table;
  std::vector<std::string> child_columns;
  std::string parent_table;
  std::vector<std::string> parent_columns;
  bool child_not_null = false;
  bool parent_unique = false;
  ForeignKey::OnDelete on_delete = ForeignKey::OnDelete::RESTRICT;
};

// Locked histogram format for Phase 4: exactly kHistogramBuckets equi-width
// buckets over [min_value, max_value]. The last bucket is closed at max_value.
inline constexpr uint32_t kHistogramBuckets = 3;

struct HistogramBucket {
  double lo = 0.0;  // inclusive
  double hi = 0.0;  // exclusive, except the last bucket
  uint64_t count = 0;
};

// Per-column statistics gathered by CREATE INDEX and ANALYZE. Values are stored
// in the column's value domain (INT as an integer, DECIMAL as a double), so a
// histogram bucket reads naturally next to a WHERE literal.
struct ColumnStats {
  std::string column_name;
  uint64_t row_count = 0;   // non-NULL values seen
  uint64_t null_count = 0;
  uint64_t ndv = 0;         // number of distinct values
  double min_value = 0.0;
  double max_value = 0.0;
  std::vector<HistogramBucket> histogram;  // kHistogramBuckets entries when valid
  bool valid = false;
};

// Tree metadata for one B+tree index. root_page_id and height describe the
// on-disk tree; order is recorded so a future build can reject order mismatches.
struct IndexMetadata {
  std::string index_name;
  std::string column_name;
  uint32_t root_page_id = 0;
  uint32_t height = 0;
  uint32_t order = 4;
};

struct TableSchema {
  std::string table_name;
  std::string data_file_path;
  std::vector<Column> columns;
  std::vector<std::string> primary_key_columns;
  std::vector<std::vector<std::string>> unique_constraints;
  std::vector<ForeignKey> foreign_keys;
  std::vector<CheckConstraint> check_constraints;
  std::vector<IndexMetadata> indexes;
  std::vector<ColumnStats> column_stats;

  int FindColumnIndex(const std::string& col_name) const;
  const Column* GetColumn(const std::string& col_name) const;
  bool HasColumn(const std::string& col_name) const;
  // Statistics for one column, or nullptr when the column was never analyzed.
  const ColumnStats* GetColumnStats(const std::string& col_name) const;
};

class Catalog {
 public:
  explicit Catalog(std::string catalog_path = "./data/retail/catalog.json");

  // Loads table metadata from catalog.json if it exists.
  bool Load(std::string* err = nullptr);

  // Persists all table metadata to catalog.json.
  bool Save(std::string* err = nullptr) const;

  // Validates schema and registers a new table.
  bool CreateTable(const TableSchema& schema, std::string* err = nullptr);

  // Drops a table from the catalog.
  bool DropTable(const std::string& table_name, std::string* err = nullptr);

  // Looks up a table (case-insensitive).
  const TableSchema* GetTable(const std::string& table_name) const;
  bool HasTable(const std::string& table_name) const;

  std::vector<std::string> GetTableNames() const;

  // Registers an index for a table.
  bool AddIndex(const std::string& table_name, const IndexMetadata& idx, std::string* err = nullptr);

  // Replaces (or adds) the statistics of one column and persists the catalog.
  bool SetColumnStats(const std::string& table_name, const ColumnStats& stats,
                      std::string* err = nullptr);

  // Every foreign-key edge in the schema, sorted for deterministic output.
  // This is the Phase 7 -> Phase 4B hand-off that lets rules R3 (join
  // elimination) and R4 (FK join cardinality) run against the real retail
  // schema.
  std::vector<ForeignKeyInfo> GetForeignKeyInfo() const;

  // Foreign keys whose child table is `table_name` (case-insensitive).
  std::vector<ForeignKeyInfo> GetForeignKeysOn(const std::string& table_name) const;

  // ---- Phase 4B constraint catalog -------------------------------------
  // These three read *guaranteed* facts out of the DDL. Unlike statistics
  // they cannot be wrong, which is what lets rules R1-R4 rewrite a plan
  // instead of merely guessing at its cardinality. Whether the facts are
  // actually enforced in the running session is decided by the caller
  // (`constraints_enabled`); these only report what was declared.

  // True when `columns` is a PRIMARY KEY or a declared UNIQUE group.
  bool IsUnique(const std::string& table_name,
                const std::vector<std::string>& columns) const;

  // Inclusive/exclusive value interval implied by a CHECK on one column,
  // e.g. `CHECK (StockLevel >= 0)` -> [0, inf). Empty when no single-column
  // CHECK names that column.
  struct CheckRange {
    bool has_lo = false;
    double lo = 0.0;
    bool lo_closed = true;
    bool has_hi = false;
    double hi = 0.0;
    bool hi_closed = true;
  };
  std::optional<CheckRange> GetCheckRange(const std::string& table_name,
                                          const std::string& column_name) const;

  // The FK edge leaving `child_table` through `child_column`, if declared.
  std::optional<ForeignKeyInfo> GetForeignKeyFrom(const std::string& child_table,
                                                  const std::string& child_column) const;

  const std::string& catalog_path() const { return catalog_path_; }

  static bool IsValidIdentifier(const std::string& id);

 private:
  static std::string ToLower(std::string s);

  std::string catalog_path_;
  std::unordered_map<std::string, TableSchema> tables_;  // keyed by lower-case table name
};

}  // namespace sthir
