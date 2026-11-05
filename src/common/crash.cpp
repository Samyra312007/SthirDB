#include "common/crash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unistd.h>

namespace sthir {

namespace {

const char* CrashTarget() {
  static const char* target = std::getenv("STHIRDB_CRASH_POINT");
  return target;
}

long CrashSkip() {
  const char* skip = std::getenv("STHIRDB_CRASH_SKIP");
  if (skip == nullptr || skip[0] == '\0') return 0;
  return std::strtol(skip, nullptr, 10);
}

std::mutex& CounterLatch() {
  static std::mutex latch;
  return latch;
}

std::unordered_map<std::string, long>& Counters() {
  static std::unordered_map<std::string, long> counters;
  return counters;
}

}  // namespace

bool crash_armed(const char* name) {
  const char* target = CrashTarget();
  if (target == nullptr || target[0] == '\0' || name == nullptr) return false;
  return std::strcmp(target, name) == 0;
}

void crash_point(const char* name) {
  if (!crash_armed(name)) return;

  long occurrence = 0;
  {
    std::lock_guard<std::mutex> lock(CounterLatch());
    occurrence = ++Counters()[name];
  }
  if (occurrence <= CrashSkip()) return;  // still in the skipped prefix

  std::fprintf(stderr, "[crash_point] simulated crash at '%s' (occurrence %ld)\n", name,
               occurrence);
  ::_exit(99);  // no stdio flush, no stack unwinding — mirrors power loss
}

}  // namespace sthir
