/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_DXIL_BINDER_H_
#define XENIA_GPU_METAL_METAL_DXIL_BINDER_H_

#include <array>
#include <cstdint>
#include <vector>

#include "xenia/gpu/metal/metal_shader_converter.h"
#include "xenia/gpu/metal/metal_upload_cache.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/ui/metal/metal_api.h"

struct IRDescriptorTableEntry;

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;

// Binds one draw's resources for the SPIR-V -> DXIL -> AIR path. Metal Shader
// Converter shaders take every root parameter from a top-level argument buffer
// and reach textures and samplers by indexing two descriptor heaps, so nothing
// lands in a Metal texture or sampler slot.
class MetalDxilBinder {
 public:
  // One constant buffer's contents, copied into a transient slice whose GPU
  // address goes into the argument buffer.
  struct ConstantBlock {
    const void* data = nullptr;
    uint32_t size = 0;
  };
  // The guest constant buffers. The runtime data buffer is not here: the guest
  // shaders never read it, so it gets a zeroed placeholder.
  struct Constants {
    ConstantBlock system;
    ConstantBlock float_vertex;
    ConstantBlock float_pixel;
    ConstantBlock bool_loop;
    ConstantBlock fetch;
  };

  // Out of line so IRDescriptorTableEntry only has to be complete in the
  // implementation.
  MetalDxilBinder(MetalCommandProcessor& command_processor,
                  const MetalShaderConverter& converter);
  ~MetalDxilBinder();

  // Writes this draw's descriptor heaps, per-stage index buffers and top-level
  // argument buffer, binds them to the stages the pipeline runs and makes
  // everything they reference resident.
  bool Bind(MTL::RenderCommandEncoder* encoder,
            const SpirvShader* vertex_shader, const SpirvShader* pixel_shader,
            const Constants& constants, bool memexport_used, bool tessellated);

  // Drops every reused upload slice. Call before the transient argument pages
  // they live in can be recycled.
  void ResetUploadCaches();

 private:
  static constexpr uint32_t kStageVertex = 0;
  static constexpr uint32_t kStagePixel = 1;
  static constexpr uint32_t kStageCount = 2;
  // Slices are bound as buffer offsets and read back as constant buffer
  // addresses, so 256 covers both alignment requirements on every Mac.
  static constexpr uint32_t kSliceAlignment = 256;

  struct Slice {
    MTL::Buffer* buffer = nullptr;
    NS::UInteger offset = 0;
  };
  bool descriptor_heap_slices_valid_ = false;
  std::vector<IRDescriptorTableEntry> cached_texture_heap_entries_;
  std::vector<IRDescriptorTableEntry> cached_sampler_heap_entries_;
  Slice cached_texture_heap_slice_;
  Slice cached_sampler_heap_slice_;
  using ArgumentBufferKey =
      std::array<uint64_t, size_t(MetalRootParameter::kCount)>;
  UploadCache<Slice, ArgumentBufferKey> argument_buffer_slices_;

  // Where a stage's descriptors start in the heaps both stages share.
  struct StageRange {
    uint32_t texture_start = 0;
    uint32_t texture_count = 0;
    uint32_t sampler_start = 0;
    uint32_t sampler_count = 0;
  };
  struct GatherKey {
    const SpirvShader* vertex_shader = nullptr;
    const SpirvShader* pixel_shader = nullptr;
    uint64_t binding_state_generation = 0;
    bool operator==(const GatherKey&) const = default;
  };
  bool gather_cache_valid_ = false;
  GatherKey gather_cache_key_;
  std::array<StageRange, kStageCount> gather_cache_ranges_;

  // Copies size bytes into a fresh transient slice, zero filling when there is
  // nothing to copy so the slot still points at readable memory.
  bool Upload(const void* data, uint32_t size, Slice& slice_out);
  // Appends a stage's textures and samplers to the heap entries.
  bool GatherStage(const SpirvShader* shader, StageRange& range_out);

  MetalCommandProcessor& command_processor_;
  const MetalShaderConverter& converter_;

  // Rebuilt per draw. Members rather than locals to keep the allocations.
  std::vector<IRDescriptorTableEntry> texture_heap_entries_;
  std::vector<IRDescriptorTableEntry> sampler_heap_entries_;
  std::vector<MTL::Texture*> resident_textures_;
  std::vector<uint32_t> stage_indices_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_DXIL_BINDER_H_
