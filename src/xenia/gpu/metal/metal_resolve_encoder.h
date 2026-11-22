/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_RESOLVE_ENCODER_H_
#define XENIA_GPU_METAL_METAL_RESOLVE_ENCODER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "third_party/metal-cpp/Metal/Metal.hpp"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/metal/metal_texture_cache.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;
class MetalRenderTargetCache;

// A tile resolve's refresh of textures an earlier draw of the pass sampled,
// written to a scratch texture of their shape in the pass and copied into them
// when it ends (or swapped in directly when every texel was written).
struct PostPassRefreshCopy {
  // Retained, back to refresh_scratch_pool_ after the copy is encoded.
  MTL::Texture* scratch = nullptr;
  std::vector<NS::SharedPtr<MTL::Texture>> destinations;
  uint32_t origin_x = 0;
  uint32_t origin_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  struct Swap {
    // The texture's refresh write view, by which it is found.
    NS::SharedPtr<MTL::Texture> write_view;
    std::unique_ptr<MetalTextureCache::ResolveRefreshStorage> storage;
  };
  std::vector<Swap> swaps;
  // The resolves left to the pass end before it, which run first.
  size_t post_pass_resolves_before = 0;
};

// Stateless-with-respect-to-pass-state encoder for Metal EDRAM resolves:
//   - Direct compute resolves reading host render targets directly.
//   - Open-pass tile-shader resolves reading on-chip tile memory via
//     imageblocks.
//   - EDRAM buffer compute resolves (generic + specialized pipelines).
class MetalResolveEncoder {
 public:
  MetalResolveEncoder(MetalRenderTargetCache& render_target_cache,
                      MetalCommandProcessor& command_processor);
  ~MetalResolveEncoder();

  bool Initialize(MTL::Device* device, bool draw_resolution_scaled);
  void Shutdown();

  // Read the render targets directly into shared memory (and optional texture
  // cache refresh views) without dumping to the EDRAM buffer. Returns false if
  // the caller should fall back to the EDRAM round trip.
  bool DirectResolveRenderTargets(
      const draw_util::ResolveInfo& resolve_info,
      draw_util::ResolveCopyShaderIndex copy_shader,
      const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
      uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
      uint32_t dump_pitch, uint32_t group_count_x, uint32_t group_count_y,
      MTL::CommandBuffer* command_buffer,
      std::span<MTL::Texture* const> refresh_textures = {},
      uint32_t refresh_offset_x = 0, uint32_t refresh_offset_y = 0,
      bool refresh_depth_float = false, uint32_t refresh_raw_count = 0);

  // Executes a color resolve as a tile dispatch in the guest's open render
  // pass, reading pixels from tile memory instead of ending the pass.
  // Returns nullptr on success, or a static reason string on fallback (having
  // encoded nothing). If the dispatch requires a post-pass refresh copy/swap,
  // it is returned via |refresh_copy_out| and |has_refresh_copy_out|.
  const char* TileResolveInOpenPass(
      const draw_util::ResolveInfo& resolve_info,
      draw_util::ResolveCopyShaderIndex copy_shader,
      const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
      uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
      uint32_t dump_pitch, std::span<MTL::Texture* const> refresh_textures,
      uint32_t refresh_offset_x, uint32_t refresh_offset_y,
      size_t post_pass_resolves_count, PostPassRefreshCopy* refresh_copy_out,
      bool* has_refresh_copy_out);

  // Selects the compute pipeline for resolving out of the EDRAM buffer.
  MTL::ComputePipelineState* GetEdramResolvePipeline(
      draw_util::ResolveCopyShaderIndex copy_shader,
      const draw_util::ResolveCopyShaderConstants& copy_constants,
      bool draw_resolution_scaled) const;

  static bool SupportsDirectResolveRefreshExpBias(
      draw_util::ResolveCopyShaderIndex copy_shader,
      const draw_util::ResolveCopyShaderConstants& copy_constants);
  static bool IsFull32RGB10FourSampleAverage(
      draw_util::ResolveCopyShaderIndex copy_shader,
      const draw_util::ResolveCopyShaderConstants& copy_constants);

 private:
  enum class TileResolveKernel : uint32_t {
    k8888,
    k16_16Float,
    k32Float,
    k16_16_16_16Float,
    k32_32Float,
    // IsFull64RGBA16OneSample's conversion, not a bit copy.
    k16_16_16_16,
    // IsFull32RGBA8FourSampleAverage's average, of an 8_8_8_8 or an
    // 8_8_8_8_GAMMA (RGBA16Unorm) render target.
    k8888Average4,
    k8888GammaAverage4,
    // IsFull32RGB10FourSampleAverage's average, of a 2_10_10_10 render target
    // (RGB10A2Unorm).
    kRGB10Average4,
    // The bit copies of one-sample 8_8_8_8_GAMMA (RGBA16Unorm) and 2_10_10_10
    // (RGB10A2Unorm) render targets, as the dump shader packs them.
    k8888Gamma,
    kRGB10,
    // The bit copy of a one-sample 2_10_10_10_FLOAT render target
    // (RGBA16Float) as the dump shader packs it, and
    // IsFull32RGB10FloatOneSample's conversion of it into 2_10_10_10.
    kRGB10Float,
    kRGB10FloatToRGB10,
    kCount,
  };

  MTL::RenderPipelineState* GetOrCreateTileResolvePipeline(
      MTL::RenderPassDescriptor* pass_descriptor, uint32_t color_index,
      TileResolveKernel kernel, uint32_t sample_count, uint32_t refresh_count);

  MetalRenderTargetCache& render_target_cache_;
  MetalCommandProcessor& command_processor_;
  MTL::Device* device_ = nullptr;

  // Resolve compute pipelines (Metal XeSL / Slang -> MSL metallib).
  MTL::ComputePipelineState* resolve_full_8bpp_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_16bpp_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_rgb10_4x_direct_pipeline_ =
      nullptr;
  MTL::ComputePipelineState*
      resolve_full_32bpp_rgb10_4x_direct_refresh_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_1x_direct_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_1x_direct_refresh_pipeline_ =
      nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_rgb10f_1x_direct_pipeline_ =
      nullptr;
  MTL::ComputePipelineState*
      resolve_full_32bpp_rgb10f_1x_direct_refresh_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_64bpp_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_64bpp_rgba16_1x_direct_pipeline_ =
      nullptr;
  MTL::ComputePipelineState*
      resolve_full_64bpp_rgba16_1x_direct_refresh_pipeline_ = nullptr;
  MTL::ComputePipelineState*
      resolve_full_64bpp_rgb10f_rgba16_1x_direct_pipeline_ = nullptr;
  MTL::ComputePipelineState*
      resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_pipeline_ = nullptr;
  MTL::ComputePipelineState*
      resolve_full_64bpp_rgb10f_rgba16_msaa_direct_pipeline_ = nullptr;
  MTL::ComputePipelineState*
      resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_pipeline_ = nullptr;
  // One per kResolveSpecializations entry, in its order.
  std::vector<MTL::ComputePipelineState*> resolve_specialized_pipelines_;
  MTL::ComputePipelineState* resolve_full_128bpp_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_32bpp_1x2xmsaa_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_32bpp_4xmsaa_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_64bpp_1x2xmsaa_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_64bpp_4xmsaa_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_8bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_16bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_64bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_128bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ =
      nullptr;
  MTL::ComputePipelineState* resolve_fast_32bpp_4xmsaa_scaled_pipeline_ =
      nullptr;
  MTL::ComputePipelineState* resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ =
      nullptr;
  MTL::ComputePipelineState* resolve_fast_64bpp_4xmsaa_scaled_pipeline_ =
      nullptr;

  // Tile resolve library and pipelines (on-chip open-pass resolves).
  MTL::Library* tile_resolve_library_ = nullptr;
  bool tile_resolve_library_failed_ = false;
  std::unordered_map<uint64_t, MTL::RenderPipelineState*>
      tile_resolve_pipelines_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_RESOLVE_ENCODER_H_
