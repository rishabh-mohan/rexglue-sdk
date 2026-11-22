/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <chrono>
#include <cstring>
#include <thread>

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/flags.h>
#include <rex/audio/xma/decoder.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/string/buffer.h>
#include <rex/system/thread_state.h>
#include <rex/thread.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(
    audio_maxqframes, 8, "Audio",
    "Max buffered audio frames (range 4-64). Lower reduces latency but may cause stuttering.");

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

REXCVAR_DEFINE_BOOL(
    audio_shutdown_detach_worker, false, "Audio",
    "If the audio worker has not exited when shutdown times out, wait up to 2 s more, then "
    "leave it running and leak what it can still reach instead of force-terminating it "
    "(TerminateThread can leave the global lock held, so later shutdown steps never finish). "
    "Off: force-terminate after the timeout (old).");

namespace rex::audio {

namespace {
constexpr std::chrono::milliseconds kWorkerShutdownTimeout{500};
// Detach mode only: after the timeout, how long Shutdown() keeps waiting for the worker to
// exit by itself (a slow guest callback usually returns) before detaching it. A detached
// worker that resumes guest code later would run it during the runtime teardown.
constexpr std::chrono::milliseconds kWorkerExitGrace{2000};
// Detach mode only: how long Shutdown() then waits for the worker to be in a blocking call
// (it is inside only for short, non-blocking stretches) before detaching it anyway.
constexpr std::chrono::milliseconds kWorkerDetachGrace{2000};
}  // namespace

// Detach mode (audio_shutdown_detach_worker): the worker runs every blocking call (WaitAny,
// the global lock, guest callbacks, sleeps) "outside", touching no member of the AudioSystem,
// and the short stretches between them "inside". Shutdown() detaches the worker only while it
// is outside; the worker's next Reenter() then fails and it parks forever, never touching the
// AudioSystem again. Process exit reaps it. Nothing it can still use is freed (see Shutdown).
struct AudioSystem::WorkerControl {
  enum : uint32_t { kInside = 0, kOutside = 1, kDetached = 2 };
  std::atomic<uint32_t> phase{kInside};

  // Worker: about to block. Parks if Shutdown() force-detached it meanwhile.
  void Leave() {
    uint32_t expected = kInside;
    if (!phase.compare_exchange_strong(expected, kOutside)) {
      Park();
    }
  }
  // Worker: back from a blocking call. False if Shutdown() detached it meanwhile.
  bool Reenter() {
    uint32_t expected = kOutside;
    return phase.compare_exchange_strong(expected, kInside);
  }
  // Shutdown(): succeeds only while the worker is outside.
  bool TryDetach() {
    uint32_t expected = kOutside;
    return phase.compare_exchange_strong(expected, kDetached);
  }
  void ForceDetach() { phase.store(kDetached); }

  // Holds no lock and is not alertable, so no guest code runs on the parked worker.
  [[noreturn]] static void Park() {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::hours(1));
    }
  }
};

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()),
      function_dispatcher_(function_dispatcher),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));

  queued_frames_ = std::min(
      static_cast<uint32_t>(kMaximumQueuedFrames),
      std::max(static_cast<uint32_t>(REXCVAR_GET(audio_maxqframes)), static_cast<uint32_t>(4)));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = rex::thread::Semaphore::Create(0, queued_frames_);
    assert_not_null(client_semaphores_[i]);
    wait_handles_[i] = client_semaphores_[i].get();
  }
  shutdown_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(shutdown_event_);
  wait_handles_[kMaximumClientCount] = shutdown_event_.get();

  xma_decoder_ = std::make_unique<rex::audio::XmaDecoder>(function_dispatcher_);

  resume_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  if (REXCVAR_GET(audio_shutdown_detach_worker)) {
    worker_control_ = std::make_shared<WorkerControl>();
  }
  WorkerControl* ctl = worker_control_.get();

  worker_running_ = true;
  // The lambda (owned by the XHostThread) keeps the control block alive for a detached worker.
  worker_thread_ = system::object_ref<system::XHostThread>(new system::XHostThread(
      kernel_state, 128 * 1024, 0, [this, ctl, keep = worker_control_]() {
        (void)keep;
        WorkerThreadMain(ctl);
        return 0;
      }));

  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain(WorkerControl* ctl) {
  // ctl is null unless audio_shutdown_detach_worker is on; then leave()/reenter() bracket every
  // blocking call (see WorkerControl). With the cvar off both are no-ops.
  const auto leave = [ctl] {
    if (ctl) {
      ctl->Leave();
    }
  };
  const auto reenter = [ctl] {
    if (ctl && !ctl->Reenter()) {
      WorkerControl::Park();
    }
  };

  // Initialize driver and ringbuffer.
  Initialize();

  // Main run loop.
  uint32_t diag_pump_count = 0;
  while (worker_running_) {
    // These handles signify the number of submitted samples. Once we reach
    // 64 samples, we wait until our audio backend releases a semaphore
    // (signaling a sample has finished playing)
    // Local copy: the wait must not read members while outside. The handles themselves
    // are never freed while a detached worker can still wait on them.
    rex::thread::WaitHandle* wait_handles[kMaximumClientCount + 1];
    std::memcpy(wait_handles, wait_handles_, sizeof(wait_handles));
    leave();
    auto result = rex::thread::WaitAny(wait_handles, rex::countof(wait_handles), true,
                                       std::chrono::milliseconds(500));
    reenter();
    if (result.first == rex::thread::WaitResult::kFailed) {
      REXAPU_WARN("AudioWorker: WaitAny failed");
      continue;
    }

    if (result.first == rex::thread::WaitResult::kTimeout) {
      if (diag_pump_count < 5) {
        REXAPU_NOISY_DEBUG("AudioWorker: WaitAny timed out (no semaphore signals)");
      }
    }

    if (result.first == thread::WaitResult::kSuccess && result.second == kMaximumClientCount) {
      // Shutdown event signaled.
      if (paused_) {
        pause_fence_.Signal();
        rex::thread::Event* resume_event = resume_event_.get();
        leave();
        thread::Wait(resume_event, false);
        reenter();
      }

      continue;
    }

    // Number of clients pumped
    bool pumped = false;
    if (result.first == rex::thread::WaitResult::kSuccess) {
      auto index = result.second;

      // Same static mutex as global_critical_region_.Acquire(), taken while outside.
      leave();
      auto global_lock = rex::thread::global_critical_region::AcquireDirect();
      if (ctl && !ctl->Reenter()) {
        global_lock.unlock();
        WorkerControl::Park();
      }
      uint32_t client_callback = clients_[index].callback;
      uint32_t client_callback_arg = clients_[index].wrapped_callback_arg;
      global_lock.unlock();

      if (client_callback) {
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: dispatching callback {:08X} with arg {:08X} for client {}",
                       client_callback, client_callback_arg, index);
        }
        SCOPE_profile_cpu_i("apu", "rex::audio::AudioSystem->client_callback");
        uint64_t args[] = {client_callback_arg};
        runtime::FunctionDispatcher* dispatcher = function_dispatcher_;
        auto* thread_state = worker_thread_->thread_state();
        leave();
        dispatcher->Execute(thread_state, client_callback, args, rex::countof(args));
        reenter();
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: callback returned for client {}", index);
        }
        diag_pump_count++;
      } else {
        REXAPU_DEBUG("AudioWorker: semaphore signaled for client {} but callback is 0", index);
      }

      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      SCOPE_profile_cpu_i("apu", "Sleep");
      leave();
      rex::thread::Sleep(std::chrono::milliseconds(500));
      reenter();
    }
  }
  worker_running_ = false;
  // Nothing after this touches the AudioSystem (XHostThread::Exit uses only the XThread).
  leave();

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  if (!worker_running_) {
    return;
  }

  // Shut down XMA decoder first - its worker can stall in FFmpeg
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }

  worker_running_ = false;
  const bool detach_mode = worker_control_ != nullptr;
  if (detach_mode && paused_) {
    // A paused worker sits in the resume wait and never sees the shutdown event; wake it
    // so it can exit normally instead of timing out (detach mode only).
    resume_event_->Set();
  }
  shutdown_event_->Set();
  bool detached = false;
  if (worker_thread_) {
    // The worker may be stuck inside a guest callback that is itself blocked on
    // guest objects (e.g. KeWaitForMultipleObjects), so terminating is the last
    // resort. Give it a chance to unwind first: TerminateThread abandons any
    // lock the thread holds, including the CRT heap lock.
    rex::thread::Thread* host_thread = worker_thread_->thread();
    bool exited = host_thread && rex::thread::Wait(host_thread, false, kWorkerShutdownTimeout) ==
                                     rex::thread::WaitResult::kSuccess;
    if (!exited) {
      if (detach_mode) {
        // Never force-terminate: the worker may hold or wait for the global lock.
        detached = host_thread && DetachWorker(host_thread);
      } else {
        REXAPU_WARN("Audio worker did not exit within {} ms; terminating",
                    kWorkerShutdownTimeout.count());
        worker_thread_->Terminate(0);
      }
    }
    if (detached) {
      // Leak our reference: the still-running worker uses its XThread (thread state,
      // stack) and the host_fn_ that owns the control block.
      (void)worker_thread_.release();
    } else {
      worker_thread_.reset();
    }
  }

  if (detached) {
    // Free nothing the worker (or guest code it is running) can still reach: its wait
    // handles, the client drivers (SubmitFrame) and the wrapped callback args. The
    // clients_ entries stay as they are. Process exit reclaims all of it.
    for (auto& semaphore : client_semaphores_) {
      (void)semaphore.release();
    }
    (void)shutdown_event_.release();
    (void)resume_event_.release();
    REXAPU_WARN(
        "Audio worker did not exit within {} ms; detached it and leaked its thread, client "
        "drivers and wait handles instead of terminating it",
        (kWorkerShutdownTimeout + kWorkerExitGrace).count());
    return;
  }

  // Destroy all active client drivers (closes SDL audio devices, stopping
  // callback threads) before the semaphores they reference are destroyed.
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      DestroyDriver(clients_[i].driver);
      if (clients_[i].wrapped_callback_arg) {
        memory()->SystemHeapFree(clients_[i].wrapped_callback_arg);
      }
      clients_[i] = {nullptr, 0, 0, 0, false};
    }
  }
}

// Detach mode only. Returns true once the worker is detached (it will never touch this
// object again), false if it exited meanwhile.
bool AudioSystem::DetachWorker(rex::thread::Thread* host_thread) {
  // Bounded cooperative wait first: a worker in a slow guest callback usually returns, sees
  // worker_running_ == false and exits, and then the normal teardown runs.
  if (rex::thread::Wait(host_thread, false, kWorkerExitGrace) ==
      rex::thread::WaitResult::kSuccess) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + kWorkerDetachGrace;
  while (!worker_control_->TryDetach()) {
    // Inside: a short, non-blocking stretch of worker code. Poll until it blocks or exits.
    if (rex::thread::Wait(host_thread, false, std::chrono::milliseconds(1)) ==
        rex::thread::WaitResult::kSuccess) {
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      worker_control_->ForceDetach();
      REXAPU_ERROR("Audio worker stayed inside the audio system for {} ms; detached it anyway",
                   kWorkerDetachGrace.count());
      return true;
    }
  }
  return true;
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index) {
  REXAPU_DEBUG("AudioSystem::RegisterClient: callback={:08X} callback_arg={:08X}", callback,
               callback_arg);
  auto global_lock = global_critical_region_.Acquire();

  auto index = FindFreeClient();
  assert_true(index >= 0);
  REXAPU_DEBUG("AudioSystem::RegisterClient: using client index={} queued_frames={}", index,
               queued_frames_);

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    return result;
  }
  assert_not_null(driver);

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  memory::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index] = {driver, callback, callback_arg, ptr, true};

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, uint32_t samples_ptr) {
  SCOPE_profile_cpu_f("apu");

  static uint32_t submit_count = 0;
  if (submit_count < 10) {
    REXAPU_DEBUG("AudioSystem::SubmitFrame called: index={} samples_ptr={:08X}", index,
                 samples_ptr);
    submit_count++;
  }

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  assert_true(clients_[index].driver != NULL);
  (clients_[index].driver)->SubmitFrame(samples_ptr);
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  DestroyDriver(clients_[index].driver);
  memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {nullptr, 0, 0, 0, false};

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  rex::thread::WaitResult wait_result;
  do {
    wait_result = rex::thread::Wait(client_semaphore, false, std::chrono::milliseconds(0));
  } while (wait_result == rex::thread::WaitResult::kSuccess);
  assert_true(wait_result == rex::thread::WaitResult::kTimeout);
}

bool AudioSystem::Save(stream::ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(stream::ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    REXAPU_ERROR("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    assert_true(id < kMaximumClientCount);

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(queued_frames_, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      REXAPU_ERROR(
          "AudioSystem::Restore - Call to CreateDriver failed with status "
          "{:08X}",
          status);
      return false;
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  shutdown_event_->Set();
  pause_fence_.Wait();

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace rex::audio
