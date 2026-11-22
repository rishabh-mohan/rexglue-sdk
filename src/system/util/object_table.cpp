/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

// Agent 11 (task #10, item 7): see perf-notes/agent11-global-mutex-design.md.
REXCVAR_DEFINE_BOOL(object_lookup_fast_path, false, "Kernel",
                    "Experimental: handle lookups (and the already-initialized path of "
                    "XObject::GetNativeObject) take a reader-writer lock on the handle table "
                    "instead of the global critical region. Writers still take the global "
                    "critical region. Also turns on guest_suspend_guard. Read once at startup");

namespace rex::system::util {

namespace {

// Lock "T" from the design doc. It protects only the table memory
// (table_, table_capacity_, last_free_entry_ and every entry.object) when the
// fast path is on. Lock order: G (global critical region) -> T -> name_mutex_.
// Rules (perf-notes/agent12-gcr-review.md):
//  - T shared is a leaf. A fast lookup holds it only for the slot read and the
//    Retain, never acquires it twice and takes no other lock under it. SRW is
//    not re-entrant: a recursive shared acquire deadlocks once a writer queues
//    between the two acquires (Agent 12's bench S1).
//  - Never take G, or U (XFile::unlocked_read_lock_), while holding T in
//    either mode.
// std::shared_mutex is an SRW lock on MSVC: trivially destructible, so it is
// safe even if ObjectTable is destroyed during static destruction.
std::shared_mutex table_lock_;

// Latched once, before any guest thread exists (ObjectTable is constructed with
// KernelState). Readers and writers test the same value, so they always agree.
std::atomic<bool> fast_path_enabled_{false};

// SRW is not recursive. This makes writer sections re-entrant (Restore ->
// Resize, AddHandle -> FindFreeSlot -> Resize) and sends any lookup made while
// this thread holds T exclusive down the old G path. Invariant: a section
// that holds T without G (only Restore and RestoreHandle) must not look up
// objects, since that would take G after T (lock order is G -> T).
thread_local uint32_t table_write_depth_ = 0;

inline bool FastPathEnabled() {
  return fast_path_enabled_.load(std::memory_order_acquire);
}

void LockTableExclusive() {
  if (table_write_depth_++ == 0) {
    table_lock_.lock();
  }
}

void UnlockTableExclusive() {
  if (--table_write_depth_ == 0) {
    table_lock_.unlock();
  }
}

// Takes T exclusive only when the fast path is on. With it off this is a no-op
// and the surrounding code is exactly the old code.
class TableWriteLock {
 public:
  TableWriteLock() : locked_(FastPathEnabled()) {
    if (locked_) {
      LockTableExclusive();
    }
  }
  ~TableWriteLock() {
    if (locked_) {
      UnlockTableExclusive();
    }
  }
  TableWriteLock(const TableWriteLock&) = delete;
  TableWriteLock& operator=(const TableWriteLock&) = delete;

 private:
  bool locked_;
};

}  // namespace

// Used by xobject.cpp (GetNativeObject fast path) and xthread.cpp (suspend guard).
bool ObjectLookupFastPathEnabled() {
  return FastPathEnabled();
}

// XThread::Suspend (guarded path) holds G and then T exclusive across
// SuspendThread + GetThreadContext, so the target never stops while it holds T
// shared. No-ops when the fast path is off.
void LockObjectTableForSuspend() {
  if (FastPathEnabled()) {
    LockTableExclusive();
  }
}

void UnlockObjectTableForSuspend() {
  if (FastPathEnabled()) {
    UnlockTableExclusive();
  }
}

ObjectTable::ObjectTable() {
  // Latched for the whole process: a second table (if one is ever made) must
  // not flip the flag while another table is in use.
  static const bool fast_path = REXCVAR_GET(object_lookup_fast_path);
  fast_path_enabled_.store(fast_path, std::memory_order_release);
}

ObjectTable::~ObjectTable() {
  Reset();
}

void ObjectTable::Reset() {
  auto global_lock = global_critical_region_.Acquire();

  if (FastPathEnabled()) {
    // Detach the table under T, then release outside T: no destructor runs
    // while T is held.
    ObjectTableEntry* old_table = nullptr;
    uint32_t old_capacity = 0;
    {
      TableWriteLock table_lock;
      old_table = table_;
      old_capacity = table_capacity_;
      table_ = nullptr;
      table_capacity_ = 0;
      last_free_entry_ = 0;
    }
    for (uint32_t n = 0; n < old_capacity; n++) {
      ObjectTableEntry& entry = old_table[n];
      if (entry.object) {
        entry.object->Release();
      }
    }
    free(old_table);
    return;
  }

  // Release all objects.
  for (uint32_t n = 0; n < table_capacity_; n++) {
    ObjectTableEntry& entry = table_[n];
    if (entry.object) {
      entry.object->Release();
    }
  }

  table_capacity_ = 0;
  last_free_entry_ = 0;
  free(table_);
  table_ = nullptr;
}

X_STATUS ObjectTable::FindFreeSlot(uint32_t* out_slot) {
  // Find a free slot.
  uint32_t slot = last_free_entry_;
  uint32_t scan_count = 0;
  while (scan_count < table_capacity_) {
    ObjectTableEntry& entry = table_[slot];
    if (!entry.object) {
      *out_slot = slot;
      return X_STATUS_SUCCESS;
    }
    scan_count++;
    slot = (slot + 1) % table_capacity_;
    if (slot == 0) {
      // Never allow 0 handles.
      scan_count++;
      slot++;
    }
  }

  // Table out of slots, expand.
  uint32_t new_table_capacity = std::max(16 * 1024u, table_capacity_ * 2);
  if (!Resize(new_table_capacity)) {
    return X_STATUS_NO_MEMORY;
  }

  // Never allow 0 handles.
  slot = ++last_free_entry_;
  *out_slot = slot;

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Resize(uint32_t new_capacity) {
  // realloc may free the old array: no fast reader may be inside it.
  TableWriteLock table_lock;

  uint32_t new_size = new_capacity * sizeof(ObjectTableEntry);
  uint32_t old_size = table_capacity_ * sizeof(ObjectTableEntry);
  auto new_table = reinterpret_cast<ObjectTableEntry*>(realloc(table_, new_size));
  if (!new_table) {
    return false;
  }

  // Zero out new entries.
  if (new_size > old_size) {
    std::memset(reinterpret_cast<uint8_t*>(new_table) + old_size, 0, new_size - old_size);
  }

  last_free_entry_ = table_capacity_;
  table_capacity_ = new_capacity;
  table_ = new_table;

  return true;
}

X_STATUS ObjectTable::AddHandle(XObject* object, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  uint32_t handle = 0;
  {
    auto global_lock = global_critical_region_.Acquire();
    // The Retain below happens inside the same T section that publishes the
    // entry, so a fast reader never sees an entry whose table ref is missing.
    TableWriteLock table_lock;

    // Find a free slot.
    uint32_t slot = 0;
    result = FindFreeSlot(&slot);

    // Stash.
    if (XSUCCEEDED(result)) {
      ObjectTableEntry& entry = table_[slot];
      entry.object = object;
      entry.handle_ref_count = 1;
      handle = XObject::kHandleBase + (slot << 2);
      object->handles().push_back(handle);

      // Retain so long as the object is in the table.
      object->Retain();

      REXSYS_NOISY_DEBUG("Added handle:{:08X} for {}", handle, typeid(*object).name());
    }
  }

  if (XSUCCEEDED(result)) {
    if (out_handle) {
      *out_handle = handle;
    }
  }

  return result;
}

X_STATUS ObjectTable::DuplicateHandle(X_HANDLE handle, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;
  handle = TranslateHandle(handle);

  XObject* object = LookupObject(handle, false);
  if (object) {
    result = AddHandle(object, out_handle);
    object->Release();  // Release the ref that LookupObject took
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

X_STATUS ObjectTable::RetainHandle(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  entry->handle_ref_count++;
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::ReleaseHandle(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (--entry->handle_ref_count == 0) {
    // No more references. Remove it from the table.
    return RemoveHandle(handle);
  }

  // FIXME: Return a status code telling the caller it wasn't released
  // (but not a failure code)
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::RemoveHandle(X_HANDLE handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  handle = TranslateHandle(handle);
  if (!handle) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Take G before LookupTable, not after it: Resize can realloc the table, so
  // the entry pointer is only valid while G is held (part A of the design;
  // reviewed by Agent 12). No new lock order: LookupTable takes G itself.
  auto global_lock = global_critical_region_.Acquire();
  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (entry->object) {
    auto object = entry->object;
    {
      // Unpublish under T; everything else (including Release) stays under G only.
      TableWriteLock table_lock;
      entry->object = nullptr;
    }
    assert_zero(entry->handle_ref_count);
    entry->handle_ref_count = 0;

    // Walk the object's handles and remove this one.
    auto handle_entry = std::find(object->handles().begin(), object->handles().end(), handle);
    if (handle_entry != object->handles().end()) {
      object->handles().erase(handle_entry);
    }

    REXSYS_NOISY_DEBUG("Removed handle:{:08X} for {}", handle, typeid(*object).name());

    // Remove object name from mapping to prevent naming collision.
    if (!object->name().empty()) {
      RemoveNameMapping(object->name());
    }
    // Release now that the object has been removed from the table.
    object->Release();
  }

  return X_STATUS_SUCCESS;
}

std::vector<object_ref<XObject>> ObjectTable::GetAllObjects() {
  auto lock = global_critical_region_.Acquire();
  std::vector<object_ref<XObject>> results;

  for (uint32_t slot = 0; slot < table_capacity_; slot++) {
    auto& entry = table_[slot];
    if (entry.object && std::find(results.begin(), results.end(), entry.object) == results.end()) {
      entry.object->Retain();
      results.push_back(object_ref<XObject>(entry.object));
    }
  }

  return results;
}

void ObjectTable::PurgeAllObjects() {
  auto lock = global_critical_region_.Acquire();

  if (FastPathEnabled()) {
    // Unpublish under T, release outside T.
    std::vector<XObject*> purged;
    {
      TableWriteLock table_lock;
      for (uint32_t slot = 0; slot < table_capacity_; slot++) {
        auto& entry = table_[slot];
        if (entry.object && !entry.object->is_host_object()) {
          entry.handle_ref_count = 0;
          purged.push_back(entry.object);
          entry.object = nullptr;
        }
      }
    }
    for (XObject* object : purged) {
      object->Release();
    }
    return;
  }

  for (uint32_t slot = 0; slot < table_capacity_; slot++) {
    auto& entry = table_[slot];
    if (entry.object && !entry.object->is_host_object()) {
      entry.handle_ref_count = 0;
      entry.object->Release();

      entry.object = nullptr;
    }
  }
}

ObjectTable::ObjectTableEntry* ObjectTable::LookupTable(X_HANDLE handle) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Lower 2 bits are ignored.
  uint32_t slot = GetHandleSlot(handle);
  if (slot < table_capacity_) {
    return &table_[slot];
  }

  return nullptr;
}

// Generic lookup
template <>
object_ref<XObject> ObjectTable::LookupObject<XObject>(X_HANDLE handle) {
  auto object = ObjectTable::LookupObject(handle, false);
  auto result = object_ref<XObject>(reinterpret_cast<XObject*>(object));
  return result;
}

XObject* ObjectTable::LookupObject(X_HANDLE handle, bool already_locked) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return nullptr;
  }

  if (!already_locked && FastPathEnabled() && table_write_depth_ == 0) {
    // Fast path: T shared, no G. Only the slot read and one atomic increment
    // happen under T (see the memory-model argument in the design doc).
    XObject* object = nullptr;
    std::shared_lock<std::shared_mutex> table_lock(table_lock_);
    uint32_t slot = GetHandleSlot(handle);
    if (slot < table_capacity_) {
      object = table_[slot].object;
      if (object) {
        object->Retain();
      }
    }
    return object;
  }

  XObject* object = nullptr;
  if (!already_locked) {
    global_critical_region_.mutex().lock();
  }

  // Lower 2 bits are ignored.
  uint32_t slot = GetHandleSlot(handle);

  // Verify slot.
  if (slot < table_capacity_) {
    ObjectTableEntry& entry = table_[slot];
    if (entry.object) {
      object = entry.object;
    }
  }

  // Retain the object pointer.
  if (object) {
    object->Retain();
  }

  if (!already_locked) {
    global_critical_region_.mutex().unlock();
  }

  return object;
}

void ObjectTable::GetObjectsByType(XObject::Type type, std::vector<object_ref<XObject>>* results) {
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t slot = 0; slot < table_capacity_; ++slot) {
    auto& entry = table_[slot];
    if (entry.object) {
      if (entry.object->type() == type) {
        entry.object->Retain();
        results->push_back(object_ref<XObject>(entry.object));
      }
    }
  }
}

X_HANDLE ObjectTable::TranslateHandle(X_HANDLE handle) {
  if (handle == 0xFFFFFFFF) {
    // CurrentProcess
    // assert_always();
    return 0;
  } else if (handle == 0xFFFFFFFE) {
    // CurrentThread
    return XThread::GetCurrentThreadHandle();
  } else {
    return handle;
  }
}

X_STATUS ObjectTable::AddNameMapping(const std::string_view name, X_HANDLE handle) {
  std::lock_guard<std::mutex> name_lock(name_mutex_);
  if (name_table_.count(string::string_key_case(name))) {
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  name_table_.insert({string::string_key_case::create(name), handle});
  return X_STATUS_SUCCESS;
}

void ObjectTable::RemoveNameMapping(const std::string_view name) {
  // Names are case-insensitive.
  std::lock_guard<std::mutex> name_lock(name_mutex_);
  auto it = name_table_.find(string::string_key_case(name));
  if (it != name_table_.end()) {
    name_table_.erase(it);
  }
}

X_STATUS ObjectTable::GetObjectByName(const std::string_view name, X_HANDLE* out_handle) {
  // Names are case-insensitive.
  // Look up handle under name lock only -- do NOT hold name_mutex_ while
  // acquiring global lock (RemoveHandle takes global -> name ordering).
  X_HANDLE handle;
  {
    std::lock_guard<std::mutex> name_lock(name_mutex_);
    auto it = name_table_.find(string::string_key_case(name));
    if (it == name_table_.end()) {
      *out_handle = X_INVALID_HANDLE_VALUE;
      return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    handle = it->second;
  }
  *out_handle = handle;

  // Retain under global lock via normal LookupObject path.
  // The handle may have been removed between releasing name_mutex_ and
  // acquiring global lock -- LookupObject returns nullptr in that case.
  auto obj = LookupObject(handle, false);
  if (obj) {
    obj->RetainHandle();
    obj->Release();
  }

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Save(stream::ByteStream* stream) {
  stream->Write<uint32_t>(table_capacity_);
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    stream->Write<int32_t>(entry.handle_ref_count);
  }

  return true;
}

bool ObjectTable::Restore(stream::ByteStream* stream) {
  // T without G: must be a top-level T section and must not look up objects
  // (that would take G after T). The depth is always 0 with the fast path off.
  assert_true(table_write_depth_ == 0);
  TableWriteLock table_lock;  // No G here today; T keeps fast readers out.
  Resize(stream->Read<uint32_t>());
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    // entry.object = nullptr;
    entry.handle_ref_count = stream->Read<int32_t>();
  }

  return true;
}

X_STATUS ObjectTable::RestoreHandle(X_HANDLE handle, XObject* object) {
  assert_true(table_write_depth_ == 0);  // Same rule as Restore.
  TableWriteLock table_lock;  // No G here today; T keeps fast readers out.
  uint32_t slot = GetHandleSlot(handle);
  assert_true(table_capacity_ > slot);

  if (table_capacity_ > slot) {
    auto& entry = table_[slot];
    entry.object = object;
    object->Retain();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::system::util
