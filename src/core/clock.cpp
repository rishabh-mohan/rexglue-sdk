/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/math.h>

REXCVAR_DEFINE_BOOL(clock_no_scaling, false, "Clock",
                    "Disable clock scaling (inverted: false = scaling enabled)");

REXCVAR_DEFINE_BOOL(clock_source_raw, false, "Clock", "Use raw clock source without scaling");

REXCVAR_DEFINE_BOOL(clock_lockfree, true, "Clock",
                    "Read the guest clock without taking a lock (applied when the guest clock "
                    "rate is set, at startup)");

namespace rex::chrono {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();
// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// Native guest ticks.
uint64_t last_guest_tick_count_ = 0;
// Last sampled host tick count.
uint64_t last_host_tick_count_ = Clock::QueryHostTickCount();
// Mutex to ensure last_host_tick_count_ and last_guest_tick_count_ are in sync
std::mutex tick_mutex_;

// Lock-free guest clock (a seqlock). While clock_den_ != 0, the guest tick
// count at host tick t is
//   clock_guest_base_ + (t - clock_host_base_) * clock_num_ / clock_den_.
// Only SetGuestTickRatioLocked writes these, under tick_mutex_; clock_seq_ is
// odd while it does. clock_den_ == 0 selects the locked clock below.
std::atomic<uint32_t> clock_seq_{0};
std::atomic<uint64_t> clock_host_base_{0};
std::atomic<uint64_t> clock_guest_base_{0};
std::atomic<uint64_t> clock_num_{0};
std::atomic<uint64_t> clock_den_{0};

// delta * num / den, exact while num * den < 2^64 (checked when publishing).
inline uint64_t ScaleTickDelta(uint64_t delta, uint64_t num, uint64_t den) {
  return (delta / den) * num + (delta % den) * num / den;
}

// With tick_mutex_ held: switch the guest clock to the ratio `frac`.
void SetGuestTickRatioLocked(std::pair<uint64_t, uint64_t> frac) {
  const uint64_t old_den = clock_den_.load(std::memory_order_relaxed);
  const bool lock_free = REXCVAR_GET(clock_lockfree) && frac.second != 0 &&
                         frac.first <= std::numeric_limits<uint64_t>::max() / frac.second;
  if (!lock_free && !old_den) {
    guest_tick_ratio_ = frac;  // Locked clock only, exactly as before.
    return;
  }

  const uint32_t seq = clock_seq_.load(std::memory_order_relaxed);
  clock_seq_.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);

  // Rebase at the current time so the clock stays continuous. Lock-free
  // readers have been using the published ratio; the locked clock applies a
  // new ratio from its last update on, as UpdateGuestClock always did.
  const uint64_t now = Clock::QueryHostTickCount();
  uint64_t guest_now;
  if (old_den) {
    const uint64_t host_base = clock_host_base_.load(std::memory_order_relaxed);
    guest_now = clock_guest_base_.load(std::memory_order_relaxed) +
                ScaleTickDelta(now > host_base ? now - host_base : 0,
                               clock_num_.load(std::memory_order_relaxed), old_den);
  } else {
    const uint64_t delta = now > last_host_tick_count_ ? now - last_host_tick_count_ : 0;
    guest_now = last_guest_tick_count_ + delta * frac.first / frac.second;
  }
  last_host_tick_count_ = now;
  last_guest_tick_count_ = guest_now;
  guest_tick_ratio_ = frac;

  clock_host_base_.store(now, std::memory_order_relaxed);
  clock_guest_base_.store(guest_now, std::memory_order_relaxed);
  clock_num_.store(frac.first, std::memory_order_relaxed);
  clock_den_.store(lock_free ? frac.second : 0, std::memory_order_relaxed);
  clock_seq_.store(seq + 2, std::memory_order_release);
}

// Reads the guest clock without the lock. False if the lock-free clock is
// off, or a rate change is in progress (the caller then takes the lock).
bool ReadGuestClockLockFree(uint64_t* guest_tick_count) {
  for (int attempt = 0; attempt < 16; ++attempt) {
    const uint32_t seq = clock_seq_.load(std::memory_order_acquire);
    const uint64_t den = clock_den_.load(std::memory_order_relaxed);
    if (!den) {
      return false;
    }
    if (seq & 1) {
      continue;
    }
    const uint64_t num = clock_num_.load(std::memory_order_relaxed);
    const uint64_t host_base = clock_host_base_.load(std::memory_order_relaxed);
    const uint64_t guest_base = clock_guest_base_.load(std::memory_order_relaxed);
    // Sample the host clock inside the read window, after the parameters.
    const uint64_t now = Clock::QueryHostTickCount();
    std::atomic_thread_fence(std::memory_order_acquire);
    if (clock_seq_.load(std::memory_order_relaxed) != seq) {
      continue;
    }
    *guest_tick_count =
        guest_base + ScaleTickDelta(now > host_base ? now - host_base : 0, num, den);
    return true;
  }
  return false;
}

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac = std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  std::lock_guard<std::mutex> lock(tick_mutex_);
  SetGuestTickRatioLocked(frac);
}

// Update the guest timer for all threads.
// Return a copy of the value so locking is reduced.
uint64_t UpdateGuestClock() {
  if (!REXCVAR_GET(clock_no_scaling)) {
    uint64_t guest_tick_count;
    if (ReadGuestClockLockFree(&guest_tick_count)) {
      return guest_tick_count;
    }
  }

  uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (REXCVAR_GET(clock_no_scaling)) {
    // Nothing to update, calculate on the fly
    return host_tick_count * guest_tick_ratio_.first / guest_tick_ratio_.second;
  }

  std::unique_lock<std::mutex> lock(tick_mutex_, std::defer_lock);
  if (lock.try_lock()) {
    // Translate host tick count to guest tick count.
    uint64_t host_tick_delta =
        host_tick_count > last_host_tick_count_ ? host_tick_count - last_host_tick_count_ : 0;
    last_host_tick_count_ = host_tick_count;
    uint64_t guest_tick_delta =
        host_tick_delta * guest_tick_ratio_.first / guest_tick_ratio_.second;
    last_guest_tick_count_ += guest_tick_delta;
    return last_guest_tick_count_;
  } else {
    // Wait until another thread has finished updating the clock.
    lock.lock();
    return last_guest_tick_count_;
  }
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  uint64_t numerator = 10000000;  // 100ns/10MHz resolution
  uint64_t denominator = guest_tick_frequency_;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}

uint64_t Clock::QueryHostTickFrequency() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() {
  return guest_time_scalar_;
}

void Clock::set_guest_time_scalar(double scalar) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<std::mutex> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() {
  return guest_tick_frequency_;
}

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() {
  return guest_system_time_base_;
}

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t Clock::QueryGuestSystemTime() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                                                  std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms =
      static_cast<uint64_t>((static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time = static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time =
        static_cast<uint64_t>((static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) * guest_time_scalar_);
  uint64_t scaled_usec =
      static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) * guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace rex::chrono
