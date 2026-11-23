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

#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/platform.h>
#include <rex/ui/d3d12/d3d12_util.h>

#if REX_ARCH_AMD64
#include <emmintrin.h>
#endif

REXCVAR_DEFINE_BOOL(d3d12_tiled_shared_memory, true, "GPU/D3D12",
                    "Use tiled shared memory on D3D12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// S4 u diagnostic (logging only, default off). When on, every successful
// UploadRanges call hashes the guest pages it uploaded and compares each hash
// with the one from that page's previous upload. One info line per frame that
// had uploads. Off: UploadRanges runs its original code plus one cvar load and
// one untaken branch per successful call.
REXCVAR_DEFINE_BOOL(shared_memory_upload_hash_log, false, "GPU/D3D12",
                    "Diagnostic: log per frame how many re-uploaded shared memory pages are "
                    "byte-identical to their previous upload (costs one XXH3 pass over "
                    "every uploaded byte on the GPU thread)");

// S4 M1 (default off). When on, UploadRanges copies the guest pages into the
// upload buffer (write-combined memory on discrete GPUs) with SSE2 non-temporal
// stores and an sfence instead of std::memcpy (x86-64 only, std::memcpy
// elsewhere). Same bytes, same order of MakeRangeValid, copy and copy command.
// Off: the original std::memcpy plus one cvar load per UploadRanges call.
REXCVAR_DEFINE_BOOL(shared_memory_upload_nontemporal_copy, false, "GPU/D3D12",
                    "Copy shared memory uploads into the write-combined upload buffer with "
                    "non-temporal (streaming) SSE2 stores and an sfence instead of memcpy");

namespace rex::graphics::d3d12 {

namespace {

// State of the shared_memory_upload_hash_log diagnostic. Only touched while the
// cvar is on, and then always under the mutex.
struct UploadHashLog {
  // Keeping the write watch over a lock (S4 P1) turns guest writes into write
  // watch faults, and a fault invalidates and unwatches the whole 256 KB block:
  // the shared memory and primitive processor callbacks both widen non-exact
  // ranges to 256 KB. The block counters estimate that.
  static constexpr uint32_t kBlockSizeLog2 = 18;
  static constexpr uint32_t kBlockCount = SharedMemory::kBufferSize >> kBlockSizeLog2;

  std::mutex mutex;
  uint32_t page_size_log2 = 0;
  std::vector<uint64_t> page_hashes;  // per page, meaningful where page_hashed is set
  std::vector<uint64_t> page_hashed;  // bit set: the page was hashed by an earlier upload
  uint64_t frame = 0;
  // Counters of the frame being collected.
  uint32_t calls = 0, ranges = 0, pages = 0, pages_first = 0, pages_unchanged = 0;
  uint64_t blocks_uploaded[kBlockCount / 64] = {};
  uint64_t blocks_changed[kBlockCount / 64] = {};
  uint32_t block_pages_rehashed[kBlockCount] = {};  // re-uploaded pages (not first) per block
};
UploadHashLog upload_hash_log;

void UploadHashLogFlushFrame(UploadHashLog& log) {
  if (log.pages) {
    uint32_t pages_rehashed = log.pages - log.pages_first;
    uint32_t blocks = 0, blocks_changed = 0, pages_in_changed_blocks = 0;
    for (uint32_t block = 0; block < UploadHashLog::kBlockCount; ++block) {
      uint64_t block_bit = uint64_t(1) << (block & 63);
      if (log.blocks_uploaded[block >> 6] & block_bit) {
        ++blocks;
      }
      if (log.blocks_changed[block >> 6] & block_bit) {
        ++blocks_changed;
        pages_in_changed_blocks += log.block_pages_rehashed[block];
      }
    }
    // u_page: share of the re-uploaded pages that are byte-identical to their
    // previous upload. u_block: share of the re-uploaded pages in 256 KB blocks
    // with no changed page. -1 if the frame only had first uploads. Both are
    // upper bounds for P1: an identical rewrite counts as unchanged here but
    // still faults and re-uploads under P1.
    double u_page = -1.0, u_block = -1.0;
    if (pages_rehashed) {
      u_page = double(log.pages_unchanged) / double(pages_rehashed);
      u_block = 1.0 - double(pages_in_changed_blocks) / double(pages_rehashed);
    }
    REXGPU_INFO(
        "S4 upload hash: frame {} calls {} ranges {} pages {} ({} KB) first {} unchanged {} "
        "changed {} blocks {} changed_blocks {} pages_in_changed_blocks {} u_page {:.4f} "
        "u_block {:.4f}",
        log.frame, log.calls, log.ranges, log.pages,
        (uint64_t(log.pages) << log.page_size_log2) >> 10, log.pages_first,
        log.pages_unchanged, pages_rehashed - log.pages_unchanged, blocks, blocks_changed,
        pages_in_changed_blocks, u_page, u_block);
  }
  log.calls = 0;
  log.ranges = 0;
  log.pages = 0;
  log.pages_first = 0;
  log.pages_unchanged = 0;
  std::memset(log.blocks_uploaded, 0, sizeof(log.blocks_uploaded));
  std::memset(log.blocks_changed, 0, sizeof(log.blocks_changed));
  std::memset(log.block_pages_rehashed, 0, sizeof(log.block_pages_rehashed));
}

void UploadHashLogBeginCall(UploadHashLog& log, uint64_t frame, uint32_t page_size_log2) {
  if (log.page_hashes.empty()) {
    log.page_size_log2 = page_size_log2;
    size_t page_count = size_t(SharedMemory::kBufferSize) >> page_size_log2;
    log.page_hashes.assign(page_count, 0);
    log.page_hashed.assign((page_count + 63) / 64, 0);
  }
  if (frame != log.frame) {
    UploadHashLogFlushFrame(log);
    log.frame = frame;
  }
  ++log.calls;
}

void UploadHashLogPages(UploadHashLog& log, const uint8_t* guest_pages, uint32_t page_first,
                        uint32_t page_count) {
  uint32_t page_total = uint32_t(log.page_hashes.size());
  if (page_first >= page_total) {
    return;
  }
  if (page_count > page_total - page_first) {
    page_count = page_total - page_first;
  }
  ++log.ranges;
  size_t page_size = size_t(1) << log.page_size_log2;
  for (uint32_t i = 0; i < page_count; ++i) {
    uint32_t page = page_first + i;
    uint64_t hash = XXH3_64bits(guest_pages + page_size * i, page_size);
    uint32_t block =
        uint32_t((uint64_t(page) << log.page_size_log2) >> UploadHashLog::kBlockSizeLog2);
    uint64_t block_bit = uint64_t(1) << (block & 63);
    log.blocks_uploaded[block >> 6] |= block_bit;
    uint64_t& hashed_bits = log.page_hashed[page >> 6];
    uint64_t hashed_bit = uint64_t(1) << (page & 63);
    if (!(hashed_bits & hashed_bit)) {
      hashed_bits |= hashed_bit;
      ++log.pages_first;
    } else {
      ++log.block_pages_rehashed[block];
      if (log.page_hashes[page] == hash) {
        ++log.pages_unchanged;
      } else {
        log.blocks_changed[block >> 6] |= block_bit;
      }
    }
    log.page_hashes[page] = hash;
  }
  log.pages += page_count;
}

// S4 M1 (shared_memory_upload_nontemporal_copy): std::memcpy replacement for
// copying guest memory into the upload buffer mapping, which is write-combined
// on discrete GPUs and never read back here. UploadRanges passes a page-aligned
// dst and a whole number of pages, so normally only the streaming loop runs;
// the head and tail keep it correct for any alignment and size:
// - head: plain copy up to the first 64-byte boundary of dst, because
//   _mm_stream_si128 needs 16-byte alignment (it faults otherwise), and so no
//   cache line gets both plain and streaming stores;
// - body: whole 64-byte lines, unaligned loads from src, aligned streaming
//   stores to dst (4 x 16 bytes fill one write-combining buffer);
// - tail: plain copy of the last size % 64 bytes.
// Streaming stores (and plain stores to write-combined memory) are weakly
// ordered: the sfence makes them all globally visible before returning, so
// before the copy command is recorded and long before ExecuteCommandLists lets
// the GPU read them.
void CopyToUploadBufferNonTemporal(uint8_t* dst, const uint8_t* src, size_t size) {
#if REX_ARCH_AMD64
  size_t head = (64 - (reinterpret_cast<uintptr_t>(dst) & 63)) & 63;
  if (head > size) {
    head = size;
  }
  if (head) {
    std::memcpy(dst, src, head);
    dst += head;
    src += head;
    size -= head;
  }
  size_t body = size & ~size_t(63);
  for (size_t offset = 0; offset < body; offset += 64) {
    __m128i data0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + offset));
    __m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + offset + 16));
    __m128i data2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + offset + 32));
    __m128i data3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + offset + 48));
    _mm_stream_si128(reinterpret_cast<__m128i*>(dst + offset), data0);
    _mm_stream_si128(reinterpret_cast<__m128i*>(dst + offset + 16), data1);
    _mm_stream_si128(reinterpret_cast<__m128i*>(dst + offset + 32), data2);
    _mm_stream_si128(reinterpret_cast<__m128i*>(dst + offset + 48), data3);
  }
  if (size > body) {
    std::memcpy(dst + body, src + body, size - body);
  }
  _mm_sfence();
#else
  std::memcpy(dst, src, size);
#endif
}

}  // namespace

D3D12SharedMemory::D3D12SharedMemory(D3D12CommandProcessor& command_processor,
                                     memory::Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

D3D12SharedMemory::~D3D12SharedMemory() {
  Shutdown(true);
}

bool D3D12SharedMemory::Initialize() {
  InitializeCommon();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kBufferSize,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (REXCVAR_GET(d3d12_tiled_shared_memory) &&
      provider.GetTiledResourcesTier() != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
      !provider.GetGraphicsAnalysis()) {
    if (FAILED(device->CreateReservedResource(&buffer_desc, buffer_state_, nullptr,
                                              IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB tiled buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    static_assert(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == (1 << 16));
    InitializeSparseHostGpuMemory(
        std::max(kHostGpuMemoryOptimalSparseAllocationLog2, uint32_t(16)));
  } else {
    REXGPU_INFO(
        "Direct3D 12 tiled resources are not used for shared memory "
        "emulation - video memory usage may increase significantly "
        "because a full {} MB buffer will be created",
        kBufferSize >> 20);
    if (provider.GetGraphicsAnalysis()) {
      // As of October 8th, 2018, PIX doesn't support tiled buffers.
      // FIXME(Triang3l): Re-enable tiled resources with PIX once fixed.
      REXGPU_INFO(
          "This is caused by PIX being attached, which doesn't support tiled "
          "resources yet.");
    }
    if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                               provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                               buffer_state_, nullptr, IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
  }
  buffer_gpu_address_ = buffer_->GetGPUVirtualAddress();
  buffer_uav_writes_commit_needed_ = false;

  D3D12_DESCRIPTOR_HEAP_DESC buffer_descriptor_heap_desc;
  buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  buffer_descriptor_heap_desc.NumDescriptors = uint32_t(BufferDescriptorIndex::kCount);
  buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&buffer_descriptor_heap_)))) {
    REXGPU_ERROR("Shared memory: Failed to create the descriptor heap for buffer views");
    Shutdown();
    return false;
  }
  buffer_descriptor_heap_start_ = buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintSRV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintUAV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);

  upload_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, rex::align(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           size_t(1) << page_size_log2()));

  return true;
}

void D3D12SharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();

  ui::d3d12::util::ReleaseAndNull(buffer_descriptor_heap_);

  // First free the buffer to detach it from the heaps.
  ui::d3d12::util::ReleaseAndNull(buffer_);

  for (ID3D12Heap* heap : buffer_tiled_heaps_) {
    heap->Release();
  }
  buffer_tiled_heaps_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12SharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void D3D12SharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void D3D12SharedMemory::BeginSubmission() {
  // ExecuteCommandLists is a full UAV barrier.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state) {
  if (buffer_state_ == new_state) {
    if (new_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && buffer_uav_writes_commit_needed_) {
      command_processor_.PushUAVBarrier(buffer_);
      buffer_uav_writes_commit_needed_ = false;
    }
    return;
  }
  command_processor_.PushTransitionBarrier(buffer_, buffer_state_, new_state);
  buffer_state_ = new_state;
  // "UAV -> anything" transition commits the writes implicitly.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                         uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  uint32_t offset_bytes = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  uint32_t length_bytes = length_allocations << host_gpu_memory_sparse_granularity_log2();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = length_bytes;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed();
  ID3D12Heap* heap;
  if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    REXGPU_ERROR("Shared memory: Failed to create a tile heap");
    return false;
  }
  buffer_tiled_heaps_.push_back(heap);

  D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
  region_start_coordinates.X = offset_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_start_coordinates.Y = 0;
  region_start_coordinates.Z = 0;
  region_start_coordinates.Subresource = 0;
  D3D12_TILE_REGION_SIZE region_size;
  region_size.NumTiles = length_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_size.UseBox = FALSE;
  D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_range_start_offset = 0;
  direct_queue->UpdateTileMappings(buffer_, 1, &region_start_coordinates, &region_size, heap, 1,
                                   &range_flags, &heap_range_start_offset, &region_size.NumTiles,
                                   D3D12_TILE_MAPPING_FLAG_NONE);
  command_processor_.NotifyQueueOperationsDoneDirectly();
  return true;
}

bool D3D12SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  auto& command_list = command_processor_.GetDeferredCommandList();
  const bool nontemporal_copy = REXCVAR_GET(shared_memory_upload_nontemporal_copy);
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length != 0) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), &upload_buffer, &upload_buffer_offset, &upload_buffer_size,
          nullptr);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
        return false;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      if (nontemporal_copy) {
        CopyToUploadBufferNonTemporal(
            upload_buffer_mapping,
            memory().TranslatePhysical(upload_range_start << page_size_log2()),
            upload_buffer_size);
      } else {
        std::memcpy(upload_buffer_mapping,
                    memory().TranslatePhysical(upload_range_start << page_size_log2()),
                    upload_buffer_size);
      }
      command_list.D3DCopyBufferRegion(buffer_, upload_range_start << page_size_log2(),
                                       upload_buffer, UINT64(upload_buffer_offset),
                                       UINT64(upload_buffer_size));
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
  }
  if (REXCVAR_GET(shared_memory_upload_hash_log)) {
    // S4 u diagnostic: hash the guest copy of every page just uploaded (never
    // the write-combined upload mapping, which is very slow to read).
    std::lock_guard<std::mutex> upload_hash_log_lock(upload_hash_log.mutex);
    UploadHashLogBeginCall(upload_hash_log, command_processor_.GetCurrentFrame(),
                           page_size_log2());
    for (const auto& upload_range : upload_page_ranges) {
      UploadHashLogPages(upload_hash_log,
                         static_cast<const uint8_t*>(
                             memory().TranslatePhysical(upload_range.first << page_size_log2())),
                         upload_range.first, upload_range.second);
    }
  }
  return true;
}

}  // namespace rex::graphics::d3d12
