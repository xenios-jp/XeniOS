/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/gpu_flags.h"

#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <pthread/qos.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "third_party/metal-cpp/Foundation/NSProcessInfo.hpp"
#include "third_party/metal-cpp/Foundation/NSURL.hpp"
#include "third_party/metal-cpp/Metal/MTLEvent.hpp"

#include "spirv2air.h"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/metal/metal_compile_activity.h"
#include "xenia/gpu/metal/metal_graphics_system.h"
#include "xenia/gpu/metal/metal_pipeline_cache.h"
#include "xenia/gpu/metal/metal_util.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shaders/bytecode/metal/metal_tessellation_prep_cs.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/ui/metal/metal_presenter.h"

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

DECLARE_bool(precise_interpolation);
DECLARE_bool(clear_memory_page_state);
DECLARE_bool(submit_on_primary_buffer_end);

DEFINE_bool(
    skip_no_effect_draws, true,
    "Don't issue a draw that writes no color, depth or stencil, exports "
    "nothing to memory and is counted by no occlusion query.",
    "GPU");
DEFINE_bool(metal_stand_in_exact_depth_stencil, true,
            "A draw that writes depth or stencil and whose pixel shader "
            "decides where (kill, alpha test, alpha to coverage, depth "
            "output) waits for its pipeline instead of drawing through the "
            "vertex-only placeholder.",
            "Metal");
DEFINE_bool(
    metal_log_frame_gpu_waits, false,
    "Log, at every swap, the time the CPU spent waiting for the GPU in the "
    "frame by reason, and the command buffers it created by kind.",
    "Metal");

namespace xe {
namespace gpu {
namespace metal {

namespace {

// A draw that only exports memory from the vertex shader: non-rasterizing,
// auto-indexed (a host builtin index buffer may be used for the expansion, but
// no guest indices are read from the shared buffer being exported), with no
// pixel shader, and not tessellated - tessellation lowers the guest vertex
// shader to object/mesh work, which a barrier naming only
// MTL::RenderStageVertex does not order. Matches PrimitiveProcessor::Process
// before it can read or convert indices.
bool IsPureMemexportDraw(const RegisterFile& regs, bool rasterization_done,
                         bool vertex_memexport, bool has_export_range,
                         bool has_pixel_shader, bool has_guest_index_buffer) {
  const auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  return !rasterization_done && vertex_memexport && has_export_range &&
         !has_pixel_shader && !has_guest_index_buffer &&
         initiator.source_select == xenos::SourceSelect::kAutoIndex &&
         !(xenos::IsMajorModeExplicit(initiator.major_mode,
                                      initiator.prim_type) &&
           regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
               xenos::VGTOutputPath::kTessellationEnable);
}

// Merges a write into one it overlaps or touches, so long chains of narrowed
// exports keep the overlap checks against them short.
void AddPendingWrite(std::vector<draw_util::MemExportRange>& writes,
                     const draw_util::MemExportRange& write) {
  uint64_t begin = uint64_t(write.base_address_dwords) << 2;
  uint64_t end = begin + write.size_bytes;
  for (draw_util::MemExportRange& other : writes) {
    uint64_t other_begin = uint64_t(other.base_address_dwords) << 2;
    uint64_t other_end = other_begin + other.size_bytes;
    if (begin <= other_end && other_begin <= end) {
      begin = std::min(begin, other_begin);
      other.base_address_dwords = uint32_t(begin >> 2);
      other.size_bytes = uint32_t(std::max(end, other_end) - begin);
      return;
    }
  }
  writes.push_back(write);
}

// Direct vertex buffer slot where spirv2air reads tessellation control point
// floats (controlPointBuffer=6, element p * N + k).
constexpr NS::UInteger kSpirv2airControlPointBufferIndex = 6;

// IEEE 754 binary32 -> binary16 (round to nearest even), preserving NaN/Inf
// and <= 0 values so Metal culls patches consistently with D3D12/Vulkan.
uint16_t Float32ToHalf(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  uint32_t sign = (bits >> 16) & 0x8000u;
  uint32_t exp = (bits >> 23) & 0xFFu;
  uint32_t mant = bits & 0x7FFFFFu;
  if (exp == 0xFFu) {
    return uint16_t(sign | 0x7C00u | (mant ? 0x0200u : 0u));
  }
  int half_exp = int(exp) - 127 + 15;
  if (half_exp >= 31) {
    return uint16_t(sign | 0x7C00u);
  }
  if (half_exp <= 0) {
    if (half_exp < -10) {
      return uint16_t(sign);
    }
    mant |= 0x800000u;
    uint32_t shift = uint32_t(14 - half_exp);
    uint32_t half_mant = mant >> shift;
    uint32_t round_bit = 1u << (shift - 1u);
    uint32_t remainder = mant & ((1u << shift) - 1u);
    if (remainder > round_bit || (remainder == round_bit && (half_mant & 1u))) {
      ++half_mant;
    }
    return uint16_t(sign | half_mant);
  }
  uint32_t half_val = (uint32_t(half_exp) << 10) | (mant >> 13);
  uint32_t remainder = mant & 0x1FFFu;
  if (remainder > 0x1000u || (remainder == 0x1000u && (half_val & 1u))) {
    ++half_val;
  }
  return uint16_t(sign | half_val);
}

void GetRenderPassSize(MTL::RenderPassDescriptor* pass_descriptor,
                       uint32_t fallback_width, uint32_t fallback_height,
                       uint32_t& width_out, uint32_t& height_out) {
  uint32_t width = uint32_t(pass_descriptor->renderTargetWidth());
  uint32_t height = uint32_t(pass_descriptor->renderTargetHeight());
  // The encoded pass is constrained by every attached mip level, as well as
  // any explicit render target dimensions. The cache may bind different
  // targets by the time a later draw reuses this encoder.
  auto constrain = [&](MTL::RenderPassAttachmentDescriptor* attachment) {
    if (MTL::Texture* texture = attachment->texture()) {
      uint32_t attachment_width =
          std::max(uint32_t(texture->width() >> attachment->level()), 1u);
      uint32_t attachment_height =
          std::max(uint32_t(texture->height() >> attachment->level()), 1u);
      width = width ? std::min(width, attachment_width) : attachment_width;
      height = height ? std::min(height, attachment_height) : attachment_height;
    }
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    constrain(pass_descriptor->colorAttachments()->object(i));
  }
  constrain(pass_descriptor->depthAttachment());
  constrain(pass_descriptor->stencilAttachment());
  width_out = std::max(width ? width : fallback_width, 1u);
  height_out = std::max(height ? height : fallback_height, 1u);
}

void ClampScissorToBounds(draw_util::Scissor& scissor, uint32_t width,
                          uint32_t height) {
  width = std::max(width, uint32_t(1));
  height = std::max(height, uint32_t(1));

  scissor.offset[0] = std::min(scissor.offset[0], width);
  scissor.offset[1] = std::min(scissor.offset[1], height);

  uint32_t max_scissor_width = width - scissor.offset[0];
  uint32_t max_scissor_height = height - scissor.offset[1];
  scissor.extent[0] = std::min(scissor.extent[0], max_scissor_width);
  scissor.extent[1] = std::min(scissor.extent[1], max_scissor_height);
}

// name what ended the previous submission.
const char* const kCommandBufferKindNames[] = {
    "submission_other",
    "submission_copy_draw_sync",
    "submission_zpd_query",
    "submission_uniforms_rollover",
    "submission_primary_end",
    "submission_wait",
    "texture_upload_batch",
    "texture_upload_private",
    "texture_other",
    "rt_resolve",
    "rt_dump",
    "rt_other",
    "submission_pass_limit",
    "submission_after_drain",
    "tess_prep",
};
static_assert(std::size(kCommandBufferKindNames) ==
                  size_t(MetalCommandProcessor::CommandBufferKind::kCount),
              "Command buffer kind names must match the enum");

// Indexed by GpuWaitReason.
const char* const kGpuWaitReasonNames[] = {
    "memexport",
    "zpd_query",
    "shader_done_fence",
    "trace_playback",
    "uniforms",
    "resolve",
    "texture_upload",
    "scaled_resolve_copy",
    "snapshot",
    "dump",
    "resolve_before_guest_read",
};
static_assert(std::size(kGpuWaitReasonNames) ==
                  size_t(MetalCommandProcessor::GpuWaitReason::kCount),
              "GPU wait reason names must match the enum");

MTL::CompareFunction ToMetalCompareFunction(xenos::CompareFunction compare) {
  static const MTL::CompareFunction kCompareMap[8] = {
      MTL::CompareFunctionNever,         // 0
      MTL::CompareFunctionLess,          // 1
      MTL::CompareFunctionEqual,         // 2
      MTL::CompareFunctionLessEqual,     // 3
      MTL::CompareFunctionGreater,       // 4
      MTL::CompareFunctionNotEqual,      // 5
      MTL::CompareFunctionGreaterEqual,  // 6
      MTL::CompareFunctionAlways,        // 7
  };
  return kCompareMap[uint32_t(compare) & 0x7];
}

MTL::StencilOperation ToMetalStencilOperation(xenos::StencilOp op) {
  static const MTL::StencilOperation kStencilOpMap[8] = {
      MTL::StencilOperationKeep,            // 0
      MTL::StencilOperationZero,            // 1
      MTL::StencilOperationReplace,         // 2
      MTL::StencilOperationIncrementClamp,  // 3
      MTL::StencilOperationDecrementClamp,  // 4
      MTL::StencilOperationInvert,          // 5
      MTL::StencilOperationIncrementWrap,   // 6
      MTL::StencilOperationDecrementWrap,   // 7
  };
  return kStencilOpMap[uint32_t(op) & 0x7];
}

}  // namespace

MetalCommandProcessor::MetalCommandProcessor(
    MetalGraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      pipeline_cache_(std::make_unique<MetalPipelineCache>(*this)),
      argument_binder_(*this) {}

bool MetalCommandProcessor::primitive_fragment_helpers_enabled() const {
  return pipeline_cache_->spirv_translator_features()
      .primitive_fragment_helpers;
}

bool MetalCommandProcessor::SupportsHostRenderTargetFeedback() const {
  return pipeline_cache_->SupportsHostRenderTargetFeedback();
}

void MetalCommandProcessor::SetCompileThreadBoost(bool boost) {
  pipeline_cache_->SetCompileThreadBoost(boost);
}

void MetalCommandProcessor::SetCompileThreadBoostAllowed(bool allowed) {
  pipeline_cache_->SetCompileThreadBoostAllowed(allowed);
}

void MetalCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking,
                                            nullptr);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking,
                                           std::move(completion_callback));
}

Shader* MetalCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                          const uint32_t* host_address,
                                          uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

Shader* MetalCommandProcessor::LoadShaderAtAddress(
    xenos::ShaderType shader_type, uint32_t guest_address,
    uint32_t dword_count) {
  return pipeline_cache_->LoadShaderAtAddress(shader_type, guest_address,
                                              dword_count);
}

std::string MetalCommandProcessor::MakeTitleStateSuffix() const {
  if (!render_target_cache_) {
    return {};
  }
  std::ostringstream suffix;
  suffix << " - spirv2air";
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    suffix << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
  }
  return suffix.str();
}

MetalCommandProcessor::~MetalCommandProcessor() {
  render_encoder_shared_memory_hazards_.Abandon();
  // End any active render encoder before releasing
  // Note: Only call endEncoding if the encoder is still active
  // (not already ended by a committed command buffer)
  if (current_render_encoder_) {
    // The encoder may already be ended if the command buffer was committed
    // In that case, just release it
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
  }
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  WaitForPendingCompletionHandlers();
  if (pipeline_cache_) {
    pipeline_cache_->ShutdownAsyncCompilation();
  }
  if (render_pass_descriptor_) {
    render_pass_descriptor_->release();
    render_pass_descriptor_ = nullptr;
  }
  if (render_target_texture_) {
    render_target_texture_->release();
    render_target_texture_ = nullptr;
  }
  if (depth_stencil_texture_) {
    depth_stencil_texture_->release();
    depth_stencil_texture_ = nullptr;
  }

  for (auto& pair : depth_stencil_state_cache_) {
    if (pair.second) {
      pair.second->release();
    }
  }
  depth_stencil_state_cache_.clear();

  // Release IR Converter runtime buffers and resources
  if (null_buffer_) {
    null_buffer_->release();
    null_buffer_ = nullptr;
  }
  if (null_texture_) {
    null_texture_->release();
    null_texture_ = nullptr;
  }
  if (null_sampler_) {
    null_sampler_->release();
    null_sampler_ = nullptr;
  }
}

MetalCommandProcessor::SpirvArgumentBufferPage::~SpirvArgumentBufferPage() {
  if (buffer) {
    buffer->release();
    buffer = nullptr;
  }
}

void MetalCommandProcessor::TracePlaybackWillWriteMemory(uint32_t base_ptr,
                                                         uint32_t length) {
  if (!shared_memory_ || !shared_memory_->is_zero_copy() || !length) {
    return;
  }
  // A resolve left to the end of the open pass writes there only when the
  // submission ends, so it has to land before the restore too.
  const bool post_pass_write =
      render_target_cache_ &&
      render_target_cache_->PostPassResolvesWrite(base_ptr, length);
  auto* hazards = shared_memory_->hazards();
  // The open encoders publish their accesses to the tracker only when they
  // end: the render pass, and the compute encoder resolves and texture loads
  // share.
  if (post_pass_write || !hazards ||
      render_encoder_shared_memory_hazards_.HasAccess(base_ptr, length) ||
      shared_compute_hazards_.HasAccess(base_ptr, length) ||
      hazards->HasPendingAccess(base_ptr, length)) {
    // Submission alone does not keep the GPU from seeing a later CPU restore.
    // Consume this recorded version before overwriting aliased guest RAM.
    int64_t wait_start_ns = GetSteadyTimeNs();
    AwaitAllQueueOperationsCompletion();
    // Texture loads that could not use the main command buffer are committed
    // in command buffers of their own, which no submission number names, and
    // with zero-copy they read guest memory when the GPU runs them. The
    // hazard tracker holds every command buffer that accesses the shared
    // memory buffer. Only here: nothing during play waits for them.
    if (shared_memory_ && shared_memory_->hazards()) {
      await_all_side_command_buffers_.clear();
      shared_memory_->hazards()->GetCommittedUnfinished(
          nullptr, await_all_side_command_buffers_);
      for (MTL::CommandBuffer* command_buffer :
           await_all_side_command_buffers_) {
        command_buffer->waitUntilCompleted();
      }
    }
    NoteGpuWait(GpuWaitReason::kTracePlayback, wait_start_ns);
  }
}

void MetalCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                     uint32_t length) {
  if (shared_memory_) {
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
  if (primitive_processor_) {
    primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
  }
}

void MetalCommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();

  auto abandon_trace = [this](const char* reason) {
    XELOGE("Metal: abandoning frame trace: {}", reason);
    trace_writer_.Close();
    trace_state_ = TraceState::kDisabled;
    trace_frame_file_path_.clear();
  };

  // The resolve work left to the end of an open pass changes ownership too,
  // and comes first.
  if (RenderEncoderResolveOnly()) {
    EndRenderEncoder();
  }
  // Ownership may already name destinations whose transfers were deferred by
  // an abandoned draw. Materialize them before the standalone snapshot reads.
  if (render_target_cache_ &&
      !render_target_cache_->FlushPendingDrawPassTransfers()) {
    abandon_trace("the queued ownership transfers could not be completed");
    return;
  }

  // Neither download is bracketed by a submission of its own, so everything in
  // flight has to have landed before they read what the GPU wrote.
  EndCommandBuffer();
  if (submission_current_) {
    AwaitSubmissionCompletion(submission_current_);
  }

  if (render_target_cache_) {
    // DumpRenderTargets uses submission-owned argument pages even when its
    // compute commands are submitted separately. EndCommandBuffer above removed
    // that owner. Keep a fresh submission alive while the standalone dump and
    // readback run synchronously; the next guest draw can reuse it afterwards.
    if (!EnsureCommandBuffer() ||
        !render_target_cache_->InitializeTraceSubmitDownloads()) {
      abandon_trace("the initial EDRAM snapshot could not be captured");
      return;
    }
    render_target_cache_->InitializeTraceCompleteDownloads();
  }
  if (shared_memory_ && shared_memory_->InitializeTraceSubmitDownloads()) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

bool MetalCommandProcessor::DumpEdramSnapshotToFile(
    const std::filesystem::path& path) {
  if (!render_target_cache_) {
    return false;
  }
  // The resolve work left to the end of an open pass changes ownership too,
  // and comes first.
  if (RenderEncoderResolveOnly()) {
    EndRenderEncoder();
  }
  // Ownership already names the destinations of transfers queued for the next
  // draw's pass, which a replay stopped mid-frame never encodes.
  if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
    return false;
  }
  // As in InitializeTrace, the download reads what everything in flight wrote.
  EndCommandBuffer();
  if (submission_current_) {
    AwaitSubmissionCompletion(submission_current_);
  }
  // DumpRenderTargets takes its descriptor heap from the open command
  // buffer's argument buffer pages: without one, nothing is dumped and the
  // snapshot is whatever the EDRAM buffer last held.
  if (!EnsureCommandBuffer()) {
    return false;
  }
  if (!render_target_cache_->InitializeTraceSubmitDownloads()) {
    return false;
  }
  return render_target_cache_->WriteEdramSnapshotToFile(path);
}

void MetalCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Restore the guest EDRAM snapshot captured in the trace into the Metal
  // render-target cache so that subsequent host render targets created from
  // EDRAM (via LoadTiledData) see the same initial contents as other
  // backends like D3D12.
  if (!snapshot) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called with null "
        "snapshot");
    return;
  }
  if (!render_target_cache_) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called before render "
        "target "
        "cache initialization");
    return;
  }
  // Restoring the snapshot also dumps its bootstrap render target. The dump's
  // descriptor allocations require a live submission, just like capture.
  if (!EnsureCommandBuffer()) {
    XELOGE("Metal: unable to begin submission for EDRAM snapshot restore");
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

void MetalCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  // TODO(wmarti): Add cache_clear_requested_ flag like D3D12 for deferred
  // clearing of pipeline caches, texture caches, etc.
}

void MetalCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void MetalCommandProcessor::ClearReadbackBuffers() {
  // TODO(wmarti): Implement readback buffer clearing when resolve readback
  // is added. See D3D12's readback_buffers_.
}

ui::metal::MetalProvider& MetalCommandProcessor::GetMetalProvider() const {
  return *static_cast<ui::metal::MetalProvider*>(graphics_system_->provider());
}

uint64_t MetalCommandProcessor::GetCurrentSubmission() const {
  return submission_current_ ? submission_current_ : 1;
}

uint64_t MetalCommandProcessor::GetCompletedSubmission() const {
  return completed_command_buffers_.load(std::memory_order_acquire);
}

bool MetalCommandProcessor::TryWriteShaderDoneFence(uint32_t address,
                                                    uint32_t value) {
  if (!shared_memory_ || !shared_memory_->is_zero_copy()) {
    return false;
  }
  if (!trace_writer_.is_open()) {
    // The ordered copy would end an open render pass. Nothing in the pass sees
    // the fence before the pass ends except a later draw of it accessing the
    // address, which ends the pass first (IssueDraw), so the copy is encoded
    // when the pass ends, still after the work the fence follows.
    bool deferred = current_render_encoder_ &&
                    address <= SharedMemory::kBufferSize - sizeof(value) &&
                    shared_memory_->RequestRange(address, sizeof(value));
    // With no pass open, the fence is copied before whatever is encoded next,
    // where its own blit encoder would have been, together with any fences
    // that follow it before that. The guest's fences come in runs with nothing
    // encoded between them, as Halo 3 writes them before a frame's first draw.
    if (!deferred && !current_render_encoder_ && (address & 3) == 0 &&
        address <= SharedMemory::kBufferSize - sizeof(value) &&
        EnsureCommandBuffer()) {
      if (!shader_done_fences_outside_pass_) {
        // Ordered after everything encoded so far, as CopyToBufferGpuOrdered
        // orders it: ends the shared compute encoder, and encodes the resolves
        // a pass left to its end. Fences already queued mean nothing was
        // encoded since the first of them.
        EndRenderEncoder();
      }
      if (!current_render_encoder_ &&
          shared_memory_->RequestRange(address, sizeof(value))) {
        deferred = true;
        shader_done_fences_outside_pass_ = true;
      }
    }
    if (deferred) {
      pending_shader_done_fence_ranges_.emplace_back(address >> 2,
                                                     uint32_t(sizeof(value)));
      pending_shader_done_fence_values_.push_back(value);
      shared_memory_->RangeWrittenByGpu(address, sizeof(value));
    }
    if (deferred || shared_memory_->WriteGuestMemoryGpuOrdered(address, &value,
                                                               sizeof(value))) {
      // The guest may stop submitting commands while polling this fence. The
      // pending resolve write makes OnPrimaryBufferEnd commit this submission,
      // and a ring-empty stall commits it in PrepareForWait, so the fence is
      // published without a submission of its own.
      copy_resolve_writes_pending_ = true;
      shader_done_fence_last_submission_ = submission_current_;
      return true;
    }
  }
  // Trace records read guest RAM immediately. Allocation failure also needs a
  // safe CPU fallback: never report completion while the GPU still uses RAM.
  int64_t wait_start_ns = GetSteadyTimeNs();
  AwaitAllQueueOperationsCompletion();
  NoteGpuWait(GpuWaitReason::kShaderDoneFence, wait_start_ns);
  return false;
}

void MetalCommandProcessor::EncodeDeferredShaderDoneFences() {
  // Cleared first: EndSharedComputeEncoder below would encode them again.
  shader_done_fences_outside_pass_ = false;
  if (pending_shader_done_fence_values_.empty()) {
    return;
  }
  uint32_t size = uint32_t(pending_shader_done_fence_values_.size() *
                           sizeof(pending_shader_done_fence_values_[0]));
  MTL::Buffer* source = nullptr;
  NS::UInteger source_offset = 0;
  MTL::BlitCommandEncoder* encoder = nullptr;
  if (AcquireSpirvArgumentBufferSlice(size, 16, &source, &source_offset)) {
    EndSharedComputeEncoder();
    encoder = current_command_buffer_->blitCommandEncoder();
  }
  if (encoder) {
    std::memcpy(static_cast<uint8_t*>(source->contents()) + source_offset,
                pending_shader_done_fence_values_.data(), size);
    MetalBufferHazards::Encoder hazards;
    if (shared_memory_->hazards()) {
      hazards.Begin(*shared_memory_->hazards(), current_command_buffer_,
                    encoder);
      // Keep shader-done publication ordered after all preceding buffer uses.
      hazards.OrderPreviousAccesses();
    }
    for (size_t i = 0; i < pending_shader_done_fence_ranges_.size(); ++i) {
      const uint32_t address =
          pending_shader_done_fence_ranges_[i].base_address_dwords << 2;
      if (hazards.is_active()) {
        hazards.Access(address, sizeof(uint32_t), true);
      }
      encoder->copyFromBuffer(source, source_offset + i * sizeof(uint32_t),
                              shared_memory_->GetBuffer(), address,
                              sizeof(uint32_t));
    }
    hazards.End();
    encoder->endEncoding();
  } else {
    // EndRenderEncoder has already cleared the active encoder. Submit and
    // finish its work before publishing completion on the CPU. The nested
    // submission end cannot re-enter fence encoding without an active encoder.
    XELOGE("Metal: couldn't encode shader-done fence writes, waiting for GPU");
    assert_null(current_render_encoder_);
    int64_t wait_start_ns = GetSteadyTimeNs();
    AwaitAllQueueOperationsCompletion();
    NoteGpuWait(GpuWaitReason::kShaderDoneFence, wait_start_ns);
    for (size_t i = 0; i < pending_shader_done_fence_ranges_.size(); ++i) {
      std::memcpy(
          memory_->TranslatePhysical(
              pending_shader_done_fence_ranges_[i].base_address_dwords << 2),
          &pending_shader_done_fence_values_[i], sizeof(uint32_t));
    }
  }
  pending_shader_done_fence_ranges_.clear();
  pending_shader_done_fence_values_.clear();
}

void MetalCommandProcessor::NoteMemexportRangesWritten() {
  if (!shared_memory_ || memexport_ranges_.empty()) {
    return;
  }
  // Awaited output can go under the resolve read watch instead of being
  // waited for at coherency requests and signals (FlushMemexportWatchRanges).
  bool watch = memexport_ranges_awaited_ && !memexport_watch_holdoff_frames_ &&
               ResolveReadWatchActive();
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t base_bytes = memexport_range.base_address_dwords << 2;
    shared_memory_->RangeWrittenByGpu(base_bytes, memexport_range.size_bytes);
    if (watch && memexport_range.size_bytes &&
        base_bytes < SharedMemory::kBufferSize) {
      memexport_watch_pending_.emplace_back(
          base_bytes, std::min(memexport_range.size_bytes,
                               SharedMemory::kBufferSize - base_bytes));
      NoteWatchedGpuWrite(base_bytes, memexport_range.size_bytes);
    } else {
      MarkMemexportPagesWritten(base_bytes, memexport_range.size_bytes,
                                memexport_ranges_awaited_);
    }
  }
  if (watch) {
    // A guest sync point submits this command buffer first
    // (SubmitResolvesForGuestSync), which arms the watch before the commit.
    resolve_unsubmitted_owner_ = true;
  }
  copy_resolve_writes_pending_ = true;
}

void MetalCommandProcessor::FlushMemexportWatchRanges() {
  if (memexport_watch_pending_.empty()) {
    return;
  }
  // One owner and one arm per run of ranges sharing pages, not per export
  // draw. Still in the command buffer that holds the draws, so the owners
  // name its submission.
  std::sort(memexport_watch_pending_.begin(), memexport_watch_pending_.end());
  bool active = ResolveReadWatchActive();
  size_t i = 0;
  while (i < memexport_watch_pending_.size()) {
    uint32_t address = memexport_watch_pending_[i].first;
    uint64_t end = uint64_t(address) + memexport_watch_pending_[i].second;
    for (++i;
         i < memexport_watch_pending_.size() &&
         memexport_watch_pending_[i].first <= ((end + 4095) & ~uint64_t(4095));
         ++i) {
      end = std::max(end, uint64_t(memexport_watch_pending_[i].first) +
                              memexport_watch_pending_[i].second);
    }
    uint32_t length = uint32_t(end - address);
    if (active && IsResolveDestinationResident(address, length)) {
      // Not a destination to submit ahead for (SubmitRequestedResolve).
      bool submit_requested = resolve_submit_requested_;
      NoteResolveForReadback(address, length, false);
      resolve_submit_requested_ = submit_requested;
      uint32_t first_page = address >> 12;
      uint32_t last_page = uint32_t((end - 1) >> 12);
      for (uint32_t page = first_page; page <= last_page; ++page) {
        memexport_watch_page_bits_[page >> 6].fetch_or(
            uint64_t(1) << (page & 63), std::memory_order_relaxed);
      }
      memexport_watch_any_pages_ = true;
    } else {
      // Not watchable after all: awaited as without the watch.
      MarkMemexportPagesWritten(address, length, true);
    }
  }
  memexport_watch_pending_.clear();
}

void MetalCommandProcessor::EndMemexportWatchFrame() {
  uint32_t faults =
      memexport_watch_frame_faults_.exchange(0, std::memory_order_relaxed);
  uint32_t waits =
      memexport_watch_frame_waits_.exchange(0, std::memory_order_relaxed);
  if (memexport_watch_holdoff_frames_) {
    --memexport_watch_holdoff_frames_;
  } else if (faults > kMemexportWatchMaxFrameFaults ||
             waits > kMemexportWatchMaxFrameWaits) {
    // The CPU works in the export pages every frame: each access faults and
    // may wait for the GPU, which costs guest threads more than the await it
    // replaces costs this one. Awaited as before for a while.
    memexport_watch_holdoff_frames_ = kMemexportWatchHoldoffFrames;
    XELOGW(
        "Metal: export output read watch took {} faults and {} waits in a "
        "frame; export output is awaited again for {} frames",
        faults, waits, kMemexportWatchHoldoffFrames);
  }
  // Merged export ranges need not recur, so their read marks may never be
  // taken; forgetting them only forgets which destinations were read.
  std::lock_guard<std::mutex> lock(resolve_read_mutex_);
  if (resolve_read_keys_.size() > 8192) {
    resolve_read_keys_.clear();
  }
}

bool MetalCommandProcessor::CanKeepRenderPassAfterTileResolve() const {
  // Past this many guest indices in one pass, the tiler's parameter buffer may
  // overflow and split the pass into partial renders anyway (assumed from the
  // measured knee of 1-1.5 M small triangles).
  constexpr uint64_t kTileResolvePassIndexBudget = 1000000;
  return current_render_encoder_ &&
         render_encoder_index_count_ <= kTileResolvePassIndexBudget;
}

void MetalCommandProcessor::NoteTileResolveWrite(uint32_t start,
                                                 uint32_t length) {
  if (!length) {
    return;
  }
  AddPendingWrite(render_encoder_tile_resolve_ranges_,
                  draw_util::MemExportRange(start >> 2, (start & 3) + length));
}

bool MetalCommandProcessor::TileResolveWritesOverlap(uint32_t start,
                                                     uint32_t length) const {
  for (const auto& written : render_encoder_tile_resolve_ranges_) {
    uint64_t written_base = uint64_t(written.base_address_dwords) << 2;
    if (uint64_t(start) < written_base + written.size_bytes &&
        written_base < uint64_t(start) + length) {
      return true;
    }
  }
  return false;
}

bool MetalCommandProcessor::DrawOverlapsPendingWrites(
    const std::vector<draw_util::MemExportRange>& pending_writes,
    const Shader& vertex_shader, const Shader* pixel_shader,
    const IndexBufferInfo* index_buffer_info,
    const draw_util::VertexIndexRange* vertex_indices) const {
  if (pending_writes.empty()) {
    return false;
  }
  auto overlaps = [&](uint32_t base, uint64_t length) {
    if (!length) {
      return false;
    }
    for (const auto& written : pending_writes) {
      uint64_t written_base = uint64_t(written.base_address_dwords) << 2;
      if (uint64_t(base) < written_base + written.size_bytes &&
          written_base < uint64_t(base) + length) {
        return true;
      }
    }
    return false;
  };
  // Both shader stages can perform guest vertex fetches from shared memory.
  auto fetches_overlap = [&](const Shader& shader) {
    if (vertex_indices && shader.vertex_fetches_vertex_indexed()) {
      for (const Shader::VertexFetchStride& stride :
           shader.vertex_fetch_strides()) {
        const auto fetch =
            register_file_->GetVertexFetch(stride.fetch_constant);
        int64_t first =
            std::max(int64_t(vertex_indices->first) * stride.stride_words +
                         shader.vertex_fetch_min_word_offset(),
                     int64_t(0));
        int64_t end =
            std::min(int64_t(vertex_indices->last) * stride.stride_words +
                         shader.vertex_fetch_end_word_offset(),
                     int64_t(fetch.size));
        if (first < end &&
            overlaps(xenos::CpuToGpu(fetch.address << 2) + uint32_t(first << 2),
                     uint64_t(end - first) << 2)) {
          return true;
        }
      }
      return false;
    }
    const auto& bitmap = shader.constant_register_map().vertex_fetch_bitmap;
    for (uint32_t i = 0; i < xe::countof(bitmap); ++i) {
      uint32_t bits = bitmap[i], bit;
      while (xe::bit_scan_forward(bits, &bit)) {
        bits &= ~(uint32_t(1) << bit);
        const auto fetch = register_file_->GetVertexFetch(i * 32 + bit);
        if (overlaps(xenos::CpuToGpu(fetch.address << 2), uint64_t(fetch.size)
                                                              << 2)) {
          return true;
        }
      }
    }
    return false;
  };
  if (fetches_overlap(vertex_shader) ||
      (pixel_shader && fetches_overlap(*pixel_shader))) {
    return true;
  }
  if (index_buffer_info &&
      overlaps(xenos::CpuToGpu(index_buffer_info->guest_base),
               index_buffer_info->length)) {
    return true;
  }
  // Overlapping exports also require ordering, including partial-word RMW.
  for (const auto& written : memexport_ordering_ranges_) {
    if (overlaps(written.base_address_dwords << 2, written.size_bytes)) {
      return true;
    }
  }
  return false;
}

bool MetalCommandProcessor::DrawExportsOverlapPassReads() const {
  for (const draw_util::MemExportRange& written : memexport_ordering_ranges_) {
    uint32_t start = written.base_address_dwords << 2;
    if (render_encoder_shared_memory_hazards_.HasReadAccess(
            start, written.size_bytes)) {
      return true;
    }
    uint64_t end = uint64_t(start) + written.size_bytes;
    for (const std::pair<uint32_t, uint32_t>& read :
         render_encoder_narrowed_reads_) {
      if (read.first < end && start < read.second) {
        return true;
      }
    }
  }
  return false;
}

void MetalCommandProcessor::ForceIssueSwap() {
  // Force a swap to push any pending render target to presenter
  // This is used by trace dumps to capture output when there's no explicit swap
  if (saw_swap_) {
    return;
  }
  IssueSwap(0, render_target_width_, render_target_height_);
}

void MetalCommandProcessor::SetSwapDestSwap(uint32_t dest_base, bool swap) {
  if (!dest_base) {
    return;
  }
  if (swap_dest_swaps_by_base_.size() > 256) {
    swap_dest_swaps_by_base_.clear();
  }
  swap_dest_swaps_by_base_[dest_base] = swap;
}

bool MetalCommandProcessor::ConsumeSwapDestSwap(uint32_t dest_base,
                                                bool* swap_out) {
  if (!swap_out || !dest_base) {
    return false;
  }
  auto it = swap_dest_swaps_by_base_.find(dest_base);
  if (it == swap_dest_swaps_by_base_.end()) {
    return false;
  }
  *swap_out = it->second;
  swap_dest_swaps_by_base_.erase(it);
  return true;
}

bool MetalCommandProcessor::SetupContext() {
  saw_swap_ = false;
  last_swap_ptr_ = 0;
  last_swap_width_ = 0;
  last_swap_height_ = 0;
  swap_dest_swaps_by_base_.clear();
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  const ui::metal::MetalProvider& provider = GetMetalProvider();
  device_ = provider.GetDevice();
  command_queue_ = provider.GetCommandQueue();

  if (!device_ || !command_queue_) {
    XELOGE("MetalCommandProcessor: No Metal device or command queue available");
    return false;
  }

  MTL::CaptureManager* capture_manager =
      MTL::CaptureManager::sharedCaptureManager();
  debug_labels_ =
      (capture_manager && (capture_manager->supportsDestination(
                               MTL::CaptureDestinationDeveloperTools) ||
                           capture_manager->supportsDestination(
                               MTL::CaptureDestinationGPUTraceDocument))) ||
      std::getenv("MTL_DEBUG_LAYER");

  wait_shared_event_ = device_->newSharedEvent();
  if (wait_shared_event_) {
    wait_shared_event_->setLabel(
        NS::String::string("XeniaWaitEvent", NS::UTF8StringEncoding));
    wait_shared_event_value_ = 0;
  } else {
    XELOGW(
        "MetalCommandProcessor: SharedEvent unavailable; falling back to "
        "waitUntilCompleted");
  }

  // Native patch tessellation limit: 64 on Apple5+/Mac2, 16 on older Apple GPUs
  // (both even, as required by fractional_even partitioning).
  max_tessellation_factor_ = (device_->supportsFamily(MTL::GPUFamilyApple5) ||
                              device_->supportsFamily(MTL::GPUFamilyMac2))
                                 ? 64u
                                 : 16u;

  msl_system_constants_version_ = 1;
  msl_current_float_constant_map_vertex_.fill(0);
  msl_current_float_constant_map_pixel_.fill(0);
  msl_float_constants_dirty_vertex_ = true;
  msl_float_constants_dirty_pixel_ = true;
  msl_bool_loop_constants_dirty_ = true;
  msl_fetch_constants_dirty_ = true;

  // Initialize shared memory
  shared_memory_ =
      std::make_unique<MetalSharedMemory>(*this, *memory_, trace_writer_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  // Resolve read watch (ResolveReadWatchActive decides whether it is used).
  InitResolveReadWatch();
  resolve_read_callback_ = memory_->RegisterPhysicalMemoryReadCallback(
      ResolveReadCallbackThunk, this);

  // Initialize primitive processor (index/primitive conversion like D3D12).
  primitive_processor_ = std::make_unique<MetalPrimitiveProcessor>(
      *this, *register_file_, *memory_, trace_writer_, *shared_memory_);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize Metal primitive processor");
    return false;
  }

  texture_cache_ = std::make_unique<MetalTextureCache>(this, *register_file_,
                                                       *shared_memory_, 1, 1);
  if (!texture_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal texture cache");
    return false;
  }

  // Initialize render target cache
  render_target_cache_ = std::make_unique<MetalRenderTargetCache>(
      *register_file_, *memory_, &trace_writer_, 1, 1, *this);
  if (!render_target_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal render target cache");
    return false;
  }
  render_target_cache_->SetStripMerge(&strip_merge_);
  strip_merge_.SetBackend(this);
  if (render_target_cache_) {
    render_target_cache_->SetUnfinishedGpuWritesQuery(
        [this](uint32_t address, uint32_t length) {
          return UnfinishedGpuWriteSubmission(address, length) != 0;
        });
  }

  // Fallback for query segment normalization when no draw pinned a scale.
  zpd_draw_resolution_scale_x_ = texture_cache_->draw_resolution_scale_x();
  zpd_draw_resolution_scale_y_ = texture_cache_->draw_resolution_scale_y();

  zpd_visibility_pool_ = std::make_unique<MetalZPDVisibilityPool>();
  EnsureQueryResources();

  // Initialize shader translation pipeline
  if (!pipeline_cache_->InitializeShaderTranslation()) {
    XELOGE("Failed to initialize shader translation");
    return false;
  }
  // Create render target texture for offscreen rendering
  MTL::TextureDescriptor* color_desc = MTL::TextureDescriptor::alloc()->init();
  color_desc->setTextureType(MTL::TextureType2D);
  color_desc->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
  color_desc->setWidth(render_target_width_);
  color_desc->setHeight(render_target_height_);
  color_desc->setStorageMode(MTL::StorageModePrivate);
  color_desc->setUsage(MTL::TextureUsageRenderTarget |
                       MTL::TextureUsageShaderRead);

  render_target_texture_ = device_->newTexture(color_desc);
  color_desc->release();

  if (!render_target_texture_) {
    XELOGE("Failed to create render target texture");
    return false;
  }
  render_target_texture_->setLabel(
      NS::String::string("XeniaRenderTarget", NS::UTF8StringEncoding));

  // Create depth/stencil texture
  MTL::TextureDescriptor* depth_desc = MTL::TextureDescriptor::alloc()->init();
  depth_desc->setTextureType(MTL::TextureType2D);
  depth_desc->setPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
  depth_desc->setWidth(render_target_width_);
  depth_desc->setHeight(render_target_height_);
#if XE_PLATFORM_IOS
  // This fallback depth/stencil target is transient (clear/dontcare only) and
  // never sampled, so memoryless is the most efficient iOS storage mode.
  depth_desc->setStorageMode(MTL::StorageModeMemoryless);
#else
  depth_desc->setStorageMode(MTL::StorageModePrivate);
#endif
  depth_desc->setUsage(MTL::TextureUsageRenderTarget);

  depth_stencil_texture_ = device_->newTexture(depth_desc);
  depth_desc->release();

  if (!depth_stencil_texture_) {
    XELOGE("Failed to create depth/stencil texture");
    return false;
  }
  depth_stencil_texture_->setLabel(
      NS::String::string("XeniaDepthStencil", NS::UTF8StringEncoding));

  // Create render pass descriptor
  render_pass_descriptor_ = MTL::RenderPassDescriptor::alloc()->init();

  auto color_attachment =
      render_pass_descriptor_->colorAttachments()->object(0);
  color_attachment->setTexture(render_target_texture_);
  color_attachment->setLoadAction(MTL::LoadActionClear);
  color_attachment->setStoreAction(MTL::StoreActionStore);
  color_attachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));

  auto depth_attachment = render_pass_descriptor_->depthAttachment();
  depth_attachment->setTexture(depth_stencil_texture_);
  depth_attachment->setLoadAction(MTL::LoadActionClear);
  depth_attachment->setStoreAction(MTL::StoreActionDontCare);
  depth_attachment->setClearDepth(1.0);

  auto stencil_attachment = render_pass_descriptor_->stencilAttachment();
  stencil_attachment->setTexture(depth_stencil_texture_);
  stencil_attachment->setLoadAction(MTL::LoadActionClear);
  stencil_attachment->setStoreAction(MTL::StoreActionDontCare);
  stencil_attachment->setClearStencil(0);

  // Create a null buffer for unused descriptor entries
  // This prevents shader validation errors when accessing unpopulated
  // descriptors
  null_buffer_ =
      device_->newBuffer(kNullBufferSize, MTL::ResourceStorageModeShared);
  if (!null_buffer_) {
    XELOGE("Failed to create null buffer");
    return false;
  }
  null_buffer_->setLabel(
      NS::String::string("NullBuffer", NS::UTF8StringEncoding));
  std::memset(null_buffer_->contents(), 0, kNullBufferSize);

  // Create a 1x1x1 placeholder 2D array texture for unbound texture slots
  // Xbox 360 textures are typically 2D arrays (for texture atlases, cubemaps)
  // Using 2DArray prevents "Invalid texture type" validation errors
  MTL::TextureDescriptor* null_tex_desc =
      MTL::TextureDescriptor::alloc()->init();
  null_tex_desc->setTextureType(MTL::TextureType2DArray);
  null_tex_desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  null_tex_desc->setWidth(1);
  null_tex_desc->setHeight(1);
  null_tex_desc->setArrayLength(1);  // Single slice in the array
  null_tex_desc->setStorageMode(MTL::StorageModeShared);
  null_tex_desc->setUsage(MTL::TextureUsageShaderRead);

  null_texture_ = device_->newTexture(null_tex_desc);
  null_tex_desc->release();

  if (!null_texture_) {
    XELOGE("Failed to create null texture");
    return false;
  }
  null_texture_->setLabel(
      NS::String::string("NullTexture2DArray", NS::UTF8StringEncoding));

  // Fill the 1x1x1 texture with opaque white (helps debug if sampled)
  uint32_t white_pixel = 0xFFFFFFFF;
  MTL::Region region =
      MTL::Region(0, 0, 0, 1, 1, 1);  // x,y,z origin, w,h,d size
  null_texture_->replaceRegion(region, 0, 0, &white_pixel, 4, 0);  // slice 0

  // Create a default sampler for unbound sampler slots
  // Must set supportsArgumentBuffers=YES for use in argument buffers
  MTL::SamplerDescriptor* null_smp_desc =
      MTL::SamplerDescriptor::alloc()->init();
  null_smp_desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMipFilter(MTL::SamplerMipFilterLinear);
  null_smp_desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setSupportArgumentBuffers(true);

  null_sampler_ = device_->newSamplerState(null_smp_desc);
  null_smp_desc->release();

  if (!null_sampler_) {
    XELOGE("Failed to create null sampler");
    return false;
  }

  pipeline_cache_->InitializeAsyncCompilation();

  return true;
}

void MetalCommandProcessor::PrepareForWait() {
  // Runs on every ring-empty stall, so it must not block on the GPU.
  EndCommandBuffer(CommandBufferKind::kSubmissionWait);

  CommandProcessor::PrepareForWait();
}

uint64_t MetalCommandProcessor::GetHeldGpuWritesSubmission() {
  // Resolve output, with zero-copy. Command processor stores aren't held: a
  // late held store could overwrite a newer guest write (a read-through
  // variant hung BF4 on a spin at 0x1FC1B002).
  // Output a read watch guards makes a CPU access of it wait instead: held
  // behind the rest only (not watchable), and behind all of it again once the
  // watch has given up.
  uint64_t submission =
      !resolve_read_watch_disabled_.load(std::memory_order_relaxed)
          ? resolve_unwatched_last_submission_
          : resolve_last_submission_;
  return submission > GetCompletedSubmission() ? submission : 0;
}

bool MetalCommandProcessor::ResolveReadWatchActive() const {
  return resolve_page_owners_ && shared_memory_ &&
         shared_memory_->is_zero_copy() &&
         !resolve_read_watch_disabled_.load(std::memory_order_relaxed);
}

bool MetalCommandProcessor::EndResolveSubmission() {
  if (current_command_buffer_) {
    EndCommandBuffer(CommandBufferKind::kSubmissionOther);
  }
  return true;
}

bool MetalCommandProcessor::FaultCopyResolveToGuestRam(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  if (ranges.empty()) {
    return false;
  }
  // Under resolve_read_mutex_. The ranges are pages the owner still owns.
  uint64_t submission = GetResolveSubmittedThrough();
  auto owner_it = resolve_owners_.find(
      resolve_page_owners_[ranges.front().first >> 12].load(
          std::memory_order_relaxed));
  if (owner_it != resolve_owners_.end()) {
    submission = std::min(submission, owner_it->second.submission);
  }
  // Export output is never submitted ahead.
  bool adaptive = owner_it != resolve_owners_.end() &&
                  !IsMemexportWatchPage(ranges.front().first);
  if (completed_command_buffers_.load(std::memory_order_acquire) >=
      submission) {
    if (adaptive) {
      AdaptiveNoteAccess(owner_it->first, owner_it->second.submission, false,
                         0);
    }
    return true;
  }
  // Not the GPU thread's AwaitSubmissionCompletion: this may be any thread,
  // with or without the global lock. Completion handlers take only
  // completion_mutex_, so they never wait for this.
  auto wait_start = std::chrono::steady_clock::now();
  bool completed;
  {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    completed = completion_cond_.wait_for(
        lock, kResolveFaultWaitBound, [this, submission]() {
          return completed_command_buffers_.load(std::memory_order_acquire) >=
                 submission;
        });
  }
  uint64_t wait_ns =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - wait_start)
                   .count());
  if (adaptive) {
    AdaptiveNoteAccess(owner_it->first, owner_it->second.submission, true,
                       wait_ns);
  }
  if (IsMemexportWatchPage(ranges.front().first)) {
    memexport_watch_frame_waits_.fetch_add(1, std::memory_order_relaxed);
  }
  if (!completed) {
    uint32_t hits =
        resolve_fault_bound_hits_.fetch_add(1, std::memory_order_relaxed) + 1;
    XELOGW(
        "Metal: a CPU access of resolve output at 0x{:08X} waited {} ms for "
        "submission {} (completed {}) and goes ahead without it",
        ranges.front().first, kResolveFaultWaitBound.count(), submission,
        completed_command_buffers_.load(std::memory_order_relaxed));
    if (hits >= kResolveFaultMaxBoundHits &&
        !resolve_read_watch_disabled_.exchange(true,
                                               std::memory_order_relaxed)) {
      XELOGE(
          "Metal: resolve read watch waits ran out {} times, so guest signals "
          "are held behind resolve output again for the rest of the run",
          hits);
    }
  }
  // Either way the watch is done with: after a bound hit the access goes
  // ahead rather than faulting again into another wait.
  // TODO(xenios-jp): for a write (or kDiscard) the GPU output then lands over
  // what the guest wrote. Needs counts of how often the bound is hit by writes
  // before changing it for them.
  return true;
}

int MetalCommandProcessor::ResolveOwnerStateAt(uint32_t physical_address,
                                               bool note_read) {
  if (!resolve_page_owners_ || physical_address >= SharedMemory::kBufferSize) {
    return 0;
  }
  uint64_t owner_key = resolve_page_owners_[physical_address >> 12].load(
      std::memory_order_relaxed);
  if (!owner_key) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(resolve_read_mutex_);
  auto owner_it = resolve_owners_.find(owner_key);
  if (owner_it == resolve_owners_.end() || !owner_it->second.copyable) {
    return 0;
  }
  const ResolveOwner& owner = owner_it->second;
  if (owner.submission > GetResolveSubmittedThrough()) {
    // rearm_pending: an earlier callback of this or another fault counted it.
    if (note_read && !owner.in_guest_ram && !owner.rearm_pending) {
      // The read goes ahead with what guest RAM holds; the command processor
      // thread submits the resolve at its next draw (SubmitWantedResolve).
      resolve_submit_wanted_.store(true, std::memory_order_relaxed);
      if (!IsMemexportWatchPage(physical_address)) {
        AdaptivePromote(owner_key, kAdaptiveUnsubmittedCredit);
      }
    }
    return 1;
  }
  return owner.submission >
                 completed_command_buffers_.load(std::memory_order_acquire)
             ? 2
             : 3;
}

void MetalCommandProcessor::SubmitWantedResolve() {
  if (!resolve_unsubmitted_owner_) {
    resolve_submit_wanted_.store(false, std::memory_order_relaxed);
    return;
  }
  // Strip merging keeps its passes in one command buffer; asked again at the
  // next draw.
  if (strip_merge_.recording()) {
    return;
  }
  resolve_submit_wanted_.store(false, std::memory_order_relaxed);
  EndResolveSubmission();
}

void MetalCommandProcessor::AdaptivePromote(uint64_t resolve_key,
                                            uint32_t credit) {
  auto it = adaptive_destinations_.find(resolve_key);
  if (it == adaptive_destinations_.end()) {
    if (adaptive_destinations_.size() >= kMaxAdaptiveDestinations) {
      adaptive_destinations_.clear();
    }
    it = adaptive_destinations_.emplace(resolve_key, AdaptiveDestination())
             .first;
  }
  it->second.credit = std::max(it->second.credit, credit);
}

void MetalCommandProcessor::AdaptiveNoteAccess(uint64_t resolve_key,
                                               uint64_t owner_submission,
                                               bool waited,
                                               uint64_t waited_ns) {
  auto it = adaptive_destinations_.find(resolve_key);
  if (it == adaptive_destinations_.end() ||
      it->second.early_submission != owner_submission) {
    // Submitted lazily.
    if (waited && waited_ns >= kAdaptiveMinWaitNs) {
      AdaptivePromote(
          resolve_key,
          uint32_t(std::min(
              uint64_t(kAdaptiveMaxCredit),
              std::max(uint64_t(2), waited_ns / kAdaptiveSubmitCostNs))));
    }
    return;
  }
  // Submitted early: would the access have waited for a lazy submission?
  AdaptiveDestination& destination = it->second;
  destination.early_submission = 0;
  bool used = waited;
  if (!used) {
    size_t slot = size_t(owner_submission & (kAdaptiveRingSize - 1));
    if (adaptive_sync_submission_[slot].load(std::memory_order_acquire) !=
        owner_submission) {
      // No guest sync point would have submitted it yet.
      used = true;
    } else {
      // A lazy submission would have been committed at the sync point and
      // could not have completed sooner than its GPU time after that.
      uint64_t lazy_done_ns =
          adaptive_sync_ns_[slot].load(std::memory_order_relaxed);
      if (adaptive_gpu_submission_[slot].load(std::memory_order_acquire) ==
          owner_submission) {
        lazy_done_ns += adaptive_gpu_ns_[slot].load(std::memory_order_relaxed);
      }
      used = uint64_t(GetSteadyTimeNs()) < lazy_done_ns;
    }
  }
  if (used) {
    if (waited) {
    }
    destination.credit = std::max(destination.credit, kAdaptiveUsedCredit);
    return;
  }
  if (destination.credit <= 1) {
    adaptive_destinations_.erase(it);
  } else {
    --destination.credit;
  }
}

bool MetalCommandProcessor::AdaptiveResolveSubmitsEarly(uint64_t resolve_key) {
  {
    std::lock_guard<std::mutex> lock(resolve_read_mutex_);
    auto it = adaptive_destinations_.find(resolve_key);
    if (it == adaptive_destinations_.end()) {
      return false;
    }
    AdaptiveDestination& destination = it->second;
    if (destination.early_submission) {
      // Resolved again with no access of the last output.
      destination.early_submission = 0;
      if (destination.credit <= 1) {
        adaptive_destinations_.erase(it);
        return false;
      }
      --destination.credit;
    }
    adaptive_early_frame_ = resolve_frame_index_;
    // Strip merging keeps its passes in one command buffer.
    if (strip_merge_.recording() || !current_command_buffer_) {
      return false;
    }
    destination.early_submission = submission_current_;
  }
  adaptive_awaiting_sync_.push_back(submission_current_);
  return true;
}

void MetalCommandProcessor::OnResolveGuestSync() {
  if (adaptive_awaiting_sync_.empty()) {
    return;
  }
  uint64_t now_ns = uint64_t(GetSteadyTimeNs());
  for (uint64_t submission : adaptive_awaiting_sync_) {
    size_t slot = size_t(submission & (kAdaptiveRingSize - 1));
    adaptive_sync_ns_[slot].store(now_ns, std::memory_order_relaxed);
    adaptive_sync_submission_[slot].store(submission,
                                          std::memory_order_release);
  }
  adaptive_awaiting_sync_.clear();
}

void MetalCommandProcessor::SubmitResolvesAtGpuFence() {
  if (!resolve_unsubmitted_owner_ || !current_command_buffer_ ||
      !ResolveReadWatchActive()) {
    return;
  }
  // Worth it only while an access of resolve output waits for the GPU (an
  // early destination was resolved this frame or the last), and while the GPU
  // has been running dry at these points.
  bool submit = adaptive_early_frame_ != UINT32_MAX &&
                resolve_frame_index_ - adaptive_early_frame_ <= 1 &&
                adaptive_fence_score_ >= kAdaptiveFenceScoreSubmit &&
                !strip_merge_.recording();
  if (!submit) {
    // One test per command buffer.
    if (!adaptive_fence_tests_.empty() &&
        adaptive_fence_tests_.back().submission == submission_current_) {
      return;
    }
    adaptive_fence_tests_.push_back(
        {submission_current_, submission_current_ - 1, false});
    return;
  }
  if (current_render_encoder_) {
  }
  // Judged when the next command buffer ends, which is when this one's work
  // would have been submitted.
  adaptive_fence_tests_.push_back(
      {submission_current_ + 1, submission_current_ - 1, true});
  EndResolveSubmission();
}

void MetalCommandProcessor::AdaptiveJudgeFenceTests() {
  uint64_t completed =
      completed_command_buffers_.load(std::memory_order_acquire);
  size_t kept = 0;
  for (const AdaptiveFenceTest& test : adaptive_fence_tests_) {
    if (test.submission > submission_current_) {
      adaptive_fence_tests_[kept++] = test;
      continue;
    }
    bool dry = completed >= test.prior_submission;
    if (dry) {
      // A dry GPU loses the time to the next submission; a submit it didn't
      // need costs one command buffer. So one dry point outweighs two that
      // weren't.
      adaptive_fence_score_ =
          std::min(adaptive_fence_score_ + 2, kAdaptiveFenceScoreMax);
    } else {
      if (test.submitted) {
      }
      adaptive_fence_score_ -= adaptive_fence_score_ ? 1 : 0;
    }
  }
  adaptive_fence_tests_.resize(kept);
}

void MetalCommandProcessor::ResolveReadCallbackThunk(
    void* context, uint32_t physical_address, uint32_t length,
    Memory::PhysicalAccess access) {
  auto command_processor = static_cast<MetalCommandProcessor*>(context);
  // A read that arrives before its resolve's submission asks for the
  // submission (ResolveOwnerStateAt).
  if (access == Memory::PhysicalAccess::kRead) {
    command_processor->ResolveOwnerStateAt(physical_address, true);
  }
  if (command_processor->IsMemexportWatchPage(physical_address)) {
    command_processor->memexport_watch_frame_faults_.fetch_add(
        1, std::memory_order_relaxed);
  }
  switch (access) {
    case Memory::PhysicalAccess::kRead:
      command_processor->MarkResolvePagesRead(physical_address, length);
      break;
    case Memory::PhysicalAccess::kWrite:
      command_processor->PrepareResolvePagesForWrite(physical_address, length);
      break;
    case Memory::PhysicalAccess::kDiscard:
      // The GPU writes guest RAM itself, so dropping the owners doesn't stop a
      // submitted resolve from landing over what the pages hold next: wait for
      // it as a write would first.
      command_processor->PrepareResolvePagesForWrite(physical_address, length);
      command_processor->DiscardResolvePages(physical_address, length);
      break;
  }
}

void MetalCommandProcessor::AwaitGpuWritesBeforeGuestRead() {
  // Export output that isn't awaited is left unordered here, as in D3D12 and
  // Vulkan.
  if (resolve_last_submission_ <= GetCompletedSubmission()) {
    return;
  }
  int64_t wait_start_ns = GetSteadyTimeNs();
  if (resolve_last_submission_ >= submission_current_ &&
      current_command_buffer_) {
    EndCommandBuffer(CommandBufferKind::kSubmissionWait);
  }
  AwaitSubmissionCompletion(resolve_last_submission_);
  NoteGpuWait(GpuWaitReason::kResolveBeforeGuestRead, wait_start_ns);
}

void MetalCommandProcessor::NoteWatchedGpuWrite(uint32_t address,
                                                uint32_t length) {
  if (!length || address >= SharedMemory::kBufferSize) {
    return;
  }
  length = std::min(length, SharedMemory::kBufferSize - address);
  watched_gpu_writes_last_submission_ =
      std::max(watched_gpu_writes_last_submission_, GetCurrentSubmission());
  watched_gpu_writes_page_min_ =
      std::min(watched_gpu_writes_page_min_, address >> 12);
  watched_gpu_writes_page_max_ =
      std::max(watched_gpu_writes_page_max_, (address + length - 1) >> 12);
}

uint64_t MetalCommandProcessor::UnfinishedGpuWriteSubmission(uint32_t address,
                                                             uint32_t length) {
  if (watched_gpu_writes_page_min_ > watched_gpu_writes_page_max_ || !length) {
    return 0;
  }
  // Export output of the open command buffer has no owners yet.
  FlushMemexportWatchRanges();
  // As TranslatePhysical masks.
  address &= 0x1FFFFFFF;
  uint32_t first_page = address >> 12;
  uint32_t last_page =
      uint32_t(std::min(uint64_t(address) + length - 1,
                        uint64_t(SharedMemory::kBufferSize - 1)) >>
               12);
  if (first_page > watched_gpu_writes_page_max_ ||
      last_page < watched_gpu_writes_page_min_) {
    return 0;
  }
  uint64_t completed = GetCompletedSubmission();
  if (watched_gpu_writes_last_submission_ <= completed) {
    watched_gpu_writes_page_min_ = UINT32_MAX;
    watched_gpu_writes_page_max_ = 0;
    return 0;
  }
  if (!resolve_page_owners_) {
    return 0;
  }
  first_page = std::max(first_page, watched_gpu_writes_page_min_);
  last_page = std::min(last_page, watched_gpu_writes_page_max_);
  uint64_t submission = 0;
  uint64_t last_owner_key = 0;
  std::unique_lock<std::mutex> lock(resolve_read_mutex_, std::defer_lock);
  for (uint32_t page = first_page; page <= last_page; ++page) {
    uint64_t owner_key =
        resolve_page_owners_[page].load(std::memory_order_relaxed);
    if (!owner_key || owner_key == last_owner_key) {
      continue;
    }
    last_owner_key = owner_key;
    if (!lock.owns_lock()) {
      lock.lock();
    }
    auto owner_it = resolve_owners_.find(owner_key);
    // Not in_guest_ram: a fault's bounded wait sets it without completion.
    if (owner_it != resolve_owners_.end() && owner_it->second.copyable &&
        owner_it->second.submission > completed) {
      submission = std::max(submission, owner_it->second.submission);
    }
  }
  return submission;
}

void MetalCommandProcessor::AwaitGpuWritesTo(uint32_t physical_address,
                                             uint32_t length) {
  uint64_t submission = UnfinishedGpuWriteSubmission(physical_address, length);
  if (!submission) {
    return;
  }
  int64_t wait_start_ns = GetSteadyTimeNs();
  if (submission >= submission_current_ && current_command_buffer_) {
    EndCommandBuffer(CommandBufferKind::kSubmissionWait);
  }
  AwaitSubmissionCompletion(submission);
  NoteGpuWait(GpuWaitReason::kResolveBeforeGuestRead, wait_start_ns);
}

void MetalCommandProcessor::PollCompletedSubmission() {
  ProcessCompletedSubmissions();
  PumpQueryResolves();
}

void MetalCommandProcessor::WaitForPendingCompletionHandlers() {
  constexpr auto kMaxWait = std::chrono::seconds(5);
  auto wait_start = std::chrono::steady_clock::now();
  while (pending_completion_handlers_.load(std::memory_order_acquire) != 0) {
    if (std::chrono::steady_clock::now() - wait_start >= kMaxWait) {
      XELOGW(
          "MetalCommandProcessor: timed out waiting for {} completion "
          "handler(s) during shutdown",
          pending_completion_handlers_.load(std::memory_order_relaxed));
      // A timeout is diagnostic, not permission to destroy callback owners.
      wait_start = std::chrono::steady_clock::now();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void MetalCommandProcessor::ShutdownContext() {
  strip_merge_.SetBackend(nullptr);
  render_encoder_memexport_ranges_.clear();
  pending_shader_done_fence_ranges_.clear();
  pending_shader_done_fence_values_.clear();
  shader_done_fences_outside_pass_ = false;
  render_encoder_memexport_draws_are_pure_ = true;
  render_encoder_narrowed_reads_.clear();
  render_encoder_pixel_stage_fetches_ = false;
  argument_binder_.ResetUploadCaches();
  // End any active render encoder before shutdown
  if (current_render_encoder_) {
    if (render_target_cache_) {
      render_target_cache_->FinalizeRenderPassStoreActions(
          current_render_encoder_, current_render_pass_descriptor_);
    }
    current_render_encoder_->endEncoding();
    // Don't release yet - wait until command buffer completes
  }

  // Submit and wait for any pending command buffer
  if (current_command_buffer_) {
    EndSharedComputeEncoder();
    uint64_t wait_value = 0;
    if (wait_shared_event_) {
      wait_value = ++wait_shared_event_value_;
      current_command_buffer_->encodeSignalEvent(wait_shared_event_,
                                                 wait_value);
    }
    if (texture_cache_) {
      texture_cache_->CommitLoadsAhead();
    }
    CommitTessPrepAhead();
    ScheduleSpirvArgumentBufferRelease(current_command_buffer_);
    current_command_buffer_->commit();
    if (wait_shared_event_) {
      // Bounded so a GPU that has stopped (in the background) can't hold up
      // quitting. The command buffer keeps what it uses alive.
      if (!wait_shared_event_->waitUntilSignaledValue(wait_value, 2000)) {
        XELOGW(
            "Metal: the last command buffer didn't finish in 2 s at shutdown");
      }
    } else {
      current_command_buffer_->waitUntilCompleted();
    }
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    current_draw_index_ = 0;
    copy_resolve_writes_pending_ = false;
  }

  // Finish or cancel any standalone upload batch before joining callbacks.
  if (texture_cache_) {
    texture_cache_->FinishPendingUploads();
  }

  // Even if we have no active command buffer at this point, there may be
  // previously committed command buffers still in flight. Submit and wait for
  // a dummy command buffer to ensure all GPU work on this queue has completed
  // before tearing down resources on thread exit.
  if (command_queue_) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::CommandBuffer* sync_cmd = command_queue_->commandBuffer();
    if (sync_cmd) {
      uint64_t wait_value = 0;
      if (wait_shared_event_) {
        wait_value = ++wait_shared_event_value_;
        sync_cmd->encodeSignalEvent(wait_shared_event_, wait_value);
      }
      sync_cmd->commit();
      if (wait_shared_event_) {
        wait_shared_event_->waitUntilSignaledValue(
            wait_value, std::numeric_limits<uint64_t>::max());
      } else {
        sync_cmd->waitUntilCompleted();
      }
    }
    pool->release();
  }

  WaitForPendingCompletionHandlers();

  // Nothing is in flight now, so no fault can still be waiting for a
  // submission.
  ResetResolveReadWatch();
  adaptive_destinations_.clear();
  adaptive_awaiting_sync_.clear();
  adaptive_fence_tests_.clear();

  if (uint64_t failed =
          failed_command_buffers_.load(std::memory_order_relaxed)) {
    XELOGW("MetalCommandProcessor: {} command buffer(s) didn't complete",
           failed);
  }

  // Deterministic for a given workload, so a single-frame trace replay can be
  // read from it - the 60-frame counter window never closes in one.
  std::string kind_breakdown;
  for (size_t i = 0; i < kCommandBufferKindCount; ++i) {
    if (!command_buffer_kind_counts_[i]) {
      continue;
    }
    kind_breakdown += fmt::format(" {}={}", kCommandBufferKindNames[i],
                                  command_buffer_kind_counts_[i]);
  }
  XELOGI("Metal: {} command buffer(s) executed,{}",
         gpu_time_command_buffers_.load(std::memory_order_relaxed),
         kind_breakdown.empty() ? " none created" : kind_breakdown);

  render_encoder_shared_memory_hazards_.Abandon();
  // Now safe to release encoder and command buffer
  if (current_render_encoder_) {
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
  }
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  DrainCommandBufferAutoreleasePool();

  {
    std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
    command_buffer_spirv_argbuf_pages_.clear();
    command_buffer_tess_prep_pages_.clear();
    tess_prep_page_pool_.clear();
    pending_spirv_argbuf_releases_.clear();
    spirv_argbuf_pool_.clear();
  }

  ShutdownQueryResources();
  zpd_visibility_pool_.reset();

  if (texture_cache_) {
    texture_cache_->Shutdown();
    texture_cache_.reset();
  }

  if (primitive_processor_) {
    primitive_processor_->Shutdown();
    primitive_processor_.reset();
  }
  frame_open_ = false;
  ResetMemexportPages();

  tess_prep_ahead_.clear();
  if (tess_prep_pipeline_) {
    tess_prep_pipeline_->release();
    tess_prep_pipeline_ = nullptr;
  }
  if (pipeline_cache_) {
    pipeline_cache_->Shutdown();
  }

  shared_memory_.reset();
  if (wait_shared_event_) {
    wait_shared_event_->release();
    wait_shared_event_ = nullptr;
  }

  CommandProcessor::ShutdownContext();
}

void MetalCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  EndZPDFrame();
  ProcessCompletedSubmissions();
  LogFrameGpuWaits();
  pipeline_cache_->LogCompileStages();

  // Completion handlers land a frame or two behind, which a window this wide
  // absorbs.
  static constexpr uint32_t kGpuTimeWindowFrames = 60;
  if (++gpu_time_window_frames_ >= kGpuTimeWindowFrames) {
    uint64_t now_ns = completed_gpu_time_ns_.load(std::memory_order_relaxed);
    COUNT_profile_set("gpu/gpu_busy_us_per_frame",
                      int64_t((now_ns - gpu_time_window_start_ns_) /
                              (uint64_t(1000) * gpu_time_window_frames_)));
    gpu_time_window_start_ns_ = now_ns;

    uint64_t command_buffers_now =
        gpu_time_command_buffers_.load(std::memory_order_relaxed);
    COUNT_profile_set(
        "gpu/command_buffers_per_frame",
        int64_t((command_buffers_now - gpu_time_command_buffers_window_start_) /
                gpu_time_window_frames_));
    gpu_time_command_buffers_window_start_ = command_buffers_now;

    // Unrolled: COUNT_profile_set caches its token in a function-local static,
    // so a loop would file every kind under the first name it saw.
    auto kind_per_frame = [&](CommandBufferKind kind) {
      size_t i = size_t(kind);
      uint64_t now = command_buffer_kind_counts_[i];
      int64_t per_frame = int64_t((now - command_buffer_kind_window_start_[i]) /
                                  gpu_time_window_frames_);
      command_buffer_kind_window_start_[i] = now;
      return per_frame;
    };
    COUNT_profile_set("gpu/cb_submission_other_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionOther));
    COUNT_profile_set(
        "gpu/cb_submission_copy_draw_sync_per_frame",
        kind_per_frame(CommandBufferKind::kSubmissionCopyToDrawSync));
    COUNT_profile_set("gpu/cb_submission_zpd_query_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionZpdQuery));
    COUNT_profile_set(
        "gpu/cb_submission_uniforms_rollover_per_frame",
        kind_per_frame(CommandBufferKind::kSubmissionUniformsRollover));
    COUNT_profile_set(
        "gpu/cb_submission_primary_end_per_frame",
        kind_per_frame(CommandBufferKind::kSubmissionPrimaryBufferEnd));
    COUNT_profile_set("gpu/cb_submission_wait_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionWait));
    COUNT_profile_set("gpu/cb_submission_pass_limit_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionPassLimit));
    COUNT_profile_set("gpu/cb_submission_after_drain_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionAfterDrain));
    COUNT_profile_set("gpu/cb_texture_upload_batch_per_frame",
                      kind_per_frame(CommandBufferKind::kTextureUploadBatch));
    COUNT_profile_set("gpu/cb_texture_upload_private_per_frame",
                      kind_per_frame(CommandBufferKind::kTextureUploadPrivate));
    COUNT_profile_set("gpu/cb_texture_other_per_frame",
                      kind_per_frame(CommandBufferKind::kTextureOther));
    COUNT_profile_set("gpu/cb_rt_resolve_per_frame",
                      kind_per_frame(CommandBufferKind::kRenderTargetResolve));
    COUNT_profile_set("gpu/cb_rt_dump_per_frame",
                      kind_per_frame(CommandBufferKind::kRenderTargetDump));
    COUNT_profile_set("gpu/cb_rt_other_per_frame",
                      kind_per_frame(CommandBufferKind::kRenderTargetOther));
    COUNT_profile_set("gpu/cb_tess_prep_per_frame",
                      kind_per_frame(CommandBufferKind::kTessellationPrep));

    COUNT_profile_set(
        "gpu/render_passes_per_frame",
        int64_t((render_passes_total_ - render_passes_window_start_) /
                gpu_time_window_frames_));
    render_passes_window_start_ = render_passes_total_;

    gpu_time_window_frames_ = 0;
  }

  saw_swap_ = true;
  last_swap_ptr_ = frontbuffer_ptr;
  last_swap_width_ = frontbuffer_width;
  last_swap_height_ = frontbuffer_height;

  // Submit the frame the way every other submission ends, including the
  // argument-buffer reuse cache reset (the cached slices belong to this
  // submission's pages, which may be recycled once it retires) and the
  // autorelease pool drain.
  EndCommandBuffer();
  NoteResolveFrame();
  EndMemexportWatchFrame();

  if (primitive_processor_ && frame_open_) {
    primitive_processor_->EndFrame();
    closed_frames_.push_back({frame_current_, submission_current_});
    ++frame_current_;
    frame_open_ = false;
  }
  if (shared_memory_ && ::cvars::clear_memory_page_state) {
    shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
  }

  // Push the rendered frame to the presenter's guest output mailbox
  // This is required for trace dumps to capture the output. Use the
  // MetalRenderTargetCache color target (like D3D12) rather than the
  // legacy standalone render_target_texture_.
  auto* presenter =
      static_cast<ui::metal::MetalPresenter*>(graphics_system_->presenter());
  if (presenter && render_target_cache_) {
    uint32_t output_width =
        frontbuffer_width ? frontbuffer_width : render_target_width_;
    uint32_t output_height =
        frontbuffer_height ? frontbuffer_height : render_target_height_;

    MTL::Texture* source_texture = nullptr;
    bool use_pwl_gamma_ramp = false;
    if (texture_cache_) {
      uint32_t swap_width = 0;
      uint32_t swap_height = 0;
      xenos::TextureFormat swap_format = xenos::TextureFormat::k_8_8_8_8;
      source_texture = texture_cache_->RequestSwapTexture(
          swap_width, swap_height, swap_format);
      if (source_texture) {
        output_width = swap_width;
        output_height = swap_height;
        use_pwl_gamma_ramp =
            swap_format == xenos::TextureFormat::k_2_10_10_10 ||
            swap_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        static MTL::PixelFormat last_format = MTL::PixelFormatInvalid;
        static uint32_t last_samples = 0;
        static uint32_t last_width = 0;
        static uint32_t last_height = 0;
        static int last_swap_format = -1;
        MTL::PixelFormat src_format = source_texture->pixelFormat();
        uint32_t src_samples = source_texture->sampleCount();
        uint32_t src_width = uint32_t(source_texture->width());
        uint32_t src_height = uint32_t(source_texture->height());
        int swap_format_int = static_cast<int>(swap_format);
        if (src_format != last_format || src_samples != last_samples ||
            src_width != last_width || src_height != last_height ||
            swap_format_int != last_swap_format) {
          last_format = src_format;
          last_samples = src_samples;
          last_width = src_width;
          last_height = src_height;
          last_swap_format = swap_format_int;
        }
        if (presenter) {
          if (!gamma_ramp_256_entry_table_up_to_date_ ||
              !gamma_ramp_pwl_up_to_date_) {
            constexpr size_t kGammaRampTableBytes =
                sizeof(reg::DC_LUT_30_COLOR) * 256;
            constexpr size_t kGammaRampPwlBytes =
                sizeof(reg::DC_LUT_PWL_DATA) * 128 * 3;
            if (presenter->UpdateGammaRamp(
                    gamma_ramp_256_entry_table(), kGammaRampTableBytes,
                    gamma_ramp_pwl_rgb(), kGammaRampPwlBytes)) {
              gamma_ramp_256_entry_table_up_to_date_ = true;
              gamma_ramp_pwl_up_to_date_ = true;
            } else {
              XELOGW("Metal IssueSwap: gamma ramp upload failed");
            }
          }
        }
      }
    }

    bool swap_dest_swap = false;
    const bool has_swap_dest_swap =
        ConsumeSwapDestSwap(frontbuffer_ptr, &swap_dest_swap);
    if (!has_swap_dest_swap && frontbuffer_ptr) {
      static uint32_t swap_dest_miss_count = 0;
      if (swap_dest_miss_count < 8) {
        ++swap_dest_miss_count;
      }
    }
    bool force_swap_rb = has_swap_dest_swap && swap_dest_swap;

    if (!source_texture) {
      static bool missing_swap_logged = false;
      if (!missing_swap_logged) {
        missing_swap_logged = true;
        XELOGW(
            "MetalCommandProcessor::IssueSwap: swap texture unavailable; "
            "presenting inactive (black) output");
      }
      presenter->RefreshGuestOutput(
          0, 0, 0, 0, [](ui::Presenter::GuestOutputRefreshContext&) -> bool {
            return false;
          });
      return;
    }

    if (source_texture) {
      ui::metal::MetalPresenter* metal_presenter = presenter;
      uint32_t source_width = output_width;
      uint32_t source_height = output_height;
      bool force_swap_rb_copy = force_swap_rb;
      bool use_pwl_gamma_ramp_copy = use_pwl_gamma_ramp;
      uint32_t color_space = swap_color_space_;
      auto aspect = graphics_system_->GetScaledAspectRatio();
      guest_frames_presented.fetch_add(1, std::memory_order_relaxed);
      presenter->RefreshGuestOutput(
          output_width, output_height, aspect.first, aspect.second,
          [source_texture, metal_presenter, source_width, source_height,
           force_swap_rb_copy, use_pwl_gamma_ramp_copy, color_space](
              ui::Presenter::GuestOutputRefreshContext& context) -> bool {
            auto& metal_context =
                static_cast<ui::metal::MetalGuestOutputRefreshContext&>(
                    context);
            context.SetIs8bpc(!use_pwl_gamma_ramp_copy);
            uint64_t submission_id = 0;
            bool copy_success = metal_presenter->CopyTextureToGuestOutput(
                source_texture, metal_context.resource_uav_capable(),
                source_width, source_height, force_swap_rb_copy,
                use_pwl_gamma_ramp_copy, color_space, &submission_id);
            if (copy_success && submission_id) {
              metal_context.SetSubmissionId(submission_id);
            }
            return copy_success;
          });
    }
  }
}

void MetalCommandProcessor::OnPrimaryBufferEnd() {
  // Pump any completed resolves now since the guest is likely about to poll.
  PumpQueryResolves();
  PumpPendingRetire();

  if (!current_command_buffer_) {
    return;
  }

  // Keep command buffers open across primary-buffer boundaries unless a
  // copy->draw visibility boundary is pending.
  if (!copy_resolve_writes_pending_) {
    return;
  }

  if (!cvars::submit_on_primary_buffer_end) {
    return;
  }
  // Ending an open pass here stores and reloads its attachments, and nothing
  // waits at this point: a ring-empty stall and every wait commit at once.
  // The pass is left to end, and the draw beginning the next one commits.
  if (current_render_encoder_) {
    primary_end_commit_at_pass_end_ = true;
    return;
  }
  EndCommandBuffer(CommandBufferKind::kSubmissionPrimaryBufferEnd);
}

void MetalCommandProcessor::CommitBetweenRenderPasses() {
  if (!current_command_buffer_ || current_render_encoder_) {
    return;
  }
  if (command_buffer_render_passes_ >= kMaxRenderPassesPerCommandBuffer) {
    EndCommandBuffer(CommandBufferKind::kSubmissionPassLimit);
  } else if (primary_end_commit_at_pass_end_) {
    EndCommandBuffer(CommandBufferKind::kSubmissionPrimaryBufferEnd);
  } else if (commit_after_drain_armed_ && command_buffer_render_passes_ &&
             GetCompletedSubmission() + 1 >= submission_current_) {
    // Committed submissions run in order, so the GPU is idle once the one
    // before the open submission has completed.
    EndCommandBuffer(CommandBufferKind::kSubmissionAfterDrain);
  }
}

MTL::CommandBuffer* MetalCommandProcessor::CreateAccountedCommandBuffer(
    CommandBufferKind kind) {
  SCOPE_profile_cpu_f("gpu");
  if (!command_queue_) {
    return nullptr;
  }
  MTL::CommandBuffer* command_buffer = nullptr;
  {
    // Blocks once the queue's in-flight command buffers are all outstanding,
    // so this reads as GPU backpressure rather than allocation cost.
    SCOPE_profile_cpu_i("gpu", "MetalCommandProcessor::QueueCommandBuffer");
    uint64_t wait_start = xe::Clock::QueryHostTickCount();
    command_buffer = command_queue_->commandBuffer();
    gpu_timing.wait_ticks.fetch_add(
        xe::Clock::QueryHostTickCount() - wait_start,
        std::memory_order_relaxed);
  }
  if (command_buffer) {
    ++command_buffer_kind_counts_[size_t(kind)];
    SCOPE_profile_cpu_i("gpu", "MetalCommandProcessor::AddGpuTimeHandler");
    AddGpuTimeHandler(command_buffer);
  }
  return command_buffer;
}

void MetalCommandProcessor::DiscardAccountedCommandBuffer(
    MTL::CommandBuffer* command_buffer, CommandBufferKind kind) {
  if (command_buffer) {
    --command_buffer_kind_counts_[size_t(kind)];
    if (shared_memory_ && shared_memory_->hazards()) {
      shared_memory_->hazards()->Discard(command_buffer);
    }
    pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
  }
}

void MetalCommandProcessor::AddGpuTimeHandler(
    MTL::CommandBuffer* command_buffer) {
  pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
  command_buffer->addCompletedHandler([this](MTL::CommandBuffer* completed) {
    // Every accounted command buffer gets this handler, so a failure is seen
    // once here, per command buffer, never per draw.
    MTL::CommandBufferStatus status = completed->status();
    if (status != MTL::CommandBufferStatusCompleted) {
      LogCommandBufferFailure(completed, status);
    }
    double gpu_start = completed->GPUStartTime();
    double gpu_end = completed->GPUEndTime();
    if (gpu_end > gpu_start) {
      completed_gpu_time_ns_.fetch_add(uint64_t((gpu_end - gpu_start) * 1e9),
                                       std::memory_order_relaxed);
      // Union: only the part past the latest end already counted. Exact when
      // command buffers complete in the order they started; one that finishes
      // inside a still-running earlier one loses the earlier one's head.
      uint64_t start_ns = uint64_t(gpu_start * 1e9);
      uint64_t end_ns = uint64_t(gpu_end * 1e9);
      uint64_t counted_end_ns =
          gpu_timing.busy_end_ns.load(std::memory_order_relaxed);
      while (end_ns > counted_end_ns &&
             !gpu_timing.busy_end_ns.compare_exchange_weak(
                 counted_end_ns, end_ns, std::memory_order_relaxed)) {
      }
      if (end_ns > counted_end_ns) {
        gpu_timing.busy_ns.fetch_add(
            end_ns - std::max(start_ns, counted_end_ns),
            std::memory_order_relaxed);
      }
    }
    gpu_time_command_buffers_.fetch_add(1, std::memory_order_relaxed);
    pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
  });
}

void MetalCommandProcessor::LogCommandBufferFailure(
    MTL::CommandBuffer* command_buffer, MTL::CommandBufferStatus status) {
  failed_command_buffers_.fetch_add(1, std::memory_order_relaxed);
  // A buffer aborted by the GPU (for example, work submitted from the
  // background) was silent before; its output is lost.
  NS::Error* error = command_buffer->error();
  // Work submitted from the background is refused by design, not a GPU fault,
  // so diagnostics don't count it.
  if (!(status == MTL::CommandBufferStatusError && error &&
        error->code() == MTL::CommandBufferErrorNotPermitted)) {
    command_buffer_failures.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t kind_bit = status == MTL::CommandBufferStatusError && error
                          ? uint64_t(1) << (uint64_t(error->code()) & 31)
                          : uint64_t(1) << (32 + (uint64_t(status) & 31));
  if (logged_command_buffer_failures_.fetch_or(kind_bit,
                                               std::memory_order_relaxed) &
      kind_bit) {
    return;
  }
  // Metal's completion queue may have no pool of its own.
  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
  NS::String* description = error ? error->localizedDescription() : nullptr;
  XELOGE(
      "MetalCommandProcessor: a command buffer finished with status {}, error "
      "{}: {}. Further failures of this kind are only counted.",
      uint32_t(status), error ? int64_t(error->code()) : int64_t(0),
      description ? description->utf8String() : "no description");
  pool->release();
}

void MetalCommandProcessor::AwaitSubmissionCompletion(uint64_t submission) {
  if (completed_command_buffers_.load(std::memory_order_acquire) >=
      submission) {
    return;
  }
  uint64_t wait_start = xe::Clock::QueryHostTickCount();
  {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    completion_cond_.wait(lock, [this, submission]() {
      return completed_command_buffers_.load(std::memory_order_acquire) >=
             submission;
    });
  }
  gpu_timing.wait_ticks.fetch_add(xe::Clock::QueryHostTickCount() - wait_start,
                                  std::memory_order_relaxed);
}

void MetalCommandProcessor::NoteGpuWait(GpuWaitReason reason,
                                        int64_t wait_start_ns) {
  frame_gpu_wait_ns_[size_t(reason)] += GetSteadyTimeNs() - wait_start_ns;
  ++frame_gpu_wait_counts_[size_t(reason)];
}

void MetalCommandProcessor::CommitAndAwait(MTL::CommandBuffer* command_buffer,
                                           GpuWaitReason reason) {
  EndSharedComputeEncoder(command_buffer);
  command_buffer->commit();
  int64_t wait_start_ns = GetSteadyTimeNs();
  command_buffer->waitUntilCompleted();
  NoteGpuWait(reason, wait_start_ns);
}

void MetalCommandProcessor::LogFrameGpuWaits() {
  int64_t now_ns = GetSteadyTimeNs();
  if (cvars::metal_log_frame_gpu_waits) {
    uint64_t wait_ns = 0;
    std::string waits;
    for (size_t i = 0; i < kGpuWaitReasonCount; ++i) {
      wait_ns += frame_gpu_wait_ns_[i];
      if (frame_gpu_wait_counts_[i]) {
        waits += fmt::format(" {}={:.2f}ms/{}", kGpuWaitReasonNames[i],
                             frame_gpu_wait_ns_[i] * 1e-6,
                             frame_gpu_wait_counts_[i]);
      }
    }
    uint64_t command_buffers = 0;
    std::string kinds;
    for (size_t i = 0; i < kCommandBufferKindCount; ++i) {
      uint64_t count =
          command_buffer_kind_counts_[i] - command_buffer_kind_frame_start_[i];
      command_buffers += count;
      if (count) {
        kinds += fmt::format(" {}={}", kCommandBufferKindNames[i], count);
      }
    }
    XELOGI(
        "Metal: frame {}: {:.2f} ms, {:.2f} ms waiting for the GPU{}; {} "
        "command buffer(s){}",
        frame_current_,
        frame_log_start_ns_ ? (now_ns - frame_log_start_ns_) * 1e-6 : 0.0,
        wait_ns * 1e-6, waits, command_buffers, kinds);
  }
  std::fill(std::begin(frame_gpu_wait_ns_), std::end(frame_gpu_wait_ns_), 0);
  std::fill(std::begin(frame_gpu_wait_counts_),
            std::end(frame_gpu_wait_counts_), 0);
  std::copy(std::begin(command_buffer_kind_counts_),
            std::end(command_buffer_kind_counts_),
            command_buffer_kind_frame_start_);
  frame_log_start_ns_ = now_ns;
}

void MetalCommandProcessor::AwaitMemexportSubmission(uint64_t submission) {
  int64_t wait_start_ns = GetSteadyTimeNs();
  // submission_current_ is committed once there's no command buffer open, and
  // completed submission is a front over every earlier one.
  if (submission >= submission_current_ && current_command_buffer_) {
    EndCommandBuffer(CommandBufferKind::kSubmissionWait);
  }
  AwaitSubmissionCompletion(submission);
  NoteGpuWait(GpuWaitReason::kMemexport, wait_start_ns);
}

void MetalCommandProcessor::AwaitAllQueueOperationsCompletion() {
  // Ending first, so a memexport draw that is still only recorded gets
  // submitted rather than waited past.
  EndCommandBuffer(CommandBufferKind::kSubmissionWait);
  AwaitSubmissionCompletion(submission_current_);
}

uint64_t MetalCommandProcessor::AwaitCompletedGpuTimeNs(
    uint64_t* command_buffers) {
  AwaitAllQueueOperationsCompletion();
  WaitForPendingCompletionHandlers();
  *command_buffers = gpu_time_command_buffers_.load(std::memory_order_acquire);
  return completed_gpu_time_ns_.load(std::memory_order_acquire);
}

// ============================================================================
// ZPD (occlusion query) backend overrides.
// ============================================================================

void MetalCommandProcessor::EnsureQueryResources() {
  if (zpd_mode_ == ZPDMode::kFake || !zpd_visibility_pool_) {
    return;
  }
  if (!zpd_visibility_pool_->EnsureInitialized(device_, kQueryPoolCapacity)) {
    // CanOpenQuery gates on the pool, so OpenQuerySegment returns before
    // reaching the base class's own pool-readiness check that arms this. Left
    // unset, every report would resolve to zero samples - fully occluded -
    // instead of falling back to fake counts.
    zpd_force_fake_fallback_ = true;
  }
}

void MetalCommandProcessor::ShutdownQueryResources() {
  if (!zpd_visibility_pool_) {
    return;
  }
  zpd_resolves_in_flight_.clear();
  zpd_active_query_.Reset();
  zpd_visibility_pool_->Shutdown();
}

bool MetalCommandProcessor::IsQueryPoolReady() const {
  return zpd_visibility_pool_ && zpd_visibility_pool_->is_initialized();
}

bool MetalCommandProcessor::CanOpenQuery() const {
  // Metal visibility queries can only be enabled on a render encoder whose
  // descriptor had visibilityResultBuffer set before the encoder was created.
  return current_command_buffer_ != nullptr &&
         current_render_encoder_ != nullptr &&
         render_encoder_has_zpd_visibility_;
}

CommandProcessor::QueryOpenResult MetalCommandProcessor::OpenQuery(
    bool can_close_submission) {
  if (!IsQueryPoolReady()) {
    return QueryOpenResult::kFailed;
  }
  if (!CanOpenQuery()) {
    return QueryOpenResult::kDeferred;
  }

  bool is_pool_exhausted = !zpd_visibility_pool_->has_free_indices();
  if (is_pool_exhausted) {
    PumpQueryResolves();
    is_pool_exhausted = !zpd_visibility_pool_->has_free_indices();
  }

  bool waited_for_submission = false;

  if (is_pool_exhausted) {
    if (zpd_mode_ == ZPDMode::kFast || zpd_mode_ == ZPDMode::kFastAlt) {
      return QueryOpenResult::kPoolExhausted;
    }

    uint64_t wait_for = 0;
    if (!zpd_resolves_in_flight_.empty()) {
      wait_for = zpd_resolves_in_flight_.front().submission;
    }

    uint64_t completed_submission = GetCompletedSubmission();
    if (wait_for > completed_submission) {
      if (wait_for >= GetCurrentSubmission()) {
        // The oldest slot is held by the submission being recorded. Commit it
        // so it can retire, and let the next draw retry the segment.
        if (can_close_submission) {
          EndRenderEncoder();
          EndCommandBuffer(CommandBufferKind::kSubmissionZpdQuery);
        }
        return QueryOpenResult::kDeferred;
      }

      if (cvars::occlusion_query_log) {
        XELOGI("ZPD: Stall awaiting submission={} completed_before={}",
               wait_for, completed_submission);
      }
      int64_t wait_start_ns = GetSteadyTimeNs();
      AwaitSubmissionCompletion(wait_for);
      NoteGpuWait(GpuWaitReason::kZpdQuery, wait_start_ns);
      waited_for_submission = true;
      PumpQueryResolves();
      is_pool_exhausted = !zpd_visibility_pool_->has_free_indices();
    }
  }

  if (is_pool_exhausted) {
    return waited_for_submission ? QueryOpenResult::kPoolExhausted
                                 : QueryOpenResult::kDeferred;
  }

  MetalZPDActiveQuery active_query;
  if (!zpd_visibility_pool_->Acquire(
          active_query.index, active_query.generation, active_query.offset)) {
    return QueryOpenResult::kFailed;
  }

  current_render_encoder_->setVisibilityResultMode(
      MTL::VisibilityResultModeCounting, active_query.offset);
  zpd_active_query_ = active_query;
  return QueryOpenResult::kOpened;
}

bool MetalCommandProcessor::CloseQuery(ReportHandle report_handle,
                                       const VIZQueryHandle& viz,
                                       uint64_t& out_submission) {
  if (!current_render_encoder_ || !render_encoder_has_zpd_visibility_ ||
      !zpd_active_query_.is_open()) {
    return false;
  }

  // Disable at this segment's own offset. The offset passed alongside Disabled
  // is still touched by the pass, so a hardcoded 0 would zero pool slot 0 and
  // make whatever segment owns it resolve as fully occluded.
  current_render_encoder_->setVisibilityResultMode(
      MTL::VisibilityResultModeDisabled, zpd_active_query_.offset);

  MetalZPDResolve resolve;
  resolve.submission = GetCurrentSubmission();
  resolve.index = zpd_active_query_.index;
  resolve.generation = zpd_active_query_.generation;
  resolve.scale_area = GetZPDScaleArea();
  resolve.report_handle = report_handle;
  resolve.viz = viz;
  zpd_resolves_in_flight_.push_back(resolve);

  out_submission = resolve.submission;

  zpd_active_query_.Reset();
  return true;
}

void MetalCommandProcessor::PumpQueryResolves() {
  if (!zpd_visibility_pool_) {
    return;
  }

  uint64_t completed = GetCompletedSubmission();
  if (completed == 0) {
    return;
  }

  while (!zpd_resolves_in_flight_.empty()) {
    if (zpd_resolves_in_flight_.front().submission > completed) {
      break;
    }
    MetalZPDResolve resolve = zpd_resolves_in_flight_.front();
    zpd_resolves_in_flight_.pop_front();

    if (!zpd_visibility_pool_->IsGenerationCurrent(resolve.index,
                                                   resolve.generation)) {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD/Metal: Dropping stale query index={} generation={} handle={}",
            resolve.index, resolve.generation, resolve.report_handle);
      }
      continue;
    }

    uint64_t raw_samples;
    if (!zpd_visibility_pool_->Read(resolve.index, raw_samples)) {
      // An unpreserved result isn't known to be zero. Count one visible sample,
      // the same floor an abandoned report gets, so culling doesn't flash
      // occluded.
      raw_samples = 1;
    }
    zpd_visibility_pool_->Release(resolve.index, resolve.generation);
    if (resolve.report_handle != kInvalidReportHandle) {
      // Metal has no in-shader counter path, so only ZPass is ever counted.
      OnZPDQueryResolved(resolve.report_handle,
                         XenosZPDReport::FromNativeQuery(raw_samples),
                         resolve.scale_area);
    }
    if (resolve.viz.generation != kInvalidVIZGeneration) {
      OnVIZQueryResolved(resolve.viz.id, resolve.viz.generation,
                         raw_samples != 0);
    }
  }
}

bool MetalCommandProcessor::AwaitQueryResolve(ReportHandle report_handle,
                                              uint64_t wait_for_submission) {
  if (zpd_mode_ == ZPDMode::kFake) {
    return false;
  }

  PumpQueryResolves();

  const ZPDReport* report = FindZPDReport(report_handle);
  if (!report || !report->pending_segments) {
    return true;
  }
  if (wait_for_submission == 0) {
    return false;
  }

  // The segment may still be in the submission being recorded, which has to be
  // committed before it can ever complete. A closed command buffer needs no
  // flush: everything recorded so far is already on the queue.
  if (wait_for_submission >= GetCurrentSubmission() &&
      current_command_buffer_) {
    // No wait on the compile threads: a counted draw already resolved its own
    // shaders, and Metal records concrete pipeline states, so a compile in
    // flight for some other draw has no bearing on committing this one.
    EndRenderEncoder();
    EndCommandBuffer(CommandBufferKind::kSubmissionZpdQuery);
  }

  if (wait_for_submission > GetCompletedSubmission()) {
    int64_t wait_start_ns = GetSteadyTimeNs();
    AwaitSubmissionCompletion(wait_for_submission);
    NoteGpuWait(GpuWaitReason::kZpdQuery, wait_start_ns);
  }

  PumpQueryResolves();

  report = FindZPDReport(report_handle);
  return !report || !report->pending_segments;
}

bool MetalCommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  SCOPE_profile_cpu_f("gpu");
  const RegisterFile& regs = *register_file_;
  uint32_t normalized_color_mask = 0;

  // VIZ surveys aren't measured on Metal yet, so every ID stays visible.
  OnVIZSurveyDraw(false);

  // A CPU read arrived before the submission of the resolve it reads.
  if (resolve_submit_wanted_.load(std::memory_order_relaxed)) {
    SubmitWantedResolve();
  }
  // Guest indices (or adaptive tessellation factors in their place) are read
  // on this thread: by the primitive processor, the draw extent estimator and
  // the tessellation factor copy.
  if (index_buffer_info) {
    AwaitGpuWritesTo(index_buffer_info->guest_base,
                     uint32_t(index_buffer_info->length));
  }

  // Check for copy mode
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    if (draw_util::IsVIZSurveyDraw(regs)) {
      return true;
    }
    return IssueCopy();
  }
  // A pass kept open for resolves only ends before anything draws, with the
  // resolve work left to its end.
  if (RenderEncoderResolveOnly()) {
    EndRenderEncoder();
  }

  // The tessellation prep dispatch reads the indices (edge factors if adaptive)
  // ahead of the open command buffer. If that one writes them, commit it, so
  // this draw's dispatch and those of the draws after it can run ahead of the
  // next one, instead of each ending the render pass.
  if (index_buffer_info && shared_memory_ && shared_memory_->hazards() &&
      current_command_buffer_) {
    const auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    if (xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) &&
        regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
            xenos::VGTOutputPath::kTessellationEnable) {
      const uint32_t start = index_buffer_info->guest_base & 0x1FFFFFFF;
      const uint32_t length = uint32_t(index_buffer_info->length);
      if (shared_memory_->hazards()->HasUncommittedWrite(start, length) ||
          render_encoder_shared_memory_hazards_.HasWriteAccess(start, length) ||
          shared_compute_hazards_.HasWriteAccess(start, length)) {
        EndCommandBuffer();
      }
    }
  }

  // Vertex shader analysis — use Shader* base type so both draw paths share
  // this common code.
  Shader* vertex_shader = active_vertex_shader();
  if (!vertex_shader) {
    XELOGW("IssueDraw: No vertex shader");
    return false;
  }
  if (!vertex_shader->is_ucode_analyzed()) {
    vertex_shader->AnalyzeUcode(ucode_disasm_buffer_);
  }
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = active_pixel_shader();
      if (pixel_shader) {
        if (!pixel_shader->is_ucode_analyzed()) {
          pixel_shader->AnalyzeUcode(ucode_disasm_buffer_);
        }
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    if (!memexport_used_vertex) {
      return true;
    }
  }
  bool memexport_used_pixel =
      pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  memexport_ranges_awaited_ =
      IsMemexportAwaited(memexport_used_vertex, memexport_used_pixel);
  const bool pure_memexport_draw =
      IsPureMemexportDraw(regs, is_rasterization_done, memexport_used_vertex,
                          !memexport_ranges_.empty(), pixel_shader != nullptr,
                          index_buffer_info != nullptr);
  // A pure memexport draw is ordered only against what its vertex indices can
  // reach. Auto-indexed guest vertex indices are VGT_INDX_OFFSET plus 0 to
  // below the index count.
  draw_util::VertexIndexRange pure_vertex_indices;
  const draw_util::VertexIndexRange* vertex_indices = nullptr;
  memexport_ordering_ranges_ = memexport_ranges_;
  if (pure_memexport_draw && index_count) {
    pure_vertex_indices.first = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
    if (uint64_t(pure_vertex_indices.first) + index_count <=
        (uint64_t(1) << 24)) {
      pure_vertex_indices.last = pure_vertex_indices.first + index_count - 1;
      vertex_indices = &pure_vertex_indices;
      memexport_ordering_ranges_.clear();
      draw_util::AddMemExportRanges(regs, *vertex_shader,
                                    memexport_ordering_ranges_, vertex_indices);
    }
  }
  // What such a draw can fetch, for ordering later exports of the pass after
  // it. Any other draw is taken to read its whole fetch ranges.
  draw_reads_narrowed_ = false;
  if (vertex_indices && vertex_shader->vertex_fetches_vertex_indexed()) {
    draw_reads_narrowed_ = true;
    draw_narrowed_reads_.clear();
    for (const Shader::VertexFetchStride& stride :
         vertex_shader->vertex_fetch_strides()) {
      const auto fetch = regs.GetVertexFetch(stride.fetch_constant);
      int64_t first =
          std::max(int64_t(vertex_indices->first) * stride.stride_words +
                       vertex_shader->vertex_fetch_min_word_offset(),
                   int64_t(0));
      int64_t end =
          std::min(int64_t(vertex_indices->last) * stride.stride_words +
                       vertex_shader->vertex_fetch_end_word_offset(),
                   int64_t(fetch.size));
      if (first < end) {
        uint64_t start =
            uint64_t(xenos::CpuToGpu(fetch.address << 2)) + (first << 2);
        draw_narrowed_reads_.emplace_back(
            uint32_t(std::min(start, uint64_t(SharedMemory::kBufferSize))),
            uint32_t(std::min(start + (uint64_t(end - first) << 2),
                              uint64_t(SharedMemory::kBufferSize))));
      }
    }
  }
  // Tile resolves earlier in the pass may not have run in every tile yet.
  if (current_render_encoder_ && !render_encoder_tile_resolve_ranges_.empty() &&
      DrawOverlapsPendingWrites(render_encoder_tile_resolve_ranges_,
                                *vertex_shader, pixel_shader, index_buffer_info,
                                vertex_indices)) {
    EndRenderEncoder();
  }
  render_encoder_index_count_ += index_count;
  // Fences written while the pass is open are only stored when it ends.
  if (current_render_encoder_ && !pending_shader_done_fence_ranges_.empty() &&
      DrawOverlapsPendingWrites(pending_shader_done_fence_ranges_,
                                *vertex_shader, pixel_shader, index_buffer_info,
                                vertex_indices)) {
    EndRenderEncoder();
  }
  bool memexport_barrier_encoded = false;
  if (current_render_encoder_ &&
      DrawOverlapsPendingWrites(render_encoder_memexport_ranges_,
                                *vertex_shader, pixel_shader, index_buffer_info,
                                vertex_indices)) {
    // Order the export before this draw's accesses. Within a chain of pure
    // vertex memexport draws, which can only overlap through vertex fetches
    // and exports, a fresh vertex-to-vertex buffer memory barrier does; any
    // other draw ends the pass.
    if (pure_memexport_draw && render_encoder_memexport_draws_are_pure_) {
      current_render_encoder_->memoryBarrier(MTL::BarrierScopeBuffers,
                                             MTL::RenderStageVertex,
                                             MTL::RenderStageVertex);
      memexport_barrier_encoded = true;
    } else {
      EndRenderEncoder();
    }
  }
  // And the export after the earlier draws of the pass that read what it
  // writes: the shared memory buffer is not hazard-tracked, and without this
  // the vertex stage of this draw may run before theirs. A pure memexport draw
  // writes in the vertex stage, so the vertex-to-vertex barrier orders it
  // after every earlier fetch but a pixel shader's.
  if (current_render_encoder_ && !memexport_ordering_ranges_.empty() &&
      DrawExportsOverlapPassReads()) {
    if (pure_memexport_draw && !render_encoder_pixel_stage_fetches_) {
      if (!memexport_barrier_encoded) {
        current_render_encoder_->memoryBarrier(MTL::BarrierScopeBuffers,
                                               MTL::RenderStageVertex,
                                               MTL::RenderStageVertex);
      }
    } else {
      EndRenderEncoder();
    }
  }
  // Primitive/index processing (like D3D12/Vulkan).
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_) {
    XELOGE("IssueDraw: primitive processor is not initialized");
    return false;
  }
  // Converted indices are tagged with the current frame, which must be open.
  if (!EnsureCommandBuffer()) {
    return true;
  }
  if (!primitive_processor_->Process(primitive_processing_result)) {
    XELOGE("IssueDraw: primitive processing failed");
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }
  pipeline_cache_->UpdateDrawExactness(*vertex_shader,
                                       primitive_processing_result);
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kMemExportCompute) {
    primitive_processing_result.host_vertex_shader_type =
        Shader::HostVertexShaderType::kVertex;
  }

  // Configure render targets via MetalRenderTargetCache, similar to D3D12.
  draw_stand_in_wait_reason_ = nullptr;
  if (render_target_cache_) {
    auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
    uint32_t ps_writes_color_targets =
        pixel_shader ? pixel_shader->writes_color_targets() : 0;
    normalized_color_mask = pixel_shader ? draw_util::GetNormalizedColorMask(
                                               regs, ps_writes_color_targets)
                                         : 0;
    // Strip merge (strip_merge.h): draws on the strip surface either address
    // the frame (the strip's window, extended to the frame when merging) or
    // the strip's band itself, which the merged first strip replicates into
    // every band.
    strip_merge_replicate_draw_ = false;
    strip_merge_first_band_only_ = false;
    bool strip_merge_keep_carries = false;
    if (strip_merge_.recording()) {
      StripMerge::DrawInfo strip_merge_draw;
      strip_merge_draw.vertex_shader = vertex_shader;
      strip_merge_draw.pixel_shader = pixel_shader;
      strip_merge_draw.banded =
          is_rasterization_done && strip_merge_.IsSurfaceBanded();
      strip_merge_draw.frame_space = strip_merge_.IsFrameSpace();
      strip_merge_draw.memexport = memexport_used;
      strip_merge_draw.pixel_position =
          pixel_shader && regs.Get<reg::SQ_PROGRAM_CNTL>().param_gen;
      strip_merge_draw.tessellated =
          primitive_processing_result.IsTessellated();
      strip_merge_.CheckDraw(strip_merge_draw);
      if (strip_merge_.merging() && !strip_merge_.failed()) {
        // A draw only the first strip does goes into its band alone, as
        // that strip draws it.
        strip_merge_first_band_only_ = strip_merge_.first_strip_only_draw();
        strip_merge_keep_carries =
            strip_merge_first_band_only_ && !strip_merge_draw.banded;
        strip_merge_replicate_draw_ =
            strip_merge_draw.banded &&
            (!strip_merge_draw.frame_space || strip_merge_first_band_only_);
      }
    }
    {
      MetalRenderTargetCache::ClearQuadDraw clear_quad_draw;
      clear_quad_draw.candidate =
          is_rasterization_done && !memexport_used &&
          primitive_type == xenos::PrimitiveType::kRectangleList &&
          !primitive_processing_result.IsTessellated();
      clear_quad_draw.pass_begin =
          !current_render_encoder_ || RenderEncoderResolveOnly();
      clear_quad_draw.has_pixel_shader = pixel_shader != nullptr;
      clear_quad_draw.pixel_shader_kills =
          pixel_shader && pixel_shader->kills_pixels();
      // A merged draw in frame space covers the merged render targets' rows
      // as it does the frame's. One addressing the band is drawn in every
      // band, or in the first alone if only the first strip draws it
      // (DrawStripMergeBands).
      if (strip_merge_replicate_draw_) {
        clear_quad_draw.band_count = strip_merge_first_band_only_
                                         ? 1
                                         : strip_merge_.layout().strip_count;
        clear_quad_draw.band_rows = strip_merge_.layout().band_rows;
      }
      render_target_cache_->SetClearQuadDraw(clear_quad_draw);
      render_target_cache_->SetDepthClearRetargetCandidate(
          clear_quad_draw.candidate && !pixel_shader &&
          !strip_merge_replicate_draw_ && !draw_util::IsVIZSurveyDraw(regs));
    }
    // A draw that writes no color, depth or stencil, exports nothing to
    // memory and is counted by no query has no effect; issuing it would only
    // move the ownership of the EDRAM tiles its surface covers.
    if (::cvars::skip_no_effect_draws && is_rasterization_done &&
        !memexport_used && !normalized_color_mask &&
        !draw_util::IsVIZSurveyDraw(regs) && !IsDrawCountedByQuery() &&
        !RenderTargetCache::DoesDepthControlWrite(
            normalized_depth_control, regs.Get<reg::RB_STENCILREFMASK>(),
            regs.Get<reg::RB_STENCILREFMASK>(
                XE_GPU_REG_RB_STENCILREFMASK_BF))) {
      // Drawn, it would have been a draw of the open pass that is not a pure
      // vertex memexport draw: the next export read in that pass then ends
      // it instead of chaining behind a vertex stage memory barrier. Keep
      // that, or every memexport draw of the frame chains in one pass.
      if (current_render_encoder_) {
        render_encoder_memexport_draws_are_pure_ = false;
      }
      render_target_cache_->SetClearQuadDraw(
          MetalRenderTargetCache::ClearQuadDraw());
      render_target_cache_->SetDepthClearRetargetCandidate(false);
      return true;
    }
    // The window offset stays in the viewport on Metal.
    render_target_cache_->SetStripMergeKeepCarries(strip_merge_keep_carries);
    if (!render_target_cache_->Update(
            is_rasterization_done, normalized_depth_control,
            normalized_color_mask, *vertex_shader, 0)) {
      XELOGE(
          "MetalCommandProcessor::IssueDraw - RenderTargetCache::Update "
          "failed");
      return false;
    }
    if (render_target_cache_->last_update_depth_clear_retargeted()) {
      // The depth clear went into the 1x owner of the tiles
      // (depth_clear_retarget_4x): nothing is left of the draw.
      return true;
    }
    // A stand-in (the placeholder, or skipping the draw) only suits a pass
    // redrawn every frame. A one-off render to a texture or generated data in
    // a small render target waits for its real pipeline, as on D3D12 and
    // Vulkan. Metal has no storage warm-up to wait out.
    draw_stand_in_wait_reason_ =
        render_target_cache_->GetPipelineStandInWaitReason(
            frame_current_, *vertex_shader, pixel_shader, memexport_used_vertex,
            memexport_used_pixel, false);
    // What a stand-in may leave behind is missing color in a pass redrawn
    // every frame, nothing else. Two kinds of draw leave more, and wait:
    // - A clear quad the render target cache relies on: transfers were cut
    //   around it, or a load is skipped for it, so its rectangle would keep
    //   stale or undefined contents for good.
    // - A draw whose pixel shader decides which samples get depth or stencil
    //   (kill, alpha test, alpha to coverage, depth output): the placeholder
    //   has no pixel shader and would write depth and stencil the draw
    //   doesn't, rejecting later draws and staying in what the game keeps.
    const char* stand_in_policy_reason = nullptr;
    if (render_target_cache_->update_relies_on_quad_draw()) {
      stand_in_policy_reason = "clear quad";
    } else if (cvars::metal_stand_in_exact_depth_stencil &&
               is_rasterization_done && pixel_shader) {
      const reg::RB_DEPTHCONTROL dc = normalized_depth_control;
      const auto keep = xenos::StencilOp::kKeep;
      const bool stencil_written =
          dc.stencil_enable &&
          (dc.stencilfail != keep || dc.stencilzpass != keep ||
           dc.stencilzfail != keep ||
           (dc.backface_enable &&
            (dc.stencilfail_bf != keep || dc.stencilzpass_bf != keep ||
             dc.stencilzfail_bf != keep)));
      const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
      if (((dc.z_enable && dc.z_write_enable) || stencil_written) &&
          (pixel_shader->kills_pixels() || pixel_shader->writes_depth() ||
           color_control.alpha_to_mask_enable ||
           (color_control.alpha_test_enable &&
            color_control.alpha_func != xenos::CompareFunction::kAlways))) {
        stand_in_policy_reason = "pixel shader decides depth/stencil";
      }
    }
    if (!draw_stand_in_wait_reason_) {
      draw_stand_in_wait_reason_ = stand_in_policy_reason;
    }
  }

  // Pipeline formats can be derived from the cache without opening a render
  // encoder. Pending attachment transfers stay queued for the head of the
  // eventual pass while independent texture and shared-memory uploads are
  // encoded first. First-use clears still keep the encoder eager. Queries are
  // opened around guest draws, not around the preceding ownership transfers.
  if (current_render_encoder_ && render_target_cache_ &&
      render_target_cache_->PendingDrawPassTransfersPrepareable() &&
      !render_target_cache_->IsRenderPassDescriptorCompatible(
          current_render_pass_descriptor_, 1)) {
    EndRenderEncoder();
  }
  CommitBetweenRenderPasses();
  const bool initial_draw_encoder_deferred = CanDeferEmptyDrawEncoder();
  if (initial_draw_encoder_deferred) {
    EnsureCommandBuffer();
  } else {
    BeginCommandBuffer();
  }
  if (!current_command_buffer_ ||
      (!current_render_encoder_ && !initial_draw_encoder_deferred)) {
    static bool spirv_no_command_buffer_logged = false;
    if (!spirv_no_command_buffer_logged) {
      spirv_no_command_buffer_logged = true;
      XELOGE(
          "IssueDraw: failed to begin Metal command buffer/render encoder; "
          "skipping draw");
    }
    return true;
  }

  bool ok = IssueDraw(vertex_shader, pixel_shader, primitive_processing_result,
                      primitive_polygonal, memexport_used, pure_memexport_draw,
                      normalized_color_mask, regs);
  // A pending shader or failed upload may abandon the draw. Preserve the
  // eager path's clear and ownership writes before the next packet changes
  // registers or observes EDRAM. Successful draws already opened the pass.
  if (initial_draw_encoder_deferred && !current_render_encoder_) {
    BeginCommandBuffer();
    ok &= current_render_encoder_ != nullptr;
  }
  return ok;
}

bool MetalCommandProcessor::RequestDrawSharedMemoryRanges(
    const Shader& vertex_shader, const RegisterFile& regs) {
  SCOPE_profile_cpu_f("gpu");
  draw_shared_memory_reads_.clear();
  if (!shared_memory_) {
    return true;
  }
  const Shader::ConstantRegisterMap& constant_map_vertex =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map_vertex.vertex_fetch_bitmap);
       ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(vfetch_index);
      switch (vfetch.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (::cvars::gpu_allow_invalid_fetch_constants) {
            break;
          }
          XELOGW(
              "Metal: Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" "
              "type. Use --gpu_allow_invalid_fetch_constants to bypass.",
              vfetch_index, vfetch.dword_0, vfetch.dword_1);
          return false;
        default:
          XELOGW("Metal: Vertex fetch constant {} ({:08X} {:08X}) is invalid.",
                 vfetch_index, vfetch.dword_0, vfetch.dword_1);
          return false;
      }
      // Mask to physical like the shader - the guest may use a mirror window.
      uint32_t buffer_offset = xenos::CpuToGpu(vfetch.address << 2);
      uint32_t buffer_length = vfetch.size << 2;
      if (buffer_offset > SharedMemory::kBufferSize ||
          SharedMemory::kBufferSize - buffer_offset < buffer_length) {
        XELOGW(
            "Metal: Vertex fetch constant {} out of range (offset=0x{:08X} "
            "size={})",
            vfetch_index, buffer_offset, buffer_length);
        return false;
      }
      if (!shared_memory_->RequestRangeForGpuRead(buffer_offset,
                                                  buffer_length)) {
        XELOGE(
            "Metal: Failed to request vertex buffer at 0x{:08X} (size {}) in "
            "shared memory",
            buffer_offset, buffer_length);
        return false;
      }
      draw_shared_memory_reads_.emplace_back(buffer_offset, buffer_length);
    }
  }

  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t base_bytes = memexport_range.base_address_dwords << 2;
    if (!shared_memory_->RequestRange(base_bytes, memexport_range.size_bytes)) {
      XELOGE(
          "Metal: Failed to request memexport stream at 0x{:08X} (size {}) in "
          "shared memory",
          base_bytes, memexport_range.size_bytes);
      return false;
    }
  }

  return true;
}

void MetalCommandProcessor::OrderDrawSharedMemoryAccesses(
    const Shader* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    MTL::Buffer* index_buffer, uint64_t index_buffer_offset,
    uint64_t index_buffer_size, const RegisterFile& regs) {
  if (!render_encoder_shared_memory_hazards_.is_active()) {
    return;
  }
  for (const std::pair<uint32_t, uint32_t>& range : draw_shared_memory_reads_) {
    render_encoder_shared_memory_hazards_.Access(range.first, range.second,
                                                 false, draw_reads_narrowed_);
  }
  if (draw_reads_narrowed_) {
    for (const std::pair<uint32_t, uint32_t>& read : draw_narrowed_reads_) {
      // Consecutive draws of a chain usually continue the same range.
      if (!render_encoder_narrowed_reads_.empty() &&
          render_encoder_narrowed_reads_.back().first <= read.second &&
          read.first <= render_encoder_narrowed_reads_.back().second) {
        auto& last = render_encoder_narrowed_reads_.back();
        last.first = std::min(last.first, read.first);
        last.second = std::max(last.second, read.second);
      } else {
        render_encoder_narrowed_reads_.push_back(read);
      }
    }
  }
  // Vertex fetches in the pixel shader aren't requested with the vertex
  // shader's ones, but they still read the buffer.
  if (pixel_shader) {
    const Shader::ConstantRegisterMap& constant_map_pixel =
        pixel_shader->constant_register_map();
    for (uint32_t i = 0;
         i < xe::countof(constant_map_pixel.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining =
          constant_map_pixel.vertex_fetch_bitmap[i];
      uint32_t j;
      while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining &= ~(uint32_t(1) << j);
        xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(i * 32 + j);
        render_encoder_pixel_stage_fetches_ = true;
        render_encoder_shared_memory_hazards_.Access(
            xenos::CpuToGpu(vfetch.address << 2), vfetch.size << 2, false);
      }
    }
  }
  if (index_buffer && index_buffer == shared_memory_->GetBuffer() &&
      index_buffer_size) {
    render_encoder_shared_memory_hazards_.Access(
        uint32_t(index_buffer_offset),
        uint32_t(
            std::min(index_buffer_size, uint64_t(SharedMemory::kBufferSize))),
        false);
  }
  // Vertex shaders expanding primitives load the guest indices themselves.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    render_encoder_shared_memory_hazards_.Access(
        primitive_processing_result.guest_index_base,
        uint32_t(std::min(
            uint64_t(primitive_processing_result.guest_draw_vertex_count) *
                (regs.Get<reg::VGT_DRAW_INITIATOR>().index_size ==
                         xenos::IndexFormat::kInt32
                     ? sizeof(uint32_t)
                     : sizeof(uint16_t)),
            uint64_t(SharedMemory::kBufferSize))),
        false);
  }
  // Ordering the exports within the pass is up to the draw's own checks
  // (DrawOverlapsPendingWrites, DrawExportsOverlapPassReads), so the result
  // is not used.
  for (const draw_util::MemExportRange& range : memexport_ranges_) {
    render_encoder_shared_memory_hazards_.Access(range.base_address_dwords << 2,
                                                 range.size_bytes, true);
  }
}

void MetalCommandProcessor::ComputeDrawViewportInfo(
    const RegisterFile& regs, const Shader* pixel_shader,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    draw_util::ViewportInfo& viewport_info_out) {
  constexpr uint32_t kViewportBoundsMax = 32767;
  bool convert_z_to_float24 = ::cvars::depth_float24_convert_in_pixel_shader;
  // ZPD segments can't mix scales. The resolved sample count is divided by one
  // scale area per segment, so a change splits the segment.
  // Metal has no in-shader counter path, so a segment never counts Total.
  UpdateQuerySegment(
      (texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1) *
          (texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1),
      false, draw_util::IsVIZSurveyDraw(regs));
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1,
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1,
      texture_cache_ ? texture_cache_->draw_resolution_scale_x_divisor()
                     : divisors::MagicDiv(1),
      texture_cache_ ? texture_cache_->draw_resolution_scale_y_divisor()
                     : divisors::MagicDiv(1),
      true, kViewportBoundsMax, kViewportBoundsMax, false,
      normalized_depth_control, convert_z_to_float24, true,
      pixel_shader && pixel_shader->writes_depth(), false);
  gviargs.SetupRegisterValues(regs, false);
  if (gviargs == previous_viewport_info_args_) {
    viewport_info_out = previous_viewport_info_;
  } else {
    draw_util::GetHostViewportInfo(&gviargs, viewport_info_out);
    previous_viewport_info_args_ = gviargs;
    previous_viewport_info_ = viewport_info_out;
  }
}

uint32_t MetalCommandProcessor::GetDrawScissorBottom() const {
  if (!register_file_) {
    return 0;
  }
  const RegisterFile& regs = *register_file_;
  if (!draw_util::IsRasterizationPotentiallyDone(
          regs, draw_util::IsPrimitivePolygonal(regs))) {
    return 0;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  return (scissor.offset[1] + scissor.extent[1]) * draw_resolution_scale_y;
}

void MetalCommandProcessor::ApplyViewportAndScissor(
    const RegisterFile& regs, const draw_util::ViewportInfo& viewport_info) {
  SCOPE_profile_cpu_f("gpu");
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  // Clamp scissor to actual render target bounds (Metal requires this).
  ClampScissorToBounds(scissor, render_encoder_width_, render_encoder_height_);
  // Draws that don't rasterize (memexport-only) still run the vertex shader,
  // but must not touch the retained attachments or contribute samples.
  if (!draw_util::IsRasterizationPotentiallyDone(
          regs, draw_util::IsPrimitivePolygonal(regs))) {
    scissor = {};
  }

  MTL::Viewport mtl_viewport;
  mtl_viewport.originX = static_cast<double>(viewport_info.xy_offset[0]);
  mtl_viewport.originY = static_cast<double>(viewport_info.xy_offset[1]);
  mtl_viewport.width = static_cast<double>(viewport_info.xy_extent[0]);
  mtl_viewport.height = static_cast<double>(viewport_info.xy_extent[1]);
  mtl_viewport.znear = viewport_info.z_min;
  mtl_viewport.zfar = viewport_info.z_max;
  if (!msl_viewport_valid_ ||
      std::memcmp(&msl_viewport_, &mtl_viewport, sizeof(mtl_viewport)) != 0) {
    current_render_encoder_->setViewport(mtl_viewport);
    msl_viewport_ = mtl_viewport;
    msl_viewport_valid_ = true;
  }

  MTL::ScissorRect mtl_scissor;
  mtl_scissor.x = scissor.offset[0];
  mtl_scissor.y = scissor.offset[1];
  mtl_scissor.width = scissor.extent[0];
  mtl_scissor.height = scissor.extent[1];
  if (!msl_scissor_valid_ || msl_scissor_.x != mtl_scissor.x ||
      msl_scissor_.y != mtl_scissor.y ||
      msl_scissor_.width != mtl_scissor.width ||
      msl_scissor_.height != mtl_scissor.height) {
    current_render_encoder_->setScissorRect(mtl_scissor);
    msl_scissor_ = mtl_scissor;
    msl_scissor_valid_ = true;
  }
}

bool MetalCommandProcessor::CanDeferEmptyDrawEncoder() {
  if (current_render_encoder_ || !render_target_cache_) {
    return false;
  }
  // This only derives a descriptor. Clear consumption remains in the final
  // BeginCommandBuffer, and any pending clear keeps the original ordering.
  auto* desc = render_target_cache_->GetRenderPassDescriptor(1, true);
  if (!desc) {
    return false;
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* a = desc->colorAttachments()->object(i);
    if (a->texture() && a->loadAction() == MTL::LoadActionClear) {
      return false;
    }
  }
  if (desc->depthAttachment()->texture() &&
      desc->depthAttachment()->loadAction() == MTL::LoadActionClear) {
    return false;
  }
  if (desc->stencilAttachment()->texture() &&
      desc->stencilAttachment()->loadAction() == MTL::LoadActionClear) {
    return false;
  }
  return true;
}

bool MetalCommandProcessor::PrepareDrawTextures(uint32_t used_texture_mask,
                                                const RegisterFile& regs) {
  SCOPE_profile_cpu_f("gpu");
  if (texture_cache_ && used_texture_mask &&
      texture_cache_->AnyUsedTextureRequestWorkPending(used_texture_mask)) {
    texture_cache_->RequestTextures(used_texture_mask);
  }
  return true;
}

void MetalCommandProcessor::UpdateGuestConstantCaches(
    const Shader* vertex_shader, const Shader* pixel_shader,
    const RegisterFile& regs) {
  SCOPE_profile_cpu_f("gpu");
  // SpirvShaderTranslator uses packed float constants like Vulkan, so a change
  // in which constants a shader uses changes the packing.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < 4; ++i) {
    if (msl_current_float_constant_map_vertex_[i] !=
        float_constant_map_vertex.float_bitmap[i]) {
      msl_current_float_constant_map_vertex_[i] =
          float_constant_map_vertex.float_bitmap[i];
      msl_float_constants_dirty_vertex_ = true;
    }
  }
  if (pixel_shader) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      if (msl_current_float_constant_map_pixel_[i] !=
          float_constant_map_pixel.float_bitmap[i]) {
        msl_current_float_constant_map_pixel_[i] =
            float_constant_map_pixel.float_bitmap[i];
        msl_float_constants_dirty_pixel_ = true;
      }
    }
  } else {
    for (uint32_t i = 0; i < 4; ++i) {
      if (msl_current_float_constant_map_pixel_[i] != 0) {
        msl_current_float_constant_map_pixel_[i] = 0;
        msl_float_constants_dirty_pixel_ = true;
      }
    }
  }

  auto rebuild_packed_float_constants = [&](std::array<uint8_t, kCbvSizeBytes>&
                                                dst,
                                            const Shader* shader,
                                            uint32_t regs_base) {
    // Bind only the shader's declared packed constants.
    const uint32_t float_count =
        shader ? shader->constant_register_map().float_count : 0;
    uint32_t bound_bytes = std::min(float_count * 16u, uint32_t(kCbvSizeBytes));
    uint8_t* out = dst.data();
    uint8_t* const end = dst.data() + bound_bytes;
    if (shader && shader->constant_register_map().float_count) {
      const Shader::ConstantRegisterMap& map = shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t bits = map.float_bitmap[i];
        uint32_t constant_index;
        while (out != end && xe::bit_scan_forward(bits, &constant_index)) {
          // A run of used constants is contiguous in the registers and in
          // the packed buffer alike, so it is copied at once.
          uint32_t run_length;
          if (!xe::bit_scan_forward(~(bits >> constant_index), &run_length)) {
            run_length = 64;
          }
          run_length = std::min(
              run_length, uint32_t(size_t(end - out) / (4 * sizeof(uint32_t))));
          bits = constant_index + run_length < 64
                     ? bits & (~uint64_t(0) << (constant_index + run_length))
                     : 0;
          std::memcpy(
              out, &regs.values[regs_base + (i << 8) + (constant_index << 2)],
              run_length * 4 * sizeof(uint32_t));
          out += run_length * 4 * sizeof(uint32_t);
        }
      }
    }
    // Every gathered value replaces its previous contents. Only padding
    // visible to this shader needs clearing after the gather.
    std::memset(out, 0, size_t(end - out));
  };
  if (msl_float_constants_dirty_vertex_) {
    rebuild_packed_float_constants(msl_cached_float_constants_vertex_,
                                   vertex_shader,
                                   XE_GPU_REG_SHADER_CONSTANT_000_X);
    ++guest_constant_versions_[0];
    msl_float_constants_dirty_vertex_ = false;
  }
  if (msl_float_constants_dirty_pixel_) {
    rebuild_packed_float_constants(msl_cached_float_constants_pixel_,
                                   pixel_shader,
                                   XE_GPU_REG_SHADER_CONSTANT_256_X);
    ++guest_constant_versions_[1];
    msl_float_constants_dirty_pixel_ = false;
  }

  if (msl_bool_loop_constants_dirty_) {
    std::memcpy(msl_cached_bool_loop_constants_.data(),
                &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    ++guest_constant_versions_[2];
    msl_bool_loop_constants_dirty_ = false;
  }
  if (msl_fetch_constants_dirty_) {
    // The version names the contents, so writes that left them as they were
    // keep the uploaded copy.
    const uint32_t* fetch_constants =
        &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
    if (std::memcmp(msl_cached_fetch_constants_.data(), fetch_constants,
                    kFetchConstantsSize)) {
      std::memcpy(msl_cached_fetch_constants_.data(), fetch_constants,
                  kFetchConstantsSize);
      ++guest_constant_versions_[3];
    }
    msl_fetch_constants_dirty_ = false;
  }
}

bool MetalCommandProcessor::ResolveDrawIndexBuffer(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    Shader::HostVertexShaderType host_vertex_shader_type, bool tessellated,
    DrawIndexBuffer& index_buffer_out) {
  index_buffer_out = DrawIndexBuffer();
  // A tessellated draw feeds patches to the tessellator, so its host primitive
  // type is not one of the rasterizer topologies below.
  switch (tessellated ? xenos::PrimitiveType::kTriangleList
                      : primitive_processing_result.host_primitive_type) {
    case xenos::PrimitiveType::kPointList:
      index_buffer_out.primitive_type = MTL::PrimitiveTypePoint;
      break;
    case xenos::PrimitiveType::kLineList:
      index_buffer_out.primitive_type = MTL::PrimitiveTypeLine;
      break;
    case xenos::PrimitiveType::kLineStrip:
      index_buffer_out.primitive_type = MTL::PrimitiveTypeLineStrip;
      break;
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kRectangleList:
      index_buffer_out.primitive_type = MTL::PrimitiveTypeTriangle;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      index_buffer_out.primitive_type = MTL::PrimitiveTypeTriangleStrip;
      break;
    default:
      XELOGE("Metal: Unsupported host primitive type {}",
             uint32_t(primitive_processing_result.host_primitive_type));
      return false;
  }

  // PrimitiveProcessor::Process already requested the guest index range, before
  // the draw's render encoder was resumed. Requesting it again here could
  // encode an upload into the command buffer after the draw's state was bound.
  auto guest_index_range_in_bounds = [&](uint64_t index_base,
                                         uint32_t index_count,
                                         MTL::IndexType index_type) -> bool {
    uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                ? sizeof(uint16_t)
                                : sizeof(uint32_t);
    uint64_t index_length = uint64_t(index_count) * index_stride;
    return shared_memory_ && index_base <= SharedMemory::kBufferSize &&
           SharedMemory::kBufferSize - index_base >= index_length;
  };

  bool use_expansion_triangle_list_fallback = false;
  index_buffer_out.index_count =
      primitive_processing_result.host_draw_vertex_count;
  if ((host_vertex_shader_type ==
           Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
       host_vertex_shader_type ==
           Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) &&
      (primitive_processing_result.index_buffer_type ==
           PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto ||
       primitive_processing_result.index_buffer_type ==
           PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA)) {
    // Expansion strips normally rely on primitive restart separators. Keep a
    // Metal-local triangle-list fallback to avoid dependence on strip restart
    // behavior.
    uint32_t strip_index_count = index_buffer_out.index_count;
    uint32_t expanded_primitive_count =
        strip_index_count ? (strip_index_count + 1u) / 5u : 0u;
    index_buffer_out.index_count = expanded_primitive_count * 6u;
    index_buffer_out.primitive_type = MTL::PrimitiveTypeTriangle;
    use_expansion_triangle_list_fallback = true;
    static bool logged_expansion_triangle_list_fallback = false;
    if (!logged_expansion_triangle_list_fallback) {
      logged_expansion_triangle_list_fallback = true;
      XELOGW(
          "Metal: Using triangle-list fallback for VS primitive expansion "
          "draws to avoid strip-restart dependency");
    }
  }

  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    return true;
  }

  index_buffer_out.indexed = true;
  index_buffer_out.index_type =
      (primitive_processing_result.host_index_format ==
       xenos::IndexFormat::kInt16)
          ? MTL::IndexTypeUInt16
          : MTL::IndexTypeUInt32;
  switch (primitive_processing_result.index_buffer_type) {
    case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
      index_buffer_out.buffer =
          shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
      index_buffer_out.offset = primitive_processing_result.guest_index_base;
      if (!guest_index_range_in_bounds(index_buffer_out.offset,
                                       index_buffer_out.index_count,
                                       index_buffer_out.index_type)) {
        XELOGE("Metal: Failed to validate guest index buffer range");
        return false;
      }
      break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
      if (primitive_processor_) {
        index_buffer_out.buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle,
            index_buffer_out.offset);
      }
      break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
      if (primitive_processor_) {
        if (use_expansion_triangle_list_fallback) {
          index_buffer_out.buffer =
              primitive_processor_->GetExpansionTriangleListIndexBuffer();
          index_buffer_out.offset = 0;
          index_buffer_out.index_type = MTL::IndexTypeUInt32;
        } else {
          index_buffer_out.buffer =
              primitive_processor_->GetBuiltinIndexBuffer();
          index_buffer_out.offset =
              primitive_processing_result.host_index_buffer_handle;
        }
      }
      break;
    default:
      XELOGE("Metal: Unsupported index buffer type {}",
             uint32_t(primitive_processing_result.index_buffer_type));
      return false;
  }
  if (!index_buffer_out.buffer) {
    XELOGE("Metal: Index buffer is null");
    return false;
  }
  return true;
}

// ==========================================================================
// SPIR-V -> AIR draw path
// ==========================================================================

bool MetalCommandProcessor::EnsureTessellationPrepPipeline() {
  if (tess_prep_pipeline_) {
    return true;
  }
  if (!device_) {
    return false;
  }
  NS::Error* error = nullptr;
  dispatch_data_t data =
      dispatch_data_create(metal_tessellation_prep_cs_metallib,
                           sizeof(metal_tessellation_prep_cs_metallib), nullptr,
                           DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* lib = device_->newLibrary(data, &error);
  dispatch_release(data);
  if (!lib) {
    XELOGE("Metal: failed to load tessellation prep library: {}",
           error ? error->localizedDescription()->utf8String() : "unknown");
    return false;
  }
  MTL::Function* fn =
      lib->newFunction(NS::String::string("entry_xe", NS::UTF8StringEncoding));
  if (!fn) {
    XELOGE("Metal: tessellation prep library has no 'entry_xe' function");
    lib->release();
    return false;
  }
  tess_prep_pipeline_ = device_->newComputePipelineState(fn, &error);
  fn->release();
  lib->release();
  if (!tess_prep_pipeline_) {
    XELOGE("Metal: failed to create tessellation prep pipeline: {}",
           error ? error->localizedDescription()->utf8String() : "unknown");
    return false;
  }
  return true;
}

void MetalCommandProcessor::EncodeTessPrepDispatch(
    MTL::ComputeCommandEncoder* encoder, const TessPrepDispatch& dispatch) {
  encoder->setComputePipelineState(tess_prep_pipeline_);
  encoder->setBytes(&dispatch.constants, sizeof(dispatch.constants), 0);
  encoder->setBuffer(dispatch.cp_buffer, dispatch.cp_offset, 1);
  encoder->setBuffer(dispatch.src_buffer, dispatch.src_offset, 2);
  encoder->setBuffer(dispatch.factor_buffer, dispatch.factor_offset, 3);
  const NS::UInteger groups = (dispatch.constants.count + 63u) / 64u;
  encoder->dispatchThreadgroups(MTL::Size::Make(groups, 1, 1),
                                MTL::Size::Make(64, 1, 1));
}

void MetalCommandProcessor::CommitTessPrepAhead() {
  if (tess_prep_ahead_.empty()) {
    return;
  }
  if (!EnsureTessellationPrepPipeline()) {
    tess_prep_ahead_.clear();
    return;
  }
  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
  MTL::CommandBuffer* cmd =
      CreateAccountedCommandBuffer(CommandBufferKind::kTessellationPrep);
  if (!cmd) {
    XELOGE("Metal: failed to create pre-pass command buffer for tess prep");
    tess_prep_ahead_.clear();
    pool->release();
    return;
  }
  // Every dispatch writes its own ranges and none reads another's output.
  MTL::ComputeCommandEncoder* encoder =
      cmd->computeCommandEncoder(MTL::DispatchTypeConcurrent);
  if (!encoder) {
    XELOGE("Metal: failed to create pre-pass compute encoder for tess prep");
    DiscardAccountedCommandBuffer(cmd, CommandBufferKind::kTessellationPrep);
    tess_prep_ahead_.clear();
    pool->release();
    return;
  }
  MetalBufferHazards::Encoder hazards;
  if (shared_memory_ && shared_memory_->hazards()) {
    hazards.Begin(*shared_memory_->hazards(), cmd, encoder);
  }
  for (const TessPrepDispatch& dispatch : tess_prep_ahead_) {
    if (dispatch.reads_shared_memory && hazards.is_active()) {
      hazards.Access(dispatch.shared_memory_start,
                     dispatch.shared_memory_length, false);
    }
    EncodeTessPrepDispatch(encoder, dispatch);
  }
  if (hazards.is_active()) {
    hazards.End();
  }
  encoder->endEncoding();
  cmd->commit();
  tess_prep_ahead_.clear();
  pool->release();
}

bool MetalCommandProcessor::PrepareNativeTessellationDraw(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    const DrawIndexBuffer& draw_index_buffer, const RegisterFile& regs,
    NativeTessellationDraw& tess_out) {
  tess_out = {};
  if (!EnsureTessellationPrepPipeline() || !null_buffer_) {
    return false;
  }

  using HVS = Shader::HostVertexShaderType;
  const HVS hvs = primitive_processing_result.host_vertex_shader_type;
  const bool is_quad =
      hvs == HVS::kQuadDomainCPIndexed || hvs == HVS::kQuadDomainPatchIndexed;
  const bool is_cp_indexed =
      hvs == HVS::kTriangleDomainCPIndexed || hvs == HVS::kQuadDomainCPIndexed;
  const bool is_adaptive = primitive_processing_result.tessellation_mode ==
                           xenos::TessellationMode::kAdaptive;
  const uint32_t cp_per_patch = is_cp_indexed ? (is_quad ? 4u : 3u) : 1u;
  const uint32_t input_elements_per_patch =
      is_adaptive ? (is_quad ? 4u : 3u) : cp_per_patch;
  const uint32_t patch_count =
      draw_index_buffer.index_count / input_elements_per_patch;
  if (!patch_count) {
    return true;
  }

  const uint32_t total_control_points =
      is_adaptive ? patch_count : patch_count * cp_per_patch;
  const uint32_t cp_bytes = total_control_points * sizeof(float);
  // Auto-indexed control points need no guest memory, so the CPU writes them.
  const bool cp_from_cpu = !is_adaptive && !draw_index_buffer.indexed;
  if (!AcquireSpirvArgumentBufferSlice(cp_bytes, 4, &tess_out.cp_buffer,
                                       &tess_out.cp_offset, !cp_from_cpu)) {
    XELOGE("Metal: failed to allocate {}B tessellation control point buffer",
           cp_bytes);
    return false;
  }

  const float factor_min =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  const float factor_max =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  const float clamped_factor_max =
      factor_max > 0.0f ? std::min(factor_max, float(max_tessellation_factor_))
                        : factor_max;
  const uint32_t factor_struct_bytes = is_quad ? 12u : 8u;
  const uint32_t factor_bytes =
      is_adaptive ? patch_count * factor_struct_bytes : factor_struct_bytes;
  if (!AcquireSpirvArgumentBufferSlice(factor_bytes, 4, &tess_out.factor_buffer,
                                       &tess_out.factor_offset, is_adaptive)) {
    XELOGE("Metal: failed to allocate {}B tessellation factor buffer",
           factor_bytes);
    return false;
  }

  if (!is_adaptive) {
    // Discrete and continuous modes use TessellationFactorStepFunctionConstant
    // with one half-float struct shared by all patches.
    const uint16_t factor_half = Float32ToHalf(clamped_factor_max);
    auto* dst = reinterpret_cast<uint16_t*>(
        static_cast<uint8_t*>(tess_out.factor_buffer->contents()) +
        tess_out.factor_offset);
    const uint32_t half_count = is_quad ? 6u : 4u;
    for (uint32_t i = 0; i < half_count; ++i) {
      dst[i] = factor_half;
    }
  }

  tess_out.patch_count = patch_count;
  tess_out.cp_per_patch = cp_per_patch;

  if (cp_from_cpu) {
    // Matches the auto-index path of metal_tessellation_prep.cs.metal.
    const uint32_t index_offset = regs[XE_GPU_REG_VGT_INDX_OFFSET];
    const uint32_t min_index = regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
    const uint32_t max_index = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];
    // The shader's endian swap of the raw index.
    const xenos::Endian endian =
        primitive_processing_result.host_shader_index_endian;
    auto* dst = reinterpret_cast<float*>(
        static_cast<uint8_t*>(tess_out.cp_buffer->contents()) +
        tess_out.cp_offset);
    for (uint32_t i = 0; i < total_control_points; ++i) {
      const uint32_t index =
          (xenos::GpuSwap(i, endian) + index_offset) & 0xFFFFFFu;
      dst[i] = float(std::min(std::max(index, min_index), max_index));
    }
    return true;
  }

  TessPrepDispatch dispatch;
  dispatch.constants.count = total_control_points;
  uint32_t index_source = 0u;
  if (draw_index_buffer.indexed) {
    index_source =
        draw_index_buffer.index_type == MTL::IndexTypeUInt16 ? 1u : 2u;
  }
  dispatch.constants.mode_flags =
      index_source |
      ((uint32_t(primitive_processing_result.host_shader_index_endian) & 3u)
       << 2u) |
      (is_adaptive ? (1u << 4u) : 0u) | (is_quad ? (1u << 5u) : 0u);
  dispatch.constants.index_offset = regs[XE_GPU_REG_VGT_INDX_OFFSET];
  dispatch.constants.min_index = regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
  dispatch.constants.max_index = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];
  dispatch.constants.factor_min = factor_min;
  dispatch.constants.factor_max = clamped_factor_max;

  dispatch.cp_buffer = tess_out.cp_buffer;
  dispatch.cp_offset = tess_out.cp_offset;
  if (draw_index_buffer.indexed) {
    const NS::UInteger aligned_offset =
        NS::UInteger(draw_index_buffer.offset) & ~NS::UInteger(3);
    dispatch.src_buffer = draw_index_buffer.buffer;
    dispatch.src_offset = aligned_offset;
    dispatch.constants.src_byte_offset =
        uint32_t(draw_index_buffer.offset - aligned_offset);
  } else {
    dispatch.src_buffer = null_buffer_;
    dispatch.src_offset = 0;
    dispatch.constants.src_byte_offset = 0;
  }
  dispatch.factor_buffer = is_adaptive ? tess_out.factor_buffer : null_buffer_;
  dispatch.factor_offset = is_adaptive ? tess_out.factor_offset : 0;

  const bool is_shared_memory =
      draw_index_buffer.indexed && shared_memory_ &&
      draw_index_buffer.buffer == shared_memory_->GetBuffer();
  if (is_shared_memory) {
    const uint64_t index_stride =
        draw_index_buffer.index_type == MTL::IndexTypeUInt16 ? sizeof(uint16_t)
                                                             : sizeof(uint32_t);
    const uint64_t index_bytes =
        uint64_t(draw_index_buffer.index_count) * index_stride;
    dispatch.reads_shared_memory = true;
    dispatch.shared_memory_start = uint32_t(draw_index_buffer.offset);
    dispatch.shared_memory_length =
        uint32_t(std::min(index_bytes, uint64_t(SharedMemory::kBufferSize)));
  }

  // Queue the prep dispatch into a pre-pass command buffer committed right
  // before current_command_buffer_ (like CommitLoadsAhead) so TBDR tile memory
  // isn't broken mid-pass, unless something in current_command_buffer_ writes
  // the indices. Other reads of them don't matter. A later write there waits
  // for this draw's pass, which waits for the dispatch.
  bool encode_ahead = true;
  if (dispatch.reads_shared_memory) {
    auto* hazards = shared_memory_->hazards();
    if (!hazards ||
        hazards->HasUncommittedWrite(dispatch.shared_memory_start,
                                     dispatch.shared_memory_length) ||
        render_encoder_shared_memory_hazards_.HasWriteAccess(
            dispatch.shared_memory_start, dispatch.shared_memory_length) ||
        shared_compute_hazards_.HasWriteAccess(
            dispatch.shared_memory_start, dispatch.shared_memory_length)) {
      encode_ahead = false;
    }
  }

  if (encode_ahead) {
    tess_prep_ahead_.push_back(dispatch);
  } else {
    MetalBufferHazards::Encoder* compute_hazards = nullptr;
    MTL::ComputeCommandEncoder* compute =
        BeginSharedComputeEncoder(current_command_buffer_, &compute_hazards);
    if (!compute) {
      XELOGE("Metal: failed to open compute encoder for tessellation prep");
      return false;
    }
    if (dispatch.reads_shared_memory && compute_hazards &&
        compute_hazards->is_active()) {
      compute_hazards->Access(dispatch.shared_memory_start,
                              dispatch.shared_memory_length, false);
    }
    EncodeTessPrepDispatch(compute, dispatch);
  }

  return true;
}

bool MetalCommandProcessor::IssueDraw(
    Shader* vertex_shader, Shader* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool primitive_polygonal, bool memexport_used, bool pure_memexport_draw,
    uint32_t normalized_color_mask, const RegisterFile& regs) {
  SCOPE_profile_cpu_f("gpu");
  assert_not_null(vertex_shader);
  auto* metal_vertex_shader = static_cast<MetalShader*>(vertex_shader);
  auto* metal_pixel_shader = static_cast<MetalShader*>(pixel_shader);

  Shader::HostVertexShaderType host_vertex_shader_type =
      primitive_processing_result.host_vertex_shader_type;

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask = 0;
  if (metal_pixel_shader) {
    interpolator_mask = metal_vertex_shader->writes_interpolators() &
                        metal_pixel_shader->GetInterpolatorInputMask(
                            regs.Get<reg::SQ_PROGRAM_CNTL>(),
                            regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos);
  }

  auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  SpirvShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentSpirvVertexShaderModification(
          *metal_vertex_shader, host_vertex_shader_type, interpolator_mask,
          ps_param_gen_pos != UINT32_MAX);
  std::shared_ptr<const Shader::Specialization> vertex_specialization;
  const bool is_tessellated = primitive_processing_result.IsTessellated();
  // Specialize eligible streams only on non-tessellated export-only draws.
  // Tessellation uses a separate compilation path without profile retry.
  // Excluding pixel shaders also guarantees the synchronous pipeline fallback
  // has no pending pixel translation to consume.
  if (!metal_pixel_shader && !is_tessellated) {
    const uint32_t slot = memexport_format_profile::SelectSlot(
        pipeline_cache_->memexport_format_profiles(), metal_vertex_shader, regs,
        vertex_specialization);
    vertex_shader_modification.vertex.memexport_format_specialized = slot != 0;
    vertex_shader_modification.vertex.memexport_format_slot = slot;
  }
  SpirvShaderTranslator::Modification pixel_shader_modification =
      metal_pixel_shader
          ? pipeline_cache_->GetCurrentSpirvPixelShaderModification(
                *metal_pixel_shader, interpolator_mask, ps_param_gen_pos,
                normalized_depth_control, normalized_color_mask)
          : SpirvShaderTranslator::Modification(0);

  // Memory exports are guest-visible writes. A pending generic pipeline,
  // including after a profile guard miss, must never skip the export draw.
  // Counted draws also require the guest's exact kills and alpha test.
  const bool exact_shaders_required = ExactShadersRequired(memexport_used);

  // What a draw that may not use a stand-in spends on its own thread getting
  // its shaders and pipeline, logged when it had to build something.
  const uint64_t stand_in_wait_start_ticks =
      draw_stand_in_wait_reason_ && cvars::async_shader_compilation
          ? xe::Clock::QueryHostTickCount()
          : 0;

  MetalPipelineCache::ShaderCompileStatus compile_status =
      MetalPipelineCache::ShaderCompileStatus::kReady;
  MetalPipelineCache::ShaderCompileStatus pixel_status =
      MetalPipelineCache::ShaderCompileStatus::kReady;
  MetalShader::MetalTranslation* pixel_translation = nullptr;
  if (metal_pixel_shader) {
    pixel_translation = static_cast<MetalShader::MetalTranslation*>(
        pipeline_cache_->GetOrCreateHostTranslation(
            *metal_pixel_shader, pixel_shader_modification.value,
            !exact_shaders_required, &pixel_status));
    // Only spirv2air reads render targets. If it can't translate the shader,
    // blend the targets with fixed-function blending (gamma in gamma space)
    // rather than dropping the draw.
    if (pixel_status == MetalPipelineCache::ShaderCompileStatus::kFailed &&
        pixel_shader_modification.pixel.host_render_target_feedback) {
      pixel_shader_modification.pixel.host_render_target_feedback = 0;
      pixel_shader_modification.pixel.color_targets_used =
          MetalPipelineCache::GetColorTargetsWritten(normalized_color_mask);
      pixel_translation = static_cast<MetalShader::MetalTranslation*>(
          pipeline_cache_->GetOrCreateHostTranslation(
              *metal_pixel_shader, pixel_shader_modification.value,
              !exact_shaders_required, &pixel_status));
    }
    if (pixel_status == MetalPipelineCache::ShaderCompileStatus::kFailed) {
      return false;
    }
  }

  MTL::RenderPipelineState* pipeline = nullptr;
  bool bound_placeholder = false;
  auto* vertex_translation = static_cast<MetalShader::MetalTranslation*>(
      pipeline_cache_->GetOrCreateHostTranslation(
          *metal_vertex_shader, vertex_shader_modification.value,
          !exact_shaders_required && cvars::async_shader_skip_draws,
          &compile_status, vertex_specialization));
  // Nothing can stand in for the vertex/domain shader, so its draws wait.
  if (compile_status == MetalPipelineCache::ShaderCompileStatus::kPending) {
    pipeline_cache_->LogShaderCompilePending(
        vertex_translation, is_tessellated ? "domain" : "vertex");
    return true;
  }
  // A memory export format profile is speculative. If the compiler rejects
  // the specialized translation, or its pipeline fails to link, use the
  // generic translation, which always exists, rather than failing this draw
  // on every frame for the rest of the session or handing it to a
  // placeholder that cannot export.
  auto use_generic_vertex_translation = [&]() {
    vertex_shader_modification.vertex.memexport_format_specialized = 0;
    vertex_shader_modification.vertex.memexport_format_slot = 0;
    vertex_translation = static_cast<MetalShader::MetalTranslation*>(
        pipeline_cache_->GetOrCreateHostTranslation(
            *metal_vertex_shader, vertex_shader_modification.value,
            /*allow_async=*/false, &compile_status));
    return compile_status == MetalPipelineCache::ShaderCompileStatus::kReady;
  };
  if (compile_status != MetalPipelineCache::ShaderCompileStatus::kReady &&
      (!vertex_shader_modification.vertex.memexport_format_specialized ||
       !use_generic_vertex_translation())) {
    return false;
  }
  MetalPipelineCache::PipelineCompileStatus pipeline_compile_status =
      MetalPipelineCache::PipelineCompileStatus::kReady;
  if (pixel_status == MetalPipelineCache::ShaderCompileStatus::kReady) {
    pipeline = pipeline_cache_->GetOrCreatePipelineState(
        vertex_translation, pixel_translation, regs, &pipeline_compile_status);
    if (!pipeline && exact_shaders_required &&
        pipeline_compile_status ==
            MetalPipelineCache::PipelineCompileStatus::kPending) {
      pipeline_cache_->AwaitPendingPipeline();
      pipeline = pipeline_cache_->GetOrCreatePipelineState(
          vertex_translation, pixel_translation, regs,
          &pipeline_compile_status);
    }
  }
  if (!pipeline &&
      pixel_status == MetalPipelineCache::ShaderCompileStatus::kReady &&
      vertex_shader_modification.vertex.memexport_format_specialized) {
    if (!use_generic_vertex_translation()) {
      return false;
    }
    pipeline = pipeline_cache_->GetOrCreatePipelineState(
        vertex_translation, pixel_translation, regs, &pipeline_compile_status);
    if (!pipeline && pipeline_compile_status ==
                         MetalPipelineCache::PipelineCompileStatus::kPending) {
      pipeline_cache_->AwaitPendingPipeline();
      pipeline = pipeline_cache_->GetOrCreatePipelineState(
          vertex_translation, pixel_translation, regs,
          &pipeline_compile_status);
    }
  }
  if (!pipeline && !exact_shaders_required &&
      (pixel_status != MetalPipelineCache::ShaderCompileStatus::kReady ||
       pipeline_compile_status ==
           MetalPipelineCache::PipelineCompileStatus::kPending)) {
    pipeline = pipeline_cache_->GetOrCreatePlaceholderPipelineState(
        vertex_translation, regs);
    bound_placeholder = pipeline != nullptr;
    if (bound_placeholder) {
      pipeline_cache_->LogPlaceholderDraw(vertex_translation,
                                          pixel_translation);
    }
  }
  if (!pipeline) {
    if (pipeline_compile_status ==
            MetalPipelineCache::PipelineCompileStatus::kPending ||
        pixel_status != MetalPipelineCache::ShaderCompileStatus::kReady) {
      pipeline_cache_->LogPipelineCompilePending(vertex_translation,
                                                 pixel_translation);
      return true;
    }
    return false;
  }
  const uint32_t vertex_buffer_mask = vertex_translation->used_buffer_mask();
  // A pipeline without a pixel shader, and the placeholder, have no fragment
  // function and read no heap. The argument buffer stays bound to the
  // fragment stage all the same: without it Metal's validation reports draws
  // whose fragment function declares it and finds none bound (12 in a BF4
  // frame), so something has been relying on the binding these draws leave
  // behind.
  // TODO(xenios-jp): find those draws and have them bind it, then drop this.
  const uint32_t pixel_buffer_mask =
      pixel_translation && !bound_placeholder
          ? pixel_translation->used_buffer_mask()
          : uint32_t(1) << MetalArgumentBinder::kArgumentBufferIndex;
  if (stand_in_wait_start_ticks) {
    double wait_ms =
        double(xe::Clock::QueryHostTickCount() - stand_in_wait_start_ticks) *
        1000.0 / double(xe::Clock::QueryHostTickFrequency());
    if (wait_ms >= 1.0) {
      XELOGI(
          "Awaited real pipeline for a draw into {} ({}): VS {:016X}, PS "
          "{:016X}, {:.2f} ms",
          render_target_cache_->GetLastUpdateDrawTargetName(),
          draw_stand_in_wait_reason_, metal_vertex_shader->ucode_data_hash(),
          metal_pixel_shader ? metal_pixel_shader->ucode_data_hash() : 0,
          wait_ms);
    }
  }
  // The placeholder has no fragment function, so the pixel stage's resources
  // must not be bound to it.
  MetalShader* bind_pixel_shader =
      bound_placeholder ? nullptr : metal_pixel_shader;

  // A placeholder samples none of the pixel shader's textures, and their
  // binding list may still be being filled.
  uint32_t used_texture_mask =
      metal_vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (bind_pixel_shader) {
    used_texture_mask |=
        bind_pixel_shader->GetUsedTextureMaskAfterTranslation();
  }
  if (!PrepareDrawTextures(used_texture_mask, regs)) {
    // TODO(xenios-jp): a draw given up here, or for a failed translation or
    // pipeline above, leaves what Update decided for it: transfers cut around a
    // clear quad are not put back.
    return true;
  }
  if (texture_cache_) {
    texture_cache_->Load3DAs2DViews(*metal_vertex_shader, bind_pixel_shader);
  }

  if (!RequestDrawSharedMemoryRanges(*metal_vertex_shader, regs)) {
    return false;
  }
  DrawIndexBuffer draw_index_buffer;
  if (!ResolveDrawIndexBuffer(primitive_processing_result,
                              host_vertex_shader_type, is_tessellated,
                              draw_index_buffer)) {
    return false;
  }
  NativeTessellationDraw native_tess;
  if (is_tessellated) {
    if (!PrepareNativeTessellationDraw(primitive_processing_result,
                                       draw_index_buffer, regs, native_tess)) {
      return false;
    }
    if (!native_tess.patch_count) {
      return true;
    }
  }
  // A texture load or tessellation prep with a shared-memory hazard may have
  // ended the render encoder to be ordered after earlier draws in the command
  // buffer. A pass begun here starts with this draw, drawn with its own
  // shaders unless a placeholder stands in.
  if (render_target_cache_) {
    render_target_cache_->SetClearQuadPassBegin(!bound_placeholder);
  }
  BeginCommandBuffer();
  if (render_target_cache_) {
    render_target_cache_->SetClearQuadPassBegin(false);
  }
  if (!current_render_encoder_) {
    XELOGE("Metal: failed to resume render encoder after uploads");
    return false;
  }

  draw_util::ViewportInfo viewport_info;
  ComputeDrawViewportInfo(regs, metal_pixel_shader, normalized_depth_control,
                          viewport_info);
  ApplyViewportAndScissor(regs, viewport_info);

  if (msl_bound_pipeline_state_ != pipeline) {
    current_render_encoder_->setRenderPipelineState(pipeline);
    msl_bound_pipeline_state_ = pipeline;
  }
  ApplyRasterizerState(primitive_polygonal);
  ApplyDepthStencilState(primitive_polygonal, normalized_depth_control);

  UpdateSpirvSystemConstantValues(
      primitive_processing_result, primitive_polygonal,
      primitive_processing_result.line_loop_closing_index,
      primitive_processing_result.host_shader_index_endian, viewport_info,
      used_texture_mask, normalized_depth_control, normalized_color_mask);

  float blend_constants[] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  if (!ff_blend_factor_valid_ ||
      std::memcmp(ff_blend_factor_, blend_constants, sizeof(float) * 4) != 0) {
    std::memcpy(ff_blend_factor_, blend_constants, sizeof(float) * 4);
    ff_blend_factor_valid_ = true;
    current_render_encoder_->setBlendColor(
        blend_constants[0], blend_constants[1], blend_constants[2],
        blend_constants[3]);
  }

  UpdateGuestConstantCaches(metal_vertex_shader, metal_pixel_shader, regs);
  MetalArgumentBinder::Constants constants;
  constants.system = {&spirv_system_constants_,
                      uint32_t(sizeof(spirv_system_constants_)),
                      msl_system_constants_version_};
  // The uniform block is declared as float_count vec4s (256 with
  // float_dynamic_addressing), so nothing past that is addressable.
  auto declared_float_bytes = [](const Shader* shader) -> uint32_t {
    if (!shader) {
      return 0;
    }
    return std::min(uint32_t(shader->constant_register_map().float_count) * 16u,
                    uint32_t(kCbvSizeBytes));
  };
  constants.float_vertex = {msl_cached_float_constants_vertex_.data(),
                            declared_float_bytes(metal_vertex_shader),
                            guest_constant_versions_[0]};
  // The placeholder binds no pixel stage, so it declares no float constants.
  constants.float_pixel = {msl_cached_float_constants_pixel_.data(),
                           declared_float_bytes(bind_pixel_shader),
                           guest_constant_versions_[1]};
  constants.bool_loop = {msl_cached_bool_loop_constants_.data(),
                         uint32_t(kBoolLoopConstantsSize),
                         guest_constant_versions_[2]};
  constants.fetch = {msl_cached_fetch_constants_.data(),
                     uint32_t(kFetchConstantsSize),
                     guest_constant_versions_[3]};
  if (!argument_binder_.Bind(current_render_encoder_, metal_vertex_shader,
                             bind_pixel_shader, constants, memexport_used,
                             vertex_buffer_mask, pixel_buffer_mask)) {
    return false;
  }

  OrderDrawSharedMemoryAccesses(
      bind_pixel_shader, primitive_processing_result,
      draw_index_buffer.indexed ? draw_index_buffer.buffer : nullptr,
      draw_index_buffer.offset,
      uint64_t(draw_index_buffer.index_count) *
          (draw_index_buffer.index_type == MTL::IndexTypeUInt16
               ? sizeof(uint16_t)
               : sizeof(uint32_t)),
      regs);

  // Resume a ZPD segment waiting on a render encoder so this draw is counted.
  OpenQuerySegment(false);

  if (is_tessellated) {
    current_render_encoder_->setVertexBuffer(native_tess.cp_buffer,
                                             native_tess.cp_offset,
                                             kSpirv2airControlPointBufferIndex);
    UseRenderEncoderResource(native_tess.cp_buffer, MTL::ResourceUsageRead);
    current_render_encoder_->setTessellationFactorBuffer(
        native_tess.factor_buffer, native_tess.factor_offset, 0);
    UseRenderEncoderResource(native_tess.factor_buffer, MTL::ResourceUsageRead);
    DrawStripMergeBands([&]() {
      current_render_encoder_->drawPatches(
          NS::UInteger(native_tess.cp_per_patch), NS::UInteger(0),
          NS::UInteger(native_tess.patch_count), nullptr, 0, NS::UInteger(1),
          NS::UInteger(0));
    });
  } else {
    if (draw_index_buffer.indexed) {
      UseRenderEncoderResource(draw_index_buffer.buffer,
                               MTL::ResourceUsageRead);
    }
    DrawStripMergeBands([&]() {
      if (draw_index_buffer.indexed) {
        current_render_encoder_->drawIndexedPrimitives(
            draw_index_buffer.primitive_type,
            NS::UInteger(draw_index_buffer.index_count),
            draw_index_buffer.index_type, draw_index_buffer.buffer,
            NS::UInteger(draw_index_buffer.offset), NS::UInteger(1),
            NS::Integer(0), NS::UInteger(0));
      } else {
        current_render_encoder_->drawPrimitives(
            draw_index_buffer.primitive_type, NS::UInteger(0),
            NS::UInteger(draw_index_buffer.index_count), NS::UInteger(1),
            NS::UInteger(0));
      }
    });
  }

  if (memexport_used) {
    for (const draw_util::MemExportRange& range : memexport_ordering_ranges_) {
      AddPendingWrite(render_encoder_memexport_ranges_, range);
    }
    NoteMemexportRangesWritten();
  }
  render_encoder_memexport_draws_are_pure_ &= pure_memexport_draw;

  if (render_target_cache_) {
    render_target_cache_->NoteDrawWrites();
  }
  ++current_draw_index_;
  return true;
}

const char* MetalCommandProcessor::StripMergeBegin(
    const StripMerge::Layout& layout) {
  if (!render_target_cache_ ||
      render_target_cache_->GetPath() !=
          RenderTargetCache::Path::kHostRenderTargets) {
    return "the render targets aren't host ones";
  }
  // Transfers address a band up to strip_count - 1 bands away with a 12-bit
  // offset, biased or not (MakeTransferAddressConstant). Strip-merged render
  // targets are 32bpp.
  if (uint64_t(layout.strip_count - 1) * layout.band_tile_rows() *
          layout.pitch_tiles_at_32bpp >=
      2 * xenos::kEdramTileCount) {
    return "the last band is farther from the first than transfers address";
  }
  // The merged render targets start in a pass of their own, with nothing
  // queued for the render targets before.
  EndRenderEncoder();
  if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
    return "the queued render target transfers failed";
  }
  return render_target_cache_->BeginStripMerge(layout.strip_count,
                                               layout.band_tile_rows());
}

bool MetalCommandProcessor::StripMergeSeedCarry(
    const StripMerge::Layout& layout, uint32_t band) {
  if (!render_target_cache_) {
    return false;
  }
  // The copies read what the pass and the transfers queued for it wrote.
  EndRenderEncoder();
  if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
    render_target_cache_->DropStripMergeCarries();
    return false;
  }
  return render_target_cache_->SeedStripMergeCarry(
      band, layout.strip_count, layout.band_tile_rows(),
      layout.pitch_tiles_at_32bpp, layout.msaa_samples);
}

void MetalCommandProcessor::StripMergeDropCarry() {
  if (render_target_cache_) {
    render_target_cache_->DropStripMergeCarries();
  }
}

void MetalCommandProcessor::StripMergeEnd(uint32_t band, bool carry,
                                          uint32_t band_tile_rows) {
  strip_merge_replicate_draw_ = false;
  strip_merge_first_band_only_ = false;
  if (!render_target_cache_) {
    return;
  }
  // The transfers out of the merged render targets read what the pass and
  // the transfers queued for it wrote.
  EndRenderEncoder();
  render_target_cache_->FlushPendingDrawPassTransfers();
  render_target_cache_->EndStripMerge(band, carry, band_tile_rows);
}

void MetalCommandProcessor::DrawStripMergeBands(
    const std::function<void()>& draw) {
  if (!strip_merge_replicate_draw_) {
    draw();
    return;
  }
  // A draw addressing the strip's band (window offset 0, a window scissor
  // other than the strip's), in each band of the merged render targets: the
  // viewport moved down by the band's top and the scissor clamped to the band.
  // Drawing resolution scaling is off while merging.
  const StripMerge::Layout& layout = strip_merge_.layout();
  MTL::Viewport viewport = msl_viewport_;
  MTL::ScissorRect scissor = msl_scissor_;
  uint32_t band_count = strip_merge_first_band_only_ ? 1 : layout.strip_count;
  for (uint32_t band = 0; band < band_count; ++band) {
    uint64_t band_top = uint64_t(band) * layout.band_rows;
    uint64_t band_bottom =
        std::min(band_top + layout.band_rows, uint64_t(render_encoder_height_));
    uint64_t top = std::max(uint64_t(scissor.y) + band_top, band_top);
    uint64_t bottom = std::min(
        uint64_t(scissor.y) + uint64_t(scissor.height) + band_top, band_bottom);
    if (bottom <= top) {
      continue;
    }
    MTL::Viewport band_viewport = viewport;
    band_viewport.originY += double(band_top);
    MTL::ScissorRect band_scissor = scissor;
    band_scissor.y = NS::UInteger(top);
    band_scissor.height = NS::UInteger(bottom - top);
    current_render_encoder_->setViewport(band_viewport);
    current_render_encoder_->setScissorRect(band_scissor);
    draw();
  }
  current_render_encoder_->setViewport(viewport);
  current_render_encoder_->setScissorRect(scissor);
}

bool MetalCommandProcessor::IssueCopy() {
  SCOPE_profile_cpu_f("gpu");
  // Finish any in-flight rendering so render target contents are visible to
  // resolve logic. The render target cache ends the pass itself, once it
  // knows which of its attachments the resolve reads and clears, unless it
  // reads them from the open pass with a tile dispatch.
  if (!render_target_cache_) {
    EndRenderEncoder();
  }
  {
    // The resolve rectangle is read from vertex buffer 0 on this thread
    // (draw_util::GetResolveInfo).
    xenos::xe_gpu_vertex_fetch_t resolve_vertices =
        register_file_->GetVertexFetch(0);
    AwaitGpuWritesTo(resolve_vertices.address << 2, resolve_vertices.size << 2);
  }
  MTL::CommandBuffer* copy_command_buffer = EnsureCommandBuffer();
  if (!copy_command_buffer) {
    XELOGE("MetalCommandProcessor::IssueCopy: failed to get command buffer");
    EndRenderEncoder();
    return false;
  }

  if (!render_target_cache_) {
    XELOGW("MetalCommandProcessor::IssueCopy - No render target cache");
    return true;
  }

  uint32_t written_address = 0;
  uint32_t written_length = 0;

  if (render_target_cache_) {
    render_target_cache_->SetStripMergeKeepCarries(false);
  }
  // Strip merge (strip_merge.h): a strip-own resolve of the merged first strip
  // resolves every strip's rows. Its source is the strip-merged render target
  // as one rectangle (GetStripMergedResolveRectangle), so the tile resolve
  // can read every band from the open merged pass.
  uint32_t strip_merge_resolve_clear_bottom = 0;
  uint32_t strip_merge_resolve_bottom =
      strip_merge_.recording()
          ? strip_merge_.NoteResolve(trace_writer_,
                                     strip_merge_resolve_clear_bottom)
          : 0;
  render_target_cache_->SetStripMergeResolveBottom(
      strip_merge_resolve_bottom, strip_merge_resolve_clear_bottom);
  bool resolved = render_target_cache_->Resolve(
      *memory_, written_address, written_length, copy_command_buffer);
  render_target_cache_->SetStripMergeResolveBottom(0);
  // The pass ends at the resolve, unless a tile resolve kept it open,
  // recording what it wrote.
  if (!render_target_cache_->last_resolve_kept_pass()) {
    EndRenderEncoder();
  }
  if (!resolved) {
    XELOGE("MetalCommandProcessor::IssueCopy - Resolve failed");
    return false;
  }

  if (!written_length) {
    // Keep the submission open for no-op copies and let primary-buffer end,
    // swap, or explicit sync points choose the commit boundary.
    return true;
  }

  // The resolve overwrote any export output here, so no fence need await it.
  ClearMemexportPages(written_address, written_length);
  if (memexport_watch_any_pages_ &&
      written_address < SharedMemory::kBufferSize) {
    // Faults there are no longer counted as export pages.
    uint32_t end_page =
        std::min(written_address + written_length, SharedMemory::kBufferSize) >>
        12;
    for (uint32_t page = (written_address + 4095) >> 12; page < end_page;
         ++page) {
      memexport_watch_page_bits_[page >> 6].fetch_and(
          ~(uint64_t(1) << (page & 63)), std::memory_order_relaxed);
    }
  }
  // With zero-copy the resolve writes guest RAM itself, which the guest may
  // read once a fence or interrupt tells it the GPU got past the resolve.
  if (shared_memory_->is_zero_copy()) {
    resolve_last_submission_ = submission_current_;
    // With the read watch, a CPU access of the output waits for this
    // submission instead, so guest signals needn't
    // (GetHeldGpuWritesSubmission).
    if (ResolveReadWatchActive() &&
        IsResolveDestinationResident(written_address, written_length)) {
      NoteResolveForReadback(written_address, written_length, false);
      NoteWatchedGpuWrite(written_address, written_length);
      // A destination read recently is submitted only once a read really
      // arrives before its submission (SubmitWantedResolve).
      resolve_submit_requested_ = false;
      // Still submitted now for a destination whose accesses would wait for a
      // lazy submission.
      if (AdaptiveResolveSubmitsEarly(
              MakeReadbackResolveKey(written_address, written_length))) {
        EndResolveSubmission();
      }
    } else {
      resolve_unwatched_last_submission_ = submission_current_;
    }
  }

  // Later work in the submission that reads the output (draws, texture loads
  // encoded into the same command buffer) is ordered after the resolve on the
  // GPU, so the submission stays open. The pending output only makes the end
  // of the primary buffer commit it.
  copy_resolve_writes_pending_ = true;
  return true;
}

void MetalCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void MetalCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

MetalCommandProcessor::RegisterRangeClass
MetalCommandProcessor::ClassifyRegisterRange(uint32_t start_index,
                                             uint32_t num_registers) {
  const uint32_t end = start_index + num_registers;
  auto within = [&](uint32_t first, uint32_t last) {
    return start_index >= first && end <= last + 1;
  };
  auto overlaps = [&](uint32_t first, uint32_t last) {
    return start_index <= last && first < end;
  };
  if (end < start_index || end > RegisterFile::kRegisterCount) {
    return RegisterRangeClass::kPerRegister;
  }
  if (within(XE_GPU_REG_SHADER_CONSTANT_000_X,
             XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 - 1)) {
    return RegisterRangeClass::kFloatConstants;
  }
  if (within(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
             XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5)) {
    return RegisterRangeClass::kFetchConstants;
  }
  if (within(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
             XE_GPU_REG_SHADER_CONSTANT_LOOP_31)) {
    return RegisterRangeClass::kBoolLoopConstants;
  }
  // HandleSpecialRegisterWrite runs for the scratch, coherency and gamma ramp
  // registers, and a range mixing a shader constant class with anything else
  // needs the per-register dirtying.
  if (overlaps(XE_GPU_REG_SCRATCH_REG0, XE_GPU_REG_SCRATCH_REG7) ||
      overlaps(XE_GPU_REG_COHER_STATUS_HOST, XE_GPU_REG_COHER_STATUS_HOST) ||
      overlaps(XE_GPU_REG_DC_LUT_RW_INDEX, XE_GPU_REG_DC_LUT_30_COLOR) ||
      overlaps(XE_GPU_REG_SHADER_CONSTANT_000_X,
               XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) ||
      overlaps(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
               XE_GPU_REG_SHADER_CONSTANT_LOOP_31)) {
    return RegisterRangeClass::kPerRegister;
  }
  return RegisterRangeClass::kOrdinary;
}

void MetalCommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  const RegisterRangeClass range_class =
      ClassifyRegisterRange(start_index, num_registers);
  if (range_class == RegisterRangeClass::kPerRegister) {
    CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
    return;
  }
  WriteRegisterRangeFromMem(range_class, start_index, base, num_registers);
}

void MetalCommandProcessor::WriteRegisterRangeFromRing(xe::RingBuffer* ring,
                                                       uint32_t base,
                                                       uint32_t num_registers) {
  const RegisterRangeClass range_class =
      ClassifyRegisterRange(base, num_registers);
  if (range_class == RegisterRangeClass::kPerRegister) {
    CommandProcessor::WriteRegisterRangeFromRing(ring, base, num_registers);
    return;
  }
  // A range wrapping around the end of the ring is read in two parts.
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));
  const uint32_t first_registers =
      uint32_t(range.first_length / sizeof(uint32_t));
  WriteRegisterRangeFromMem(
      range_class, base,
      reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
      first_registers);
  if (range.second) {
    WriteRegisterRangeFromMem(
        range_class, base + first_registers,
        reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
        num_registers - first_registers);
  }
  ring->EndRead(range);
}

void MetalCommandProcessor::WriteRegisterRangeFromMem(
    RegisterRangeClass range_class, uint32_t start_index, uint32_t* base,
    uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);
  switch (range_class) {
    case RegisterRangeClass::kFloatConstants: {
      // A write to a constant the current shader uses dirties its stage
      // whether or not the value changed, matching the per-register path.
      const uint32_t first_constant =
          (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      const uint32_t last_constant = (start_index + num_registers - 1 -
                                      XE_GPU_REG_SHADER_CONSTANT_000_X) >>
                                     2;
      auto touches_live = [&](const uint64_t* constant_map,
                              uint32_t stage_first_constant) {
        if (last_constant < stage_first_constant ||
            first_constant >= stage_first_constant + 256) {
          return false;
        }
        const uint32_t first_index =
            std::max(first_constant, stage_first_constant) -
            stage_first_constant;
        const uint32_t last_index =
            std::min(last_constant, stage_first_constant + 255) -
            stage_first_constant;
        for (uint32_t word = first_index >> 6; word <= last_index >> 6;
             ++word) {
          uint64_t mask = ~uint64_t(0);
          if (word == first_index >> 6) {
            mask &= ~uint64_t(0) << (first_index & 63);
          }
          if (word == last_index >> 6) {
            mask &= ~uint64_t(0) >> (63 - (last_index & 63));
          }
          if (constant_map[word] & mask) {
            return true;
          }
        }
        return false;
      };
      msl_float_constants_dirty_vertex_ =
          msl_float_constants_dirty_vertex_ ||
          touches_live(msl_current_float_constant_map_vertex_.data(), 0);
      msl_float_constants_dirty_pixel_ =
          msl_float_constants_dirty_pixel_ ||
          touches_live(msl_current_float_constant_map_pixel_.data(), 256);
    } break;
    case RegisterRangeClass::kFetchConstants: {
      msl_fetch_constants_dirty_ = true;
      if (texture_cache_) {
        const uint32_t dword_start =
            start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
        const uint32_t first_fetch = dword_start / 6;
        const uint32_t last_fetch = (dword_start + num_registers - 1) / 6;
        texture_cache_->TextureFetchConstantsWrittenIfChanged(first_fetch,
                                                              last_fetch);
      }
    } break;
    case RegisterRangeClass::kBoolLoopConstants:
      msl_bool_loop_constants_dirty_ = true;
      break;
    default:
      break;
  }
}

void MetalCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    const uint32_t float_constant_index =
        (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    const uint32_t stage_constant_index = float_constant_index & 0xFF;
    const uint32_t map_index = stage_constant_index >> 6;
    const uint64_t map_bit = uint64_t(1) << (stage_constant_index & 63);
    if (float_constant_index >= 256) {
      if (msl_current_float_constant_map_pixel_[map_index] & map_bit) {
        msl_float_constants_dirty_pixel_ = true;
      }
    } else {
      if (msl_current_float_constant_map_vertex_[map_index] & map_bit) {
        msl_float_constants_dirty_vertex_ = true;
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    msl_bool_loop_constants_dirty_ = true;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    msl_fetch_constants_dirty_ = true;
    if (texture_cache_) {
      const uint32_t fetch_index =
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6;
      texture_cache_->TextureFetchConstantsWrittenIfChanged(fetch_index,
                                                            fetch_index);
    }
  }
}

MTL::CommandBuffer* MetalCommandProcessor::EnsureCommandBuffer() {
  SCOPE_profile_cpu_f("gpu");
  ProcessCompletedSubmissions();
  if (current_command_buffer_) {
    return current_command_buffer_;
  }
  if (!command_queue_) {
    XELOGE("EnsureCommandBuffer: no command queue");
    return nullptr;
  }

  EnsureCommandBufferAutoreleasePool();

  // Note: commandBuffer() returns an autoreleased object, we must retain it.
  {
    SCOPE_profile_cpu_i("gpu", "MetalCommandProcessor::QueueCommandBuffer");
    uint64_t wait_start = xe::Clock::QueryHostTickCount();
    current_command_buffer_ = command_queue_->commandBuffer();
    gpu_timing.wait_ticks.fetch_add(
        xe::Clock::QueryHostTickCount() - wait_start,
        std::memory_order_relaxed);
  }
  if (!current_command_buffer_) {
    XELOGE("EnsureCommandBuffer: failed to create command buffer");
    DrainCommandBufferAutoreleasePool();
    return nullptr;
  }
  current_command_buffer_->retain();
  if (debug_labels_) {
    current_command_buffer_->setLabel(
        NS::String::string("XeniaCommandBuffer", NS::UTF8StringEncoding));
  }
  command_buffer_render_passes_ = 0;

  ++submission_current_;
  ++command_buffer_kind_counts_[size_t(next_submission_kind_)];
  next_submission_kind_ = CommandBufferKind::kSubmissionOther;
  AddGpuTimeHandler(current_command_buffer_);
  pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
  current_command_buffer_->addCompletedHandler(
      [this,
       submission = submission_current_](MTL::CommandBuffer* command_buffer) {
        // For the adaptive early submit, before the submission counts as
        // complete.
        double gpu_time =
            command_buffer->GPUEndTime() - command_buffer->GPUStartTime();
        size_t adaptive_slot = size_t(submission & (kAdaptiveRingSize - 1));
        adaptive_gpu_ns_[adaptive_slot].store(
            gpu_time > 0.0 ? uint64_t(gpu_time * 1e9) : 0,
            std::memory_order_relaxed);
        adaptive_gpu_submission_[adaptive_slot].store(
            submission, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lock(completion_mutex_);
          // Handlers of different command buffers may run in any order (one
          // that fails is finished at once, ahead of earlier ones still
          // running), so the front only moves over submissions that have all
          // finished.
          uint64_t front =
              completed_command_buffers_.load(std::memory_order_relaxed);
          if (submission != front + 1) {
            completed_out_of_order_.push_back(submission);
          } else {
            ++front;
            for (size_t i = 0; i < completed_out_of_order_.size();) {
              if (completed_out_of_order_[i] == front + 1) {
                ++front;
                completed_out_of_order_[i] = completed_out_of_order_.back();
                completed_out_of_order_.pop_back();
                i = 0;
              } else {
                ++i;
              }
            }
            completed_command_buffers_.store(front, std::memory_order_release);
          }
          if (command_buffer->status() != MTL::CommandBufferStatusCompleted) {
            failed_submissions_.fetch_add(1, std::memory_order_relaxed);
          }
          completion_cond_.notify_all();
        }
        // Publish callback completion after its final owner access and unlock.
        pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
      });

  if (primitive_processor_) {
    primitive_processor_->BeginSubmission();
  }
  if (texture_cache_) {
    texture_cache_->BeginSubmission(submission_current_);
  }
  if (primitive_processor_ && !frame_open_) {
    primitive_processor_->BeginFrame();
    if (render_target_cache_) {
      render_target_cache_->BeginFrame();
      render_target_cache_->AdvanceRenderTargetUseFrame();
    }
    if (texture_cache_) {
      texture_cache_->BeginFrame();
    }
    frame_open_ = true;
  }

  return current_command_buffer_;
}

void MetalCommandProcessor::ProcessCompletedSubmissions() {
  const uint64_t completed =
      completed_command_buffers_.load(std::memory_order_relaxed);
  while (!closed_frames_.empty() &&
         closed_frames_.front().last_submission <= completed) {
    frame_completed_ = closed_frames_.front().frame;
    closed_frames_.pop_front();
  }
  if (completed <= submission_completed_processed_) {
    return;
  }
  submission_completed_processed_ = completed;
  if (primitive_processor_) {
    primitive_processor_->CompletedSubmissionUpdated();
  }
  if (texture_cache_) {
    texture_cache_->CompletedSubmissionUpdated(completed);
  }
}

void MetalCommandProcessor::EnsureCommandBufferAutoreleasePool() {
  if (command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_ = NS::AutoreleasePool::alloc()->init();
}

void MetalCommandProcessor::DrainCommandBufferAutoreleasePool() {
  if (!command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_->release();
  command_buffer_autorelease_pool_ = nullptr;
}

void MetalCommandProcessor::ResetMslRenderEncoderStateCache() {
  msl_bound_pipeline_state_ = nullptr;
  msl_viewport_valid_ = false;
  msl_scissor_valid_ = false;
  msl_rasterizer_state_valid_ = false;
  msl_depth_stencil_state_ = nullptr;
  msl_stencil_reference_valid_ = false;
  msl_stencil_reference_ = 0;
  render_encoder_buffers_ = {};
  ff_blend_factor_valid_ = false;
  ResetRenderEncoderResourceUsage();
}

void MetalCommandProcessor::InvalidateRenderEncoderStateAfterDrawPassTransfers(
    MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations) {
  if (!mutations) {
    return;
  }
  using RTC = MetalRenderTargetCache;
  if (mutations & RTC::kDrawPassTransferEncoderMutationPipeline) {
    msl_bound_pipeline_state_ = nullptr;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationDepthStencil) {
    msl_depth_stencil_state_ = nullptr;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationStencilReference) {
    msl_stencil_reference_valid_ = false;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationViewport) {
    msl_viewport_valid_ = false;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationScissor) {
    msl_scissor_valid_ = false;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationRasterizer) {
    msl_rasterizer_state_valid_ = false;
  }
  // Transfer draws bind their own descriptor heap and argument buffer at the
  // Metal Shader Converter's bind points, which the mask doesn't name.
  render_encoder_buffers_ = {};
}

MTL::ComputeCommandEncoder* MetalCommandProcessor::BeginSharedComputeEncoder(
    MTL::CommandBuffer* command_buffer,
    MetalBufferHazards::Encoder** hazards_out) {
  if (!command_buffer || command_buffer != current_command_buffer_) {
    return nullptr;
  }
  if (!shared_compute_encoder_) {
    EndRenderEncoder();
    shared_compute_encoder_ = command_buffer->computeCommandEncoder();
    if (!shared_compute_encoder_) {
      return nullptr;
    }
    shared_compute_encoder_->retain();
    shared_compute_command_buffer_ = command_buffer;
    if (shared_memory_ && shared_memory_->hazards()) {
      shared_compute_hazards_.Begin(*shared_memory_->hazards(), command_buffer,
                                    shared_compute_encoder_);
    }
  }
  *hazards_out = &shared_compute_hazards_;
  return shared_compute_encoder_;
}

void MetalCommandProcessor::EndSharedComputeEncoder() {
  if (shared_compute_encoder_) {
    shared_compute_hazards_.End();
    shared_compute_encoder_->endEncoding();
    shared_compute_encoder_->release();
    shared_compute_encoder_ = nullptr;
    shared_compute_command_buffer_ = nullptr;
  }
  // Fences written with no pass open go before the next encoder. A pass
  // begins only after this, so no pass is open while they're pending.
  if (shader_done_fences_outside_pass_) {
    assert_null(current_render_encoder_);
    EncodeDeferredShaderDoneFences();
  }
}

bool MetalCommandProcessor::ExactShadersRequired(bool memexport_used) const {
  return memexport_used || draw_stand_in_wait_reason_ ||
         (zpd_mode_ == ZPDMode::kStrict && !zpd_force_fake_fallback_ &&
          zpd_current_report_.handle != kInvalidReportHandle);
}

MTL::RenderCommandEncoder* MetalCommandProcessor::GetRenderEncoderForDepthClear(
    MTL::Texture* depth_texture, MTL::Texture* stencil_texture,
    MTL::RenderPassDescriptor** pass_descriptor_out) {
  if (!current_render_encoder_ || !current_render_pass_descriptor_ ||
      !depth_texture || !stencil_texture) {
    return nullptr;
  }
  auto* depth_attachment = current_render_pass_descriptor_->depthAttachment();
  auto* stencil_attachment =
      current_render_pass_descriptor_->stencilAttachment();
  if (!depth_attachment || depth_attachment->texture() != depth_texture ||
      !stencil_attachment || stencil_attachment->texture() != stencil_texture) {
    return nullptr;
  }
  *pass_descriptor_out = current_render_pass_descriptor_;
  return current_render_encoder_;
}

bool MetalCommandProcessor::IsDrawCountedByQuery() const {
  const reg::PA_SC_VIZ_QUERY viz_query =
      register_file_->Get<reg::PA_SC_VIZ_QUERY>();
  // A report measures only from its event (QueueZPDReport) to the end of the
  // frame (EndZPDFrame); a draw outside that opens no segment
  // (UpdateQuerySegment).
  return active_segment_.report_measuring() || active_segment_.segment_active ||
         viz_query.viz_query_ena;
}

void MetalCommandProcessor::NoteDepthClearInRenderEncoder() {
  using RTC = MetalRenderTargetCache;
  InvalidateRenderEncoderStateAfterDrawPassTransfers(
      RTC::kDrawPassTransferEncoderMutationPipeline |
      RTC::kDrawPassTransferEncoderMutationDepthStencil |
      RTC::kDrawPassTransferEncoderMutationStencilReference |
      RTC::kDrawPassTransferEncoderMutationViewport |
      RTC::kDrawPassTransferEncoderMutationScissor |
      RTC::kDrawPassTransferEncoderMutationRasterizer |
      RTC::kDrawPassTransferEncoderMutationFragmentSlot0);
}

bool MetalCommandProcessor::EncodeHeldDrawPassTransfers() {
  if (!render_target_cache_) {
    return true;
  }
  // The transfers take their value conversion from the draw that caused
  // them, not from the one that ends the hold.
  struct HeldEncode {
    MetalRenderTargetCache* cache;
    bool begun;
    ~HeldEncode() {
      if (begun) {
        cache->EndHeldTransferEncode();
      }
    }
  } held_encode = {render_target_cache_.get(),
                   render_target_cache_->BeginHeldTransferEncode()};
  const bool in_pass = render_target_cache_->ReleaseHeldDrawPassTransfers();
  if (!render_target_cache_->HasPendingDrawPassTransfers()) {
    return true;
  }
  if (!in_pass || !current_render_encoder_) {
    return render_target_cache_->FlushPendingDrawPassTransfers();
  }
  // As at the head of the pass, after the clear quads that ran ahead of them.
  CloseQuerySegment();
  MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations =
      MetalRenderTargetCache::kDrawPassTransferEncoderMutationNone;
  bool transfers_encoded = render_target_cache_->EncodePendingDrawPassTransfers(
      current_render_encoder_, current_render_pass_descriptor_, &mutations);
  InvalidateRenderEncoderStateAfterDrawPassTransfers(mutations);
  if (!transfers_encoded) {
    EndRenderEncoder();
    return render_target_cache_->FlushPendingDrawPassTransfers();
  }
  return true;
}

void MetalCommandProcessor::EndRenderEncoder() {
  SCOPE_profile_cpu_f("gpu");
  // Transfers held behind clear quads in this pass belong to it.
  if (current_render_encoder_ && render_target_cache_ &&
      render_target_cache_->DrawPassTransfersHeldInPass()) {
    EncodeHeldDrawPassTransfers();
    if (!current_render_encoder_) {
      return;
    }
  }
  render_encoder_resolve_only_ = false;
  EndSharedComputeEncoder();
  render_encoder_memexport_ranges_.clear();
  render_encoder_tile_resolve_ranges_.clear();
  render_encoder_index_count_ = 0;
  render_encoder_tile_member_class_ = 0;
  render_encoder_memexport_draws_are_pure_ = true;
  render_encoder_narrowed_reads_.clear();
  render_encoder_pixel_stage_fetches_ = false;
  if (!current_render_encoder_) {
    if (current_render_pass_descriptor_) {
      current_render_pass_descriptor_->release();
      current_render_pass_descriptor_ = nullptr;
    }
    render_encoder_has_zpd_visibility_ = false;
    // Left only by a pass that ended without EndRenderEncoder.
    FlushPostPassResolves();
    return;
  }
  // Visibility results are scoped to the render encoder and an offset can't be
  // selected again after it ends, so the segment closes here. The logical
  // report stays open and resumes on the next encoder.
  if (zpd_mode_ != ZPDMode::kFake) {
    CloseQuerySegment();
  }
  render_encoder_shared_memory_hazards_.End();
  if (render_target_cache_) {
    render_target_cache_->FinalizeRenderPassStoreActions(
        current_render_encoder_, current_render_pass_descriptor_);
  }
  current_render_encoder_->endEncoding();
  if (render_encoder_has_zpd_visibility_) {
    zpd_visibility_pool_->EndRenderPass(current_command_buffer_);
  }
  current_render_encoder_->release();
  current_render_encoder_ = nullptr;
  ++command_buffer_render_passes_;
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  render_encoder_has_zpd_visibility_ = false;
  ResetMslRenderEncoderStateCache();
  // The resolve work left to the pass end comes before anything else after
  // the pass, including the shader-done fences of commands that followed it.
  FlushPostPassResolves();
  EncodeDeferredShaderDoneFences();
}

void MetalCommandProcessor::FlushPostPassResolves() {
  if (!render_target_cache_ || !render_target_cache_->HasPostPassResolves()) {
    return;
  }
  render_target_cache_->FlushPostPassResolves(current_command_buffer_);
  // Whoever ended the pass expects no encoder open.
  EndSharedComputeEncoder();
}

void MetalCommandProcessor::ResetRenderEncoderResourceUsage() {
  render_encoder_resource_usage_.clear();
  render_encoder_textures_.clear();
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Texture* texture,
                                                     MTL::ResourceUsage usage) {
  if (current_render_encoder_ && texture &&
      !render_encoder_resource_usage_.Contains(texture)) {
    render_encoder_textures_.push_back(texture);
  }
  UseRenderEncoderResource(static_cast<MTL::Resource*>(texture), usage);
}

bool MetalCommandProcessor::RenderEncoderUsesAnyTexture(
    std::span<MTL::Texture* const> textures) const {
  auto root_texture = [](MTL::Texture* texture) {
    while (MTL::Texture* parent = texture->parentTexture()) {
      texture = parent;
    }
    return texture;
  };
  for (MTL::Texture* used : render_encoder_textures_) {
    MTL::Texture* used_root = root_texture(used);
    for (MTL::Texture* texture : textures) {
      if (texture && root_texture(texture) == used_root) {
        return true;
      }
    }
  }
  return false;
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Resource* resource,
                                                     MTL::ResourceUsage usage) {
  if (!current_render_encoder_ || !resource) {
    return;
  }
  // No useHeap: tracking on a tracked heap is heap-granular, so declaring the
  // heap would make this encoder depend on every write to anything in it.
  uint32_t usage_bits = static_cast<uint32_t>(usage);
  bool inserted;
  uint32_t& declared_usage_bits =
      render_encoder_resource_usage_.FindOrInsert(resource, inserted);
  if (!inserted && (declared_usage_bits & usage_bits) == usage_bits) {
    return;
  }
  declared_usage_bits |= usage_bits;
  current_render_encoder_->useResource(resource, usage);
}

void MetalCommandProcessor::SetRenderEncoderBuffer(RenderEncoderStage stage,
                                                   MTL::Buffer* buffer,
                                                   NS::UInteger offset,
                                                   NS::UInteger index) {
  bool offset_only = false;
  if (index < kRenderEncoderTrackedBufferCount) {
    RenderEncoderBuffer& bound = render_encoder_buffers_[size_t(stage)][index];
    if (bound.valid && bound.buffer == buffer) {
      if (bound.offset == offset) {
        return;
      }
      offset_only = true;
    }
    bound.buffer = buffer;
    bound.offset = offset;
    bound.valid = true;
  }
  switch (stage) {
    case RenderEncoderStage::kVertex:
      if (offset_only) {
        current_render_encoder_->setVertexBufferOffset(offset, index);
      } else {
        current_render_encoder_->setVertexBuffer(buffer, offset, index);
      }
      break;
    case RenderEncoderStage::kFragment:
      if (offset_only) {
        current_render_encoder_->setFragmentBufferOffset(offset, index);
      } else {
        current_render_encoder_->setFragmentBuffer(buffer, offset, index);
      }
      break;
    default:
      assert_unhandled_case(stage);
  }
}

void MetalCommandProcessor::BeginCommandBuffer() {
  SCOPE_profile_cpu_f("gpu");
  if (!EnsureCommandBuffer()) {
    return;
  }

  // The visibility result buffer has to be on the descriptor before the encoder
  // is created, so a segment waiting to open needs the pool allocated now.
  const bool zpd_segment_pending =
      zpd_mode_ != ZPDMode::kFake &&
      zpd_current_report_.handle != kInvalidReportHandle &&
      active_segment_.segment_pending_begin;
  if (zpd_segment_pending) {
    EnsureQueryResources();
  }

  if (!current_render_encoder_ && !render_encoder_resource_usage_.empty()) {
    ResetRenderEncoderResourceUsage();
  }

  // An encoder created before the pool existed can never host a query, so
  // restart it rather than leave the segment pending indefinitely.
  if (current_render_encoder_ && zpd_segment_pending && IsQueryPoolReady() &&
      !render_encoder_has_zpd_visibility_) {
    EndRenderEncoder();
  }

  // A draw reaching below the open pass's render area needs a pass covering
  // it.
  if (current_render_encoder_ && render_encoder_area_restricted_ &&
      GetDrawScissorBottom() > render_encoder_height_) {
    EndRenderEncoder();
  }

  // Obtain the render pass descriptor. Prefer the one provided by
  // MetalRenderTargetCache (host render-target path), falling back to the
  // legacy descriptor if needed.
  MTL::RenderPassDescriptor* pass_descriptor =
      current_render_encoder_ ? current_render_pass_descriptor_
                              : render_pass_descriptor_;
  if (render_target_cache_) {
    // Check attachment identity before asking the cache to rebuild a dirty
    // descriptor. Rebuilding releases the cache-owned descriptor, and an
    // allocator may reuse the same address even though the active encoder has
    // already captured its old attachments.
    if (current_render_encoder_ &&
        !render_target_cache_->IsRenderPassDescriptorCompatible(
            current_render_pass_descriptor_, 1)) {
      EndRenderEncoder();
      pass_descriptor = render_pass_descriptor_;
    }
    if (!current_render_encoder_) {
      if (MTL::RenderPassDescriptor* cache_desc =
              render_target_cache_->GetRenderPassDescriptor(1, true)) {
        pass_descriptor = cache_desc;
      }
    }
  }
  if (!pass_descriptor) {
    XELOGE("BeginCommandBuffer: No render pass descriptor available");
    return;
  }

  // Ownership transfers queued for this pass have to be encoded into it ahead
  // of the guest's draws. Resolve what it cannot take here, while no encoder
  // exists yet and the descriptor can still be rebuilt.
  if (render_target_cache_ &&
      render_target_cache_->HasPendingDrawPassTransfers() &&
      !render_target_cache_->PreflightPendingDrawPassTransfers(
          pass_descriptor)) {
    if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
      XELOGE("BeginCommandBuffer: failed to perform render target transfers");
      return;
    }
    // The flush may release the old descriptor, including on a failed rebuild.
    pass_descriptor = render_target_cache_->GetRenderPassDescriptor(
        1, !current_render_encoder_);
    if (!pass_descriptor) {
      XELOGE("BeginCommandBuffer: no render pass descriptor after transfers");
      return;
    }
  }

  // Only passes that host a query need the buffer. A segment pending later
  // restarts the encoder above to pick it up.
  if (zpd_segment_pending && IsQueryPoolReady()) {
    pass_descriptor->setVisibilityResultBuffer(
        zpd_visibility_pool_->visibility_buffer());
    // Slots from earlier passes may still await CPU readback. The accumulate
    // result type keeps them across passes; otherwise the pool copies each
    // pass's slots to readback storage when EndRenderEncoder ends the pass.
    if (__builtin_available(macOS 26.0, iOS 26.0, *)) {
      pass_descriptor->setVisibilityResultType(
          zpd_visibility_pool_->uses_accumulation()
              ? MTL::VisibilityResultTypeAccumulate
              : MTL::VisibilityResultTypeReset);
    }
  } else {
    pass_descriptor->setVisibilityResultBuffer(nullptr);
  }

  // Detect Reverse-Z usage and update clear depth.
  // A resolve clear deferred to this pass brings its own clear depth.
  if (register_file_ &&
      !(render_target_cache_ &&
        render_target_cache_->HasDeferredDepthClear(pass_descriptor))) {
    auto depth_control = register_file_->Get<reg::RB_DEPTHCONTROL>();
    bool reverse_z =
        depth_control.z_enable &&
        (depth_control.zfunc == xenos::CompareFunction::kGreater ||
         depth_control.zfunc == xenos::CompareFunction::kGreaterEqual);
    if (auto* da = pass_descriptor->depthAttachment()) {
      if (reverse_z) {
        da->setClearDepth(0.0);
      } else {
        da->setClearDepth(1.0);
      }
    }
  }

  // If the render pass configuration has changed since the current render
  // encoder was created (e.g. dummy RT0 -> real RTs, depth/stencil binding),
  // restart the render encoder with the updated descriptor.
  if (current_render_encoder_ &&
      current_render_pass_descriptor_ != pass_descriptor) {
    EndRenderEncoder();
  }

  if (!current_render_encoder_) {
    if (render_target_cache_) {
      render_target_cache_->SetRenderPassArea(pass_descriptor,
                                              GetDrawScissorBottom());
    }
    // A clear quad beginning the pass may overwrite all it would load.
    const uint32_t clear_quad_dontcare_mask =
        render_target_cache_
            ? render_target_cache_->SetClearQuadLoadActions(pass_descriptor)
            : 0;
    EndSharedComputeEncoder();
    // If some path cleared the encoder without going through EndRenderEncoder,
    // avoid leaking cached binding state into the new encoder.
    render_encoder_shared_memory_hazards_.Abandon();
    ResetMslRenderEncoderStateCache();
    // Note: renderCommandEncoder() returns an autoreleased object, we must
    // retain it.
    current_render_encoder_ =
        current_command_buffer_->renderCommandEncoder(pass_descriptor);
    if (!current_render_encoder_) {
      if (clear_quad_dontcare_mask) {
        render_target_cache_->RestoreClearQuadLoadActions(
            clear_quad_dontcare_mask);
      }
      XELOGE("Failed to create render command encoder");
      return;
    }
    current_render_encoder_->retain();
    if (shared_memory_ && shared_memory_->hazards()) {
      render_encoder_shared_memory_hazards_.Begin(*shared_memory_->hazards(),
                                                  current_command_buffer_,
                                                  current_render_encoder_);
    }
    // A non-null encoder will execute the descriptor's first-use clears when
    // the pass ends. Keep them pending across encoder-creation failures.
    if (render_target_cache_) {
      render_target_cache_->ConsumeRenderPassDescriptorClears(pass_descriptor);
    }
    ++render_passes_total_;
    if (debug_labels_) {
      current_render_encoder_->setLabel(
          NS::String::string("XeniaRenderEncoder", NS::UTF8StringEncoding));
    }
    render_encoder_has_zpd_visibility_ =
        IsQueryPoolReady() && (pass_descriptor->visibilityResultBuffer() ==
                               zpd_visibility_pool_->visibility_buffer());
    ff_blend_factor_valid_ = false;
    current_render_pass_descriptor_ = pass_descriptor;
    current_render_pass_descriptor_->retain();
    {
      RenderPassPipelineFormats& formats =
          current_render_pass_pipeline_formats_;
      std::fill(std::begin(formats.color), std::end(formats.color),
                MTL::PixelFormatInvalid);
      formats.depth = MTL::PixelFormatInvalid;
      formats.stencil = MTL::PixelFormatInvalid;
      formats.sample_count = 1;
      MetalPipelineCache::PopulatePipelineFormatsFromRenderPassDescriptor(
          pass_descriptor, formats.color, 4, &formats.depth, &formats.stencil,
          &formats.sample_count);
    }

    // Start the encoder off covering the whole active render pass rather than
    // a hard-coded 1280x720. Every draw applies the guest's own viewport and
    // scissor over this before it runs.
    uint32_t rt_width = 1;
    uint32_t rt_height = 1;
    GetRenderPassSize(pass_descriptor, render_target_width_,
                      render_target_height_, rt_width, rt_height);
    render_encoder_width_ = rt_width;
    render_encoder_height_ = rt_height;
    render_encoder_area_restricted_ =
        pass_descriptor->renderTargetHeight() != 0;
    // The descriptor is reused for later passes, which have to load.
    if (clear_quad_dontcare_mask) {
      render_target_cache_->RestoreClearQuadLoadActions(
          clear_quad_dontcare_mask);
    }
    MTL::Viewport viewport = {
        0.0, 0.0, static_cast<double>(rt_width), static_cast<double>(rt_height),
        0.0, 1.0};
    current_render_encoder_->setViewport(viewport);
    // Must not exceed the render pass dimensions.
    MTL::ScissorRect scissor = {0, 0, rt_width, rt_height};
    current_render_encoder_->setScissorRect(scissor);
  }

  if (render_target_cache_ &&
      render_target_cache_->HasPendingDrawPassTransfers() &&
      !render_target_cache_->HoldDrawPassTransfersForQuad()) {
    // An occlusion query already counting on this encoder would count the
    // transfer draws too. Closing the segment makes the next draw reopen it on
    // a fresh pool slot, the only legal way to resume counting mid-encoder.
    CloseQuerySegment();
    MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations =
        MetalRenderTargetCache::kDrawPassTransferEncoderMutationNone;
    bool transfers_encoded =
        render_target_cache_->EncodePendingDrawPassTransfers(
            current_render_encoder_, pass_descriptor, &mutations);
    InvalidateRenderEncoderStateAfterDrawPassTransfers(mutations);
    if (!transfers_encoded) {
      // Preflight passed, so this is a resource failure part way in. Drop the
      // pass and run the queue standalone: every attachment loaded DontCare is
      // one the transfers rewrite in full, so the flush restores it.
      static bool draw_pass_transfer_encode_failed_logged = false;
      if (!draw_pass_transfer_encode_failed_logged) {
        draw_pass_transfer_encode_failed_logged = true;
        XELOGE(
            "BeginCommandBuffer: failed to encode the queued render target "
            "ownership transfers into the draw pass");
      }
      EndRenderEncoder();
      if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
        XELOGE(
            "BeginCommandBuffer: failed to perform the queued render target "
            "ownership transfers");
      }
      return;
    }
  }
}

bool MetalCommandProcessor::AcquireSpirvArgumentBufferSlice(
    uint32_t bytes, uint32_t alignment, MTL::Buffer** buffer_out,
    NS::UInteger* offset_out, bool gpu_written) {
  if (!buffer_out || !offset_out) {
    return false;
  }
  *buffer_out = nullptr;
  *offset_out = 0;
  if (!device_ || !current_command_buffer_ || bytes == 0) {
    return false;
  }

  const size_t align = std::max<size_t>(1, size_t(alignment));
  auto align_up = [](size_t value, size_t alignment) -> size_t {
    return ((value + alignment - 1) / alignment) * alignment;
  };

  auto& command_buffer_pages = gpu_written ? command_buffer_tess_prep_pages_
                                           : command_buffer_spirv_argbuf_pages_;
  if (!command_buffer_pages.empty()) {
    auto& page = command_buffer_pages.back();
    const size_t aligned_offset = align_up(page->offset, align);
    if (aligned_offset + bytes <= page->bytes) {
      page->offset = aligned_offset + bytes;
      *buffer_out = page->buffer;
      *offset_out = NS::UInteger(aligned_offset);
      return *buffer_out != nullptr;
    }
  }

  constexpr size_t kDefaultSpirvArgumentBufferPageBytes = 1024 * 1024;
  const size_t required_page_bytes = align_up(bytes, align);
  const size_t page_bytes =
      std::max(kDefaultSpirvArgumentBufferPageBytes, required_page_bytes);

  std::shared_ptr<SpirvArgumentBufferPage> page;
  {
    std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
    auto& pool = gpu_written ? tess_prep_page_pool_ : spirv_argbuf_pool_;
    for (auto it = pool.begin(); it != pool.end(); ++it) {
      if ((*it) && (*it)->bytes >= page_bytes) {
        page = *it;
        pool.erase(it);
        break;
      }
    }
  }
  if (!page) {
    page = std::make_shared<SpirvArgumentBufferPage>();
    page->bytes = page_bytes;
    page->gpu_written = gpu_written;
    page->buffer =
        device_->newBuffer(page_bytes, MTL::ResourceStorageModeShared);
    if (!page->buffer) {
      return false;
    }
  }
  page->offset = 0;
  command_buffer_pages.push_back(page);

  const size_t aligned_offset = align_up(page->offset, align);
  if (aligned_offset + bytes > page->bytes) {
    return false;
  }
  page->offset = aligned_offset + bytes;
  *buffer_out = page->buffer;
  *offset_out = NS::UInteger(aligned_offset);
  return *buffer_out != nullptr;
}

void MetalCommandProcessor::ScheduleSpirvArgumentBufferRelease(
    MTL::CommandBuffer* command_buffer) {
  // Cached addresses become invalid when these pages enter the recycling pool.
  argument_binder_.ResetUploadCaches();
  if (!command_buffer || (command_buffer_spirv_argbuf_pages_.empty() &&
                          command_buffer_tess_prep_pages_.empty())) {
    return;
  }

  std::vector<std::shared_ptr<SpirvArgumentBufferPage>> pages;
  pages.swap(command_buffer_spirv_argbuf_pages_);
  pages.insert(pages.end(), command_buffer_tess_prep_pages_.begin(),
               command_buffer_tess_prep_pages_.end());
  command_buffer_tess_prep_pages_.clear();

  bool add_handler = false;
  {
    std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
    auto& pending = pending_spirv_argbuf_releases_[command_buffer];
    add_handler = pending.empty();
    pending.reserve(pending.size() + pages.size());
    for (auto& page : pages) {
      pending.push_back(std::move(page));
    }
  }

  if (add_handler) {
    pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
    command_buffer->addCompletedHandler(
        [this](MTL::CommandBuffer* completed_cmd) {
          {
            std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
            auto it = pending_spirv_argbuf_releases_.find(completed_cmd);
            if (it != pending_spirv_argbuf_releases_.end()) {
              for (auto& page : it->second) {
                if (page) {
                  page->offset = 0;
                  auto& pool = page->gpu_written ? tess_prep_page_pool_
                                                 : spirv_argbuf_pool_;
                  pool.push_back(std::move(page));
                }
              }
              pending_spirv_argbuf_releases_.erase(it);
            }
          }
          pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
        });
  }
}

void MetalCommandProcessor::EndCommandBuffer(CommandBufferKind next_kind) {
  // Before the commit: export output of this command buffer goes under the
  // read watch while its owners can still name this submission.
  FlushMemexportWatchRanges();
  pipeline_cache_->FlushStorageIfNeeded();
  pipeline_cache_->MaybeSavePipelineArchive();
  if (current_command_buffer_) {
    next_submission_kind_ = next_kind;
  }
  primary_end_commit_at_pass_end_ = false;
  if (next_kind == CommandBufferKind::kSubmissionWait) {
    commit_after_drain_armed_ = true;
  } else if (current_command_buffer_) {
    commit_after_drain_armed_ = false;
  }
  EndRenderEncoder();

  if (current_command_buffer_) {
    // Texture loads that need nothing from this command buffer run before it.
    if (texture_cache_) {
      texture_cache_->CommitLoadsAhead();
    }
    CommitTessPrepAhead();
    ScheduleSpirvArgumentBufferRelease(current_command_buffer_);
    if (!adaptive_fence_tests_.empty()) {
      AdaptiveJudgeFenceTests();
    }
    current_command_buffer_->commit();
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    current_draw_index_ = 0;
    if (shared_memory_ && shared_memory_->hazards()) {
      shared_memory_->hazards()->Retire();
    }
    // Resolves recorded into it can now be waited for from a fault, so the
    // watches reads racing them used up are armed again.
    resolve_submitted_through_.store(submission_current_,
                                     std::memory_order_release);
    OnResolveSubmissionEnded();
  }
  copy_resolve_writes_pending_ = false;
  DrainCommandBufferAutoreleasePool();
}

void MetalCommandProcessor::ApplyDepthStencilState(
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
  SCOPE_profile_cpu_f("gpu");
  if (!current_render_encoder_ || !device_) {
    return;
  }

  const RegisterFile& regs = *register_file_;
  auto stencil_ref_mask_front = regs.Get<reg::RB_STENCILREFMASK>();
  auto stencil_ref_mask_back =
      regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto depth_control = normalized_depth_control;

  bool has_depth_attachment = false;
  bool has_stencil_attachment = false;
  if (current_render_pass_descriptor_) {
    if (auto* depth_attachment =
            current_render_pass_descriptor_->depthAttachment()) {
      has_depth_attachment = depth_attachment->texture() != nullptr;
    }
    if (auto* stencil_attachment =
            current_render_pass_descriptor_->stencilAttachment()) {
      has_stencil_attachment = stencil_attachment->texture() != nullptr;
    }
  }

  // Without a depth attachment, the depth test passes and nothing is written
  // either way, but Metal rejects a state that enables either.
  if (!has_depth_attachment) {
    depth_control.z_enable = 0;
    depth_control.z_write_enable = 0;
  }

  if (!has_stencil_attachment && depth_control.stencil_enable) {
    static bool no_stencil_logged = false;
    if (!no_stencil_logged) {
      no_stencil_logged = true;
      XELOGW(
          "Metal: stencil enabled but no stencil attachment bound; disabling "
          "stencil for this pass");
    }
    depth_control.stencil_enable = 0;
    depth_control.backface_enable = 0;
    depth_control.stencilfunc = xenos::CompareFunction::kAlways;
    depth_control.stencilfail = xenos::StencilOp::kKeep;
    depth_control.stencilzpass = xenos::StencilOp::kKeep;
    depth_control.stencilzfail = xenos::StencilOp::kKeep;
    depth_control.stencilfunc_bf = xenos::CompareFunction::kAlways;
    depth_control.stencilfail_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzpass_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzfail_bf = xenos::StencilOp::kKeep;
    stencil_ref_mask_front.value = 0;
    stencil_ref_mask_back.value = 0;
  }

  DepthStencilStateKey key;
  key.depth_control = depth_control.value;
  key.stencil_ref_mask_front = stencil_ref_mask_front.value;
  key.stencil_ref_mask_back = stencil_ref_mask_back.value;
  key.polygonal_and_backface = (primitive_polygonal ? 1u : 0u) |
                               (depth_control.backface_enable ? 2u : 0u);

  MTL::DepthStencilState* state = nullptr;
  auto it = depth_stencil_state_cache_.find(key);
  if (it != depth_stencil_state_cache_.end()) {
    state = it->second;
  } else {
    MTL::DepthStencilDescriptor* ds_desc =
        MTL::DepthStencilDescriptor::alloc()->init();
    if (depth_control.z_enable) {
      ds_desc->setDepthCompareFunction(
          ToMetalCompareFunction(depth_control.zfunc));
      ds_desc->setDepthWriteEnabled(depth_control.z_write_enable != 0);
    } else {
      ds_desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
      ds_desc->setDepthWriteEnabled(false);
    }

    if (depth_control.stencil_enable) {
      auto* front = MTL::StencilDescriptor::alloc()->init();
      front->setStencilCompareFunction(
          ToMetalCompareFunction(depth_control.stencilfunc));
      front->setStencilFailureOperation(
          ToMetalStencilOperation(depth_control.stencilfail));
      front->setDepthFailureOperation(
          ToMetalStencilOperation(depth_control.stencilzfail));
      front->setDepthStencilPassOperation(
          ToMetalStencilOperation(depth_control.stencilzpass));
      front->setReadMask(stencil_ref_mask_front.stencilmask);
      front->setWriteMask(stencil_ref_mask_front.stencilwritemask);

      ds_desc->setFrontFaceStencil(front);

      if (primitive_polygonal && depth_control.backface_enable) {
        auto* back = MTL::StencilDescriptor::alloc()->init();
        back->setStencilCompareFunction(
            ToMetalCompareFunction(depth_control.stencilfunc_bf));
        back->setStencilFailureOperation(
            ToMetalStencilOperation(depth_control.stencilfail_bf));
        back->setDepthFailureOperation(
            ToMetalStencilOperation(depth_control.stencilzfail_bf));
        back->setDepthStencilPassOperation(
            ToMetalStencilOperation(depth_control.stencilzpass_bf));
        back->setReadMask(stencil_ref_mask_back.stencilmask);
        back->setWriteMask(stencil_ref_mask_back.stencilwritemask);
        ds_desc->setBackFaceStencil(back);
        back->release();
      } else {
        ds_desc->setBackFaceStencil(front);
      }

      front->release();
    }

    state = device_->newDepthStencilState(ds_desc);
    ds_desc->release();

    if (!state) {
      XELOGE("Failed to create Metal depth/stencil state");
      return;
    }
    depth_stencil_state_cache_.emplace(key, state);
  }

  if (state != msl_depth_stencil_state_) {
    current_render_encoder_->setDepthStencilState(state);
    msl_depth_stencil_state_ = state;
  }

  if (depth_control.stencil_enable) {
    uint32_t ref_front = stencil_ref_mask_front.stencilref;
    uint32_t ref_back = primitive_polygonal && depth_control.backface_enable
                            ? stencil_ref_mask_back.stencilref
                            : ref_front;
    uint32_t reference = ref_front | (ref_back << 8);
    if (!msl_stencil_reference_valid_ || msl_stencil_reference_ != reference) {
      current_render_encoder_->setStencilReferenceValues(ref_front, ref_back);
      msl_stencil_reference_valid_ = true;
      msl_stencil_reference_ = reference;
    }
  }
}

void MetalCommandProcessor::ApplyRasterizerState(bool primitive_polygonal) {
  SCOPE_profile_cpu_f("gpu");
  if (!current_render_encoder_ || !render_target_cache_) {
    return;
  }

  const RegisterFile& regs = *register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  MTL::CullMode cull_mode = MTL::CullModeNone;
  if (primitive_polygonal) {
    bool cull_front = pa_su_sc_mode_cntl.cull_front;
    bool cull_back = pa_su_sc_mode_cntl.cull_back;
    if (cull_front && !cull_back) {
      cull_mode = MTL::CullModeFront;
    } else if (cull_back && !cull_front) {
      cull_mode = MTL::CullModeBack;
    }
  }
  MTL::Winding winding = pa_su_sc_mode_cntl.face ? MTL::WindingClockwise
                                                 : MTL::WindingCounterClockwise;

  MTL::TriangleFillMode fill_mode = MTL::TriangleFillModeFill;
  if (primitive_polygonal &&
      pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
    xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
    if (!pa_su_sc_mode_cntl.cull_front) {
      polygon_type =
          std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
    }
    if (!pa_su_sc_mode_cntl.cull_back) {
      polygon_type =
          std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
    }
    if (polygon_type != xenos::PolygonType::kTriangles) {
      fill_mode = MTL::TriangleFillModeLines;
    }
  }

  float polygon_offset_scale = 0.0f;
  float polygon_offset = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(
      regs, primitive_polygonal, polygon_offset_scale, polygon_offset);
  float depth_bias_constant =
      static_cast<float>(draw_util::GetD3D10IntegerPolygonOffset(
          regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset));
  float depth_bias_slope =
      polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit *
      float(std::max(render_target_cache_->draw_resolution_scale_x(),
                     render_target_cache_->draw_resolution_scale_y()));
  if (MetalPipelineCache::NeedsExactFloatDepthBias(regs)) {
    depth_bias_constant = depth_bias_slope = 0.0f;
  }

  MTL::DepthClipMode depth_clip_mode =
      draw_util::IsDepthClampedAtZPlanes(regs) ? MTL::DepthClipModeClamp
                                               : MTL::DepthClipModeClip;

  // Encoder state persists until changed, so only what differs from the last
  // draw in this encoder is set.
  bool all = !msl_rasterizer_state_valid_;
  if (all || cull_mode != msl_cull_mode_) {
    current_render_encoder_->setCullMode(cull_mode);
  }
  if (all || winding != msl_winding_) {
    current_render_encoder_->setFrontFacingWinding(winding);
  }
  if (all || fill_mode != msl_fill_mode_) {
    current_render_encoder_->setTriangleFillMode(fill_mode);
  }
  if (all || depth_bias_constant != msl_depth_bias_constant_ ||
      depth_bias_slope != msl_depth_bias_slope_) {
    current_render_encoder_->setDepthBias(depth_bias_constant, depth_bias_slope,
                                          0.0f);
  }
  if (all || depth_clip_mode != msl_depth_clip_mode_) {
    current_render_encoder_->setDepthClipMode(depth_clip_mode);
  }
  msl_rasterizer_state_valid_ = true;
  msl_cull_mode_ = cull_mode;
  msl_winding_ = winding;
  msl_fill_mode_ = fill_mode;
  msl_depth_bias_constant_ = depth_bias_constant;
  msl_depth_bias_slope_ = depth_bias_slope;
  msl_depth_clip_mode_ = depth_clip_mode;
}

MTL::RenderPassDescriptor*
MetalCommandProcessor::GetCurrentRenderPassDescriptor() {
  return render_pass_descriptor_;
}

void MetalCommandProcessor::UpdateSpirvSystemConstantValues(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  SCOPE_profile_cpu_f("gpu");
  const SpirvShaderTranslator::SystemConstants previous_system_constants =
      spirv_system_constants_;

  const RegisterFile& regs = *register_file_;
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  auto& consts = spirv_system_constants_;
  std::memset(&consts, 0, sizeof(consts));

  if (MetalPipelineCache::NeedsExactFloatDepthBias(regs)) {
    auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    float scale =
        xenos::kPolygonOffsetScaleSubpixelUnit *
        float(std::max(render_target_cache_->draw_resolution_scale_x(),
                       render_target_cache_->draw_resolution_scale_y()));
    if (mode.poly_offset_front_enable) {
      consts.edram_poly_offset_front_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE) * scale;
      consts.edram_poly_offset_front_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET) * 0.5f;
    }
    if (mode.poly_offset_back_enable) {
      consts.edram_poly_offset_back_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE) * scale;
      consts.edram_poly_offset_back_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET) * 0.5f;
    }
  }

  // Build flags (matching Vulkan backend's SpirvShaderTranslator kSysFlag_*).
  uint32_t flags = 0;

  // Coordinate format.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }

  // Primitive type.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineList ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineStrip ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineLoop ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::k2DLineStrip) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }

  // MSAA sample count.
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;

  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }

  // Alpha test — pack the CompareFunction value directly into the flag bits
  // (matching Vulkan backend behavior).
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;

  // Gamma correction for render targets. RGBA16Unorm gamma targets contain
  // linear values; their linear-to-gamma conversion happens on the EDRAM
  // store, so the pixel shader must not pre-encode them.
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
  }
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }

  // Vertex index loading for VS-based primitive expansion (point sprites,
  // rectangle lists).  When the primitive processor builds a host-side
  // index buffer for DMA-based VS expansion the shader must load the
  // original guest vertex index from shared memory.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator::
          kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
  }

  consts.flags = flags;

  // Vertex index.
  consts.vertex_index_endian = index_endian;
  consts.vertex_base_index = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  const bool is_vs_expansion_draw =
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
  consts.vertex_index_count =
      is_vs_expansion_draw ? primitive_processing_result.guest_draw_vertex_count
                           : primitive_processing_result.host_draw_vertex_count;

  // Vertex index load address (for VS-based primitive expansion).
  if (flags &
      (SpirvShaderTranslator::kSysFlag_VertexIndexLoad |
       SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    consts.vertex_index_load_address =
        primitive_processing_result.guest_index_base;
  }

  // NDC scale/offset.
  for (uint32_t i = 0; i < 3; ++i) {
    consts.ndc_scale[i] = viewport_info.ndc_scale[i];
    consts.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point rendering (matching Vulkan backend).
  auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
  auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
  consts.point_vertex_diameter_min =
      float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
  consts.point_vertex_diameter_max =
      float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
  consts.point_constant_diameter[0] =
      float(pa_su_point_size.width) * (2.0f / 16.0f);
  consts.point_constant_diameter[1] =
      float(pa_su_point_size.height) * (2.0f / 16.0f);
  // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
  // to radius conversion — matching the Vulkan backend formula.
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  consts.point_screen_diameter_to_ndc_radius[0] =
      float(draw_resolution_scale_x) /
      float(std::max(viewport_info.xy_extent[0], uint32_t(1)));
  // Metal NDC Y increases upward; point T increases downward.
  consts.point_screen_diameter_to_ndc_radius[1] =
      -float(draw_resolution_scale_y) /
      float(std::max(viewport_info.xy_extent[1], uint32_t(1)));

  // Texture swizzled signs and swizzles — retrieved from the texture cache
  // (matching Vulkan backend behavior).
  if (texture_cache_) {
    for (uint32_t i = 0; i < 32; ++i) {
      if (!(used_texture_mask & (uint32_t(1) << i))) {
        continue;
      }
      // Swizzled signs: 8 bits per texture, 4 textures per uint32.
      uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(i);
      uint32_t signs_shift = 8 * (i & 3);
      consts.texture_swizzled_signs[i >> 2] |= uint32_t(texture_signs)
                                               << signs_shift;

      consts.texture_integer_scale_bits[i] =
          texture_cache_->GetActiveIntegerScaleBits(i);

      // Epsilon over width and height for the normalized 2D and cube fetches.
      if (pipeline_cache_->spirv_translator_features()
              .texture_coord_epsilon_constants) {
        const uint32_t texture_size_word =
            regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i * 6 + 2];
        const uint32_t size_mask =
            (UINT32_C(1) << xenos::kTexture2DCubeMaxWidthHeightLog2) - 1;
        const float width = float((texture_size_word & size_mask) + 1);
        const float height = float(
            ((texture_size_word >> xenos::kTexture2DCubeMaxWidthHeightLog2) &
             size_mask) +
            1);
        consts.texture_coord_epsilon_over_size[i >> 1][(i & 1) * 2] =
            kTextureCoordEpsilon / width;
        consts.texture_coord_epsilon_over_size[i >> 1][(i & 1) * 2 + 1] =
            kTextureCoordEpsilon / height;
      }

      // Fetch constant LOD bias and gradient exponent biases, decoded from
      // the word 4 the way the shader would (exact conversions).
      if (pipeline_cache_->spirv_translator_features()
              .predecoded_texture_fetch_lod_gradient) {
        const uint32_t word_4 =
            regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i * 6 + 4];
        consts.texture_fetch_lod_gradient[i][0] =
            float(int32_t(word_4 << 10) >> 22) * (1.0f / 32.0f);
        consts.texture_fetch_lod_gradient[i][1] =
            float(int32_t(word_4 << 5) >> 27);
        consts.texture_fetch_lod_gradient[i][2] = float(int32_t(word_4) >> 27);
      }

      // Host swizzles: 12 bits per texture, 2 textures per uint32.
      uint32_t texture_swizzle = texture_cache_->GetActiveTextureHostSwizzle(i);
      uint32_t swizzle_shift = 12 * (i & 1);
      consts.texture_swizzles[i >> 1] |= (texture_swizzle & 0xFFF)
                                         << swizzle_shift;

      // Integer num_format scale, plus bit 24 for normalized rounding.
      consts.texture_integer_scale_bits[i] =
          texture_cache_->GetActiveIntegerScaleBits(i);
    }
  }

  // Textures resolved — which textures are from scaled resolve operations
  // (matching Vulkan backend).
  if (texture_cache_) {
    uint32_t textures_resolved = 0;
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      textures_resolved |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
    consts.textures_resolved = textures_resolved;
  }

  // Alpha test reference.
  consts.alpha_test_reference = rb_alpha_ref;

  // Alpha to mask — if enabled, bits 0:7 are sample offsets, bit 8 = 1.
  // (matching Vulkan backend / MSC path).
  if (rb_colorcontrol.alpha_to_mask_enable) {
    consts.alpha_to_mask = (rb_colorcontrol.value >> 24) | (1 << 8);
  }

  // Color exponent bias (matching Vulkan backend).
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t color_exp_bias = color_infos[i].color_exp_bias;
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    if (render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        ((color_infos[i].color_format ==
              xenos::ColorRenderTargetFormat::k_16_16 &&
          !render_target_cache_->IsFixedRG16TruncatedToMinus1To1()) ||
         (color_infos[i].color_format ==
              xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
          !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1()))) {
      if (render_target_cache_->fixed16_fetch_blend()) {
        // The shader encodes the EDRAM's bits itself: a negative scale marks
        // the target (SpirvShaderTranslator::Features::
        // host_render_target_fixed16).
        color_exp_bias_scale = -color_exp_bias_scale;
      } else {
        // Remap to -1...1 by scaling the output values by 1024/32767, so the
        // SNORM bits are the EDRAM ones apart from -32.
        color_exp_bias_scale *= 1024.0f / 32767.0f;
      }
    }
    consts.color_exp_bias[i] = color_exp_bias_scale;
  }

  // The blend constant of targets blended in the pixel shader. The pipeline
  // specializes their blend controls.
  if (pipeline_cache_->spirv_translator_features()
          .host_render_target_feedback) {
    bool shader_blends = false;
    for (uint32_t i = 0; i < 4; ++i) {
      shader_blends |= MetalPipelineCache::GetShaderBlendControl(
                           regs, i, normalized_color_mask,
                           render_target_cache_->gamma_fetch_blend(),
                           render_target_cache_->fixed16_fetch_blend()) !=
                       MetalPipelineCache::kBlendControlNoBlending;
    }
    if (shader_blends) {
      consts.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
      consts.edram_blend_constant[1] =
          regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
      consts.edram_blend_constant[2] =
          regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
      consts.edram_blend_constant[3] =
          regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    }
  }

  // User clip planes and tessellation constants in the system constants.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  std::memset(spirv_system_constants_.user_clip_planes, 0,
              sizeof(spirv_system_constants_.user_clip_planes));
  if (!pa_cl_clip_cntl.clip_disable && pa_cl_clip_cntl.ucp_ena) {
    float* clip_plane_write_ptr = spirv_system_constants_.user_clip_planes[0];
    uint32_t clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t clip_plane_index;
    while (xe::bit_scan_forward(clip_planes_remaining, &clip_plane_index)) {
      clip_planes_remaining &= ~(UINT32_C(1) << clip_plane_index);
      const float* clip_plane_regs = reinterpret_cast<const float*>(
          &regs.values[XE_GPU_REG_PA_CL_UCP_0_X + clip_plane_index * 4]);
      std::memcpy(clip_plane_write_ptr, clip_plane_regs, 4 * sizeof(float));
      clip_plane_write_ptr += 4;
    }
  }
  spirv_system_constants_.tessellation_factor_range[0] =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  spirv_system_constants_.tessellation_factor_range[1] =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  spirv_system_constants_.tessellation_vertex_index_endian =
      static_cast<uint32_t>(index_endian);
  spirv_system_constants_.tessellation_vertex_index_offset =
      regs[XE_GPU_REG_VGT_INDX_OFFSET];
  spirv_system_constants_.tessellation_vertex_index_min_max[0] =
      regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
  spirv_system_constants_.tessellation_vertex_index_min_max[1] =
      regs[XE_GPU_REG_VGT_MAX_VTX_INDX];

  std::memcpy(spirv_system_constants_.draw_exact_estimates,
              pipeline_cache_->draw_exact_estimates(),
              MetalPipelineCache::kDrawExactEstimatesSize);

  // The system constants version bump covers clip planes and tessellation,
  // which live in the same struct.
  if (std::memcmp(&previous_system_constants, &spirv_system_constants_,
                  sizeof(spirv_system_constants_)) != 0) {
    ++msl_system_constants_version_;
    if (msl_system_constants_version_ == 0) {
      msl_system_constants_version_ = 1;
    }
  }
}

#define COMMAND_PROCESSOR MetalCommandProcessor
#include "../pm4_command_processor_implement.h"
#undef COMMAND_PROCESSOR

}  // namespace metal
}  // namespace gpu
}  // namespace xe
