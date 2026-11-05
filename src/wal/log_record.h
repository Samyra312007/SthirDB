#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sthir {

enum class LogRecordType : uint32_t {
  INVALID = 0,
  BEGIN = 1,
  UPDATE = 2,
  COMMIT = 3,
  ABORT = 4,
  CLR = 5,
  CHECKPOINT = 6
};

const char* LogRecordTypeToString(LogRecordType type);

// In-memory representations for snapshotting Transaction Table and Dirty Page Table
struct TxnTableEntry {
  uint32_t txn_id = 0;
  uint32_t status = 0;  // 0: RUNNING, 1: COMMITTED, 2: ABORTED
  uint64_t last_lsn = 0;
};

struct DptTableEntry {
  uint32_t table_id = 0;  // page ids are per table file, so both are needed
  uint32_t page_id = 0;
  uint64_t rec_lsn = 0;
};

// Fixed and variable payload for ARIES log records conforming to plan.md.
struct LogRecord {
  uint64_t lsn = 0;             // Monotonically increasing byte offset in log file
  LogRecordType type = LogRecordType::INVALID;
  uint32_t txn_id = 0;
  uint64_t prev_lsn = 0;        // Previous LSN for the same transaction
  uint32_t table_id = 0;        // Which table file page_id belongs to
  uint32_t page_id = 0;
  uint16_t slot_num = 0;
  uint16_t offset = 0;
  uint32_t length = 0;
  std::vector<char> before_image;  // Previous bytes for undo
  std::vector<char> after_image;   // New bytes for redo
  uint64_t undo_nxt_lsn = 0;       // Meaningful for CLR: next record to undo

  // Populated for CHECKPOINT records
  std::vector<TxnTableEntry> txn_table;
  std::vector<DptTableEntry> dpt_table;

  // Constructors for common record types
  static LogRecord MakeBegin(uint32_t txn_id);
  static LogRecord MakeCommit(uint32_t txn_id, uint64_t prev_lsn);
  static LogRecord MakeAbort(uint32_t txn_id, uint64_t prev_lsn);
  static LogRecord MakeUpdate(uint32_t txn_id, uint64_t prev_lsn, uint32_t table_id,
                              uint32_t page_id, uint16_t slot_num, uint16_t offset,
                              std::vector<char> before_image,
                              std::vector<char> after_image);
  static LogRecord MakeCLR(uint32_t txn_id, uint64_t prev_lsn, uint32_t table_id,
                           uint32_t page_id, uint16_t slot_num, uint16_t offset,
                           std::vector<char> after_image, uint64_t undo_nxt_lsn);
  static LogRecord MakeCheckpoint(std::vector<TxnTableEntry> tt,
                                  std::vector<DptTableEntry> dpt);

  // Binary serialization
  std::vector<char> Serialize() const;
  static bool Deserialize(const char* buf, size_t size, LogRecord* out, size_t* bytes_read = nullptr);

  std::string ToString() const;
};

}  // namespace sthir
