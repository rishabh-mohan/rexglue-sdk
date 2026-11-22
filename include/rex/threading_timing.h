/**
 * @file        threading_timing.h
 * @brief       Host timer resolution and precise (high-resolution) sleeps
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <chrono>
#include <cstdint>

#include <rex/thread.h>

namespace rex::thread {

struct HostTimingConfig {
  // System timer resolution to request (timeBeginPeriod), in ms. 0 = OS default.
  uint32_t timer_resolution_ms = 0;
  // Keep honouring the request while the window is occluded or minimized
  // (Windows 11 EcoQoS: PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION).
  bool keep_timer_resolution_in_background = false;
  // There is deliberately no option for high-resolution Timer objects:
  // SetWaitableTimer rejects a completion routine on a
  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer (ERROR_INVALID_PARAMETER),
  // and Win32Timer needs one for guest timer callbacks.
};

// Applies `config` to the process; call once at startup. RestoreHostTiming()
// withdraws the timer resolution request and may be called more than once.
// Both are no-ops on non-Windows hosts.
void ConfigureHostTiming(const HostTimingConfig& config);
void RestoreHostTiming();

// AlertableSleep on a high-resolution waitable timer (Windows), so a 1 ms
// sleep takes about 1 ms instead of a whole system timer tick. Returns
// kAlerted if user callbacks (APCs) ran and ended the sleep early.
SleepResult AlertableSleepPrecise(std::chrono::microseconds duration);

}  // namespace rex::thread
