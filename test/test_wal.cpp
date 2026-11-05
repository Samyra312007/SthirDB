#include "wal/wal_manager.h"

#include <cstdio>
#include <vector>

#include "test_macros.h"

using namespace sthir;

namespace {

void test_log_record_serialization() {
  // Test UPDATE record
  std::vector<char> before = {'o', 'l', 'd'};
  std::vector<char> after = {'n', 'e', 'w', '!'};
  LogRecord u = LogRecord::MakeUpdate(101, 50, 4, 7, 2, 0, before, after);
  u.lsn = 120;

  std::vector<char> bytes = u.Serialize();
  CHECK(!bytes.empty());

  LogRecord des;
  size_t bytes_read = 0;
  bool ok = LogRecord::Deserialize(bytes.data(), bytes.size(), &des, &bytes_read);
  CHECK(ok);
  CHECK_EQ(bytes_read, bytes.size());
  CHECK_EQ(des.lsn, 120u);
  CHECK(des.type == LogRecordType::UPDATE);
  CHECK_EQ(des.txn_id, 101u);
  CHECK_EQ(des.prev_lsn, 50u);
  CHECK_EQ(des.table_id, 4u);
  CHECK_EQ(des.page_id, 7u);
  CHECK_EQ(des.slot_num, 2);
  CHECK_EQ(des.before_image.size(), 3u);
  CHECK_EQ(des.after_image.size(), 4u);
  CHECK_EQ(des.after_image[0], 'n');

  // Test CLR record
  LogRecord clr = LogRecord::MakeCLR(101, 120, 4, 7, 2, 0, before, 50);
  clr.lsn = 180;
  bytes = clr.Serialize();
  ok = LogRecord::Deserialize(bytes.data(), bytes.size(), &des, &bytes_read);
  CHECK(ok);
  CHECK(des.type == LogRecordType::CLR);
  CHECK_EQ(des.undo_nxt_lsn, 50u);

  // Test CHECKPOINT record
  std::vector<TxnTableEntry> tt = {{1, 0, 100}, {2, 0, 150}};
  std::vector<DptTableEntry> dpt = {{1, 5, 80}, {2, 9, 120}};
  LogRecord cp = LogRecord::MakeCheckpoint(tt, dpt);
  cp.lsn = 200;
  bytes = cp.Serialize();
  ok = LogRecord::Deserialize(bytes.data(), bytes.size(), &des, &bytes_read);
  CHECK(ok);
  CHECK(des.type == LogRecordType::CHECKPOINT);
  CHECK_EQ(des.txn_table.size(), 2u);
  CHECK_EQ(des.dpt_table.size(), 2u);
  CHECK_EQ(des.txn_table[0].txn_id, 1u);
  CHECK_EQ(des.dpt_table[1].table_id, 2u);
  CHECK_EQ(des.dpt_table[1].page_id, 9u);
  CHECK_EQ(des.dpt_table[1].rec_lsn, 120u);

  test_passed("test_log_record_serialization");
}

void test_wal_manager_append_and_read() {
  const std::string test_log = "./build/test_wal.log";
  std::remove(test_log.c_str());

  WalManager wal(test_log);
  wal.Clear();

  LogRecord b = LogRecord::MakeBegin(1);
  uint64_t lsn1 = wal.AppendRecord(&b);
  CHECK_EQ(lsn1, 1u);

  std::vector<char> bef = {'a'};
  std::vector<char> aft = {'b'};
  LogRecord u = LogRecord::MakeUpdate(1, lsn1, 3, 10, 0, 0, bef, aft);
  uint64_t lsn2 = wal.AppendRecord(&u);
  CHECK(lsn2 > lsn1);

  LogRecord c = LogRecord::MakeCommit(1, lsn2);
  uint64_t lsn3 = wal.AppendRecord(&c);
  CHECK(lsn3 > lsn2);

  wal.FlushUpTo(lsn3);
  CHECK(wal.flushed_lsn() >= lsn3);

  // Read back individual records
  LogRecord r1, r2, r3;
  CHECK(wal.ReadRecord(lsn1, &r1));
  CHECK(r1.type == LogRecordType::BEGIN);
  CHECK_EQ(r1.txn_id, 1u);

  CHECK(wal.ReadRecord(lsn2, &r2));
  CHECK(r2.type == LogRecordType::UPDATE);
  CHECK_EQ(r2.table_id, 3u);
  CHECK_EQ(r2.page_id, 10u);

  CHECK(wal.ReadRecord(lsn3, &r3));
  CHECK(r3.type == LogRecordType::COMMIT);

  // Read all records sequentially
  auto all = wal.ReadAllRecords();
  CHECK_EQ(all.size(), 3u);
  CHECK_EQ(all[0].lsn, lsn1);
  CHECK_EQ(all[1].lsn, lsn2);
  CHECK_EQ(all[2].lsn, lsn3);

  test_passed("test_wal_manager_append_and_read");
}

}  // namespace

int main() {
  test_log_record_serialization();
  test_wal_manager_append_and_read();
  return 0;
}
