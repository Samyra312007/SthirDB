#include "storage/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "common/config.h"
#include "common/crash.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif

static ssize_t pread(int fd, void* buf, size_t count, int64_t offset) {
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
  DWORD bytesRead = 0;
  if (!ReadFile(h, buf, static_cast<DWORD>(count), &bytesRead, &ov)) {
    DWORD err = GetLastError();
    if (err == ERROR_HANDLE_EOF) return 0;
    errno = EIO;
    return -1;
  }
  return static_cast<ssize_t>(bytesRead);
}

static ssize_t pwrite(int fd, const void* buf, size_t count, int64_t offset) {
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
  DWORD bytesWritten = 0;
  if (!WriteFile(h, buf, static_cast<DWORD>(count), &bytesWritten, &ov)) {
    errno = EIO;
    return -1;
  }
  return static_cast<ssize_t>(bytesWritten);
}
#else
#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif

namespace sthir {

namespace {

[[noreturn]] void ThrowErrno(const std::string& what, const std::string& path) {
  throw std::runtime_error(what + " '" + path + "': " + std::strerror(errno));
}

}  // namespace

// Phase 7: shared across every DiskManager (tables, indexes, WAL pages).
static std::atomic<uint64_t> g_pages_read{0};

uint64_t DiskManager::TotalReads() { return g_pages_read.load(std::memory_order_relaxed); }
void DiskManager::ResetReads() { g_pages_read.store(0, std::memory_order_relaxed); }

DiskManager::DiskManager(std::string db_file) : path_(std::move(db_file)) {
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_BINARY, 0644);
  if (fd_ < 0) ThrowErrno("cannot open", path_);
  const off_t size = ::lseek(fd_, 0, SEEK_END);
  if (size < 0) ThrowErrno("cannot stat", path_);
  num_pages_ = static_cast<uint32_t>(size / kPageSize);
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) ::close(fd_);
}

void DiskManager::read_page(uint32_t page_id, char* out) {
  if (page_id >= num_pages_) {
    std::memset(out, 0, kPageSize);
    return;
  }
  g_pages_read.fetch_add(1, std::memory_order_relaxed);  // Phase 7 evidence counter
  const uint64_t offset = static_cast<uint64_t>(page_id) * kPageSize;
  size_t done = 0;
  while (done < kPageSize) {
    const ssize_t r = ::pread(fd_, out + done, kPageSize - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      ThrowErrno("read failed", path_);
    }
    if (r == 0) break;  // sparse tail: treat rest as zeros
    done += static_cast<size_t>(r);
  }
  if (done < kPageSize) std::memset(out + done, 0, kPageSize - done);
}

void DiskManager::write_page(uint32_t page_id, const char* in) {
  WriteAt(static_cast<uint64_t>(page_id) * kPageSize, in, kPageSize);
}

uint32_t DiskManager::allocate_page() {
  const uint32_t page_id = num_pages_;
  std::vector<char> zero(kPageSize, 0);
  WriteAt(static_cast<uint64_t>(page_id) * kPageSize, zero.data(), kPageSize);
  num_pages_ = page_id + 1;
  return page_id;
}

void DiskManager::WriteRaw(uint64_t offset, const char* buf, size_t n) {
  size_t done = 0;
  while (done < n) {
    const ssize_t w = ::pwrite(fd_, buf + done, n - done, static_cast<off_t>(offset + done));
    if (w < 0) {
      if (errno == EINTR) continue;
      ThrowErrno("write failed", path_);
    }
    done += static_cast<size_t>(w);
  }
}

void DiskManager::WriteAt(uint64_t offset, const char* buf, size_t n) {
  // Single crash-injection hook for every page write.
  crash_point("page_write");

  // `mid_page_flush` asks for a torn write: half of the page reaches disk and
  // the process dies, so recovery meets a page that is half old and half new.
  // The trailing half goes first on purpose. The header (page_lsn, slot_count,
  // free_space_ptr) lives in the first half, so writing it last means a torn
  // page always carries the *old* header: page_lsn is lower than the log, redo
  // is not skipped, and the after-images rebuild the page. Writing the head
  // first would leave a page whose header advertises newer contents than it
  // holds, and redo would skip the very records that repair it.
  // Only armed inside a test run, so normal writes stay one call.
  if (n == kPageSize && crash_armed("mid_page_flush")) {
    const size_t half = n / 2;
    WriteRaw(offset + half, buf + half, n - half);
    crash_point("mid_page_flush");
    WriteRaw(offset, buf, half);
    return;
  }

  WriteRaw(offset, buf, n);
}

}  // namespace sthir
