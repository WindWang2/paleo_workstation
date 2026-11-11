#pragma once

#include <QtCore/qsystemdetection.h>
#include <cstdint>
#include <limits>
#if defined(Q_OS_WIN)
#pragma push_macro("small")
#include <QtCore/qt_windows.h>
#pragma pop_macro("small")
#else
#include <time.h>
#endif

// Test-only clock for single-threaded numeric work. Reading failures are
// explicit; neither a zero sample nor a wall-clock fallback is a valid result.
class ThreadCpuTimer {
public:
  bool start() { valid_ = read(start_); return valid_; }
  bool nsecsElapsed(std::int64_t &elapsed) const {
    std::int64_t now = 0;
    if (!valid_ || !read(now) || now < start_) return false;
    elapsed = now - start_;
    return true;
  }
private:
  static bool read(std::int64_t &nanoseconds) {
#if defined(Q_OS_WIN)
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return false;
    const auto ticks = [](const FILETIME &time) {
      return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    const std::uint64_t total = ticks(kernel) + ticks(user);
    if (total > std::uint64_t(std::numeric_limits<std::int64_t>::max()) / 100) return false;
    nanoseconds = std::int64_t(total * 100);
    return true;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec sample;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &sample) != 0 || sample.tv_sec < 0 ||
        sample.tv_nsec < 0 || sample.tv_nsec >= 1000000000)
      return false;
    if (std::uint64_t(sample.tv_sec) >
        (std::uint64_t(std::numeric_limits<std::int64_t>::max()) - std::uint64_t(sample.tv_nsec)) / 1000000000)
      return false;
    nanoseconds = std::int64_t(sample.tv_sec) * 1000000000 + sample.tv_nsec;
    return true;
#else
    return false;
#endif
  }
  std::int64_t start_ = 0;
  bool valid_ = false;
};
