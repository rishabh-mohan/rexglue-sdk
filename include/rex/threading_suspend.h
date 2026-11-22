/**
 * @file        threading_suspend.h
 * @brief       Waiting for an asynchronous thread suspension to take effect
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <rex/thread.h>

namespace rex::thread {

// On Windows, Thread::Suspend() on another thread returns before that thread
// has actually stopped (SuspendThread is asynchronous). This blocks until the
// suspension has taken effect, and returns false if that couldn't be
// confirmed. On POSIX hosts it returns true without waiting.
bool WaitUntilSuspended(Thread* thread);

}  // namespace rex::thread
