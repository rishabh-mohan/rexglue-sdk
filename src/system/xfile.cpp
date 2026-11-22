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

#include <rex/cvar.h>
#include <rex/filesystem/devices/disc_image_file.h>
#include <rex/filesystem/devices/host_path_file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <vector>

#include <rex/system/file_fixups.h>
#include <rex/system/xfile.h>
#include <rex/thread/mutex.h>

#include <algorithm>
#include <span>

REXCVAR_DEFINE_BOOL(io_lockless_watch_check, false, "Kernel",
                    "After a file read into physical memory, check the write-watch bits without "
                    "the global lock and take it only if a page in the buffer is watched");

REXCVAR_DEFINE_BOOL(io_coalesce_scatter_reads, false, "Kernel",
                    "Read runs of contiguous pages in a scatter read with an explicit offset with "
                    "one host read. A run that fails or comes back short is redone page by page");

REXCVAR_DEFINE_BOOL(io_unlocked_file_reads, false, "Kernel",
                    "Reads at an explicit offset from read-only host or disc image files skip the "
                    "per-handle lock (the host orders them); position and completion are still "
                    "updated under it");

namespace rex::system {

namespace {
std::vector<FileReadFixup>& FileReadFixups() {
  // Function-local so registration from another binary's static initialiser
  // cannot run before this vector exists.
  static std::vector<FileReadFixup> fixups;
  return fixups;
}

// Positional reads through these need no per-handle lock: HostPathFile reads at
// an explicit offset on a synchronous handle, which the host kernel orders, and
// DiscImageFile copies from a read-only mapping. STFS seeks a shared FILE*, so
// it keeps the lock, as does any handle that can write.
bool AllowsUnlockedReads(rex::filesystem::File* file) {
  using rex::filesystem::FileAccess;
  if (!file || (file->file_access() & (FileAccess::kGenericWrite | FileAccess::kGenericAll |
                                       FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return false;
  }
  return dynamic_cast<rex::filesystem::HostPathFile*>(file) != nullptr ||
         dynamic_cast<rex::filesystem::DiscImageFile*>(file) != nullptr;
}

// The host byte that ReadData writes for guest_address (same translation).
const uint8_t* ReadDestination(const rex::memory::Memory* memory,
                               const rex::memory::BaseHeap* heap, uint32_t guest_address) {
  return heap->heap_type() == rex::memory::HeapType::kGuestPhysical
             ? memory->TranslatePhysical(
                   static_cast<const rex::memory::PhysicalHeap*>(heap)->GetPhysicalAddress(
                       guest_address))
             : memory->TranslateVirtual(guest_address);
}

// Whether ReadData can take [guest_address, guest_address + length) as one
// buffer: a single heap, with contiguous host bytes behind it.
bool IsSingleHostBuffer(const rex::memory::Memory* memory, uint32_t guest_address,
                        uint32_t length) {
  const uint64_t last = uint64_t(guest_address) + length - 1;
  if (!length || last > UINT32_MAX) {
    return false;
  }
  const rex::memory::BaseHeap* heap = memory->LookupHeap(guest_address);
  if (!heap || memory->LookupHeap(uint32_t(last)) != heap) {
    return false;
  }
  return ReadDestination(memory, heap, uint32_t(last)) -
             ReadDestination(memory, heap, guest_address) ==
         ptrdiff_t(length - 1);
}
}  // namespace

void RegisterFileReadFixup(FileReadFixup fixup) {
  if (fixup) {
    FileReadFixups().push_back(std::move(fixup));
  }
}

bool HasFileReadFixups() { return !FileReadFixups().empty(); }

void ApplyFileReadFixups(const std::string& name, uint64_t offset, uint8_t* data, size_t length) {
  for (const auto& fixup : FileReadFixups()) {
    fixup(name, offset, data, length);
  }
}


XFile::XFile(KernelState* kernel_state, rex::filesystem::File* file, bool synchronous)
    : XObject(kernel_state, kObjectType), file_(file), is_synchronous_(synchronous) {
  async_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
  unlocked_reads_ok_ = AllowsUnlockedReads(file_);
}

XFile::XFile() : XObject(kObjectType) {
  async_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
}

XFile::~XFile() {
  // TODO(benvanik): signal that the file is closing?
  async_event_->Set();
  file_->Destroy();
}

uint64_t XFile::position() const {
  std::lock_guard<std::mutex> lock(file_lock_);
  return position_;
}

void XFile::set_position(uint64_t value) {
  std::lock_guard<std::mutex> lock(file_lock_);
  position_ = value;
}

X_STATUS XFile::QueryDirectory(X_FILE_DIRECTORY_INFORMATION* out_info, size_t length,
                               const std::string_view file_name, bool restart) {
  std::lock_guard<std::mutex> lock(file_lock_);
  assert_not_null(out_info);

  rex::filesystem::Entry* entry = nullptr;

  if (!file_name.empty()) {
    // Only queries in the current directory are supported for now.
    assert_true(rex::string::utf8_find_any_of(file_name, "\\") == std::string_view::npos);

    find_engine_.SetRule(file_name);

    // Always restart the search?
    find_index_ = 0;
    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }
  } else {
    if (restart) {
      find_index_ = 0;
    }

    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_MORE_FILES;
    }
  }

  auto end = reinterpret_cast<uint8_t*>(out_info) + length;
  const auto& entry_name = entry->name();
  if (reinterpret_cast<uint8_t*>(&out_info->file_name[0]) + entry_name.size() > end) {
    assert_always("Buffer overflow?");
    return X_STATUS_NO_SUCH_FILE;
  }

  out_info->next_entry_offset = 0;
  out_info->file_index = static_cast<uint32_t>(find_index_);
  out_info->creation_time = entry->create_timestamp();
  out_info->last_access_time = entry->access_timestamp();
  out_info->last_write_time = entry->write_timestamp();
  out_info->change_time = entry->write_timestamp();
  out_info->end_of_file = entry->size();
  out_info->allocation_size = entry->allocation_size();
  out_info->attributes = entry->attributes();
  out_info->file_name_length = static_cast<uint32_t>(entry_name.size());
  std::memcpy(out_info->file_name, entry_name.data(), entry_name.size());

  return X_STATUS_SUCCESS;
}

X_STATUS XFile::Read(uint32_t buffer_guest_address, uint32_t buffer_length, uint64_t byte_offset,
                     uint32_t* out_bytes_read, uint32_t apc_context, bool notify_completion) {
  if (unlocked_reads_ok_ && byte_offset != uint64_t(-1) && byte_offset != uint64_t(-2) &&
      REXCVAR_GET(io_unlocked_file_reads)) {
    // A read at an explicit offset doesn't use position_, so only the
    // bookkeeping takes file_lock_; reads complete in the order they take it.
    size_t bytes_read = 0;
    X_STATUS result;
    {
      std::shared_lock<std::shared_mutex> read_lock(unlocked_read_lock_);
      result = ReadData(buffer_guest_address, buffer_length, byte_offset, &bytes_read);
    }
    std::lock_guard<std::mutex> lock(file_lock_);
    return CompleteRead(result, buffer_length, byte_offset, bytes_read, out_bytes_read, apc_context,
                        notify_completion);
  }
  std::lock_guard<std::mutex> lock(file_lock_);
  return ReadInternal(buffer_guest_address, buffer_length, byte_offset, out_bytes_read, apc_context,
                      notify_completion);
}

X_STATUS XFile::ReadInternal(uint32_t buffer_guest_address, uint32_t buffer_length,
                             uint64_t byte_offset, uint32_t* out_bytes_read, uint32_t apc_context,
                             bool notify_completion) {
  if (byte_offset == uint64_t(-1)) {
    // Read from current position.
    byte_offset = position_;
  }

  size_t bytes_read = 0;
  X_STATUS result = ReadData(buffer_guest_address, buffer_length, byte_offset, &bytes_read);
  return CompleteRead(result, buffer_length, byte_offset, bytes_read, out_bytes_read, apc_context,
                      notify_completion);
}

X_STATUS XFile::ReadData(uint32_t buffer_guest_address, uint32_t buffer_length,
                         uint64_t byte_offset, size_t* out_bytes_read) {
  size_t bytes_read = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  // Zero length means success for a valid file object according to Windows
  // tests.
  if (buffer_length) {
    if (UINT32_MAX - buffer_guest_address < buffer_length) {
      result = X_STATUS_ACCESS_VIOLATION;
    } else {
      // Games often read directly to texture/vertex buffer memory - in this
      // case, invalidation notifications must be sent. However, having any
      // memory callbacks in the range will result in STATUS_ACCESS_VIOLATION at
      // least on Windows, without anything being read or any callbacks being
      // triggered. So for physical memory, host protection must be bypassed,
      // and invalidation callbacks must be triggered manually (it's also wrong
      // to trigger invalidation callbacks before reading in this case, because
      // during the read, the guest may still access the data around the buffer
      // that is located in the same host pages as the buffer's start and end,
      // on the GPU - and that must not trigger a race condition).
      uint32_t buffer_guest_high_address = buffer_guest_address + buffer_length - 1;
      rex::memory::BaseHeap* buffer_start_heap = memory()->LookupHeap(buffer_guest_address);
      const rex::memory::BaseHeap* buffer_end_heap =
          memory()->LookupHeap(buffer_guest_high_address);
      if (!buffer_start_heap || !buffer_end_heap ||
          (buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical) !=
              (buffer_end_heap->heap_type() == memory::HeapType::kGuestPhysical) ||
          (buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical &&
           buffer_start_heap != buffer_end_heap)) {
        result = X_STATUS_ACCESS_VIOLATION;
      } else {
        rex::memory::PhysicalHeap* buffer_physical_heap =
            buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical
                ? static_cast<rex::memory::PhysicalHeap*>(buffer_start_heap)
                : nullptr;
        if (buffer_physical_heap && buffer_physical_heap->QueryRangeAccess(
                                        buffer_guest_address, buffer_guest_high_address) !=
                                        memory::PageAccess::kReadWrite) {
          result = X_STATUS_ACCESS_VIOLATION;
        } else {
          result = file_->ReadSync(
              std::span<uint8_t>(
                  buffer_physical_heap
                      ? memory()->TranslatePhysical(
                            buffer_physical_heap->GetPhysicalAddress(buffer_guest_address))
                      : memory()->TranslateVirtual(buffer_guest_address),
                  buffer_length),
              size_t(byte_offset), &bytes_read);
          if (XSUCCEEDED(result)) {
            // Before the invalidation callbacks, so a fixup that rewrites
            // bytes headed for texture or vertex memory is part of what the
            // GPU is told about.
            if (HasFileReadFixups() && bytes_read) {
              ApplyFileReadFixups(
                  name(), byte_offset,
                  buffer_physical_heap
                      ? memory()->TranslatePhysical(
                            buffer_physical_heap->GetPhysicalAddress(buffer_guest_address))
                      : memory()->TranslateVirtual(buffer_guest_address),
                  bytes_read);
            }

            // Most reads land in pages nothing watches, so check the bits
            // without the global lock first (the check is conservative).
            if (buffer_physical_heap &&
                (!REXCVAR_GET(io_lockless_watch_check) ||
                 buffer_physical_heap->MayHaveWriteWatches(buffer_guest_address, buffer_length))) {
              buffer_physical_heap->TriggerCallbacks(
                  rex::thread::global_critical_region::AcquireDirect(), buffer_guest_address,
                  buffer_length, true, true);
            }
          }
        }
      }
    }
  }

  *out_bytes_read = bytes_read;
  return result;
}

X_STATUS XFile::CompleteRead(X_STATUS result, uint32_t buffer_length, uint64_t byte_offset,
                             size_t bytes_read, uint32_t* out_bytes_read, uint32_t apc_context,
                             bool notify_completion) {
  if (XSUCCEEDED(result) && buffer_length) {
    if (byte_offset) {
      position_ = byte_offset;
    }
    position_ += bytes_read;
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(bytes_read);
  }

  if (notify_completion) {
    XIOCompletion::IONotification notify;
    notify.apc_context = apc_context;
    notify.num_bytes = uint32_t(bytes_read);
    notify.status = result;

    NotifyIOCompletionPorts(notify);

    async_event_->Set();
  }

  return result;
}

X_STATUS XFile::ReadScatter(uint32_t segments_guest_address, uint32_t length, uint64_t byte_offset,
                            uint32_t* out_bytes_read, uint32_t apc_context) {
  // Offsets 0, -1 and -2 have special meanings below, so only an explicit
  // offset that no segment offset can wrap into takes the positional path.
  if (byte_offset && byte_offset < uint64_t(-2) - length) {
    const bool coalesce = REXCVAR_GET(io_coalesce_scatter_reads);
    const bool unlocked = unlocked_reads_ok_ && REXCVAR_GET(io_unlocked_file_reads);
    if (coalesce || unlocked) {
      return ReadScatterPositional(segments_guest_address, length, byte_offset, out_bytes_read,
                                   apc_context, coalesce, unlocked);
    }
  }

  std::lock_guard<std::mutex> lock(file_lock_);
  X_STATUS result = X_STATUS_SUCCESS;

  // segments points to an array of buffer pointers of type
  // "FILE_SEGMENT_ELEMENT", but they can just be treated as normal pointers
  rex::be<uint32_t>* segments =
      reinterpret_cast<rex::be<uint32_t>*>(memory()->TranslateVirtual(segments_guest_address));

  // TODO: not sure if this is meant to change depending on buffer address?
  // (only game seen using this always seems to use 4096-byte buffers)
  uint32_t page_size = 4096;

  uint32_t read_total = 0;
  uint32_t read_remain = length;
  while (read_remain) {
    uint32_t read_length = read_remain;
    uint32_t read_buffer = *segments;
    if (read_length > page_size) {
      read_length = page_size;
      segments++;
    }

    uint32_t bytes_read = 0;
    result = ReadInternal(
        read_buffer, read_length,
        byte_offset
            ? ((byte_offset != -1 && byte_offset != -2) ? byte_offset + read_total : byte_offset)
            : -1,
        &bytes_read, apc_context, false);

    if (result != X_STATUS_SUCCESS) {
      break;
    }

    read_total += bytes_read;
    read_remain -= read_length;
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(read_total);
  }

  XIOCompletion::IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = uint32_t(read_total);
  notify.status = result;

  NotifyIOCompletionPorts(notify);

  async_event_->Set();

  return result;
}

X_STATUS XFile::ReadScatterPositional(uint32_t segments_guest_address, uint32_t length,
                                      uint64_t byte_offset, uint32_t* out_bytes_read,
                                      uint32_t apc_context, bool coalesce, bool unlocked) {
  // Same results as the loop in ReadScatter: segment by segment at
  // byte_offset + read_total, stopping at the first status that isn't success.
  // Each successful read there sets position_, so the last one is kept here and
  // stored once at the end.
  std::unique_lock<std::mutex> lock(file_lock_, std::defer_lock);
  std::shared_lock<std::shared_mutex> read_lock(unlocked_read_lock_, std::defer_lock);
  if (unlocked) {
    read_lock.lock();
  } else {
    lock.lock();
  }
  X_STATUS result = X_STATUS_SUCCESS;

  rex::be<uint32_t>* segments =
      reinterpret_cast<rex::be<uint32_t>*>(memory()->TranslateVirtual(segments_guest_address));
  const uint32_t page_size = 4096;

  uint32_t read_total = 0;
  uint32_t read_remain = length;
  bool position_set = false;
  uint64_t new_position = 0;
  // Pages still to be read one at a time after a run that failed or came back
  // short, so the outcome is decided exactly as in ReadScatter.
  uint32_t single_pages = 0;
  while (read_remain) {
    if (coalesce && !single_pages && read_remain > page_size) {
      // Following segments that continue the first one. Only entries that the
      // page loop would also use are read.
      const uint64_t run_start = uint32_t(segments[0]);
      uint32_t run_pages = 1;
      while (uint64_t(run_pages) * page_size < read_remain &&
             uint32_t(segments[run_pages]) == run_start + uint64_t(run_pages) * page_size) {
        ++run_pages;
      }
      if (run_pages > 1) {
        const uint32_t run_length =
            uint32_t(std::min<uint64_t>(uint64_t(run_pages) * page_size, read_remain));
        const uint64_t run_offset = byte_offset + read_total;
        size_t run_bytes = 0;
        if (IsSingleHostBuffer(memory(), uint32_t(run_start), run_length) &&
            ReadData(uint32_t(run_start), run_length, run_offset, &run_bytes) ==
                X_STATUS_SUCCESS &&
            run_bytes == run_length) {
          position_set = true;
          new_position = run_offset + run_length;
          read_total += run_length;
          read_remain -= run_length;
          segments += run_pages;
          continue;
        }
        single_pages = run_pages;
      }
    }
    if (single_pages) {
      --single_pages;
    }

    uint32_t read_length = read_remain;
    uint32_t read_buffer = *segments;
    if (read_length > page_size) {
      read_length = page_size;
      segments++;
    }

    const uint64_t read_offset = byte_offset + read_total;
    size_t bytes_read = 0;
    result = ReadData(read_buffer, read_length, read_offset, &bytes_read);
    if (XSUCCEEDED(result)) {
      position_set = true;
      new_position = read_offset + bytes_read;
    }

    if (result != X_STATUS_SUCCESS) {
      break;
    }

    read_total += uint32_t(bytes_read);
    read_remain -= read_length;
  }

  if (unlocked) {
    read_lock.unlock();
    lock.lock();
  }
  if (position_set) {
    position_ = new_position;
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(read_total);
  }

  XIOCompletion::IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = uint32_t(read_total);
  notify.status = result;

  NotifyIOCompletionPorts(notify);

  async_event_->Set();

  return result;
}

X_STATUS XFile::Write(uint32_t buffer_guest_address, uint32_t buffer_length, uint64_t byte_offset,
                      uint32_t* out_bytes_written, uint32_t apc_context) {
  std::lock_guard<std::mutex> lock(file_lock_);
  if (byte_offset == uint64_t(-1)) {
    // Write from current position.
    byte_offset = position_;
  }

  size_t bytes_written = 0;
  X_STATUS result = file_->WriteSync(
      std::span<const uint8_t>(memory()->TranslateVirtual(buffer_guest_address), buffer_length),
      size_t(byte_offset), &bytes_written);
  if (XSUCCEEDED(result)) {
    position_ += bytes_written;
  }

  XIOCompletion::IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = uint32_t(bytes_written);
  notify.status = result;

  NotifyIOCompletionPorts(notify);

  if (out_bytes_written) {
    *out_bytes_written = uint32_t(bytes_written);
  }

  async_event_->Set();
  return result;
}

X_STATUS XFile::SetLength(size_t length) {
  std::lock_guard<std::mutex> lock(file_lock_);
  return file_->SetLength(length);
}

X_STATUS XFile::Rename(const std::filesystem::path& file_path) {
  std::lock_guard<std::mutex> lock(file_lock_);
  // Reads running without file_lock_ use the entry's name for the fixups.
  std::unique_lock<std::shared_mutex> read_lock(unlocked_read_lock_);
  return entry()->Rename(file_path);
}

void XFile::RegisterIOCompletionPort(uint32_t key, object_ref<XIOCompletion> port) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  completion_ports_.push_back({key, port});
}

void XFile::RemoveIOCompletionPort(uint32_t key) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto it = completion_ports_.begin(); it != completion_ports_.end(); it++) {
    if (it->first == key) {
      completion_ports_.erase(it);
      break;
    }
  }
}

bool XFile::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("XFile {:08X} ({})", handle(), file_->entry()->absolute_path().c_str());

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(file_->entry()->absolute_path());
  stream->Write<uint64_t>(position_);
  stream->Write(file_access());
  stream->Write<bool>((file_->entry()->attributes() & rex::filesystem::kFileAttributeDirectory) !=
                      0);
  stream->Write<bool>(is_synchronous_);

  return true;
}

object_ref<XFile> XFile::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto file = new XFile();
  file->kernel_state_ = kernel_state;
  if (!file->RestoreObject(stream)) {
    delete file;
    return nullptr;
  }

  auto abs_path = stream->Read<std::string>();
  uint64_t position = stream->Read<uint64_t>();
  auto access = stream->Read<uint32_t>();
  auto is_directory = stream->Read<bool>();
  auto is_synchronous = stream->Read<bool>();

  REXSYS_DEBUG("XFile {:08X} ({})", file->handle(), abs_path);

  rex::filesystem::File* vfs_file = nullptr;
  rex::filesystem::FileAction action;
  auto res = kernel_state->file_system()->OpenFile(nullptr, abs_path,
                                                   rex::filesystem::FileDisposition::kOpen, access,
                                                   is_directory, false, &vfs_file, &action);
  if (XFAILED(res)) {
    REXSYS_ERROR("Failed to open XFile: error {:08X}", res);
    return object_ref<XFile>(file);
  }

  file->file_ = vfs_file;
  file->unlocked_reads_ok_ = AllowsUnlockedReads(vfs_file);
  file->position_ = position;
  file->is_synchronous_ = is_synchronous;

  return object_ref<XFile>(file);
}

void XFile::NotifyIOCompletionPorts(XIOCompletion::IONotification& notification) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto port : completion_ports_) {
    notification.key_context = port.first;
    port.second->QueueNotification(notification);
  }
}

}  // namespace rex::system
