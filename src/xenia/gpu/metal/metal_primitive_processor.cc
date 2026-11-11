/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_primitive_processor.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_command_processor.h"

namespace xe {
namespace gpu {
namespace metal {

MetalPrimitiveProcessor::MetalPrimitiveProcessor(
    MetalCommandProcessor& command_processor, const RegisterFile& register_file,
    Memory& memory, TraceWriter& trace_writer, SharedMemory& shared_memory)
    : PrimitiveProcessor(register_file, memory, trace_writer, shared_memory),
      command_processor_(command_processor) {}

MetalPrimitiveProcessor::~MetalPrimitiveProcessor() { Shutdown(true); }

bool MetalPrimitiveProcessor::Initialize() {
  // When using SPIRV-Cross (no mesh shaders / MSC), point sprites and
  // rectangle lists must be expanded in the vertex shader because there
  // are no geometry shaders available.  The SpirvShaderTranslator has
  // built-in support for kPointListAsTriangleStrip and
  // kRectangleListAsTriangleStrip host vertex shader types.
  if (!InitializeCommon(true,   // full_32bit_vertex_indices_supported
                        false,  // triangle_fans_supported (will convert)
                        false,  // line_loops_supported (will convert)
                        false,  // quad_lists_supported (will convert)
                        false,  // point_sprites_without_expansion
                        false,  // rectangle_lists_without_expansion
                        true)) {  // primitive_reset_always_enabled
    Shutdown();
    return false;
  }

  XELOGI("MetalPrimitiveProcessor initialized");

  frame_index_buffer_pool_ = std::make_unique<ui::metal::MetalUploadBufferPool>(
      command_processor_.GetMetalDevice(), "Xenia Converted Index Buffer",
      std::max(size_t(kMinRequiredConvertedIndexBufferSize),
               ui::GraphicsUploadBufferPool::kDefaultPageSize));

  {
    // The generic primitive processor emits restart-separated triangle strips
    // for VS expansion. Keep a no-restart triangle-list fallback for Metal
    // SPIRV-Cross draws in case strip restart semantics diverge.
    constexpr uint32_t kMaxExpandedPrimitiveCount = UINT16_MAX;
    constexpr uint32_t kIndicesPerExpandedPrimitive = 6;
    size_t index_count =
        size_t(kMaxExpandedPrimitiveCount) * kIndicesPerExpandedPrimitive;
    size_t buffer_size_bytes = index_count * sizeof(uint32_t);
    MTL::Device* device = command_processor_.GetMetalDevice();
    expansion_triangle_list_index_buffer_ =
        device->newBuffer(buffer_size_bytes, MTL::ResourceStorageModeShared);
    if (!expansion_triangle_list_index_buffer_) {
      XELOGE(
          "Failed to create Metal expansion triangle-list fallback index "
          "buffer");
      Shutdown();
      return false;
    }
    expansion_triangle_list_index_buffer_->setLabel(NS::String::string(
        "Xenia Expansion Triangle List Index Buffer", NS::UTF8StringEncoding));
    uint32_t* indices = reinterpret_cast<uint32_t*>(
        expansion_triangle_list_index_buffer_->contents());
    for (uint32_t i = 0; i < kMaxExpandedPrimitiveCount; ++i) {
      uint32_t base = i << 2;
      size_t write_index = size_t(i) * kIndicesPerExpandedPrimitive;
      indices[write_index + 0] = base + 0;
      indices[write_index + 1] = base + 1;
      indices[write_index + 2] = base + 2;
      indices[write_index + 3] = base + 2;
      indices[write_index + 4] = base + 1;
      indices[write_index + 5] = base + 3;
    }
  }
  return true;
}

void MetalPrimitiveProcessor::Shutdown(bool from_destructor) {
  converted_index_buffers_.clear();
  frame_index_buffer_pool_.reset();

  // Release built-in index buffer
  if (builtin_index_buffer_) {
    builtin_index_buffer_->release();
    builtin_index_buffer_ = nullptr;
    builtin_index_buffer_gpu_address_ = 0;
    builtin_index_buffer_size_ = 0;
  }
  if (expansion_triangle_list_index_buffer_) {
    expansion_triangle_list_index_buffer_->release();
    expansion_triangle_list_index_buffer_ = nullptr;
  }

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void MetalPrimitiveProcessor::CompletedSubmissionUpdated() {
  SCOPE_profile_cpu_f("gpu");
  // Nothing to do for Metal
}

void MetalPrimitiveProcessor::BeginSubmission() {
  SCOPE_profile_cpu_f("gpu");
  // Nothing to do for Metal
}

void MetalPrimitiveProcessor::BeginFrame() {
  SCOPE_profile_cpu_f("gpu");
  frame_index_buffer_pool_->Reclaim(command_processor_.GetCompletedFrame());
}

void MetalPrimitiveProcessor::EndFrame() {
  SCOPE_profile_cpu_f("gpu");
  ClearPerFrameCache();
  converted_index_buffers_.clear();
}

MTL::Buffer* MetalPrimitiveProcessor::GetConvertedIndexBuffer(
    size_t handle, uint64_t& offset_bytes_out) const {
  if (handle >= converted_index_buffers_.size()) {
    XELOGE("Converted index buffer handle {} is out of range {}", handle,
           converted_index_buffers_.size());
    offset_bytes_out = 0;
    return nullptr;
  }

  const ConvertedIndexBufferBinding& binding = converted_index_buffers_[handle];
  offset_bytes_out = binding.offset_bytes;
  return binding.buffer;
}

bool MetalPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_null(builtin_index_buffer_);

  MTL::Device* device = command_processor_.GetMetalDevice();

  // Create buffer with shared storage so we can write to it
  builtin_index_buffer_ =
      device->newBuffer(size_bytes, MTL::ResourceStorageModeShared);
  if (!builtin_index_buffer_) {
    XELOGE("Failed to create Metal built-in index buffer");
    return false;
  }
  builtin_index_buffer_size_ = size_bytes;

  builtin_index_buffer_->setLabel(NS::String::string(
      "Xenia Built-in Index Buffer", NS::UTF8StringEncoding));

  // Fill the buffer with built-in indices
  void* buffer_data = builtin_index_buffer_->contents();
  fill_callback(buffer_data);

  // Get GPU address for binding
  builtin_index_buffer_gpu_address_ = builtin_index_buffer_->gpuAddress();

  XELOGI("Created Metal built-in index buffer ({} bytes)", size_bytes);
  return true;
}

void* MetalPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  SCOPE_profile_cpu_f("gpu");
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t)
                                                           : sizeof(uint32_t);
  MTL::Buffer* buffer;
  size_t offset;
  uint8_t* mapping = frame_index_buffer_pool_->Request(
      command_processor_.GetCurrentFrame(),
      index_size * index_count +
          (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0),
      index_size, buffer, offset);
  if (!mapping) {
    return nullptr;
  }
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset =
        GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    offset += coalignment_offset;
  }
  backend_handle_out = converted_index_buffers_.size();
  converted_index_buffers_.push_back({buffer, uint64_t(offset)});
  return mapping;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
