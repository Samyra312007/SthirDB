#pragma once

#include <cstdint>
#include <string>

namespace sthir {

// One file per table; page i lives at byte offset i * kPageSize.
//
// Every write funnels through the private WriteAt() low-level function, which
// calls crash_point() first — the single hook point Novelty 2 (Phase 5B)
// attaches crashes to, so no call site ever changes.
class DiskManager {
 public:
  explicit DiskManager(std::string db_file);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  // Reads page `page_id` into `out` (kPageSize bytes). Reading a page beyond
  // EOF yields zeros rather than an error (the caller allocates first).
  void read_page(uint32_t page_id, char* out);

  // Writes kPageSize bytes at the page's offset via the crash hook.
  void write_page(uint32_t page_id, const char* in);

  // Extends the file by one zero-filled page and returns its id.
  uint32_t allocate_page();

  uint32_t num_pages() const { return num_pages_; }
  const std::string& path() const { return path_; }

  // Phase 7: process-wide count of pages actually read from disk across every
  // DiskManager instance. Used for the Novelty-1 A-vs-B evidence (EXPLAIN +
  // pages-read), so the optimizer's page-I/O claims are measurable.
  static uint64_t TotalReads();
  static void ResetReads();

 private:
  // The single low-level write path for all page writes (crash hook + torn
  // write), and the raw pread/pwrite loop it uses.
  void WriteAt(uint64_t offset, const char* buf, size_t n);
  void WriteRaw(uint64_t offset, const char* buf, size_t n);

  std::string path_;
  int fd_ = -1;
  uint32_t num_pages_ = 0;
};

}  // namespace sthir
