#include "catalog/catalog.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace fs = std::filesystem;

namespace sthir {

std::string DataTypeToString(DataType type) {
  switch (type) {
    case DataType::INT: return "INT";
    case DataType::VARCHAR: return "VARCHAR";
    case DataType::DECIMAL: return "DECIMAL";
  }
  return "UNKNOWN";
}

std::optional<DataType> StringToDataType(const std::string& str) {
  std::string s = str;
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (s == "INT" || s == "INTEGER") return DataType::INT;
  if (s == "VARCHAR" || s == "CHAR" || s == "TEXT" || s == "STRING") return DataType::VARCHAR;
  if (s == "DECIMAL" || s == "DOUBLE" || s == "FLOAT" || s == "NUMERIC") return DataType::DECIMAL;
  return std::nullopt;
}

int TableSchema::FindColumnIndex(const std::string& col_name) const {
  std::string target = col_name;
  for (char& c : target) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  // 1. Exact match (case-insensitive)
  for (size_t i = 0; i < columns.size(); ++i) {
    std::string cur = columns[i].name;
    for (char& c : cur) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (cur == target) return static_cast<int>(i);
  }

  // 2. Target has "table.col" -> match "col"
  auto target_dot = target.find('.');
  if (target_dot != std::string::npos) {
    std::string col_only = target.substr(target_dot + 1);
    for (size_t i = 0; i < columns.size(); ++i) {
      std::string cur = columns[i].name;
      for (char& c : cur) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (cur == col_only) return static_cast<int>(i);
    }
  }

  // 3. Schema column has "table.col" -> match target
  for (size_t i = 0; i < columns.size(); ++i) {
    std::string cur = columns[i].name;
    for (char& c : cur) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto cur_dot = cur.find('.');
    if (cur_dot != std::string::npos) {
      std::string col_only = cur.substr(cur_dot + 1);
      if (col_only == target) return static_cast<int>(i);
    }
  }

  return -1;
}

const Column* TableSchema::GetColumn(const std::string& col_name) const {
  const int idx = FindColumnIndex(col_name);
  if (idx >= 0 && idx < static_cast<int>(columns.size())) {
    return &columns[static_cast<size_t>(idx)];
  }
  return nullptr;
}

bool TableSchema::HasColumn(const std::string& col_name) const {
  return FindColumnIndex(col_name) >= 0;
}

const ColumnStats* TableSchema::GetColumnStats(const std::string& col_name) const {
  const int idx = FindColumnIndex(col_name);
  if (idx < 0) return nullptr;
  const std::string& name = columns[static_cast<size_t>(idx)].name;
  for (const auto& stats : column_stats) {
    if (stats.column_name == name) return &stats;
  }
  return nullptr;
}

// -------------------------------------------------------------
// Simple JSON reader/writer
// -------------------------------------------------------------
namespace json {

struct Value;
using Object = std::vector<std::pair<std::string, Value>>;
using Array = std::vector<Value>;

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
  Type type = Type::Null;
  bool bool_val = false;
  double num_val = 0.0;
  std::string str_val;
  Array arr_val;
  Object obj_val;

  const Value* Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : obj_val) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
};

class Parser {
 public:
  explicit Parser(const std::string& src) : src_(src), pos_(0) {}

  bool Parse(Value* out, std::string* err) {
    SkipWhitespace();
    if (!ParseValue(out)) {
      if (err) *err = "JSON parse error at pos " + std::to_string(pos_);
      return false;
    }
    SkipWhitespace();
    return true;
  }

 private:
  void SkipWhitespace() {
    while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                                  src_[pos_] == '\r' || src_[pos_] == '\n')) {
      ++pos_;
    }
  }

  char Peek() const {
    if (pos_ >= src_.size()) return '\0';
    return src_[pos_];
  }

  char Get() {
    if (pos_ >= src_.size()) return '\0';
    return src_[pos_++];
  }

  bool ParseValue(Value* out) {
    SkipWhitespace();
    char c = Peek();
    if (c == '{') return ParseObject(out);
    if (c == '[') return ParseArray(out);
    if (c == '"') return ParseString(out);
    if (c == 't' || c == 'f') return ParseBool(out);
    if (c == 'n') return ParseNull(out);
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber(out);
    return false;
  }

  bool ParseObject(Value* out) {
    if (Get() != '{') return false;
    out->type = Type::Object;
    out->obj_val.clear();
    SkipWhitespace();
    if (Peek() == '}') {
      Get();
      return true;
    }
    while (pos_ < src_.size()) {
      SkipWhitespace();
      if (Peek() != '"') return false;
      Value key_val;
      if (!ParseString(&key_val)) return false;
      SkipWhitespace();
      if (Get() != ':') return false;
      Value val;
      if (!ParseValue(&val)) return false;
      out->obj_val.push_back({key_val.str_val, std::move(val)});
      SkipWhitespace();
      char next_c = Get();
      if (next_c == '}') return true;
      if (next_c != ',') return false;
    }
    return false;
  }

  bool ParseArray(Value* out) {
    if (Get() != '[') return false;
    out->type = Type::Array;
    out->arr_val.clear();
    SkipWhitespace();
    if (Peek() == ']') {
      Get();
      return true;
    }
    while (pos_ < src_.size()) {
      Value val;
      if (!ParseValue(&val)) return false;
      out->arr_val.push_back(std::move(val));
      SkipWhitespace();
      char next_c = Get();
      if (next_c == ']') return true;
      if (next_c != ',') return false;
    }
    return false;
  }

  bool ParseString(Value* out) {
    if (Get() != '"') return false;
    std::string s;
    while (pos_ < src_.size()) {
      char c = Get();
      if (c == '"') {
        out->type = Type::String;
        out->str_val = s;
        return true;
      }
      if (c == '\\') {
        if (pos_ >= src_.size()) return false;
        char esc = Get();
        if (esc == '"') s.push_back('"');
        else if (esc == '\\') s.push_back('\\');
        else if (esc == '/') s.push_back('/');
        else if (esc == 'b') s.push_back('\b');
        else if (esc == 'f') s.push_back('\f');
        else if (esc == 'n') s.push_back('\n');
        else if (esc == 'r') s.push_back('\r');
        else if (esc == 't') s.push_back('\t');
        else s.push_back(esc);
      } else {
        s.push_back(c);
      }
    }
    return false;
  }

  bool ParseNumber(Value* out) {
    size_t start = pos_;
    if (Peek() == '-') Get();
    while (std::isdigit(static_cast<unsigned char>(Peek()))) Get();
    if (Peek() == '.') {
      Get();
      while (std::isdigit(static_cast<unsigned char>(Peek()))) Get();
    }
    std::string s = src_.substr(start, pos_ - start);
    try {
      out->type = Type::Number;
      out->num_val = std::stod(s);
      return true;
    } catch (...) {
      return false;
    }
  }

  bool ParseBool(Value* out) {
    if (src_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      out->type = Type::Bool;
      out->bool_val = true;
      return true;
    }
    if (src_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      out->type = Type::Bool;
      out->bool_val = false;
      return true;
    }
    return false;
  }

  bool ParseNull(Value* out) {
    if (src_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      out->type = Type::Null;
      return true;
    }
    return false;
  }

  const std::string& src_;
  size_t pos_;
};

void EscapeString(const std::string& s, std::ostream& os) {
  os << '"';
  for (char c : s) {
    if (c == '"') os << "\\\"";
    else if (c == '\\') os << "\\\\";
    else if (c == '\n') os << "\\n";
    else if (c == '\r') os << "\\r";
    else if (c == '\t') os << "\\t";
    else os << c;
  }
  os << '"';
}

}  // namespace json

// -------------------------------------------------------------
// Catalog implementation
// -------------------------------------------------------------

Catalog::Catalog(std::string catalog_path) : catalog_path_(std::move(catalog_path)) {}

std::string Catalog::ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool Catalog::IsValidIdentifier(const std::string& id) {
  if (id.empty()) return false;
  if (!std::isalpha(static_cast<unsigned char>(id[0])) && id[0] != '_') return false;
  for (size_t i = 1; i < id.size(); ++i) {
    char c = id[i];
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  }
  return true;
}

bool Catalog::Load(std::string* err) {
  tables_.clear();
  if (!fs::exists(catalog_path_)) {
    return true;  // Empty catalog is valid on fresh start
  }

  std::ifstream f(catalog_path_);
  if (!f.is_open()) {
    if (err) *err = "cannot open catalog file: " + catalog_path_;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string content = ss.str();

  json::Parser p(content);
  json::Value root;
  if (!p.Parse(&root, err)) return false;

  const json::Value* tbl_arr = root.Find("tables");
  if (!tbl_arr || tbl_arr->type != json::Type::Array) {
    if (err) *err = "catalog missing 'tables' array";
    return false;
  }

  for (const auto& item : tbl_arr->arr_val) {
    if (item.type != json::Type::Object) continue;
    TableSchema s;
    const json::Value* v_name = item.Find("name");
    if (v_name && v_name->type == json::Type::String) s.table_name = v_name->str_val;

    const json::Value* v_fp = item.Find("file_path");
    if (v_fp && v_fp->type == json::Type::String) s.data_file_path = v_fp->str_val;

    const json::Value* v_cols = item.Find("columns");
    if (v_cols && v_cols->type == json::Type::Array) {
      for (const auto& c_item : v_cols->arr_val) {
        if (c_item.type != json::Type::Object) continue;
        Column col;
        const auto* cn = c_item.Find("name");
        if (cn && cn->type == json::Type::String) col.name = cn->str_val;

        const auto* ct = c_item.Find("type");
        if (ct && ct->type == json::Type::String) {
          auto dt = StringToDataType(ct->str_val);
          if (dt) col.type = *dt;
        }

        const auto* cl = c_item.Find("length");
        if (cl && cl->type == json::Type::Number) col.length = static_cast<uint32_t>(cl->num_val);

        const auto* cnn = c_item.Find("not_null");
        if (cnn && cnn->type == json::Type::Bool) col.not_null = cnn->bool_val;

        const auto* cpk = c_item.Find("primary_key");
        if (cpk && cpk->type == json::Type::Bool) col.primary_key = cpk->bool_val;

        const auto* cu = c_item.Find("unique");
        if (cu && cu->type == json::Type::Bool) col.unique = cu->bool_val;

        s.columns.push_back(col);
      }
    }

    const json::Value* v_pk = item.Find("primary_key");
    if (v_pk && v_pk->type == json::Type::Array) {
      for (const auto& pk_col : v_pk->arr_val) {
        if (pk_col.type == json::Type::String) s.primary_key_columns.push_back(pk_col.str_val);
      }
    }

    const json::Value* v_u = item.Find("unique_constraints");
    if (v_u && v_u->type == json::Type::Array) {
      for (const auto& u_group : v_u->arr_val) {
        if (u_group.type != json::Type::Array) continue;
        std::vector<std::string> grp;
        for (const auto& col_entry : u_group.arr_val) {
          if (col_entry.type == json::Type::String) grp.push_back(col_entry.str_val);
        }
        if (!grp.empty()) s.unique_constraints.push_back(grp);
      }
    }

    const json::Value* v_fk = item.Find("foreign_keys");
    if (v_fk && v_fk->type == json::Type::Array) {
      for (const auto& fk_item : v_fk->arr_val) {
        if (fk_item.type != json::Type::Object) continue;
        ForeignKey fk;
        const auto* fkn = fk_item.Find("name");
        if (fkn && fkn->type == json::Type::String) fk.name = fkn->str_val;

        const auto* fkr = fk_item.Find("ref_table");
        if (fkr && fkr->type == json::Type::String) fk.ref_table_name = fkr->str_val;

        const auto* fkc = fk_item.Find("columns");
        if (fkc && fkc->type == json::Type::Array) {
          for (const auto& c : fkc->arr_val) {
            if (c.type == json::Type::String) fk.column_names.push_back(c.str_val);
          }
        }

        const auto* fkc_ref = fk_item.Find("ref_columns");
        if (fkc_ref && fkc_ref->type == json::Type::Array) {
          for (const auto& c : fkc_ref->arr_val) {
            if (c.type == json::Type::String) fk.ref_column_names.push_back(c.str_val);
          }
        }

        const auto* fk_act = fk_item.Find("on_delete");
        if (fk_act && fk_act->type == json::Type::String && fk_act->str_val == "CASCADE") {
          fk.on_delete = ForeignKey::OnDelete::CASCADE;
        } else {
          fk.on_delete = ForeignKey::OnDelete::RESTRICT;
        }
        s.foreign_keys.push_back(fk);
      }
    }

    const json::Value* v_chk = item.Find("check_constraints");
    if (v_chk && v_chk->type == json::Type::Array) {
      for (const auto& chk_item : v_chk->arr_val) {
        if (chk_item.type != json::Type::Object) continue;
        CheckConstraint chk;
        const auto* chkn = chk_item.Find("name");
        if (chkn && chkn->type == json::Type::String) chk.name = chkn->str_val;
        const auto* chke = chk_item.Find("expression");
        if (chke && chke->type == json::Type::String) chk.expression = chke->str_val;
        s.check_constraints.push_back(chk);
      }
    }

    const json::Value* v_stats = item.Find("statistics");
    if (v_stats && v_stats->type == json::Type::Array) {
      for (const auto& st_item : v_stats->arr_val) {
        if (st_item.type != json::Type::Object) continue;
        ColumnStats stats;
        const auto* scn = st_item.Find("column_name");
        if (scn && scn->type == json::Type::String) stats.column_name = scn->str_val;
        const auto* src6 = st_item.Find("row_count");
        if (src6 && src6->type == json::Type::Number) {
          stats.row_count = static_cast<uint64_t>(src6->num_val);
        }
        const auto* snull = st_item.Find("null_count");
        if (snull && snull->type == json::Type::Number) {
          stats.null_count = static_cast<uint64_t>(snull->num_val);
        }
        const auto* sndv = st_item.Find("ndv");
        if (sndv && sndv->type == json::Type::Number) {
          stats.ndv = static_cast<uint64_t>(sndv->num_val);
        }
        const auto* smin = st_item.Find("min");
        if (smin && smin->type == json::Type::Number) stats.min_value = smin->num_val;
        const auto* smax = st_item.Find("max");
        if (smax && smax->type == json::Type::Number) stats.max_value = smax->num_val;
        const auto* shist = st_item.Find("histogram");
        if (shist && shist->type == json::Type::Array) {
          for (const auto& b_item : shist->arr_val) {
            if (b_item.type != json::Type::Object) continue;
            HistogramBucket bucket;
            const auto* blo = b_item.Find("lo");
            if (blo && blo->type == json::Type::Number) bucket.lo = blo->num_val;
            const auto* bhi = b_item.Find("hi");
            if (bhi && bhi->type == json::Type::Number) bucket.hi = bhi->num_val;
            const auto* bcnt = b_item.Find("count");
            if (bcnt && bcnt->type == json::Type::Number) {
              bucket.count = static_cast<uint64_t>(bcnt->num_val);
            }
            stats.histogram.push_back(bucket);
          }
        }
        stats.valid = stats.row_count > 0 && stats.ndv > 0;
        if (!stats.column_name.empty()) s.column_stats.push_back(std::move(stats));
      }
    }

    const json::Value* v_idx = item.Find("indexes");
    if (v_idx && v_idx->type == json::Type::Array) {
      for (const auto& idx_item : v_idx->arr_val) {
        if (idx_item.type != json::Type::Object) continue;
        IndexMetadata idx;
        const auto* in = idx_item.Find("index_name");
        if (in && in->type == json::Type::String) idx.index_name = in->str_val;
        const auto* ic = idx_item.Find("column_name");
        if (ic && ic->type == json::Type::String) idx.column_name = ic->str_val;
        const auto* ir = idx_item.Find("root_page_id");
        if (ir && ir->type == json::Type::Number) idx.root_page_id = static_cast<uint32_t>(ir->num_val);
        const auto* ih = idx_item.Find("height");
        if (ih && ih->type == json::Type::Number) idx.height = static_cast<uint32_t>(ih->num_val);
        const auto* io = idx_item.Find("order");
        if (io && io->type == json::Type::Number) idx.order = static_cast<uint32_t>(io->num_val);
        s.indexes.push_back(idx);
      }
    }

    tables_[ToLower(s.table_name)] = s;
  }

  return true;
}

bool Catalog::Save(std::string* err) const {
  std::error_code ec;
  fs::path p(catalog_path_);
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
      if (err) *err = "failed to create directory for catalog: " + ec.message();
      return false;
    }
  }

  std::ofstream f(catalog_path_);
  if (!f.is_open()) {
    if (err) *err = "cannot open catalog for writing: " + catalog_path_;
    return false;
  }

  f << "{\n  \"tables\": [\n";
  size_t t_idx = 0;
  for (const auto& kv : tables_) {
    const TableSchema& s = kv.second;
    f << "    {\n";
    f << "      \"name\": "; json::EscapeString(s.table_name, f); f << ",\n";
    f << "      \"file_path\": "; json::EscapeString(s.data_file_path, f); f << ",\n";

    // columns
    f << "      \"columns\": [\n";
    for (size_t c_idx = 0; c_idx < s.columns.size(); ++c_idx) {
      const Column& col = s.columns[c_idx];
      f << "        {\"name\": "; json::EscapeString(col.name, f);
      f << ", \"type\": \"" << DataTypeToString(col.type) << "\"";
      f << ", \"length\": " << col.length;
      f << ", \"not_null\": " << (col.not_null ? "true" : "false");
      f << ", \"primary_key\": " << (col.primary_key ? "true" : "false");
      f << ", \"unique\": " << (col.unique ? "true" : "false") << "}";
      if (c_idx + 1 < s.columns.size()) f << ",";
      f << "\n";
    }
    f << "      ],\n";

    // primary_key
    f << "      \"primary_key\": [";
    for (size_t i = 0; i < s.primary_key_columns.size(); ++i) {
      json::EscapeString(s.primary_key_columns[i], f);
      if (i + 1 < s.primary_key_columns.size()) f << ", ";
    }
    f << "],\n";

    // unique_constraints
    f << "      \"unique_constraints\": [\n";
    for (size_t i = 0; i < s.unique_constraints.size(); ++i) {
      f << "        [";
      for (size_t j = 0; j < s.unique_constraints[i].size(); ++j) {
        json::EscapeString(s.unique_constraints[i][j], f);
        if (j + 1 < s.unique_constraints[i].size()) f << ", ";
      }
      f << "]";
      if (i + 1 < s.unique_constraints.size()) f << ",";
      f << "\n";
    }
    f << "      ],\n";

    // foreign_keys
    f << "      \"foreign_keys\": [\n";
    for (size_t i = 0; i < s.foreign_keys.size(); ++i) {
      const auto& fk = s.foreign_keys[i];
      f << "        {\"name\": "; json::EscapeString(fk.name, f);
      f << ", \"ref_table\": "; json::EscapeString(fk.ref_table_name, f);
      f << ", \"columns\": [";
      for (size_t j = 0; j < fk.column_names.size(); ++j) {
        json::EscapeString(fk.column_names[j], f);
        if (j + 1 < fk.column_names.size()) f << ", ";
      }
      f << "], \"ref_columns\": [";
      for (size_t j = 0; j < fk.ref_column_names.size(); ++j) {
        json::EscapeString(fk.ref_column_names[j], f);
        if (j + 1 < fk.ref_column_names.size()) f << ", ";
      }
      f << "], \"on_delete\": \"" << (fk.on_delete == ForeignKey::OnDelete::CASCADE ? "CASCADE" : "RESTRICT") << "\"}";
      if (i + 1 < s.foreign_keys.size()) f << ",";
      f << "\n";
    }
    f << "      ],\n";

    // check_constraints
    f << "      \"check_constraints\": [\n";
    for (size_t i = 0; i < s.check_constraints.size(); ++i) {
      const auto& chk = s.check_constraints[i];
      f << "        {\"name\": "; json::EscapeString(chk.name, f);
      f << ", \"expression\": "; json::EscapeString(chk.expression, f);
      f << "}";
      if (i + 1 < s.check_constraints.size()) f << ",";
      f << "\n";
    }
    f << "      ],\n";

    // indexes
    f << "      \"indexes\": [\n";
    for (size_t i = 0; i < s.indexes.size(); ++i) {
      const auto& idx = s.indexes[i];
      f << "        {\"index_name\": "; json::EscapeString(idx.index_name, f);
      f << ", \"column_name\": "; json::EscapeString(idx.column_name, f);
      f << ", \"root_page_id\": " << idx.root_page_id;
      f << ", \"height\": " << idx.height;
      f << ", \"order\": " << idx.order << "}";
      if (i + 1 < s.indexes.size()) f << ",";
      f << "\n";
    }
    f << "      ],\n";

    // statistics (one entry per analyzed column)
    f << "      \"statistics\": [\n";
    for (size_t i = 0; i < s.column_stats.size(); ++i) {
      const auto& stats = s.column_stats[i];
      f << "        {\"column_name\": "; json::EscapeString(stats.column_name, f);
      f << ", \"row_count\": " << stats.row_count;
      f << ", \"null_count\": " << stats.null_count;
      f << ", \"ndv\": " << stats.ndv;
      f << ", \"min\": " << std::setprecision(17) << stats.min_value;
      f << ", \"max\": " << stats.max_value;
      f << ", \"histogram\": [";
      for (size_t b = 0; b < stats.histogram.size(); ++b) {
        f << "{\"lo\": " << stats.histogram[b].lo << ", \"hi\": " << stats.histogram[b].hi
          << ", \"count\": " << stats.histogram[b].count << "}";
        if (b + 1 < stats.histogram.size()) f << ", ";
      }
      f << "]}";
      if (i + 1 < s.column_stats.size()) f << ",";
      f << "\n";
    }
    f << "      ]\n";

    f << "    }";
    if (t_idx + 1 < tables_.size()) f << ",";
    f << "\n";
    ++t_idx;
  }
  f << "  ]\n}\n";

  return true;
}

bool Catalog::CreateTable(const TableSchema& schema, std::string* err) {
  if (!IsValidIdentifier(schema.table_name)) {
    if (err) *err = "invalid table name: '" + schema.table_name + "'";
    return false;
  }

  const std::string lower_name = ToLower(schema.table_name);
  if (tables_.find(lower_name) != tables_.end()) {
    if (err) *err = "table '" + schema.table_name + "' already exists";
    return false;
  }

  if (schema.columns.empty()) {
    if (err) *err = "table must have at least one column";
    return false;
  }

  std::vector<std::string> seen_cols;
  for (const auto& col : schema.columns) {
    if (!IsValidIdentifier(col.name)) {
      if (err) *err = "invalid column name: '" + col.name + "'";
      return false;
    }
    std::string lower_col = ToLower(col.name);
    if (std::find(seen_cols.begin(), seen_cols.end(), lower_col) != seen_cols.end()) {
      if (err) *err = "duplicate column name: '" + col.name + "'";
      return false;
    }
    seen_cols.push_back(lower_col);

    if (col.type == DataType::VARCHAR && col.length == 0) {
      if (err) *err = "VARCHAR column '" + col.name + "' must have length > 0";
      return false;
    }
  }

  // Validate declared primary key
  for (const auto& pk_col : schema.primary_key_columns) {
    if (!schema.HasColumn(pk_col)) {
      if (err) *err = "primary key column '" + pk_col + "' not found in table '" + schema.table_name + "'";
      return false;
    }
  }

  // Validate declared unique constraints
  for (const auto& u_group : schema.unique_constraints) {
    for (const auto& col_name : u_group) {
      if (!schema.HasColumn(col_name)) {
        if (err) *err = "unique constraint column '" + col_name + "' not found in table '" + schema.table_name + "'";
        return false;
      }
    }
  }

  // Validate foreign key local columns
  for (const auto& fk : schema.foreign_keys) {
    for (const auto& col_name : fk.column_names) {
      if (!schema.HasColumn(col_name)) {
        if (err) *err = "foreign key column '" + col_name + "' not found in table '" + schema.table_name + "'";
        return false;
      }
    }
  }

  TableSchema saved_schema = schema;
  if (saved_schema.data_file_path.empty()) {
    saved_schema.data_file_path = "./data/retail/" + lower_name + ".dat";
  }

  tables_[lower_name] = saved_schema;
  return Save(err);
}

bool Catalog::DropTable(const std::string& table_name, std::string* err) {
  const std::string lower = ToLower(table_name);
  auto it = tables_.find(lower);
  if (it == tables_.end()) {
    if (err) *err = "table '" + table_name + "' does not exist";
    return false;
  }
  tables_.erase(it);
  return Save(err);
}

const TableSchema* Catalog::GetTable(const std::string& table_name) const {
  const std::string lower = ToLower(table_name);
  auto it = tables_.find(lower);
  if (it != tables_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool Catalog::HasTable(const std::string& table_name) const {
  return tables_.find(ToLower(table_name)) != tables_.end();
}

std::vector<std::string> Catalog::GetTableNames() const {
  std::vector<std::string> names;
  names.reserve(tables_.size());
  for (const auto& kv : tables_) {
    names.push_back(kv.second.table_name);
  }
  return names;
}

bool Catalog::AddIndex(const std::string& table_name, const IndexMetadata& idx, std::string* err) {
  const std::string lower = ToLower(table_name);
  auto it = tables_.find(lower);
  if (it == tables_.end()) {
    if (err) *err = "table '" + table_name + "' not found";
    return false;
  }
  if (!it->second.HasColumn(idx.column_name)) {
    if (err) *err = "column '" + idx.column_name + "' not found in table '" + table_name + "'";
    return false;
  }
  it->second.indexes.push_back(idx);
  return Save(err);
}

bool Catalog::SetColumnStats(const std::string& table_name, const ColumnStats& stats,
                             std::string* err) {
  const std::string lower = ToLower(table_name);
  auto it = tables_.find(lower);
  if (it == tables_.end()) {
    if (err) *err = "table '" + table_name + "' not found";
    return false;
  }
  if (!it->second.HasColumn(stats.column_name)) {
    if (err) *err = "column '" + stats.column_name + "' not found in table '" + table_name + "'";
    return false;
  }

  ColumnStats stored = stats;
  stored.column_name = it->second.columns[static_cast<size_t>(it->second.FindColumnIndex(stats.column_name))].name;
  stored.valid = true;

  bool replaced = false;
  for (auto& existing : it->second.column_stats) {
    if (existing.column_name == stored.column_name) {
      existing = stored;
      replaced = true;
      break;
    }
  }
  if (!replaced) it->second.column_stats.push_back(stored);
  return Save(err);
}

namespace {

std::string LowerCopy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Case-insensitive column-list comparison (order-sensitive: FK column order
// must match the referenced key's order).
bool SameColumns(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerCopy(a[i]) != LowerCopy(b[i])) return false;
  }
  return true;
}

}  // namespace

std::vector<ForeignKeyInfo> Catalog::GetForeignKeyInfo() const {
  std::vector<ForeignKeyInfo> out;
  for (const auto& entry : tables_) {
    const TableSchema& child = entry.second;
    for (const auto& fk : child.foreign_keys) {
      ForeignKeyInfo info;
      info.child_table = child.table_name;
      info.child_columns = fk.column_names;
      info.parent_table = fk.ref_table_name;
      info.parent_columns = fk.ref_column_names;
      info.on_delete = fk.on_delete;

      // R3 safety: the edge only guarantees a parent match when every child
      // column is NOT NULL (declared NOT NULL or part of the primary key).
      info.child_not_null = true;
      for (const auto& col_name : fk.column_names) {
        const Column* col = child.GetColumn(col_name);
        if (col == nullptr || (!col->not_null && !col->primary_key)) {
          info.child_not_null = false;
          break;
        }
      }

      // R4 input: the referenced columns must be a declared key (primary key
      // or a UNIQUE group) for child-join-parent to yield rows(child).
      info.parent_unique = false;
      if (const TableSchema* parent = GetTable(fk.ref_table_name); parent != nullptr) {
        info.parent_unique = SameColumns(parent->primary_key_columns, fk.ref_column_names);
        for (const auto& group : parent->unique_constraints) {
          if (SameColumns(group, fk.ref_column_names)) {
            info.parent_unique = true;
            break;
          }
        }
      }
      out.push_back(std::move(info));
    }
  }
  // tables_ is an unordered_map; sort so consumers see a stable order.
  std::sort(out.begin(), out.end(), [](const ForeignKeyInfo& a, const ForeignKeyInfo& b) {
    if (a.child_table != b.child_table) return a.child_table < b.child_table;
    if (a.parent_table != b.parent_table) return a.parent_table < b.parent_table;
    return a.child_columns < b.child_columns;
  });
  return out;
}

std::vector<ForeignKeyInfo> Catalog::GetForeignKeysOn(const std::string& table_name) const {
  const std::string lower = ToLower(table_name);
  std::vector<ForeignKeyInfo> out;
  for (auto& info : GetForeignKeyInfo()) {
    if (ToLower(info.child_table) == lower) out.push_back(std::move(info));
  }
  return out;
}

// -------------------------------------------------------------
// Phase 4B constraint catalog
// -------------------------------------------------------------

bool Catalog::IsUnique(const std::string& table_name,
                       const std::vector<std::string>& columns) const {
  const TableSchema* table = GetTable(table_name);
  if (table == nullptr || columns.empty()) return false;
  if (SameColumns(table->primary_key_columns, columns)) return true;
  for (const auto& group : table->unique_constraints) {
    if (SameColumns(group, columns)) return true;
  }
  return false;
}

namespace {

// Parses one simple CHECK expression of the form `<column> <op> <number>`.
// Anything more elaborate (expressions, ANDs, functions) is deliberately
// rejected: an unparsed CHECK yields no fact, and no fact means no rule fires.
bool ParseSimpleCheck(const std::string& expr, std::string* column, std::string* op,
                      double* value) {
  size_t i = 0;
  auto skip = [&]() {
    while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i]))) ++i;
  };
  skip();
  const size_t name_start = i;
  while (i < expr.size() &&
         (std::isalnum(static_cast<unsigned char>(expr[i])) || expr[i] == '_' || expr[i] == '.')) {
    ++i;
  }
  if (i == name_start) return false;
  *column = expr.substr(name_start, i - name_start);
  skip();

  if (i >= expr.size()) return false;
  const char c = expr[i];
  if (c == '>') {
    *op = (i + 1 < expr.size() && expr[i + 1] == '=') ? ">=" : ">";
  } else if (c == '<') {
    *op = (i + 1 < expr.size() && expr[i + 1] == '=') ? "<=" : "<";
  } else if (c == '=') {
    *op = "=";
  } else {
    return false;
  }
  i += op->size();
  skip();

  const char* start = expr.c_str() + i;
  char* end = nullptr;
  *value = std::strtod(start, &end);
  if (end == start) return false;
  while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
  return *end == '\0';
}

}  // namespace

std::optional<Catalog::CheckRange> Catalog::GetCheckRange(
    const std::string& table_name, const std::string& column_name) const {
  const TableSchema* table = GetTable(table_name);
  if (table == nullptr) return std::nullopt;
  const int col_idx = table->FindColumnIndex(column_name);
  if (col_idx < 0) return std::nullopt;
  const std::string& target = table->columns[static_cast<size_t>(col_idx)].name;

  std::optional<CheckRange> out;
  for (const auto& check : table->check_constraints) {
    std::string col;
    std::string op;
    double value = 0.0;
    if (!ParseSimpleCheck(check.expression, &col, &op, &value)) continue;
    if (ToLower(col) != ToLower(target)) continue;

    CheckRange range;
    if (op == ">=") {
      range.has_lo = true;
      range.lo = value;
      range.lo_closed = true;
    } else if (op == ">") {
      range.has_lo = true;
      range.lo = value;
      range.lo_closed = false;
    } else if (op == "<=") {
      range.has_hi = true;
      range.hi = value;
      range.hi_closed = true;
    } else if (op == "<") {
      range.has_hi = true;
      range.hi = value;
      range.hi_closed = false;
    } else if (op == "=") {
      range.has_lo = true;
      range.lo = value;
      range.lo_closed = true;
      range.has_hi = true;
      range.hi = value;
      range.hi_closed = true;
    } else {
      continue;
    }

    if (!out.has_value()) {
      out = range;
    } else {
      // Two CHECKs on the same column: keep the tighter bound.
      if (range.has_lo && (!out->has_lo || range.lo > out->lo ||
                           (range.lo == out->lo && !range.lo_closed))) {
        out->has_lo = true;
        out->lo = range.lo;
        out->lo_closed = range.lo_closed;
      }
      if (range.has_hi && (!out->has_hi || range.hi < out->hi ||
                           (range.hi == out->hi && !range.hi_closed))) {
        out->has_hi = true;
        out->hi = range.hi;
        out->hi_closed = range.hi_closed;
      }
    }
  }
  return out;
}

std::optional<ForeignKeyInfo> Catalog::GetForeignKeyFrom(const std::string& child_table,
                                                         const std::string& child_column) const {
  for (auto& info : GetForeignKeysOn(child_table)) {
    if (info.child_columns.size() != 1) continue;  // single-column edges only
    if (LowerCopy(info.child_columns[0]) == LowerCopy(child_column)) return info;
  }
  return std::nullopt;
}

}  // namespace sthir
