/**
 * @file        threading_timeout.h
 * @brief       Conversion of NT-style guest timeouts (header-only, no dependencies)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>

namespace rex::thread {

// NT timeouts (KeWaitFor*, KeDelayExecutionThread, ...) are in 100 ns units:
// negative = relative to now, positive = absolute system time (FILETIME, 1601
// epoch; for the guest, Clock::QueryGuestSystemTime()), 0 = poll.

// Remaining time in 100 ns units: 0 if the timeout is 0 or already due.
constexpr uint64_t NtTimeoutToRelative100ns(int64_t timeout, uint64_t now_system_time) {
  if (timeout < 0) {
    // Negate in unsigned arithmetic so INT64_MIN does not overflow.
    return uint64_t(0) - static_cast<uint64_t>(timeout);
  }
  if (timeout > 0) {
    uint64_t due_time = static_cast<uint64_t>(timeout);
    return due_time > now_system_time ? due_time - now_system_time : 0;
  }
  return 0;
}

// Longest finite host wait in milliseconds (0xFFFFFFFF is INFINITE).
constexpr uint32_t kMaxFiniteWaitMillis = 0xFFFFFFFEu;

// Whole milliseconds, rounded up: NT never ends a timed wait early, so a
// sub-millisecond timeout must wait rather than become a 0 ms poll. Saturates
// below INFINITE so a finite timeout never becomes an infinite wait.
constexpr uint32_t Relative100nsToMillis(uint64_t relative_100ns) {
  uint64_t ms = relative_100ns / 10000 + (relative_100ns % 10000 ? 1 : 0);
  return ms < kMaxFiniteWaitMillis ? static_cast<uint32_t>(ms) : kMaxFiniteWaitMillis;
}

// Whole microseconds, rounded up.
constexpr uint64_t Relative100nsToMicros(uint64_t relative_100ns) {
  return relative_100ns / 10 + (relative_100ns % 10 ? 1 : 0);
}

// Host wait milliseconds for an NT timeout.
constexpr uint32_t NtTimeoutToMillis(int64_t timeout, uint64_t now_system_time) {
  return Relative100nsToMillis(NtTimeoutToRelative100ns(timeout, now_system_time));
}

}  // namespace rex::thread
