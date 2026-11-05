#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/buffer_pool.h"
#include "wal/log_record.h"
#include "wal/wal_manager.h"

namespace sthir {

enum class TxnStatus : uint32_t {
  RUNNING = 0,
  COMMITTED = 1,
  ABORTED = 2
};

struct TxnInfo {
  uint32_t txn_id = 0;
  TxnStatus status = TxnStatus::RUNNING;
  uint64_t last_lsn = 0;
};

struct RecoveryReport {
  size_t analysis_records_scanned = 0;
  size_t redo_records_applied = 0;
  size_t redo_records_skipped = 0;
  size_t undo_records_applied = 0;
  std::vector<uint32_t> loser_txns;
  uint64_t checkpoint_lsn = 0;
  uint64_t redo_start_lsn = 0;
  double recovery_time_ms = 0.0;
  uint64_t wal_size_bytes = 0;
  // Bytes from the redo start LSN to the end of the WAL: the work the Redo
  // pass had to look at. Novelty 2B plots it against the checkpoint interval.
  uint64_t log_scanned_bytes = 0;

  std::string ToString() const;
};

class RecoveryManager {
 public:
  RecoveryManager(WalManager* wal, BufferPool* bp);
  ~RecoveryManager() = default;

  // Transaction manager operations
  uint32_t Begin();
  bool Commit(uint32_t txn_id);
  bool Abort(uint32_t txn_id);

  // Called whenever a page modification (insert, update, delete) occurs.
  // `table_id` names the table file the page belongs to, because page ids are
  // only unique inside one table.
  uint64_t LogUpdate(uint32_t txn_id, uint32_t table_id, uint32_t page_id,
                     uint16_t slot_num, uint16_t offset,
                     std::vector<char> before_image,
                     std::vector<char> after_image);

  // Writes a checkpoint record to WAL with current Transaction Table and Dirty Page Table
  uint64_t Checkpoint(bool fuzzy = true);

  // ARIES Three-Pass Restart Recovery
  RecoveryReport Recover();

  // Inspect current tables (useful for tests and verification)
  std::unordered_map<uint32_t, TxnInfo> GetTransactionTable() const;
  // Keyed by (table_id << 32) | page_id so two tables' page 0 never collide.
  std::unordered_map<uint64_t, uint64_t> GetDirtyPageTable() const;
  static uint64_t PageKey(uint32_t table_id, uint32_t page_id) {
    return (static_cast<uint64_t>(table_id) << 32) | page_id;
  }

  void SetBufferPool(BufferPool* bp) {
    std::lock_guard<std::mutex> lock(latch_);
    bp_ = bp;
  }

  // Registers the buffer pool of table `table_id` so recovery can replay a
  // logged page into the right file. ExecutionEngine registers every table
  // before Recover() runs.
  void RegisterTable(uint32_t table_id, BufferPool* bp) {
    std::lock_guard<std::mutex> lock(latch_);
    pools_by_table_[table_id] = bp;
    if (bp_ == nullptr) bp_ = bp;
  }

  void SetCheckpointInterval(size_t interval_records) {
    std::lock_guard<std::mutex> lock(latch_);
    checkpoint_interval_ = interval_records;
  }

  size_t checkpoint_interval() const {
    std::lock_guard<std::mutex> lock(latch_);
    return checkpoint_interval_;
  }

 private:
  void CheckIntervalTrigger();

  mutable std::mutex latch_;
  WalManager* wal_;
  BufferPool* bp_;

  // Buffer pool for the table a record belongs to, falling back to bp_ when the
  // catalog no longer knows the table.
  BufferPool* PoolFor(uint32_t table_id) {
    const auto it = pools_by_table_.find(table_id);
    if (it != pools_by_table_.end()) return it->second;
    return bp_;
  }

  uint32_t next_txn_id_ = 1;
  std::unordered_map<uint32_t, TxnInfo> txn_table_;
  std::unordered_map<uint64_t, uint64_t> dirty_page_table_;  // page key -> rec_lsn
  std::unordered_map<uint32_t, BufferPool*> pools_by_table_;

  size_t checkpoint_interval_ = 0;  // 0 = none/disabled
  size_t records_since_checkpoint_ = 0;
  uint64_t last_checkpoint_lsn_ = 0;
  uint64_t recovery_start_lsn_ = 0;
};

}  // namespace sthir
