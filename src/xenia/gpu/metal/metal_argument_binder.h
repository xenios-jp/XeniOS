/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_ARGUMENT_BINDER_H_
#define XENIA_GPU_METAL_METAL_ARGUMENT_BINDER_H_

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "xenia/gpu/metal/metal_upload_cache.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;

// Binds one draw's resources for the spirv2air (and MSC fallback) guest path.
// Shaders take every root parameter from a top-level argument buffer at
// buffer(2) and reach textures and samplers by indexing two descriptor heaps
// at buffer(0) and buffer(1), so nothing lands in a Metal texture or sampler
// slot.
class MetalArgumentBinder {
 public:
  // Buffer bind points shared by spirv2air's default graphics layout and MSC.
  static constexpr uint32_t kDescriptorHeapBufferIndex = 0;
  static constexpr uint32_t kSamplerHeapBufferIndex = 1;
  static constexpr uint32_t kArgumentBufferIndex = 2;

  // Top-level argument buffer slots (each an 8-byte GPU virtual address at
  // byte offset index * 8), matching spirv2air/docs/bindings.md and MSC's
  // guest root signature.
  enum class ArgumentSlot : uint32_t {
    kSystemConstants = 0,       // byte 0  (CBV b0, space1)
    kFloatConstantsVertex = 1,  // byte 8  (CBV b1, space1)
    kFloatConstantsPixel = 2,   // byte 16 (CBV b2, space1)
    kBoolLoopConstants = 3,     // byte 24 (CBV b3, space1)
    kFetchConstants = 4,        // byte 32 (CBV b4, space1)
    kRuntimeData = 5,           // byte 40 (CBV b0, space31)
    kSharedMemorySrv = 6,       // byte 48 (SRV t0, space0)
    kSharedMemoryUav = 7,       // byte 56 (UAV u0, space0)
    kTextureIndicesVertex = 8,  // byte 64 (SRV t2, space0)
    kTextureIndicesPixel = 9,   // byte 72 (SRV t3, space0)
    kTextureRangeVertex = 10,   // byte 80 (SRV table, space2)
    kTextureRangePixel = 11,    // byte 88 (SRV table, space3)
    kSamplerRangeVertex = 12,   // byte 96 (Sampler table, space2)
    kSamplerRangePixel = 13,    // byte 104 (Sampler table, space3)
    kCount = 14,
  };

  // 24-byte descriptor heap record shared by spirv2air and MSC.
  struct DescriptorEntry {
    uint64_t gpu_va = 0;
    uint64_t texture_view_id = 0;
    uint64_t metadata = 0;
  };
  static_assert(sizeof(DescriptorEntry) == 24,
                "DescriptorEntry must match spirv2air/MSC 24-byte record");

  // One constant buffer's contents, copied into a transient slice whose GPU
  // address goes into the argument buffer.
  struct ConstantBlock {
    const void* data = nullptr;
    uint32_t size = 0;
    uint64_t revision = 0;
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

  explicit MetalArgumentBinder(MetalCommandProcessor& command_processor);
  ~MetalArgumentBinder();

  // Writes this draw's descriptor heaps, per-stage index buffers and top-level
  // argument buffer, binds them to the stages the pipeline runs and makes
  // everything they reference resident.
  bool Bind(MTL::RenderCommandEncoder* encoder,
            const SpirvShader* vertex_shader, const SpirvShader* pixel_shader,
            const Constants& constants, bool memexport_used,
            uint32_t vertex_buffer_mask, uint32_t pixel_buffer_mask);

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
    // buffer->gpuAddress() + offset, taken once when the slice is written.
    uint64_t gpu_address = 0;
  };
  static constexpr uint32_t kConstantCount = 5;
  // Keyed by the block size and the revision the command processor bumps
  // whenever the contents change.
  using ConstantKey = std::pair<uint32_t, uint64_t>;
  std::array<UploadCache<Slice, ConstantKey>, kConstantCount> constant_slices_;
  // Complete heap starts and binding counts for one stage. Equal keys imply
  // equal index-buffer bytes, including the all-zero no-binding placeholder.
  using StageIndexKey = std::array<uint32_t, 4>;
  std::array<UploadCache<Slice, StageIndexKey>, kStageCount>
      stage_index_slices_;
  bool descriptor_heap_slices_valid_ = false;
  std::vector<DescriptorEntry> cached_texture_heap_entries_;
  std::vector<DescriptorEntry> cached_sampler_heap_entries_;
  Slice cached_texture_heap_slice_;
  Slice cached_sampler_heap_slice_;
  using ArgumentBufferKey = std::array<uint64_t, size_t(ArgumentSlot::kCount)>;
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
  // The fetch constants the gathered stages' textures and samplers use, and
  // their words at the gather, which the descriptors (the samplers in
  // particular) are a function of.
  uint32_t gather_cache_fetch_mask_ = 0;
  std::array<uint32_t, xenos::kTextureFetchConstantCount * 6>
      gather_cache_fetch_words_{};

  // Copies size bytes into a fresh transient slice, zero filling when there is
  // nothing to copy so the slot still points at readable memory.
  bool Upload(const void* data, uint32_t size, Slice& slice_out);
  // Appends a stage's textures and samplers to the heap entries.
  bool GatherStage(const SpirvShader* shader, StageRange& range_out);

  MetalCommandProcessor& command_processor_;

  // Rebuilt per draw. Members rather than locals to keep the allocations.
  std::vector<DescriptorEntry> texture_heap_entries_;
  std::vector<DescriptorEntry> sampler_heap_entries_;
  std::vector<MTL::Texture*> resident_textures_;
  std::vector<uint32_t> stage_indices_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_ARGUMENT_BINDER_H_
