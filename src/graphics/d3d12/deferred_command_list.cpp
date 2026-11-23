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

#include <rex/assert.h>
#include <rex/dbg.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/flags.h>
#include <rex/math.h>

// S4 #20 step 1 (R2): per-command-type counters for Execute. They only count:
// every command still reaches the real command list, unchanged and in order.
// Compiled out by default. Build this file with -DREX_D3D12_DEFERRED_COUNTERS=1
// to log, about every 10 s, how many commands of each type Execute replayed and
// how many pure state setters set exactly the state already set earlier in the
// same native command list (what a step 2 filter would drop). The macro changes
// no header, class layout or other file.
#ifndef REX_D3D12_DEFERRED_COUNTERS
#define REX_D3D12_DEFERRED_COUNTERS 0
#endif

#if REX_D3D12_DEFERRED_COUNTERS
#include <chrono>
#include <cstring>

#include <rex/logging.h>
#endif  // REX_D3D12_DEFERRED_COUNTERS

namespace rex::graphics::d3d12 {

#if REX_D3D12_DEFERRED_COUNTERS
namespace {

// DeferredCommandList::Command names, in enum order (Execute checks the count).
constexpr const char* kDeferredCommandNames[] = {
    "ClearDepthStencilView",
    "ClearRenderTargetView",
    "ClearUnorderedAccessViewUint",
    "CopyBufferRegion",
    "CopyResource",
    "CopyTexture",
    "CopyTextureRegion",
    "Dispatch",
    "DrawIndexedInstanced",
    "DrawInstanced",
    "BeginQuery",
    "EndQuery",
    "ResolveQueryData",
    "IASetIndexBuffer",
    "IASetPrimitiveTopology",
    "IASetVertexBuffers",
    "OMSetBlendFactor",
    "OMSetRenderTargets",
    "OMSetStencilRef",
    "ResourceBarrier",
    "RSSetScissorRect",
    "RSSetViewport",
    "SetComputeRoot32BitConstants",
    "SetGraphicsRoot32BitConstants",
    "SetComputeRootConstantBufferView",
    "SetGraphicsRootConstantBufferView",
    "SetComputeRootDescriptorTable",
    "SetGraphicsRootDescriptorTable",
    "SetComputeRootShaderResourceView",
    "SetGraphicsRootShaderResourceView",
    "SetComputeRootSignature",
    "SetGraphicsRootSignature",
    "SetComputeRootUnorderedAccessView",
    "SetGraphicsRootUnorderedAccessView",
    "SetDescriptorHeaps",
    "SetPipelineState",
    "SetPipelineStateHandle",
    "SetSamplePositions",
    "BeginDebugMarker",
    "EndDebugMarker",
    "InsertDebugMarker",
};
constexpr size_t kDeferredCommandCount =
    sizeof(kDeferredCommandNames) / sizeof(kDeferredCommandNames[0]);

// Counts since the last log line. Touched only by Execute, which runs only
// inside D3D12CommandProcessor::EndSubmission on the GPU Commands thread.
struct DeferredCommandCounters {
  uint64_t total[kDeferredCommandCount] = {};
  // Pure state setters that set exactly the state already set earlier in the
  // same native command list: what a step 2 filter could drop.
  uint64_t repeat[kDeferredCommandCount] = {};
  // Barriers inside ResourceBarrier commands. Counted only; never filter targets.
  uint64_t barriers = 0;
  // Execute calls (native command lists) and their stream bytes.
  uint64_t lists = 0;
  uint64_t bytes = 0;
  std::chrono::steady_clock::time_point last_log = {};
};
DeferredCommandCounters deferred_command_counters;

// The state that the commands replayed so far have set on the native command
// list; a pure state setter that sets exactly this counts as a repeat. Execute
// constructs one, empty, for each call. Each call replays one submission into
// a native command list that EndSubmission has just reset, and nothing else
// records into that list before its Close. So the state starts empty at every
// Execute. An R1 split (S4 #18) is an EndSubmission(false): it ends one
// Execute, and the next EndSubmission resets the native list and starts the
// next from empty. These are R2 conds (b) and (d).
struct DeferredCommandShadow {
  static constexpr UINT kMaxRootParameters = 16;
  static constexpr UINT kMaxRootConstants = 64;
  enum class RootKind : uint8_t { kUnknown, kConstants, kCBV, kSRV, kUAV, kTable };
  struct RootParameter {
    RootKind kind = RootKind::kUnknown;
    // GPU virtual address (CBV, SRV, UAV) or GPU descriptor handle (table).
    uint64_t value = 0;
    // Bit i: constants[i] holds the value set in this list.
    uint64_t constants_valid = 0;
    uint32_t constants[kMaxRootConstants];
  };
  // Graphics and compute keep separate root state. Root parameters past
  // kMaxRootParameters, or constants past kMaxRootConstants, are not tracked
  // and never count as repeats.
  struct Root {
    bool signature_valid = false;
    ID3D12RootSignature* signature = nullptr;
    RootParameter parameters[kMaxRootParameters];

    // Every root signature set resets all root arguments, a redundant one too.
    bool SetSignature(ID3D12RootSignature* new_signature) {
      bool repeat = signature_valid && signature == new_signature;
      signature_valid = true;
      signature = new_signature;
      for (RootParameter& parameter : parameters) {
        parameter.kind = RootKind::kUnknown;
        parameter.constants_valid = 0;
      }
      return repeat;
    }
    bool SetValue(UINT index, RootKind kind, uint64_t value) {
      if (index >= kMaxRootParameters) {
        return false;
      }
      RootParameter& parameter = parameters[index];
      bool repeat = parameter.kind == kind && parameter.value == value;
      parameter.kind = kind;
      parameter.value = value;
      parameter.constants_valid = 0;
      return repeat;
    }
    // Compares every value at its offset.
    bool SetConstants(UINT index, UINT count, UINT offset, const uint32_t* values) {
      if (index >= kMaxRootParameters) {
        return false;
      }
      RootParameter& parameter = parameters[index];
      if (parameter.kind != RootKind::kConstants) {
        parameter.kind = RootKind::kConstants;
        parameter.value = 0;
        parameter.constants_valid = 0;
      }
      if (offset >= kMaxRootConstants || count > kMaxRootConstants - offset) {
        parameter.constants_valid = 0;
        return false;
      }
      uint64_t mask = (count >= 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1) << offset;
      bool repeat = (parameter.constants_valid & mask) == mask &&
                    !std::memcmp(parameter.constants + offset, values, sizeof(uint32_t) * count);
      std::memcpy(parameter.constants + offset, values, sizeof(uint32_t) * count);
      parameter.constants_valid |= mask;
      return repeat;
    }
    // Every descriptor heap set resets the descriptor tables.
    void ResetTables() {
      for (RootParameter& parameter : parameters) {
        if (parameter.kind == RootKind::kTable) {
          parameter.kind = RootKind::kUnknown;
        }
      }
    }
  };

  Root graphics;
  Root compute;
  bool heaps_valid = false;
  ID3D12DescriptorHeap* heaps[2] = {};
  bool topology_valid = false;
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  bool index_buffer_valid = false;
  D3D12_INDEX_BUFFER_VIEW index_buffer = {};
  // The last IASetVertexBuffers call; vertex_buffer_count 0 means none yet.
  UINT vertex_buffer_start = 0;
  UINT vertex_buffer_count = 0;
  D3D12_VERTEX_BUFFER_VIEW vertex_buffers[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
  bool render_targets_valid = false;
  UINT render_target_count = 0;
  bool render_target_range = false;
  D3D12_CPU_DESCRIPTOR_HANDLE render_targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  bool depth_stencil = false;
  D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_descriptor = {};
  bool viewport_valid = false;
  D3D12_VIEWPORT viewport = {};
  bool scissor_valid = false;
  D3D12_RECT scissor = {};
  bool blend_factor_valid = false;
  FLOAT blend_factor[4] = {};
  bool stencil_ref_valid = false;
  UINT stencil_ref = 0;
};

}  // namespace
#endif  // REX_D3D12_DEFERRED_COUNTERS

DeferredCommandList::DeferredCommandList(const D3D12CommandProcessor& command_processor,
                                         size_t initial_size)
    : command_processor_(command_processor) {
  command_stream_.reserve(initial_size / sizeof(uintmax_t));
}

void DeferredCommandList::Reset() {
  command_stream_.clear();
}

void DeferredCommandList::Execute(ID3D12GraphicsCommandList* command_list,
                                  ID3D12GraphicsCommandList1* command_list_1) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  const uintmax_t* stream = command_stream_.data();
  size_t stream_remaining = command_stream_.size();
  ID3D12PipelineState* current_pipeline_state = nullptr;
#if REX_D3D12_DEFERRED_COUNTERS
  static_assert(size_t(Command::kInsertDebugMarker) + 1 == kDeferredCommandCount,
                "kDeferredCommandNames must name every DeferredCommandList::Command");
  // Empty at the start of every Execute (see DeferredCommandShadow).
  DeferredCommandShadow shadow;
  // Counts one command before it is replayed, so current_pipeline_state is
  // still the pipeline from before it. Only pure state setters can repeat
  // (R2 cond (a)); the whole payload is compared (cond (c)).
  auto count_command = [&](Command command, const uintmax_t* args) {
    using Shadow = DeferredCommandShadow;
    auto set_root_value = [args](Shadow::Root& root, Shadow::RootKind kind) {
      auto& view = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(args);
      return root.SetValue(view.root_parameter_index, kind, view.buffer_location);
    };
    bool repeat = false;
    switch (command) {
      case Command::kD3DIASetIndexBuffer: {
        auto& view = *reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(args);
        // A null index buffer (DXGI_FORMAT_UNKNOWN) repeats a null one.
        repeat = shadow.index_buffer_valid && shadow.index_buffer.Format == view.Format &&
                 (view.Format == DXGI_FORMAT_UNKNOWN ||
                  (shadow.index_buffer.BufferLocation == view.BufferLocation &&
                   shadow.index_buffer.SizeInBytes == view.SizeInBytes));
        shadow.index_buffer_valid = true;
        shadow.index_buffer = view;
      } break;
      case Command::kD3DIASetPrimitiveTopology: {
        auto topology = *reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY*>(args);
        repeat = shadow.topology_valid && shadow.topology == topology;
        shadow.topology_valid = true;
        shadow.topology = topology;
      } break;
      case Command::kD3DIASetVertexBuffers: {
        // The whole call, with its start slot and count.
        auto& vb = *reinterpret_cast<const D3DIASetVertexBuffersHeader*>(args);
        auto views = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(
            reinterpret_cast<const uint8_t*>(args) +
            rex::align(sizeof(D3DIASetVertexBuffersHeader), alignof(D3D12_VERTEX_BUFFER_VIEW)));
        if (vb.num_views > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
          shadow.vertex_buffer_count = 0;
          break;
        }
        size_t views_size = sizeof(D3D12_VERTEX_BUFFER_VIEW) * vb.num_views;
        repeat = shadow.vertex_buffer_count != 0 && shadow.vertex_buffer_count == vb.num_views &&
                 shadow.vertex_buffer_start == vb.start_slot &&
                 !std::memcmp(shadow.vertex_buffers, views, views_size);
        shadow.vertex_buffer_start = vb.start_slot;
        shadow.vertex_buffer_count = vb.num_views;
        std::memcpy(shadow.vertex_buffers, views, views_size);
      } break;
      case Command::kD3DOMSetBlendFactor:
        repeat = shadow.blend_factor_valid &&
                 !std::memcmp(shadow.blend_factor, args, sizeof(shadow.blend_factor));
        shadow.blend_factor_valid = true;
        std::memcpy(shadow.blend_factor, args, sizeof(shadow.blend_factor));
        break;
      case Command::kD3DOMSetRenderTargets: {
        // The whole array with its count, the range flag and the DSV handle.
        auto& rts = *reinterpret_cast<const D3DOMSetRenderTargetsArguments*>(args);
        UINT rt_count = rts.num_render_target_descriptors;
        bool rt_range = rts.rts_single_handle_to_descriptor_range;
        UINT rt_handles = rt_range ? std::min(rt_count, UINT(1)) : rt_count;
        repeat = shadow.render_targets_valid && shadow.render_target_count == rt_count &&
                 shadow.render_target_range == rt_range &&
                 shadow.depth_stencil == rts.depth_stencil &&
                 (!rts.depth_stencil ||
                  shadow.depth_stencil_descriptor.ptr == rts.depth_stencil_descriptor.ptr);
        for (UINT i = 0; repeat && i < rt_handles; ++i) {
          repeat = shadow.render_targets[i].ptr == rts.render_target_descriptors[i].ptr;
        }
        shadow.render_targets_valid = true;
        shadow.render_target_count = rt_count;
        shadow.render_target_range = rt_range;
        for (UINT i = 0; i < rt_handles; ++i) {
          shadow.render_targets[i] = rts.render_target_descriptors[i];
        }
        shadow.depth_stencil = rts.depth_stencil;
        shadow.depth_stencil_descriptor = rts.depth_stencil_descriptor;
      } break;
      case Command::kD3DOMSetStencilRef: {
        UINT stencil_ref = *reinterpret_cast<const UINT*>(args);
        repeat = shadow.stencil_ref_valid && shadow.stencil_ref == stencil_ref;
        shadow.stencil_ref_valid = true;
        shadow.stencil_ref = stencil_ref;
      } break;
      case Command::kD3DResourceBarrier:
        // Counted, never compared: a barrier is not a state.
        deferred_command_counters.barriers += *reinterpret_cast<const UINT*>(args);
        break;
      case Command::kRSSetScissorRect:
        repeat = shadow.scissor_valid && !std::memcmp(&shadow.scissor, args, sizeof(D3D12_RECT));
        shadow.scissor_valid = true;
        std::memcpy(&shadow.scissor, args, sizeof(D3D12_RECT));
        break;
      case Command::kRSSetViewport:
        repeat =
            shadow.viewport_valid && !std::memcmp(&shadow.viewport, args, sizeof(D3D12_VIEWPORT));
        shadow.viewport_valid = true;
        std::memcpy(&shadow.viewport, args, sizeof(D3D12_VIEWPORT));
        break;
      case Command::kD3DSetComputeRoot32BitConstants:
      case Command::kD3DSetGraphicsRoot32BitConstants: {
        auto constants = reinterpret_cast<const SetRoot32BitConstantsHeader*>(args);
        Shadow::Root& root = command == Command::kD3DSetComputeRoot32BitConstants
                                 ? shadow.compute
                                 : shadow.graphics;
        repeat = root.SetConstants(constants->root_parameter_index,
                                   constants->num_32bit_values_to_set,
                                   constants->dest_offset_in_32bit_values,
                                   reinterpret_cast<const uint32_t*>(constants + 1));
      } break;
      case Command::kD3DSetComputeRootConstantBufferView:
        repeat = set_root_value(shadow.compute, Shadow::RootKind::kCBV);
        break;
      case Command::kD3DSetGraphicsRootConstantBufferView:
        repeat = set_root_value(shadow.graphics, Shadow::RootKind::kCBV);
        break;
      case Command::kD3DSetComputeRootShaderResourceView:
        repeat = set_root_value(shadow.compute, Shadow::RootKind::kSRV);
        break;
      case Command::kD3DSetGraphicsRootShaderResourceView:
        repeat = set_root_value(shadow.graphics, Shadow::RootKind::kSRV);
        break;
      case Command::kD3DSetComputeRootUnorderedAccessView:
        repeat = set_root_value(shadow.compute, Shadow::RootKind::kUAV);
        break;
      case Command::kD3DSetGraphicsRootUnorderedAccessView:
        repeat = set_root_value(shadow.graphics, Shadow::RootKind::kUAV);
        break;
      case Command::kD3DSetComputeRootDescriptorTable:
      case Command::kD3DSetGraphicsRootDescriptorTable: {
        auto& table = *reinterpret_cast<const SetRootDescriptorTableArguments*>(args);
        Shadow::Root& root = command == Command::kD3DSetComputeRootDescriptorTable
                                 ? shadow.compute
                                 : shadow.graphics;
        repeat = root.SetValue(table.root_parameter_index, Shadow::RootKind::kTable,
                               table.base_descriptor.ptr);
      } break;
      case Command::kD3DSetComputeRootSignature:
        repeat = shadow.compute.SetSignature(*reinterpret_cast<ID3D12RootSignature* const*>(args));
        break;
      case Command::kD3DSetGraphicsRootSignature:
        repeat =
            shadow.graphics.SetSignature(*reinterpret_cast<ID3D12RootSignature* const*>(args));
        break;
      case Command::kSetDescriptorHeaps: {
        auto& heaps = *reinterpret_cast<const SetDescriptorHeapsArguments*>(args);
        repeat = shadow.heaps_valid && shadow.heaps[0] == heaps.cbv_srv_uav_descriptor_heap &&
                 shadow.heaps[1] == heaps.sampler_descriptor_heap;
        shadow.heaps_valid = true;
        shadow.heaps[0] = heaps.cbv_srv_uav_descriptor_heap;
        shadow.heaps[1] = heaps.sampler_descriptor_heap;
        // Every heap set resets the descriptor tables, a redundant one too.
        shadow.graphics.ResetTables();
        shadow.compute.ResetTables();
      } break;
      case Command::kD3DSetPipelineState: {
        // Only the pipeline Execute would still draw with repeats. A pipeline
        // change does not reset the root arguments.
        ID3D12PipelineState* pipeline = *reinterpret_cast<ID3D12PipelineState* const*>(args);
        repeat = pipeline != nullptr && pipeline == current_pipeline_state;
      } break;
      case Command::kSetPipelineStateHandle: {
        ID3D12PipelineState* pipeline =
            command_processor_.GetD3D12PipelineByHandle(*reinterpret_cast<void* const*>(args));
        repeat = pipeline != nullptr && pipeline == current_pipeline_state;
      } break;
      default:
        // Draws, dispatches, clears, copies, queries, sample positions and
        // debug markers: counted only.
        break;
    }
    size_t index = size_t(command);
    if (index < kDeferredCommandCount) {
      ++deferred_command_counters.total[index];
      if (repeat) {
        ++deferred_command_counters.repeat[index];
      }
    }
  };
#endif  // REX_D3D12_DEFERRED_COUNTERS
  while (stream_remaining != 0) {
    const CommandHeader& header = *reinterpret_cast<const CommandHeader*>(stream);
    stream += kCommandHeaderSizeElements;
    stream_remaining -= kCommandHeaderSizeElements;
#if REX_D3D12_DEFERRED_COUNTERS
    count_command(header.command, stream);
#endif  // REX_D3D12_DEFERRED_COUNTERS
    switch (header.command) {
      case Command::kD3DClearDepthStencilView: {
        auto& args = *reinterpret_cast<const ClearDepthStencilViewHeader*>(stream);
        command_list->ClearDepthStencilView(
            args.depth_stencil_view, args.clear_flags, args.depth, args.stencil, args.num_rects,
            args.num_rects ? reinterpret_cast<const D3D12_RECT*>(&args + 1) : nullptr);
      } break;
      case Command::kD3DClearRenderTargetView: {
        auto& args = *reinterpret_cast<const ClearRenderTargetViewHeader*>(stream);
        command_list->ClearRenderTargetView(
            args.render_target_view, args.color_rgba, args.num_rects,
            args.num_rects ? reinterpret_cast<const D3D12_RECT*>(&args + 1) : nullptr);
      } break;
      case Command::kD3DClearUnorderedAccessViewUint: {
        auto& args = *reinterpret_cast<const ClearUnorderedAccessViewHeader*>(stream);
        command_list->ClearUnorderedAccessViewUint(
            args.view_gpu_handle_in_current_heap, args.view_cpu_handle, args.resource,
            args.values_uint, args.num_rects,
            args.num_rects ? reinterpret_cast<const D3D12_RECT*>(&args + 1) : nullptr);
      } break;
      case Command::kD3DCopyBufferRegion: {
        auto& args = *reinterpret_cast<const D3DCopyBufferRegionArguments*>(stream);
        command_list->CopyBufferRegion(args.dst_buffer, args.dst_offset, args.src_buffer,
                                       args.src_offset, args.num_bytes);
      } break;
      case Command::kD3DCopyResource: {
        auto& args = *reinterpret_cast<const D3DCopyResourceArguments*>(stream);
        command_list->CopyResource(args.dst_resource, args.src_resource);
      } break;
      case Command::kCopyTexture: {
        auto& args = *reinterpret_cast<const CopyTextureArguments*>(stream);
        command_list->CopyTextureRegion(&args.dst, 0, 0, 0, &args.src, nullptr);
      } break;
      case Command::kD3DCopyTextureRegion: {
        auto& args = *reinterpret_cast<const D3DCopyTextureRegionArguments*>(stream);
        command_list->CopyTextureRegion(&args.dst, args.dst_x, args.dst_y, args.dst_z, &args.src,
                                        args.has_src_box ? &args.src_box : nullptr);
      } break;
      case Command::kD3DDispatch: {
        if (current_pipeline_state != nullptr) {
          auto& args = *reinterpret_cast<const D3DDispatchArguments*>(stream);
          command_list->Dispatch(args.thread_group_count_x, args.thread_group_count_y,
                                 args.thread_group_count_z);
        }
      } break;
      case Command::kD3DDrawIndexedInstanced: {
        if (current_pipeline_state != nullptr) {
          auto& args = *reinterpret_cast<const D3DDrawIndexedInstancedArguments*>(stream);
          command_list->DrawIndexedInstanced(args.index_count_per_instance, args.instance_count,
                                             args.start_index_location, args.base_vertex_location,
                                             args.start_instance_location);
        }
      } break;
      case Command::kD3DDrawInstanced: {
        if (current_pipeline_state != nullptr) {
          auto& args = *reinterpret_cast<const D3DDrawInstancedArguments*>(stream);
          command_list->DrawInstanced(args.vertex_count_per_instance, args.instance_count,
                                      args.start_vertex_location, args.start_instance_location);
        }
      } break;
      case Command::kD3DBeginQuery: {
        auto& args = *reinterpret_cast<const D3DQueryArguments*>(stream);
        command_list->BeginQuery(args.query_heap, args.type, args.index);
      } break;
      case Command::kD3DEndQuery: {
        auto& args = *reinterpret_cast<const D3DQueryArguments*>(stream);
        command_list->EndQuery(args.query_heap, args.type, args.index);
      } break;
      case Command::kD3DResolveQueryData: {
        auto& args = *reinterpret_cast<const D3DResolveQueryDataArguments*>(stream);
        command_list->ResolveQueryData(args.query_heap, args.type, args.start_index,
                                       args.num_queries, args.destination_buffer,
                                       args.aligned_destination_buffer_offset);
      } break;
      case Command::kD3DIASetIndexBuffer: {
        auto view = reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(stream);
        command_list->IASetIndexBuffer(view->Format != DXGI_FORMAT_UNKNOWN ? view : nullptr);
      } break;
      case Command::kD3DIASetPrimitiveTopology: {
        command_list->IASetPrimitiveTopology(
            *reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY*>(stream));
      } break;
      case Command::kD3DIASetVertexBuffers: {
        static_assert(alignof(D3D12_VERTEX_BUFFER_VIEW) <= alignof(uintmax_t));
        auto& args = *reinterpret_cast<const D3DIASetVertexBuffersHeader*>(stream);
        command_list->IASetVertexBuffers(args.start_slot, args.num_views,
                                         reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(
                                             reinterpret_cast<const uint8_t*>(stream) +
                                             rex::align(sizeof(D3DIASetVertexBuffersHeader),
                                                        alignof(D3D12_VERTEX_BUFFER_VIEW))));
      } break;
      case Command::kD3DOMSetBlendFactor: {
        command_list->OMSetBlendFactor(reinterpret_cast<const FLOAT*>(stream));
      } break;
      case Command::kD3DOMSetRenderTargets: {
        auto& args = *reinterpret_cast<const D3DOMSetRenderTargetsArguments*>(stream);
        command_list->OMSetRenderTargets(
            args.num_render_target_descriptors, args.render_target_descriptors,
            args.rts_single_handle_to_descriptor_range ? TRUE : FALSE,
            args.depth_stencil ? &args.depth_stencil_descriptor : nullptr);
      } break;
      case Command::kD3DOMSetStencilRef: {
        command_list->OMSetStencilRef(*reinterpret_cast<const UINT*>(stream));
      } break;
      case Command::kD3DResourceBarrier: {
        static_assert(alignof(D3D12_RESOURCE_BARRIER) <= alignof(uintmax_t));
        command_list->ResourceBarrier(
            *reinterpret_cast<const UINT*>(stream),
            reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(
                reinterpret_cast<const uint8_t*>(stream) +
                rex::align(sizeof(UINT), alignof(D3D12_RESOURCE_BARRIER))));
      } break;
      case Command::kRSSetScissorRect: {
        command_list->RSSetScissorRects(1, reinterpret_cast<const D3D12_RECT*>(stream));
      } break;
      case Command::kRSSetViewport: {
        command_list->RSSetViewports(1, reinterpret_cast<const D3D12_VIEWPORT*>(stream));
      } break;
      case Command::kD3DSetComputeRoot32BitConstants: {
        auto args = reinterpret_cast<const SetRoot32BitConstantsHeader*>(stream);
        command_list->SetComputeRoot32BitConstants(args->root_parameter_index,
                                                   args->num_32bit_values_to_set, args + 1,
                                                   args->dest_offset_in_32bit_values);
      } break;
      case Command::kD3DSetGraphicsRoot32BitConstants: {
        auto args = reinterpret_cast<const SetRoot32BitConstantsHeader*>(stream);
        command_list->SetGraphicsRoot32BitConstants(args->root_parameter_index,
                                                    args->num_32bit_values_to_set, args + 1,
                                                    args->dest_offset_in_32bit_values);
      } break;
      case Command::kD3DSetComputeRootConstantBufferView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetComputeRootConstantBufferView(args.root_parameter_index,
                                                       args.buffer_location);
      } break;
      case Command::kD3DSetGraphicsRootConstantBufferView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetGraphicsRootConstantBufferView(args.root_parameter_index,
                                                        args.buffer_location);
      } break;
      case Command::kD3DSetComputeRootDescriptorTable: {
        auto& args = *reinterpret_cast<const SetRootDescriptorTableArguments*>(stream);
        command_list->SetComputeRootDescriptorTable(args.root_parameter_index,
                                                    args.base_descriptor);
      } break;
      case Command::kD3DSetGraphicsRootDescriptorTable: {
        auto& args = *reinterpret_cast<const SetRootDescriptorTableArguments*>(stream);
        command_list->SetGraphicsRootDescriptorTable(args.root_parameter_index,
                                                     args.base_descriptor);
      } break;
      case Command::kD3DSetComputeRootShaderResourceView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetComputeRootShaderResourceView(args.root_parameter_index,
                                                       args.buffer_location);
      } break;
      case Command::kD3DSetGraphicsRootShaderResourceView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetGraphicsRootShaderResourceView(args.root_parameter_index,
                                                        args.buffer_location);
      } break;
      case Command::kD3DSetComputeRootSignature: {
        command_list->SetComputeRootSignature(
            *reinterpret_cast<ID3D12RootSignature* const*>(stream));
      } break;
      case Command::kD3DSetGraphicsRootSignature: {
        command_list->SetGraphicsRootSignature(
            *reinterpret_cast<ID3D12RootSignature* const*>(stream));
      } break;
      case Command::kD3DSetComputeRootUnorderedAccessView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetComputeRootUnorderedAccessView(args.root_parameter_index,
                                                        args.buffer_location);
      } break;
      case Command::kD3DSetGraphicsRootUnorderedAccessView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetGraphicsRootUnorderedAccessView(args.root_parameter_index,
                                                         args.buffer_location);
      } break;
      case Command::kSetDescriptorHeaps: {
        auto& args = *reinterpret_cast<const SetDescriptorHeapsArguments*>(stream);
        UINT num_descriptor_heaps = 0;
        ID3D12DescriptorHeap* descriptor_heaps[2];
        if (args.cbv_srv_uav_descriptor_heap != nullptr) {
          descriptor_heaps[num_descriptor_heaps++] = args.cbv_srv_uav_descriptor_heap;
        }
        if (args.sampler_descriptor_heap != nullptr) {
          descriptor_heaps[num_descriptor_heaps++] = args.sampler_descriptor_heap;
        }
        command_list->SetDescriptorHeaps(num_descriptor_heaps, descriptor_heaps);
      } break;
      case Command::kD3DSetPipelineState: {
        current_pipeline_state = *reinterpret_cast<ID3D12PipelineState* const*>(stream);
        if (current_pipeline_state) {
          command_list->SetPipelineState(current_pipeline_state);
        }
      } break;
      case Command::kSetPipelineStateHandle: {
        current_pipeline_state =
            command_processor_.GetD3D12PipelineByHandle(*reinterpret_cast<void* const*>(stream));
        if (current_pipeline_state) {
          command_list->SetPipelineState(current_pipeline_state);
        }
      } break;
      case Command::kD3DSetSamplePositions: {
        if (command_list_1 != nullptr) {
          auto& args = *reinterpret_cast<const D3DSetSamplePositionsArguments*>(stream);
          command_list_1->SetSamplePositions(
              args.num_samples_per_pixel, args.num_pixels,
              (args.num_samples_per_pixel && args.num_pixels)
                  ? const_cast<D3D12_SAMPLE_POSITION*>(args.sample_positions)
                  : nullptr);
        }
      } break;
      case Command::kBeginDebugMarker: {
        auto& args = *reinterpret_cast<const DebugMarkerHeader*>(stream);
        const char* label_name = reinterpret_cast<const char*>(
            reinterpret_cast<const uint8_t*>(stream) + sizeof(DebugMarkerHeader));
        command_list->BeginEvent(1, label_name, static_cast<UINT>(args.label_length + 1));
      } break;
      case Command::kEndDebugMarker: {
        command_list->EndEvent();
      } break;
      case Command::kInsertDebugMarker: {
        auto& args = *reinterpret_cast<const DebugMarkerHeader*>(stream);
        const char* label_name = reinterpret_cast<const char*>(
            reinterpret_cast<const uint8_t*>(stream) + sizeof(DebugMarkerHeader));
        command_list->SetMarker(1, label_name, static_cast<UINT>(args.label_length + 1));
      } break;
      default:
        assert_unhandled_case(header.command);
        break;
    }
    stream += header.arguments_size_elements;
    stream_remaining -= header.arguments_size_elements;
  }
#if REX_D3D12_DEFERRED_COUNTERS
  // About every 10 s, log the counts since the last line and start over.
  DeferredCommandCounters& counters = deferred_command_counters;
  ++counters.lists;
  counters.bytes += command_stream_.size() * sizeof(uintmax_t);
  auto now = std::chrono::steady_clock::now();
  if (counters.last_log == std::chrono::steady_clock::time_point()) {
    counters.last_log = now;
  } else if (now - counters.last_log >= std::chrono::seconds(10)) {
    REXGPU_INFO("deferred_counters: {:.1f} s, {} lists, {} KB, {} barriers",
                std::chrono::duration<double>(now - counters.last_log).count(), counters.lists,
                counters.bytes >> 10, counters.barriers);
    for (size_t i = 0; i < kDeferredCommandCount; ++i) {
      if (counters.total[i]) {
        REXGPU_INFO("deferred_counters:   {:<33} total={:<8} repeat={:<8} ({:.1f}%)",
                    kDeferredCommandNames[i], counters.total[i], counters.repeat[i],
                    100.0 * double(counters.repeat[i]) / double(counters.total[i]));
      }
    }
    counters = DeferredCommandCounters();
    counters.last_log = now;
  }
#endif  // REX_D3D12_DEFERRED_COUNTERS
}

void* DeferredCommandList::WriteCommand(Command command, size_t arguments_size_bytes) {
  size_t arguments_size_elements =
      (arguments_size_bytes + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);
  size_t offset = command_stream_.size();
  command_stream_.resize(offset + kCommandHeaderSizeElements + arguments_size_elements);
  CommandHeader& header = *reinterpret_cast<CommandHeader*>(command_stream_.data() + offset);
  header.command = command;
  header.arguments_size_elements = uint32_t(arguments_size_elements);
  return command_stream_.data() + (offset + kCommandHeaderSizeElements);
}

}  // namespace rex::graphics::d3d12
