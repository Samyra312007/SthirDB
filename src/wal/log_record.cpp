#include "wal/log_record.h"

#include <cstring>
#include <sstream>

namespace sthir {

const char* LogRecordTypeToString(LogRecordType type) {
  switch (type) {
    case LogRecordType::BEGIN: return "BEGIN";
    case LogRecordType::UPDATE: return "UPDATE";
    case LogRecordType::COMMIT: return "COMMIT";
    case LogRecordType::ABORT: return "ABORT";
    case LogRecordType::CLR: return "CLR";
    case LogRecordType::CHECKPOINT: return "CHECKPOINT";
    default: return "INVALID";
  }
}

LogRecord LogRecord::MakeBegin(uint32_t txn_id) {
  LogRecord rec;
  rec.type = LogRecordType::BEGIN;
  rec.txn_id = txn_id;
  rec.prev_lsn = 0;
  return rec;
}

LogRecord LogRecord::MakeCommit(uint32_t txn_id, uint64_t prev_lsn) {
  LogRecord rec;
  rec.type = LogRecordType::COMMIT;
  rec.txn_id = txn_id;
  rec.prev_lsn = prev_lsn;
  return rec;
}

LogRecord LogRecord::MakeAbort(uint32_t txn_id, uint64_t prev_lsn) {
  LogRecord rec;
  rec.type = LogRecordType::ABORT;
  rec.txn_id = txn_id;
  rec.prev_lsn = prev_lsn;
  return rec;
}

LogRecord LogRecord::MakeUpdate(uint32_t txn_id, uint64_t prev_lsn, uint32_t table_id,
                                uint32_t page_id, uint16_t slot_num, uint16_t offset,
                                std::vector<char> before_image,
                                std::vector<char> after_image) {
  LogRecord rec;
  rec.type = LogRecordType::UPDATE;
  rec.txn_id = txn_id;
  rec.prev_lsn = prev_lsn;
  rec.table_id = table_id;
  rec.page_id = page_id;
  rec.slot_num = slot_num;
  rec.offset = offset;
  rec.length = static_cast<uint32_t>(after_image.size());
  rec.before_image = std::move(before_image);
  rec.after_image = std::move(after_image);
  return rec;
}

LogRecord LogRecord::MakeCLR(uint32_t txn_id, uint64_t prev_lsn, uint32_t table_id,
                             uint32_t page_id, uint16_t slot_num, uint16_t offset,
                             std::vector<char> after_image, uint64_t undo_nxt_lsn) {
  LogRecord rec;
  rec.type = LogRecordType::CLR;
  rec.txn_id = txn_id;
  rec.prev_lsn = prev_lsn;
  rec.table_id = table_id;
  rec.page_id = page_id;
  rec.slot_num = slot_num;
  rec.offset = offset;
  rec.length = static_cast<uint32_t>(after_image.size());
  rec.after_image = std::move(after_image);
  rec.undo_nxt_lsn = undo_nxt_lsn;
  return rec;
}

LogRecord LogRecord::MakeCheckpoint(std::vector<TxnTableEntry> tt,
                                    std::vector<DptTableEntry> dpt) {
  LogRecord rec;
  rec.type = LogRecordType::CHECKPOINT;
  rec.txn_table = std::move(tt);
  rec.dpt_table = std::move(dpt);
  return rec;
}

std::vector<char> LogRecord::Serialize() const {
  // Compute total size:
  // 4 (total_size) + 8 (lsn) + 4 (type) + 4 (txn_id) + 8 (prev_lsn)
  // + 4 (table_id) + 4 (page_id) + 2 (slot_num) + 2 (offset) + 4 (length)
  // + 8 (undo_nxt_lsn) + 4 (before_len) + before_bytes + 4 (after_len) + after_bytes
  // + (if checkpoint: 4 (tt_count) + count*16 + 4 (dpt_count) + count*16)
  size_t payload_size = 4 + 8 + 4 + 4 + 8 + 4 + 4 + 2 + 2 + 4 + 8 +
                        4 + before_image.size() +
                        4 + after_image.size();

  if (type == LogRecordType::CHECKPOINT) {
    payload_size += 4 + txn_table.size() * (4 + 4 + 8);
    payload_size += 4 + dpt_table.size() * (4 + 4 + 8);
  }

  std::vector<char> buf(payload_size);
  char* ptr = buf.data();

  auto write_val = [&ptr](const auto& val) {
    std::memcpy(ptr, &val, sizeof(val));
    ptr += sizeof(val);
  };

  uint32_t total_size = static_cast<uint32_t>(payload_size);
  write_val(total_size);
  write_val(lsn);
  uint32_t raw_type = static_cast<uint32_t>(type);
  write_val(raw_type);
  write_val(txn_id);
  write_val(prev_lsn);
  write_val(table_id);
  write_val(page_id);
  write_val(slot_num);
  write_val(offset);
  write_val(length);
  write_val(undo_nxt_lsn);

  uint32_t before_len = static_cast<uint32_t>(before_image.size());
  write_val(before_len);
  if (before_len > 0) {
    std::memcpy(ptr, before_image.data(), before_len);
    ptr += before_len;
  }

  uint32_t after_len = static_cast<uint32_t>(after_image.size());
  write_val(after_len);
  if (after_len > 0) {
    std::memcpy(ptr, after_image.data(), after_len);
    ptr += after_len;
  }

  if (type == LogRecordType::CHECKPOINT) {
    uint32_t tt_count = static_cast<uint32_t>(txn_table.size());
    write_val(tt_count);
    for (const auto& entry : txn_table) {
      write_val(entry.txn_id);
      write_val(entry.status);
      write_val(entry.last_lsn);
    }

    uint32_t dpt_count = static_cast<uint32_t>(dpt_table.size());
    write_val(dpt_count);
    for (const auto& entry : dpt_table) {
      write_val(entry.table_id);
      write_val(entry.page_id);
      write_val(entry.rec_lsn);
    }
  }

  return buf;
}

bool LogRecord::Deserialize(const char* buf, size_t size, LogRecord* out, size_t* bytes_read) {
  if (!buf || size < 4) return false;

  uint32_t total_size = 0;
  std::memcpy(&total_size, buf, 4);
  if (total_size < 52 || total_size > size) {
    return false;
  }

  const char* ptr = buf + 4;
  auto read_val = [&ptr](auto& val) {
    std::memcpy(&val, ptr, sizeof(val));
    ptr += sizeof(val);
  };

  read_val(out->lsn);
  uint32_t raw_type = 0;
  read_val(raw_type);
  out->type = static_cast<LogRecordType>(raw_type);
  read_val(out->txn_id);
  read_val(out->prev_lsn);
  read_val(out->table_id);
  read_val(out->page_id);
  read_val(out->slot_num);
  read_val(out->offset);
  read_val(out->length);
  read_val(out->undo_nxt_lsn);

  uint32_t before_len = 0;
  read_val(before_len);
  if (ptr + before_len > buf + total_size) return false;
  out->before_image.resize(before_len);
  if (before_len > 0) {
    std::memcpy(out->before_image.data(), ptr, before_len);
    ptr += before_len;
  }

  uint32_t after_len = 0;
  read_val(after_len);
  if (ptr + after_len > buf + total_size) return false;
  out->after_image.resize(after_len);
  if (after_len > 0) {
    std::memcpy(out->after_image.data(), ptr, after_len);
    ptr += after_len;
  }

  out->txn_table.clear();
  out->dpt_table.clear();

  if (out->type == LogRecordType::CHECKPOINT) {
    if (ptr + 4 > buf + total_size) return false;
    uint32_t tt_count = 0;
    read_val(tt_count);
    if (ptr + tt_count * 16 > buf + total_size) return false;
    out->txn_table.resize(tt_count);
    for (uint32_t i = 0; i < tt_count; ++i) {
      read_val(out->txn_table[i].txn_id);
      read_val(out->txn_table[i].status);
      read_val(out->txn_table[i].last_lsn);
    }

    if (ptr + 4 > buf + total_size) return false;
    uint32_t dpt_count = 0;
    read_val(dpt_count);
    if (ptr + dpt_count * 16 > buf + total_size) return false;
    out->dpt_table.resize(dpt_count);
    for (uint32_t i = 0; i < dpt_count; ++i) {
      read_val(out->dpt_table[i].table_id);
      read_val(out->dpt_table[i].page_id);
      read_val(out->dpt_table[i].rec_lsn);
    }
  }

  if (bytes_read) *bytes_read = total_size;
  return true;
}

std::string LogRecord::ToString() const {
  std::ostringstream ss;
  ss << "LogRecord(LSN=" << lsn << ", Type=" << LogRecordTypeToString(type)
     << ", Txn=" << txn_id << ", PrevLSN=" << prev_lsn;
  if (type == LogRecordType::UPDATE || type == LogRecordType::CLR) {
    ss << ", Table=" << table_id << ", Page=" << page_id << ", Slot=" << slot_num
       << ", Len=" << length;
    if (type == LogRecordType::CLR) {
      ss << ", UndoNxtLSN=" << undo_nxt_lsn;
    }
  } else if (type == LogRecordType::CHECKPOINT) {
    ss << ", TT_Entries=" << txn_table.size() << ", DPT_Entries=" << dpt_table.size();
  }
  ss << ")";
  return ss.str();
}

}  // namespace sthir
