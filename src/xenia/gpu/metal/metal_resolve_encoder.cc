/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_resolve_encoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/edram_dump_shader.h"
#include "xenia/gpu/metal/metal_buffer_hazards.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_render_target_cache.h"
#include "xenia/gpu/metal/metal_shared_memory.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/metal/metal_transfer_encoder.h"
#include "xenia/gpu/metal/metal_util.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_128bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_128bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_16bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_16bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_1x_direct_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_1x_direct_refresh_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_rgb10_4x_direct_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_rgb10_4x_direct_refresh_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_rgb10f_1x_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_rgb10f_1x_direct_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_rgb10f_1x_direct_refresh_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_rgb10f_rgba8_2x_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_rgba8_2x_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_rgba8_4x_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_rgb10f_as_rgba16_rgba16f_1x_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_rgb10f_rgba16_1x_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_rgb10f_rgba16_1x_direct_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_rgb10f_rgba16_msaa_direct_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_rgb10f_rgba16f_1x_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_rgba16_1x_direct_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_rgba16_1x_direct_refresh_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_8bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_8bpp_scaled_cs.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

constexpr uint32_t kInternalComputeSliceAlignment = 256;

// The parts of the resolve copy constants that pick a copy specialized for one
// format combination: the source format, sample count and depth bits of the
// EDRAM info, the destination format and number of the destination info, and
// the sample selection.
struct ResolveCopyKey {
  uint32_t edram;
  uint32_t dest;
  uint32_t sample;
};

ResolveCopyKey GetResolveCopyKey(
    const draw_util::ResolveCopyShaderConstants& constants) {
  const auto& dest_relative = constants.dest_relative;
  return {dest_relative.edram_info.packed & 0x1F001C00u,
          dest_relative.dest_info.value & 0x0000FF80u,
          (dest_relative.dest_coordinate_info.packed >> 28) & 7u};
}

// Copies with a resolve shader specialized for them. The shaders only admit
// non-gamma source formats, so the gamma decoding bit that GetResolveInfo sets
// for all color resolves makes no difference to them.
const struct ResolveSpecialization {
  draw_util::ResolveCopyShaderIndex copy_shader;
  ResolveCopyKey key;
  const uint8_t* metallib;
  size_t metallib_size;
  const char* name;
} kResolveSpecializations[] = {
    // 8_8_8_8, averaging four samples, to 8_8_8_8.
    {draw_util::ResolveCopyShaderIndex::kFull32bpp,
     {0x00000800u, 6u << 7, 6},
     resolve_full_32bpp_rgba8_4x_cs_metallib,
     sizeof(resolve_full_32bpp_rgba8_4x_cs_metallib),
     "resolve_full_32bpp_rgba8_4x"},
    // 8_8_8_8, averaging two samples, to 8_8_8_8.
    {draw_util::ResolveCopyShaderIndex::kFull32bpp,
     {0x00000400u, 6u << 7, 4},
     resolve_full_32bpp_rgba8_2x_cs_metallib,
     sizeof(resolve_full_32bpp_rgba8_2x_cs_metallib),
     "resolve_full_32bpp_rgba8_2x"},
    // 2_10_10_10_FLOAT, averaging two samples, to 8_8_8_8.
    {draw_util::ResolveCopyShaderIndex::kFull32bpp,
     {0x03000400u, 6u << 7, 4},
     resolve_full_32bpp_rgb10f_rgba8_2x_cs_metallib,
     sizeof(resolve_full_32bpp_rgb10f_rgba8_2x_cs_metallib),
     "resolve_full_32bpp_rgb10f_rgba8_2x"},
    // 2_10_10_10_FLOAT_AS_16_16_16_16, one sample, to 2_10_10_10.
    {draw_util::ResolveCopyShaderIndex::kFull32bpp,
     {0x0C000000u, 7u << 7, 0},
     resolve_full_32bpp_rgb10f_1x_cs_metallib,
     sizeof(resolve_full_32bpp_rgb10f_1x_cs_metallib),
     "resolve_full_32bpp_rgb10f_1x"},
    // 2_10_10_10_FLOAT, one sample, to 16_16_16_16_FLOAT.
    {draw_util::ResolveCopyShaderIndex::kFull64bpp,
     {0x03000000u, 0x0000F000u, 0},
     resolve_full_64bpp_rgb10f_rgba16f_1x_cs_metallib,
     sizeof(resolve_full_64bpp_rgb10f_rgba16f_1x_cs_metallib),
     "resolve_full_64bpp_rgb10f_rgba16f_1x"},
    // 2_10_10_10_FLOAT, one sample, to 16_16_16_16.
    {draw_util::ResolveCopyShaderIndex::kFull64bpp,
     {0x03000000u, 0x00000D00u, 0},
     resolve_full_64bpp_rgb10f_rgba16_1x_cs_metallib,
     sizeof(resolve_full_64bpp_rgb10f_rgba16_1x_cs_metallib),
     "resolve_full_64bpp_rgb10f_rgba16_1x"},
    // 2_10_10_10_FLOAT_AS_16_16_16_16, one sample, to 16_16_16_16_FLOAT.
    {draw_util::ResolveCopyShaderIndex::kFull64bpp,
     {0x0C000000u, 0x0000F000u, 0},
     resolve_full_64bpp_rgb10f_as_rgba16_rgba16f_1x_cs_metallib,
     sizeof(resolve_full_64bpp_rgb10f_as_rgba16_rgba16f_1x_cs_metallib),
     "resolve_full_64bpp_rgb10f_as_rgba16_rgba16f_1x"},
};

// An 8_8_8_8 four-sample average into 8_8_8_8, which the direct resolve can do
// reading the render target itself.
bool IsFull32RGBA8FourSampleAverage(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& constants) {
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFull32bpp) {
    return false;
  }
  const ResolveCopyKey key = GetResolveCopyKey(constants);
  return key.edram == 0x00000800u && key.dest == (6u << 7) && key.sample == 6;
}

// An 8_8_8_8 two-sample average into 8_8_8_8, which the direct resolve does
// like the four-sample one.
bool IsFull32RGBA8TwoSampleAverage(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& constants) {
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFull32bpp) {
    return false;
  }
  const ResolveCopyKey key = GetResolveCopyKey(constants);
  return key.edram == 0x00000400u && key.dest == (6u << 7) && key.sample == 4;
}

// A 2_10_10_10 (or 2_10_10_10_AS_10_10_10_10) four-sample average into
// 2_10_10_10 as an unsigned fraction without exponent bias.
bool IsFull32RGB10FourSampleAverage(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& constants) {
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFull32bpp) {
    return false;
  }
  const ResolveCopyKey key = GetResolveCopyKey(constants);
  return (key.edram == 0x02000800u || key.edram == 0x0A000800u) &&
         key.dest == (7u << 7) && key.sample == 6;
}

// A one-sample 2_10_10_10_FLOAT (or 2_10_10_10_FLOAT_AS_16_16_16_16) copy into
// 2_10_10_10 as an unsigned fraction, with any exponent bias.
bool IsFull32RGB10FloatOneSample(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& constants) {
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFull32bpp) {
    return false;
  }
  const ResolveCopyKey key = GetResolveCopyKey(constants);
  return (key.edram == 0x03000000u || key.edram == 0x0C000000u) &&
         key.dest == (7u << 7) && key.sample == 0;
}

// A one-sample 2_10_10_10_FLOAT (or 2_10_10_10_FLOAT_AS_16_16_16_16) copy into
// 16_16_16_16 or 16_16_16_16_FLOAT, with any exponent bias and number format.
bool IsFull64RGB10FloatToRGBA16OneSample(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& constants) {
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFull64bpp) {
    return false;
  }
  const ResolveCopyKey key = GetResolveCopyKey(constants);
  return (key.edram == 0x03000000u || key.edram == 0x0C000000u) &&
         ((key.dest & 0x1F80u) == (26u << 7) ||
          (key.dest & 0x1F80u) == (32u << 7)) &&
         key.sample == 0;
}

// Any other one-sample copy of an 8_8_8_8, 2_10_10_10 (or
// 2_10_10_10_AS_10_10_10_10) or 2_10_10_10_FLOAT (or
// 2_10_10_10_FLOAT_AS_16_16_16_16) source into a 32bpp destination, with any
// exponent bias and number format.
bool IsFull32OneSampleConversion(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& constants) {
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFull32bpp ||
      IsFull32RGB10FloatOneSample(copy_shader, constants)) {
    return false;
  }
  const ResolveCopyKey key = GetResolveCopyKey(constants);
  return (key.edram == 0x00000000u || key.edram == 0x02000000u ||
          key.edram == 0x0A000000u || key.edram == 0x03000000u ||
          key.edram == 0x0C000000u) &&
         key.sample == 0;
}

// The two-sample average of a 2x or the four-sample one of a 4x
// 2_10_10_10_FLOAT (or 2_10_10_10_FLOAT_AS_16_16_16_16) source into
// 16_16_16_16 or 16_16_16_16_FLOAT, with any exponent bias and number format.
bool IsFull64RGB10FloatToRGBA16Average(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& constants) {
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFull64bpp) {
    return false;
  }
  const ResolveCopyKey key = GetResolveCopyKey(constants);
  return (((key.edram == 0x03000400u || key.edram == 0x0C000400u) &&
           key.sample == 4) ||
          ((key.edram == 0x03000800u || key.edram == 0x0C000800u) &&
           key.sample == 6)) &&
         ((key.dest & 0x1F80u) == (26u << 7) ||
          (key.dest & 0x1F80u) == (32u << 7));
}

// A one-sample 16_16_16_16 copy into 16_16_16_16, with any exponent bias and
// number format.
bool IsFull64RGBA16OneSample(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& constants) {
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFull64bpp) {
    return false;
  }
  const ResolveCopyKey key = GetResolveCopyKey(constants);
  // 16_16_16_16, 64bpp, one sample per pixel.
  return key.edram == 0x15000000u && (key.dest & 0x1F80u) == (26u << 7) &&
         key.sample == 0;
}

}  // namespace

MetalResolveEncoder::MetalResolveEncoder(
    MetalRenderTargetCache& render_target_cache,
    MetalCommandProcessor& command_processor)
    : render_target_cache_(render_target_cache),
      command_processor_(command_processor) {}

MetalResolveEncoder::~MetalResolveEncoder() { Shutdown(); }

bool MetalResolveEncoder::Initialize(MTL::Device* device,
                                     bool draw_resolution_scaled) {
  device_ = device;
  resolve_full_8bpp_pipeline_ = nullptr;
  resolve_full_16bpp_pipeline_ = nullptr;
  resolve_full_32bpp_pipeline_ = nullptr;
  resolve_full_64bpp_pipeline_ = nullptr;
  resolve_full_128bpp_pipeline_ = nullptr;
  resolve_fast_32bpp_1x2xmsaa_pipeline_ = nullptr;
  resolve_fast_32bpp_4xmsaa_pipeline_ = nullptr;
  resolve_fast_64bpp_1x2xmsaa_pipeline_ = nullptr;
  resolve_fast_64bpp_4xmsaa_pipeline_ = nullptr;
  resolve_full_8bpp_scaled_pipeline_ = nullptr;
  resolve_full_16bpp_scaled_pipeline_ = nullptr;
  resolve_full_32bpp_scaled_pipeline_ = nullptr;
  resolve_full_64bpp_scaled_pipeline_ = nullptr;
  resolve_full_128bpp_scaled_pipeline_ = nullptr;
  resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  resolve_fast_32bpp_4xmsaa_scaled_pipeline_ = nullptr;
  resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  resolve_fast_64bpp_4xmsaa_scaled_pipeline_ = nullptr;

  // Resolve compute pipelines.
  resolve_full_8bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_8bpp_cs_metallib,
      sizeof(resolve_full_8bpp_cs_metallib), "resolve_full_8bpp");
  resolve_full_16bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_16bpp_cs_metallib,
      sizeof(resolve_full_16bpp_cs_metallib), "resolve_full_16bpp");
  resolve_full_32bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_32bpp_cs_metallib,
      sizeof(resolve_full_32bpp_cs_metallib), "resolve_full_32bpp");
  resolve_full_32bpp_rgb10_4x_direct_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_32bpp_rgb10_4x_direct_cs_metallib,
          sizeof(resolve_full_32bpp_rgb10_4x_direct_cs_metallib),
          "resolve_full_32bpp_rgb10_4x_direct");
  resolve_full_32bpp_rgb10_4x_direct_refresh_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_32bpp_rgb10_4x_direct_refresh_cs_metallib,
          sizeof(resolve_full_32bpp_rgb10_4x_direct_refresh_cs_metallib),
          "resolve_full_32bpp_rgb10_4x_direct_refresh");
  resolve_full_32bpp_1x_direct_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_32bpp_1x_direct_cs_metallib,
          sizeof(resolve_full_32bpp_1x_direct_cs_metallib),
          "resolve_full_32bpp_1x_direct");
  resolve_full_32bpp_1x_direct_refresh_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_32bpp_1x_direct_refresh_cs_metallib,
          sizeof(resolve_full_32bpp_1x_direct_refresh_cs_metallib),
          "resolve_full_32bpp_1x_direct_refresh");
  resolve_full_32bpp_rgb10f_1x_direct_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_32bpp_rgb10f_1x_direct_cs_metallib,
          sizeof(resolve_full_32bpp_rgb10f_1x_direct_cs_metallib),
          "resolve_full_32bpp_rgb10f_1x_direct");
  resolve_full_32bpp_rgb10f_1x_direct_refresh_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_32bpp_rgb10f_1x_direct_refresh_cs_metallib,
          sizeof(resolve_full_32bpp_rgb10f_1x_direct_refresh_cs_metallib),
          "resolve_full_32bpp_rgb10f_1x_direct_refresh");
  resolve_full_64bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_64bpp_cs_metallib,
      sizeof(resolve_full_64bpp_cs_metallib), "resolve_full_64bpp");
  resolve_full_64bpp_rgba16_1x_direct_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_64bpp_rgba16_1x_direct_cs_metallib,
          sizeof(resolve_full_64bpp_rgba16_1x_direct_cs_metallib),
          "resolve_full_64bpp_rgba16_1x_direct");
  resolve_full_64bpp_rgba16_1x_direct_refresh_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_64bpp_rgba16_1x_direct_refresh_cs_metallib,
          sizeof(resolve_full_64bpp_rgba16_1x_direct_refresh_cs_metallib),
          "resolve_full_64bpp_rgba16_1x_direct_refresh");
  resolve_full_64bpp_rgb10f_rgba16_1x_direct_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_64bpp_rgb10f_rgba16_1x_direct_cs_metallib,
          sizeof(resolve_full_64bpp_rgb10f_rgba16_1x_direct_cs_metallib),
          "resolve_full_64bpp_rgb10f_rgba16_1x_direct");
  resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_,
          resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_cs_metallib,
          sizeof(
              resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_cs_metallib),
          "resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh");
  resolve_full_64bpp_rgb10f_rgba16_msaa_direct_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_full_64bpp_rgb10f_rgba16_msaa_direct_cs_metallib,
          sizeof(resolve_full_64bpp_rgb10f_rgba16_msaa_direct_cs_metallib),
          "resolve_full_64bpp_rgb10f_rgba16_msaa_direct");
  resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_,
          resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_cs_metallib,
          sizeof(
              resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_cs_metallib),
          "resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh");
  resolve_specialized_pipelines_.clear();
  for (const ResolveSpecialization& specialization : kResolveSpecializations) {
    resolve_specialized_pipelines_.push_back(
        CreateComputePipelineFromEmbeddedLibrary(
            device_, specialization.metallib, specialization.metallib_size,
            specialization.name));
  }
  resolve_full_128bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_128bpp_cs_metallib,
      sizeof(resolve_full_128bpp_cs_metallib), "resolve_full_128bpp");
  resolve_fast_32bpp_1x2xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_32bpp_1x2xmsaa_cs_metallib,
          sizeof(resolve_fast_32bpp_1x2xmsaa_cs_metallib),
          "resolve_fast_32bpp_1x2xmsaa");
  resolve_fast_32bpp_4xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_32bpp_4xmsaa_cs_metallib,
          sizeof(resolve_fast_32bpp_4xmsaa_cs_metallib),
          "resolve_fast_32bpp_4xmsaa");
  resolve_fast_64bpp_1x2xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_64bpp_1x2xmsaa_cs_metallib,
          sizeof(resolve_fast_64bpp_1x2xmsaa_cs_metallib),
          "resolve_fast_64bpp_1x2xmsaa");
  resolve_fast_64bpp_4xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_64bpp_4xmsaa_cs_metallib,
          sizeof(resolve_fast_64bpp_4xmsaa_cs_metallib),
          "resolve_fast_64bpp_4xmsaa");

  if (!resolve_full_8bpp_pipeline_ || !resolve_full_16bpp_pipeline_ ||
      !resolve_full_32bpp_pipeline_ || !resolve_full_64bpp_pipeline_ ||
      !resolve_full_128bpp_pipeline_ ||
      !resolve_fast_32bpp_1x2xmsaa_pipeline_ ||
      !resolve_fast_32bpp_4xmsaa_pipeline_ ||
      !resolve_fast_64bpp_1x2xmsaa_pipeline_ ||
      !resolve_fast_64bpp_4xmsaa_pipeline_) {
    XELOGE("Metal: failed to initialize resolve compute pipelines");
    return false;
  }

  if (draw_resolution_scaled) {
    resolve_full_8bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_8bpp_scaled_cs_metallib,
            sizeof(resolve_full_8bpp_scaled_cs_metallib),
            "resolve_full_8bpp_scaled");
    resolve_full_16bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_16bpp_scaled_cs_metallib,
            sizeof(resolve_full_16bpp_scaled_cs_metallib),
            "resolve_full_16bpp_scaled");
    resolve_full_32bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_32bpp_scaled_cs_metallib,
            sizeof(resolve_full_32bpp_scaled_cs_metallib),
            "resolve_full_32bpp_scaled");
    resolve_full_64bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_64bpp_scaled_cs_metallib,
            sizeof(resolve_full_64bpp_scaled_cs_metallib),
            "resolve_full_64bpp_scaled");
    resolve_full_128bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_128bpp_scaled_cs_metallib,
            sizeof(resolve_full_128bpp_scaled_cs_metallib),
            "resolve_full_128bpp_scaled");
    resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_32bpp_1x2xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_32bpp_1x2xmsaa_scaled_cs_metallib),
            "resolve_fast_32bpp_1x2xmsaa_scaled");
    resolve_fast_32bpp_4xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_32bpp_4xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_32bpp_4xmsaa_scaled_cs_metallib),
            "resolve_fast_32bpp_4xmsaa_scaled");
    resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_64bpp_1x2xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_64bpp_1x2xmsaa_scaled_cs_metallib),
            "resolve_fast_64bpp_1x2xmsaa_scaled");
    resolve_fast_64bpp_4xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_64bpp_4xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_64bpp_4xmsaa_scaled_cs_metallib),
            "resolve_fast_64bpp_4xmsaa_scaled");
    if (!resolve_full_8bpp_scaled_pipeline_ ||
        !resolve_full_16bpp_scaled_pipeline_ ||
        !resolve_full_32bpp_scaled_pipeline_ ||
        !resolve_full_64bpp_scaled_pipeline_ ||
        !resolve_full_128bpp_scaled_pipeline_ ||
        !resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ ||
        !resolve_fast_32bpp_4xmsaa_scaled_pipeline_ ||
        !resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ ||
        !resolve_fast_64bpp_4xmsaa_scaled_pipeline_) {
      XELOGE("Metal: failed to initialize scaled resolve compute pipelines");
      return false;
    }
  }

  return true;
}

void MetalResolveEncoder::Shutdown() {
  for (auto& it : tile_resolve_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  tile_resolve_pipelines_.clear();
  if (tile_resolve_library_) {
    tile_resolve_library_->release();
    tile_resolve_library_ = nullptr;
  }
  tile_resolve_library_failed_ = false;
  if (resolve_full_8bpp_pipeline_) {
    resolve_full_8bpp_pipeline_->release();
    resolve_full_8bpp_pipeline_ = nullptr;
  }
  if (resolve_full_16bpp_pipeline_) {
    resolve_full_16bpp_pipeline_->release();
    resolve_full_16bpp_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_pipeline_) {
    resolve_full_32bpp_pipeline_->release();
    resolve_full_32bpp_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_rgb10_4x_direct_pipeline_) {
    resolve_full_32bpp_rgb10_4x_direct_pipeline_->release();
    resolve_full_32bpp_rgb10_4x_direct_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_rgb10_4x_direct_refresh_pipeline_) {
    resolve_full_32bpp_rgb10_4x_direct_refresh_pipeline_->release();
    resolve_full_32bpp_rgb10_4x_direct_refresh_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_1x_direct_pipeline_) {
    resolve_full_32bpp_1x_direct_pipeline_->release();
    resolve_full_32bpp_1x_direct_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_1x_direct_refresh_pipeline_) {
    resolve_full_32bpp_1x_direct_refresh_pipeline_->release();
    resolve_full_32bpp_1x_direct_refresh_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_rgb10f_1x_direct_pipeline_) {
    resolve_full_32bpp_rgb10f_1x_direct_pipeline_->release();
    resolve_full_32bpp_rgb10f_1x_direct_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_rgb10f_1x_direct_refresh_pipeline_) {
    resolve_full_32bpp_rgb10f_1x_direct_refresh_pipeline_->release();
    resolve_full_32bpp_rgb10f_1x_direct_refresh_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_rgb10f_rgba16_1x_direct_pipeline_) {
    resolve_full_64bpp_rgb10f_rgba16_1x_direct_pipeline_->release();
    resolve_full_64bpp_rgb10f_rgba16_1x_direct_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_pipeline_) {
    resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_pipeline_->release();
    resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_rgb10f_rgba16_msaa_direct_pipeline_) {
    resolve_full_64bpp_rgb10f_rgba16_msaa_direct_pipeline_->release();
    resolve_full_64bpp_rgb10f_rgba16_msaa_direct_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_pipeline_) {
    resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_pipeline_->release();
    resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_rgba16_1x_direct_pipeline_) {
    resolve_full_64bpp_rgba16_1x_direct_pipeline_->release();
    resolve_full_64bpp_rgba16_1x_direct_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_rgba16_1x_direct_refresh_pipeline_) {
    resolve_full_64bpp_rgba16_1x_direct_refresh_pipeline_->release();
    resolve_full_64bpp_rgba16_1x_direct_refresh_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_pipeline_) {
    resolve_full_64bpp_pipeline_->release();
    resolve_full_64bpp_pipeline_ = nullptr;
  }
  for (MTL::ComputePipelineState* pipeline : resolve_specialized_pipelines_) {
    if (pipeline) {
      pipeline->release();
    }
  }
  resolve_specialized_pipelines_.clear();
  if (resolve_full_128bpp_pipeline_) {
    resolve_full_128bpp_pipeline_->release();
    resolve_full_128bpp_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_1x2xmsaa_pipeline_) {
    resolve_fast_32bpp_1x2xmsaa_pipeline_->release();
    resolve_fast_32bpp_1x2xmsaa_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_4xmsaa_pipeline_) {
    resolve_fast_32bpp_4xmsaa_pipeline_->release();
    resolve_fast_32bpp_4xmsaa_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_1x2xmsaa_pipeline_) {
    resolve_fast_64bpp_1x2xmsaa_pipeline_->release();
    resolve_fast_64bpp_1x2xmsaa_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_4xmsaa_pipeline_) {
    resolve_fast_64bpp_4xmsaa_pipeline_->release();
    resolve_fast_64bpp_4xmsaa_pipeline_ = nullptr;
  }
  if (resolve_full_8bpp_scaled_pipeline_) {
    resolve_full_8bpp_scaled_pipeline_->release();
    resolve_full_8bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_16bpp_scaled_pipeline_) {
    resolve_full_16bpp_scaled_pipeline_->release();
    resolve_full_16bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_scaled_pipeline_) {
    resolve_full_32bpp_scaled_pipeline_->release();
    resolve_full_32bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_scaled_pipeline_) {
    resolve_full_64bpp_scaled_pipeline_->release();
    resolve_full_64bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_128bpp_scaled_pipeline_) {
    resolve_full_128bpp_scaled_pipeline_->release();
    resolve_full_128bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_) {
    resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_->release();
    resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_4xmsaa_scaled_pipeline_) {
    resolve_fast_32bpp_4xmsaa_scaled_pipeline_->release();
    resolve_fast_32bpp_4xmsaa_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_) {
    resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_->release();
    resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_4xmsaa_scaled_pipeline_) {
    resolve_fast_64bpp_4xmsaa_scaled_pipeline_->release();
    resolve_fast_64bpp_4xmsaa_scaled_pipeline_ = nullptr;
  }

  device_ = nullptr;
}

bool MetalResolveEncoder::SupportsDirectResolveRefreshExpBias(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& copy_constants) {
  return IsFull32RGB10FloatOneSample(copy_shader, copy_constants) ||
         IsFull32OneSampleConversion(copy_shader, copy_constants) ||
         IsFull64RGBA16OneSample(copy_shader, copy_constants) ||
         IsFull64RGB10FloatToRGBA16OneSample(copy_shader, copy_constants) ||
         IsFull64RGB10FloatToRGBA16Average(copy_shader, copy_constants);
}

bool MetalResolveEncoder::IsFull32RGB10FourSampleAverage(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& copy_constants) {
  if (copy_shader != draw_util::ResolveCopyShaderIndex::kFull32bpp) {
    return false;
  }
  const ResolveCopyKey key = GetResolveCopyKey(copy_constants);
  return (key.edram == 0x02000800u || key.edram == 0x0A000800u) &&
         key.dest == (7u << 7) && key.sample == 6;
}

MTL::ComputePipelineState* MetalResolveEncoder::GetEdramResolvePipeline(
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& copy_constants,
    bool draw_resolution_scaled) const {
  MTL::ComputePipelineState* pipeline = nullptr;
  if (draw_resolution_scaled) {
    switch (copy_shader) {
      case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
        return resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_;
      case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
        return resolve_fast_32bpp_4xmsaa_scaled_pipeline_;
      case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
        return resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_;
      case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
        return resolve_fast_64bpp_4xmsaa_scaled_pipeline_;
      case draw_util::ResolveCopyShaderIndex::kFull8bpp:
        return resolve_full_8bpp_scaled_pipeline_;
      case draw_util::ResolveCopyShaderIndex::kFull16bpp:
        return resolve_full_16bpp_scaled_pipeline_;
      case draw_util::ResolveCopyShaderIndex::kFull32bpp:
        return resolve_full_32bpp_scaled_pipeline_;
      case draw_util::ResolveCopyShaderIndex::kFull64bpp:
        return resolve_full_64bpp_scaled_pipeline_;
      case draw_util::ResolveCopyShaderIndex::kFull128bpp:
        return resolve_full_128bpp_scaled_pipeline_;
      default:
        return nullptr;
    }
  }
  switch (copy_shader) {
    case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
      pipeline = resolve_fast_32bpp_1x2xmsaa_pipeline_;
      break;
    case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
      pipeline = resolve_fast_32bpp_4xmsaa_pipeline_;
      break;
    case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
      pipeline = resolve_fast_64bpp_1x2xmsaa_pipeline_;
      break;
    case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
      pipeline = resolve_fast_64bpp_4xmsaa_pipeline_;
      break;
    case draw_util::ResolveCopyShaderIndex::kFull8bpp:
      pipeline = resolve_full_8bpp_pipeline_;
      break;
    case draw_util::ResolveCopyShaderIndex::kFull16bpp:
      pipeline = resolve_full_16bpp_pipeline_;
      break;
    case draw_util::ResolveCopyShaderIndex::kFull32bpp:
      pipeline = resolve_full_32bpp_pipeline_;
      break;
    case draw_util::ResolveCopyShaderIndex::kFull64bpp:
      pipeline = resolve_full_64bpp_pipeline_;
      break;
    case draw_util::ResolveCopyShaderIndex::kFull128bpp:
      pipeline = resolve_full_128bpp_pipeline_;
      break;
    default:
      pipeline = nullptr;
      break;
  }
  const ResolveCopyKey key = GetResolveCopyKey(copy_constants);
  for (size_t i = 0; i < resolve_specialized_pipelines_.size(); ++i) {
    const ResolveSpecialization& specialization = kResolveSpecializations[i];
    if (specialization.copy_shader == copy_shader &&
        specialization.key.edram == key.edram &&
        specialization.key.dest == key.dest &&
        specialization.key.sample == key.sample &&
        resolve_specialized_pipelines_[i]) {
      pipeline = resolve_specialized_pipelines_[i];
      break;
    }
  }
  return pipeline;
}

bool MetalResolveEncoder::DirectResolveRenderTargets(
    const draw_util::ResolveInfo& resolve_info,
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, uint32_t group_count_x, uint32_t group_count_y,
    MTL::CommandBuffer* command_buffer,
    std::span<MTL::Texture* const> refresh_textures, uint32_t refresh_offset_x,
    uint32_t refresh_offset_y, bool refresh_depth_float,
    uint32_t refresh_raw_count) {
  MTL::Texture* refresh_texture =
      refresh_textures.empty() ? nullptr : refresh_textures.front();
  const uint32_t refresh_alias_count =
      refresh_textures.empty() ? 0 : uint32_t(refresh_textures.size() - 1);
  SCOPE_profile_cpu_f("gpu");
  auto* shared = command_processor_.shared_memory();
  MTL::Buffer* dest_buffer = shared ? shared->GetBuffer() : nullptr;
  MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
  if (!dest_buffer || !queue) {
    return false;
  }

  using ResolveCopyDumpRectangle =
      MetalRenderTargetCache::ResolveCopyDumpRectangle;
  using RenderTargetKey = MetalRenderTargetCache::RenderTargetKey;
  std::vector<ResolveCopyDumpRectangle> rectangles;
  render_target_cache_.GetResolveCopyRectanglesToDump(
      dump_base, dump_row_length_used, dump_rows, dump_pitch, rectangles);
  if (rectangles.empty()) {
    return false;
  }
  const xenos::MsaaSamples resolve_msaa_samples =
      resolve_info.IsCopyingDepth()
          ? resolve_info.depth_edram_info.msaa_samples
          : resolve_info.color_edram_info.msaa_samples;

  // The 2_10_10_10 four-sample average reads the samples from the one 4x
  // render target owning the whole source with the exact arithmetic of the
  // rgb10-4x copy, and the 2_10_10_10_FLOAT to 2_10_10_10 conversion the pixels
  // of the one 1x render target with that of the rgb10f-1x copy, rather than
  // through the dump shader.
  const bool rgb10_4x_average =
      IsFull32RGB10FourSampleAverage(copy_shader, copy_shader_constants);
  // The same 2_10_10_10_FLOAT pixels also go into 16_16_16_16 with the
  // arithmetic of the generic 64bpp copy.
  const bool rgb10f_rgba16_1x =
      IsFull64RGB10FloatToRGBA16OneSample(copy_shader, copy_shader_constants);
  // And so are their 2x and 4x averages, from the multisampled render target.
  const bool rgb10f_rgba16_average =
      IsFull64RGB10FloatToRGBA16Average(copy_shader, copy_shader_constants);
  const bool rgb10f_1x =
      !rgb10_4x_average &&
      (rgb10f_rgba16_1x ||
       IsFull32RGB10FloatOneSample(copy_shader, copy_shader_constants));
  // Any other conversion of one 8_8_8_8, 2_10_10_10 or 2_10_10_10_FLOAT sample
  // into a 32bpp destination goes the same way, with the arithmetic of the
  // generic 32bpp copy.
  const bool full32_1x =
      !rgb10_4x_average && !rgb10f_1x &&
      IsFull32OneSampleConversion(copy_shader, copy_shader_constants);
  // So is the 16_16_16_16 to 16_16_16_16 conversion, with that of the generic
  // 64bpp copy, from the render target's integer ownership transfer view.
  const bool rgba16_1x =
      IsFull64RGBA16OneSample(copy_shader, copy_shader_constants);
  const bool copy_from_render_target = rgb10_4x_average || rgb10f_1x ||
                                       rgba16_1x || rgb10f_rgba16_average ||
                                       full32_1x;
  // Only the dump shader writes raw aliases of a depth refresh.
  if (refresh_raw_count &&
      (copy_from_render_target || !resolve_info.IsCopyingDepth() ||
       refresh_raw_count > refresh_alias_count)) {
    return false;
  }
  // The dump shader's direct resolve only copies bits.
  if (copy_shader == draw_util::ResolveCopyShaderIndex::kFull64bpp &&
      !rgba16_1x && !rgb10f_rgba16_1x && !rgb10f_rgba16_average) {
    return false;
  }
  MTL::ComputePipelineState* rgb10_pipeline = nullptr;
  MTL::Texture* rgb10_source = nullptr;
  uint32_t rgb10_origin[2] = {};
  // For the 2x and 4x averages, the host sample of each guest sample in the
  // order the copy adds them, a byte each.
  uint32_t rgb10_host_samples = 0;
  if (copy_from_render_target) {
    if (rgba16_1x) {
      rgb10_pipeline =
          refresh_texture
              ? resolve_full_64bpp_rgba16_1x_direct_refresh_pipeline_
              : resolve_full_64bpp_rgba16_1x_direct_pipeline_;
    } else if (rgb10f_rgba16_average) {
      rgb10_pipeline =
          refresh_texture
              ? resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_pipeline_
              : resolve_full_64bpp_rgb10f_rgba16_msaa_direct_pipeline_;
    } else if (rgb10f_rgba16_1x) {
      rgb10_pipeline =
          refresh_texture
              ? resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_pipeline_
              : resolve_full_64bpp_rgb10f_rgba16_1x_direct_pipeline_;
    } else if (rgb10_4x_average) {
      rgb10_pipeline =
          refresh_texture ? resolve_full_32bpp_rgb10_4x_direct_refresh_pipeline_
                          : resolve_full_32bpp_rgb10_4x_direct_pipeline_;
    } else if (full32_1x) {
      rgb10_pipeline = refresh_texture
                           ? resolve_full_32bpp_1x_direct_refresh_pipeline_
                           : resolve_full_32bpp_1x_direct_pipeline_;
    } else {
      rgb10_pipeline =
          refresh_texture
              ? resolve_full_32bpp_rgb10f_1x_direct_refresh_pipeline_
              : resolve_full_32bpp_rgb10f_1x_direct_pipeline_;
    }
    if (rectangles.size() != 1 || !rectangles[0].render_target ||
        !rgb10_pipeline || !group_count_x || !group_count_y) {
      return false;
    }
    auto* rt = static_cast<MetalRenderTargetCache::MetalRenderTarget*>(
        rectangles[0].render_target);
    const RenderTargetKey rt_key = rt->key();
    rgb10_source = rgba16_1x ? rt->transfer_texture() : rt->texture();
    const draw_util::ResolveEdramInfo& edram_info =
        resolve_info.color_edram_info;
    // The shader reads resolve pixel p at render target texel p + origin,
    // which holds while the resolve's rows are the render target's rows: the
    // same pitch, a base at or after the render target's, not wrapping around
    // EDRAM. The rgb10f-1x copy converts the floats of an RGBA16Float render
    // target the way the dump does, which needs the render target to hold the
    // source format, not just its layout.
    const xenos::MsaaSamples source_msaa_samples =
        rgb10_4x_average        ? xenos::MsaaSamples::k4X
        : rgb10f_rgba16_average ? edram_info.msaa_samples
                                : xenos::MsaaSamples::k1X;
    // 2x render targets are 4x where the device has no 2x.
    const NS::UInteger source_sample_count =
        source_msaa_samples == xenos::MsaaSamples::k1X ? 1
        : source_msaa_samples == xenos::MsaaSamples::k2X
            ? (render_target_cache_.msaa_2x_supported_ ? 2 : 4)
            : 4;
    if (source_msaa_samples == xenos::MsaaSamples::k2X) {
      rgb10_host_samples = draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                               0, render_target_cache_.msaa_2x_supported_) |
                           (draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                                1, render_target_cache_.msaa_2x_supported_)
                            << 8);
    } else if (source_msaa_samples == xenos::MsaaSamples::k4X) {
      // The guest 4x sample numbering matches the host one.
      rgb10_host_samples = 0x03020100u;
    }
    // The generic 1x conversion packs the pixels as the resolve's source
    // format, so the render target has to hold that format, apart from the
    // variants with the same packing.
    const auto source_format =
        xenos::ColorRenderTargetFormat(edram_info.format);
    const xenos::ColorRenderTargetFormat rt_format = rt_key.GetColorFormat();
    auto is_rgb10 = [](xenos::ColorRenderTargetFormat format) {
      return format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
             format ==
                 xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10;
    };
    auto is_rgb10f = [](xenos::ColorRenderTargetFormat format) {
      return format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
             format == xenos::ColorRenderTargetFormat::
                           k_2_10_10_10_FLOAT_AS_16_16_16_16;
    };
    const bool full32_1x_format_matches =
        source_format == xenos::ColorRenderTargetFormat::k_8_8_8_8
            ? rt_format == xenos::ColorRenderTargetFormat::k_8_8_8_8
        : is_rgb10(source_format) ? is_rgb10(rt_format)
                                  : is_rgb10f(rt_format);
    const MTL::PixelFormat source_pixel_format =
        rgba16_1x ? MTL::PixelFormatRGBA16Uint
        : rgb10_4x_average || (full32_1x && is_rgb10(source_format))
            ? MTL::PixelFormatRGB10A2Unorm
        : full32_1x &&
                source_format == xenos::ColorRenderTargetFormat::k_8_8_8_8
            ? MTL::PixelFormatRGBA8Unorm
            : MTL::PixelFormatRGBA16Float;
    if (!rgb10_source || rt_key.is_depth ||
        rt_key.msaa_samples != source_msaa_samples ||
        rt_key.Is64bpp() != rgba16_1x ||
        rgb10_source->pixelFormat() != source_pixel_format ||
        rgb10_source->sampleCount() != source_sample_count ||
        ((rgb10f_1x || rgb10f_rgba16_average) &&
         rt_key.GetColorFormat() !=
             xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT &&
         rt_key.GetColorFormat() != xenos::ColorRenderTargetFormat::
                                        k_2_10_10_10_FLOAT_AS_16_16_16_16) ||
        (full32_1x && !full32_1x_format_matches) ||
        (rgba16_1x && rt_key.GetColorFormat() !=
                          xenos::ColorRenderTargetFormat::k_16_16_16_16) ||
        !edram_info.pitch_tiles ||
        rt_key.GetPitchTiles() != edram_info.pitch_tiles ||
        edram_info.base_tiles < rt_key.base_tiles) {
      return false;
    }
    // 64bpp tiles are half as wide in pixels, and so are 4x ones, which are
    // also half as tall as 1x ones, like 2x ones.
    const uint32_t tile_width =
        xenos::kEdramTileWidthSamples >>
        uint32_t(source_msaa_samples >= xenos::MsaaSamples::k4X || rgba16_1x);
    const uint32_t tile_height =
        xenos::kEdramTileHeightSamples >>
        uint32_t(source_msaa_samples >= xenos::MsaaSamples::k2X);
    const uint32_t base_delta = edram_info.base_tiles - rt_key.base_tiles;
    rgb10_origin[0] = base_delta % edram_info.pitch_tiles * tile_width +
                      resolve_info.coordinate_info.edram_offset_x_div_8 * 8;
    rgb10_origin[1] = base_delta / edram_info.pitch_tiles * tile_height +
                      resolve_info.coordinate_info.edram_offset_y_div_8 * 8;
    const uint32_t width = resolve_info.coordinate_info.width_div_8 * 8;
    const uint32_t height = resolve_info.height_div_8 * 8;
    if (rgb10_origin[0] + width > edram_info.pitch_tiles * tile_width ||
        rgb10_origin[0] + width > rgb10_source->width() ||
        rgb10_origin[1] + height > rgb10_source->height()) {
      return false;
    }
  }

  // Every pipeline and source has to resolve before anything is encoded -
  // skipping a rectangle partway through would leave its part of the
  // destination stale, with nothing to say so.
  const bool resolve_average =
      IsFull32RGBA8FourSampleAverage(copy_shader, copy_shader_constants) ||
      IsFull32RGBA8TwoSampleAverage(copy_shader, copy_shader_constants);
  struct Source {
    MTL::ComputePipelineState* pipeline;
    MTL::Texture* texture;
    MTL::Texture* stencil_texture;
  };
  std::vector<Source> sources;
  sources.reserve(rectangles.size());
  for (const ResolveCopyDumpRectangle& rectangle : rectangles) {
    if (copy_from_render_target) {
      break;
    }
    auto* rt = static_cast<MetalRenderTargetCache::MetalRenderTarget*>(
        rectangle.render_target);
    if (!rt) {
      return false;
    }
    RenderTargetKey rt_key = rt->key();
    EdramDumpShaderKey shader_key;
    shader_key.is_depth = rt_key.is_depth;
    shader_key.resource_format = rt_key.resource_format;
    shader_key.msaa_samples = rt_key.msaa_samples;
    shader_key.direct_resolve = 1;
    shader_key.direct_resolve_texture = refresh_texture != nullptr;
    shader_key.direct_resolve_texture_aliases = refresh_alias_count;
    shader_key.direct_resolve_cross_depth_color =
        rt_key.is_depth != uint32_t(resolve_info.IsCopyingDepth());
    shader_key.direct_resolve_texture_depth_float =
        refresh_texture && resolve_info.IsCopyingDepth() && refresh_depth_float;
    shader_key.direct_resolve_texture_raw_aliases = refresh_raw_count;
    shader_key.direct_resolve_average = resolve_average;
    shader_key.direct_resolve_samples_as_pixels =
        rt_key.msaa_samples != resolve_msaa_samples;
    Source source;
    source.pipeline = render_target_cache_.GetOrCreateDumpPipeline(shader_key);
    // The source is the ownership transfer view, whose format the shader was
    // emitted against - an integer one for the formats where exact bits matter.
    source.stencil_texture = nullptr;
    if (rt_key.is_depth) {
      source.texture = rt->texture();
      source.stencil_texture = render_target_cache_.GetStencilTextureView(rt);
      if (!source.stencil_texture) {
        return false;
      }
    } else {
      bool source_is_uint = false;
      render_target_cache_.GetColorOwnershipTransferPixelFormat(
          rt_key.GetColorFormat(), &source_is_uint);
      source.texture = source_is_uint ? rt->transfer_texture() : rt->texture();
    }
    if (!source.pipeline || !source.texture) {
      return false;
    }
    sources.push_back(source);
  }

  // The GPU is about to overwrite this range, so any CPU-side data has to be
  // uploaded first, as it is for the resolve copy.
  if (!shared->RequestRange(resolve_info.copy_dest_extent_start,
                            resolve_info.copy_dest_extent_length)) {
    XELOGE(
        "MetalRenderTargetCache::DirectResolveRenderTargets: RequestRange "
        "failed for 0x{:08X} len {}",
        resolve_info.copy_dest_extent_start,
        resolve_info.copy_dest_extent_length);
    return false;
  }

  ScopedAutoreleasePool autorelease_pool;
  bool owns_command_buffer = false;
  MTL::CommandBuffer* cmd = command_buffer;
  if (!cmd) {
    cmd = command_processor_.CreateAccountedCommandBuffer(
        MetalCommandProcessor::CommandBufferKind::kRenderTargetResolve);
    if (!cmd) {
      return false;
    }
    owns_command_buffer = true;
  }
  // In the current command buffer, share the compute encoder with the
  // resolves and texture loads around this one.
  MetalBufferHazards::Encoder own_hazards;
  MetalBufferHazards::Encoder* hazards = &own_hazards;
  MTL::ComputeCommandEncoder* encoder =
      command_processor_.BeginSharedComputeEncoder(cmd, &hazards);
  bool shared_encoder = encoder != nullptr;
  if (!shared_encoder) {
    command_processor_.EndSharedComputeEncoder(cmd);
    encoder = cmd->computeCommandEncoder();
  }
  if (!encoder) {
    if (owns_command_buffer) {
      command_processor_.DiscardAccountedCommandBuffer(
          cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetResolve);
    }
    return false;
  }

  if (shared->hazards()) {
    if (!shared_encoder) {
      own_hazards.Begin(*shared->hazards(), cmd, encoder);
    }
    if (hazards->Access(resolve_info.copy_dest_extent_start,
                        resolve_info.copy_dest_extent_length, true)) {
      encoder->memoryBarrier(MTL::BarrierScopeBuffers);
    }
  }

  bool encode_failed = false;
  if (copy_from_render_target) {
    // The kernel declares the origin binding as a writable device buffer, so
    // it is passed in a buffer slice like the other internal compute
    // constants.
    // The origin, then, for the refresh, the texture's width with the row
    // offset above it, and its logical height with the alias count and, in
    // the top byte, the column offset in 32-texel tiles - after the host
    // sample indices for the 64bpp averages, which the 32bpp four-sample one
    // doesn't read.
    const uint32_t refresh_constants[2] = {
        refresh_texture
            ? uint32_t(refresh_texture->width()) | (refresh_offset_y << 16)
            : 0,
        refresh_texture
            ? uint32_t(refresh_texture->height()) |
                  (refresh_alias_count << 16) | (refresh_offset_x >> 5 << 24)
            : 0};
    uint32_t rgb10_constants[5] = {rgb10_origin[0], rgb10_origin[1]};
    if (rgb10f_rgba16_average) {
      rgb10_constants[2] = rgb10_host_samples;
      rgb10_constants[3] = refresh_constants[0];
      rgb10_constants[4] = refresh_constants[1];
    } else if (refresh_texture) {
      rgb10_constants[2] = refresh_constants[0];
      rgb10_constants[3] = refresh_constants[1];
    } else {
      rgb10_constants[2] = rgb10_host_samples;
    }
    MTL::Buffer* origin_buffer = nullptr;
    NS::UInteger origin_offset = 0;
    if (command_processor_.AcquireSpirvArgumentBufferSlice(
            sizeof(rgb10_constants), kInternalComputeSliceAlignment,
            &origin_buffer, &origin_offset)) {
      std::memcpy(
          static_cast<uint8_t*>(origin_buffer->contents()) + origin_offset,
          rgb10_constants, sizeof(rgb10_constants));
      encoder->setComputePipelineState(rgb10_pipeline);
      encoder->setBytes(&copy_shader_constants, sizeof(copy_shader_constants),
                        0);
      encoder->setBuffer(dest_buffer, 0, 1);
      encoder->setBuffer(origin_buffer, origin_offset, 2);
      encoder->setTexture(rgb10_source, 0);
      encoder->useResource(rgb10_source, MTL::ResourceUsageRead);
      if (refresh_texture) {
        encoder->setTexture(refresh_texture, 1);
        // The Slang kernels declare all slots, even when their count is zero.
        for (uint32_t i = 0;
             i < MetalTextureCache::ResolveTextureRefreshTarget::kMaxAliases;
             ++i) {
          encoder->setTexture(i < refresh_alias_count ? refresh_textures[1 + i]
                                                      : refresh_texture,
                              2 + i);
        }
      }
      encoder->dispatchThreadgroups(
          MTL::Size::Make(group_count_x, group_count_y, 1),
          MTL::Size::Make(8, 8, 1));
    } else {
      encode_failed = true;
    }
  }
  EdramDumpShaderPitches pitches;
  pitches.dest_pitch = dump_pitch;

  // Constant across the resolve, unlike the pitches and offsets.
  uint32_t push_constants[kEdramDumpShaderPushConstantCount] = {};
  push_constants[kEdramDumpShaderPushConstantResolveEdramInfo] =
      copy_shader_constants.dest_relative.edram_info.packed;
  push_constants[kEdramDumpShaderPushConstantResolveCoordinateInfo] =
      copy_shader_constants.dest_relative.coordinate_info.packed;
  push_constants[kEdramDumpShaderPushConstantResolveDestInfo] =
      copy_shader_constants.dest_relative.dest_info.value;
  push_constants[kEdramDumpShaderPushConstantResolveDestCoordinateInfo] =
      copy_shader_constants.dest_relative.dest_coordinate_info.packed;
  push_constants[kEdramDumpShaderPushConstantResolveDestBase] =
      copy_shader_constants.dest_base;
  push_constants[kEdramDumpShaderPushConstantResolveHeightDiv8] =
      resolve_info.height_div_8;
  if (refresh_texture) {
    push_constants[kEdramDumpShaderPushConstantResolveTextureWidth] =
        uint32_t(refresh_texture->width()) | (refresh_offset_y << 16);
    push_constants[kEdramDumpShaderPushConstantResolveTextureHeight] =
        uint32_t(refresh_texture->height()) | (refresh_offset_x >> 5 << 24);
    encoder->setTexture(refresh_texture, 1);
    encoder->useResource(refresh_texture, MTL::ResourceUsageWrite);
    for (uint32_t i = 0; i < refresh_alias_count; ++i) {
      encoder->useResource(refresh_textures[1 + i], MTL::ResourceUsageWrite);
      // The Slang paths bound 2..4 above; generic refresh reserves 2 for
      // stencil.
      if (!copy_from_render_target) {
        encoder->setTexture(refresh_textures[1 + i],
                            kResolveRefreshMslFirstAliasTexture + i);
      }
    }
  }

  for (size_t rectangle_index = 0;
       !copy_from_render_target && rectangle_index < rectangles.size() &&
       !encode_failed;
       ++rectangle_index) {
    const ResolveCopyDumpRectangle& rect = rectangles[rectangle_index];
    RenderTargetKey rt_key =
        static_cast<MetalRenderTargetCache::MetalRenderTarget*>(
            rect.render_target)
            ->key();
    MTL::Texture* source_texture = sources[rectangle_index].texture;
    MTL::Texture* stencil_texture = sources[rectangle_index].stencil_texture;

    encoder->setComputePipelineState(sources[rectangle_index].pipeline);
    encoder->setTexture(source_texture, 0);
    if (stencil_texture) {
      encoder->setTexture(stencil_texture, 2);
    }
    encoder->setBuffer(dest_buffer, 0, 1);

    pitches.source_pitch = rt_key.GetPitchTiles();
    push_constants[kEdramDumpShaderPushConstantPitches] = pitches.pitches;

    // Tiles cover this many destination pixels, which is what the dispatch is
    // sized in - in the resolve's layout, which differs from the render
    // target's when a 1x resolve reads a 4x one's samples as pixels.
    uint32_t tile_pixels_x =
        (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) >>
        uint32_t(resolve_msaa_samples >= xenos::MsaaSamples::k4X);
    uint32_t tile_pixels_y =
        xenos::kEdramTileHeightSamples >>
        uint32_t(resolve_msaa_samples >= xenos::MsaaSamples::k2X);
    uint32_t pixels_per_thread = GetEdramDumpShaderResolvePixelsPerThread(
        rt_key.Is64bpp(), resolve_average, true);

    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rect.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      uint32_t dispatch_first_tile = dump_base + dispatch.offset;
      EdramDumpShaderOffsets offsets;
      offsets.dispatch_first_tile =
          rt_key.GetNonWrappedTileIndex(dispatch_first_tile);
      offsets.source_base_tiles = rt_key.base_tiles;
      push_constants[kEdramDumpShaderPushConstantOffsets] = offsets.offsets;

      // Where the dispatch starts in the resolve's tile grid, which the
      // threads place themselves against.
      uint32_t dispatch_tile_relative =
          dispatch_first_tile -
          copy_shader_constants.dest_relative.edram_info.base_tiles;
      EdramDumpShaderResolveDispatchTile dispatch_tile;
      dispatch_tile.tile_x = dispatch_tile_relative % dump_pitch;
      dispatch_tile.tile_y = dispatch_tile_relative / dump_pitch;
      push_constants[kEdramDumpShaderPushConstantResolveDispatchTile] =
          dispatch_tile.packed;

      encoder->setBytes(push_constants, sizeof(push_constants), 0);

      uint32_t threads_x =
          (dispatch.width_tiles * tile_pixels_x + (pixels_per_thread - 1)) /
          pixels_per_thread;
      encoder->dispatchThreadgroups(
          MTL::Size::Make(
              (threads_x + (kEdramDumpShaderResolveThreadsPerGroupX - 1)) /
                  kEdramDumpShaderResolveThreadsPerGroupX,
              (dispatch.height_tiles * tile_pixels_y +
               (kEdramDumpShaderResolveThreadsPerGroupY - 1)) /
                  kEdramDumpShaderResolveThreadsPerGroupY,
              1),
          MTL::Size::Make(kEdramDumpShaderResolveThreadsPerGroupX,
                          kEdramDumpShaderResolveThreadsPerGroupY, 1));
    }
  }

  if (!shared_encoder) {
    own_hazards.End();
    encoder->endEncoding();
  }
  if (owns_command_buffer) {
    command_processor_.CommitAndAwait(
        cmd, MetalCommandProcessor::GpuWaitReason::kResolve);
  }
  // cmd is autoreleased from commandBuffer() - do not release
  if (encode_failed) {
    // Whatever was encoded wrote part of the destination, but the round trip
    // the caller falls back to rewrites all of it, and later encoders in this
    // command buffer run after these.
    XELOGE(
        "MetalRenderTargetCache::DirectResolveRenderTargets: out of shader "
        "argument space, falling back to the EDRAM round trip");
    return false;
  }
  return true;
}

namespace {

// Laid out as XeTileResolveConstants in kTileResolveLibrarySource.
struct TileResolveConstants {
  // The render target pixel of resolve pixel (0, 0).
  uint32_t origin[2];
  // The resolve rectangle, in pixels.
  uint32_t size[2];
  // Where the rectangle starts in the destination, in pixels.
  uint32_t dest_offset[2];
  uint32_t dest_pitch_macro_tiles;
  uint32_t dest_slice_pitch_macro_tiles;
  uint32_t dest_base;
  uint32_t dest_array;
  uint32_t dest_slice;
  // 0 for none, 1 for XeResolveSwapRedBlue_8_8_8_8, 2 for
  // XeResolveSwapRedBlue_2_10_10_10, 3 for XeResolveSwap4PixelsRedBlue64bpp.
  uint32_t swap;
  // XeEndianSwap32, of each dword at 64bpp.
  uint32_t swap_bytes_in_16;
  uint32_t swap_halves_in_32;
  // XeEndianSwap64's dword exchange for 8in64, before the 32-bit swaps.
  uint32_t exchange_dwords;
  // 2 for 32bpp, 3 for 64bpp.
  uint32_t bytes_per_block_log2;
  // The refreshed texture-cache textures' size, and the texel of resolve
  // pixel (0, 0).
  uint32_t refresh_size[2];
  uint32_t refresh_offset[2];
  // The 16_16_16_16 conversion: the destination's number format, and the
  // exponent bias factor's float bits, as XeResolveGetInfo decodes them.
  uint32_t num_format;
  uint32_t exp_bias_factor_bits;
};
// The shader struct's size, a multiple of its uint2 alignment.
static_assert(sizeof(TileResolveConstants) == 88);

// The dump shader's direct resolve per pixel (edram_dump_shader.cc), reading
// the render target from the pass's tiles: the render target's bits packed as
// the dump packs them, the red/blue swap of the resolve's format, the endian
// swap, and the store at XeResolveDestPixelAddress. One thread per pixel, one
// threadgroup per tile.
const char kTileResolveLibrarySource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct XeTileResolveConstants {
  uint2 origin;
  uint2 size;
  uint2 dest_offset;
  uint dest_pitch_macro_tiles;
  uint dest_slice_pitch_macro_tiles;
  uint dest_base;
  uint dest_array;
  uint dest_slice;
  uint swap;
  uint swap_bytes_in_16;
  uint swap_halves_in_32;
  uint exchange_dwords;
  uint bytes_per_block_log2;
  uint2 refresh_size;
  uint2 refresh_offset;
  uint num_format;
  uint exp_bias_factor_bits;
};

// XenosTextureTiledAddressCombine.
static uint XeTileResolveCombine(uint outer_inner_bytes, uint bank, uint pipe,
                                 uint y_lsb) {
  return (y_lsb << 4) | (pipe << 6) | (bank << 11) |
         (outer_inner_bytes & 0xFu) | (((outer_inner_bytes >> 4) & 0x1u) << 5) |
         (((outer_inner_bytes >> 5) & 0x7u) << 8) |
         (outer_inner_bytes >> 8 << 12);
}

// XenosTextureTiledAddress2D / 3D.
static uint XeTileResolveTiledAddress(uint2 p,
                                      constant XeTileResolveConstants& c) {
  if (c.dest_array != 0u) {
    uint outer = ((((c.dest_slice >> 2) * c.dest_slice_pitch_macro_tiles +
                    (p.y >> 4)) * c.dest_pitch_macro_tiles) + (p.x >> 5)) << 7;
    uint inner = ((c.dest_slice & 0x3u) << 5) | (((p.y >> 1) & 0x3u) << 3) |
                 (p.x & 0x7u);
    uint bank = ((p.y >> 3) ^ (c.dest_slice >> 2)) & 0x1u;
    return XeTileResolveCombine((outer | inner) << c.bytes_per_block_log2,
                                bank,
                                ((p.x >> 3) & 0x3u) ^ (bank << 1), p.y & 1u);
  }
  uint outer = ((p.y >> 5) * c.dest_pitch_macro_tiles + (p.x >> 5)) << 6;
  uint inner = (((p.y >> 1) & 0x7u) << 3) | (p.x & 0x7u);
  return XeTileResolveCombine((outer | inner) << c.bytes_per_block_log2,
                              (p.y >> 4) & 0x1u,
                              ((p.x >> 3) & 0x3u) ^ (((p.y >> 3) & 0x1u) << 1),
                              p.y & 1u);
}

// XeResolveSwapRedBlue_8_8_8_8 / XeResolveSwapRedBlue_2_10_10_10, keyed on the
// resolve's format as in the dump shader.
static uint XeTileResolveSwapRedBlue(constant XeTileResolveConstants& c,
                                     uint value) {
  if (c.swap == 1u) {
    value = (value & ~0xFF00FFu) | ((value & 0xFFu) << 16) |
            ((value >> 16) & 0xFFu);
  } else if (c.swap == 2u) {
    value = (value & ~0x3FF003FFu) | ((value & 0x3FFu) << 20) |
            ((value >> 20) & 0x3FFu);
  }
  return value;
}
// XeResolveSwap4PixelsRedBlue64bpp - the low halves of the two dwords.
static uint2 XeTileResolveSwapRedBlue(constant XeTileResolveConstants& c,
                                      uint2 value) {
  if (c.swap == 3u) {
    value = uint2((value.x & ~0xFFFFu) | (value.y & 0xFFFFu),
                  (value.y & ~0xFFFFu) | (value.x & 0xFFFFu));
  }
  return value;
}

// The texture-cache refresh texel: the swapped bits, not yet
// endian-transformed, as the ordinary loader would copy them from the
// resolved memory.
static uint4 XeTileResolveRefreshTexel(uint value) {
  return uint4(value, 0u, 0u, 0u);
}
static uint4 XeTileResolveRefreshTexel(uint2 value) {
  return uint4(value, 0u, 0u);
}

// XeEndianSwap32, and the store at XeResolveDestPixelAddress.
static void XeTileResolveStore(constant XeTileResolveConstants& c,
                               device uint* dest, uint2 p, uint value) {
  if (c.swap_bytes_in_16 != 0u) {
    value = ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
  }
  if (c.swap_halves_in_32 != 0u) {
    value = (value << 16) | (value >> 16);
  }
  dest[(c.dest_base + XeTileResolveTiledAddress(p + c.dest_offset, c)) >> 2] =
      value;
}
// XeEndianSwap64 - the dword exchange for 8in64, then XeEndianSwap32 on each
// dword - and the store of both.
static void XeTileResolveStore(constant XeTileResolveConstants& c,
                               device uint* dest, uint2 p, uint2 value) {
  if (c.exchange_dwords != 0u) {
    value = value.yx;
  }
  if (c.swap_bytes_in_16 != 0u) {
    value = ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
  }
  if (c.swap_halves_in_32 != 0u) {
    value = (value << 16) | (value >> 16);
  }
  uint index =
      (c.dest_base + XeTileResolveTiledAddress(p + c.dest_offset, c)) >> 2;
  dest[index] = value.x;
  dest[index + 1] = value.y;
}

// As the dump shader packs a k_8_8_8_8 render target: each component
// converted with OpConvertFToU(c * 255 + 0.5).
static uint XeTileResolvePack_8888(float4 color,
                                    constant XeTileResolveConstants& c) {
  uint4 bytes = uint4(color * 255.0f + 0.5f);
  return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}
// The ownership transfer view's bits. Every tile dispatch of a pass has to
// declare the same imageblock member type, so the attachments are read as
// half4 (16-bit float, exact, denormals included) or float4 (the rest; a 32-bit
// float is exact either way).
static uint XeTileResolvePack_16_16Float(half4 color,
                                    constant XeTileResolveConstants& c) {
  return as_type<uint>(color.xy);
}
static uint XeTileResolvePack_32Float(float4 color,
                                    constant XeTileResolveConstants& c) {
  return as_type<uint>(color.x);
}
// The RGBA16Uint ownership transfer view's components, two to a dword, red
// and blue in the low halves.
static uint2 XeTileResolvePack_16_16_16_16Float(half4 color,
                                     constant XeTileResolveConstants& c) {
  return as_type<uint2>(color);
}
// The RG32Uint ownership transfer view's.
static uint2 XeTileResolvePack_32_32Float(float4 color,
                                     constant XeTileResolveConstants& c) {
  return as_type<uint2>(color.xy);
}

// XePackFixed for 16 bits.
static uint XeTileResolvePackFixed16(float value, uint num_format) {
  uint unsigned_max = (1u << 16u) - 1u;
  uint packed = 0u;
  if (num_format == 1u) {
    // Signed fraction uses the positive endpoint, then keeps the destination
    // bit width.
    float positive_max = float((1u << (16u - 1u)) - 1u);
    packed = uint(int(min(max(value, -1.0f), 1.0f) * positive_max +
                      (value >= 0.0f ? 0.5f : -0.5f)));
  } else if (num_format == 2u) {
    packed = uint(min(max(value, 0.0f), float(unsigned_max)) + 0.5f);
  } else if (num_format == 3u) {
    int signed_min = -int(1u << (16u - 1u));
    int signed_max = int((1u << (16u - 1u)) - 1u);
    packed = uint(int(min(max(value, float(signed_min)), float(signed_max)) +
                      (value >= 0.0f ? 0.5f : -0.5f)));
  } else {
    // Unsigned fraction, float-on-fixed, or anything unexpected: keep the
    // usual unsigned-fraction pack.
    packed = uint(saturate(value) * float(unsigned_max) + 0.5f);
  }
  return packed & unsigned_max;
}
// resolve_full_64bpp_rgba16_1x_direct's conversion of one pixel of a
// 16_16_16_16 render target (RGBA16Snorm, read as float, whose stored bits are
// rint(v * 32767)): XeUnpackR16G16B16A16Edram, the exponent bias and
// XePackR16G16B16A16. The red/blue swap comes after, on the packed halves,
// which is the same as swapping the floats before packing.
static uint2 XeTileResolvePack_16_16_16_16(float4 color,
                                           constant XeTileResolveConstants& c) {
  float4 pixel = float4(int4(rint(color * 32767.0f))) * (1.0f / 1024.0f);
  pixel *= as_type<float>(c.exp_bias_factor_bits);
  uint4 n = uint4(XeTileResolvePackFixed16(pixel.x, c.num_format),
                  XeTileResolvePackFixed16(pixel.y, c.num_format),
                  XeTileResolvePackFixed16(pixel.z, c.num_format),
                  XeTileResolvePackFixed16(pixel.w, c.num_format));
  return n.xz | (n.yw << 16u);
}

// The console's average of 8-bit unorm samples, as the dump shader's
// load_average_rgba8 does it: each byte widened to 10 bits by replication,
// the sum divided by 4 truncating, then truncated to 8 bits.
static uint4 XeTileResolveWiden8(uint4 bytes) {
  return (bytes << 2u) | (bytes >> 6u);
}
static uint4 XeTileResolveBytes(float4 unorm) {
  return uint4(unorm * 255.0f + 0.5f);
}
static uint4 XeTileResolveBytes(uint packed) {
  return (uint4(packed) >> uint4(0u, 8u, 16u, 24u)) & 0xFFu;
}
static uint XeTileResolveAverage4Exact(uint4 sum10) {
  uint4 n = sum10 >> 4u;
  return n.x | (n.y << 8u) | (n.z << 16u) | (n.w << 24u);
}
static uint XeTileResolvePack_8888Average4(float4 s0, float4 s1, float4 s2,
                                           float4 s3,
                                           constant XeTileResolveConstants& c) {
  return XeTileResolveAverage4Exact(
      XeTileResolveWiden8(XeTileResolveBytes(s0)) +
      XeTileResolveWiden8(XeTileResolveBytes(s1)) +
      XeTileResolveWiden8(XeTileResolveBytes(s2)) +
      XeTileResolveWiden8(XeTileResolveBytes(s3)));
}
// SPIRV-Cross's NoContraction helpers, inlined as CompileFunctionMsl inlines
// them.
static float XeTileResolveNoContractionMul(float l, float r) {
  return fma(l, r, 0.0f);
}
static float XeTileResolveNoContractionAdd(float l, float r) {
  return fma(1.0f, l, r);
}
// SpirvShaderTranslator::LinearToPWLGamma of a pre-saturated value, with its
// float constants' exact bits.
static float XeTileResolveLinearToPWLGamma(float linear) {
  bool is_piece_at_least_3 = linear >= as_type<float>(0x3F002008u);
  float scale_3_or_2 = is_piece_at_least_3 ? as_type<float>(0x42FFC000u)
                                           : as_type<float>(0x437FC000u);
  float offset_3_or_2 = is_piece_at_least_3 ? as_type<float>(0x3F008081u)
                                            : as_type<float>(0x3E808081u);
  bool is_piece_at_least_1 = linear >= as_type<float>(0x3D802008u);
  float scale_1_or_0 = is_piece_at_least_1 ? as_type<float>(0x43FFC000u)
                                           : 1023.0f;
  float offset_1_or_0 =
      is_piece_at_least_1 ? as_type<float>(0x3E008081u) : 0.0f;
  bool is_piece_at_least_2 = linear >= as_type<float>(0x3E002008u);
  float scale = is_piece_at_least_2 ? scale_3_or_2 : scale_1_or_0;
  float offset = is_piece_at_least_2 ? offset_3_or_2 : offset_1_or_0;
  return XeTileResolveNoContractionAdd(
      XeTileResolveNoContractionMul(
          trunc(XeTileResolveNoContractionMul(linear, scale)),
          as_type<float>(0x3B808081u)),
      offset);
}
// An 8_8_8_8_GAMMA render target's sample (linear, in RGBA16Unorm) as the
// dump shader's load_and_pack packs it.
static uint XeTileResolveGammaPack(float4 linear) {
  uint packed = uint(XeTileResolveLinearToPWLGamma(linear.x) * 255.0f + 0.5f);
  packed |= uint(XeTileResolveLinearToPWLGamma(linear.y) * 255.0f + 0.5f)
            << 8u;
  packed |= uint(XeTileResolveLinearToPWLGamma(linear.z) * 255.0f + 0.5f)
            << 16u;
  packed |= uint(linear.w * 255.0f + 0.5f) << 24u;
  return packed;
}
// The bit copy of a one-sample 8_8_8_8_GAMMA render target.
static uint XeTileResolvePack_8888Gamma(float4 color,
                                        constant XeTileResolveConstants& c) {
  return XeTileResolveGammaPack(color);
}
// As the dump shader packs a k_2_10_10_10 render target (RGB10A2Unorm): each
// component converted with OpConvertFToU(c * 1023 + 0.5), alpha with 3, and
// inserted 10 (alpha 2) bits wide.
static uint XeTileResolvePack_RGB10(float4 color,
                                    constant XeTileResolveConstants& c) {
  uint4 n = uint4(color * float4(1023.0f, 1023.0f, 1023.0f, 3.0f) + 0.5f) &
            uint4(1023u, 1023u, 1023u, 3u);
  return n.x | (n.y << 10u) | (n.z << 20u) | (n.w << 30u);
}
// The dump's 7e3 packing of a host color component, on the bits
// (XeResolveDirectFloat32To7e3): NaNs and everything with the sign bit become
// 0, the rest is clamped to 31.875 and rounded to the nearest even.
static uint XeTileResolveFloat32To7e3(float value) {
  uint f32 = as_type<uint>(value);
  if (f32 > 0x7F800000u) {
    f32 = 0u;
  }
  f32 = min(f32, 0x41FF0000u);
  uint biased_f32;
  if (f32 < 0x3E800000u) {
    biased_f32 = ((f32 & 0x7FFFFFu) | 0x800000u) >>
                 min(125u - (f32 >> 23u), 24u);
  } else {
    biased_f32 = f32 - (124u << 23u);
  }
  return ((biased_f32 + 0x7FFFu + ((biased_f32 >> 16u) & 1u)) >> 16u) &
         0x3FFu;
}
// The bit copy of a one-sample 2_10_10_10_FLOAT render target (RGBA16Float,
// read as float like the compute copies read it), as the dump packs it
// (XeResolveDirectPackRGB10A2Float): 7e3 RGB, and alpha clamped to [0, 1] the
// same way as a 2-bit unsigned fraction. Half denormals are below half the
// smallest 7e3 step, so they pack to 0 whether or not they are flushed.
static uint XeTileResolvePack_RGB10Float(float4 color,
                                         constant XeTileResolveConstants& c) {
  uint alpha_bits = as_type<uint>(color.w);
  if (alpha_bits > 0x7F800000u) {
    alpha_bits = 0u;
  }
  float alpha = as_type<float>(min(alpha_bits, 0x3F800000u));
  return XeTileResolveFloat32To7e3(color.x) |
         (XeTileResolveFloat32To7e3(color.y) << 10u) |
         (XeTileResolveFloat32To7e3(color.z) << 20u) |
         (uint(alpha * 3.0f + 0.5f) << 30u);
}
// XeUnpackR10G10B10A2Float's 7e3 component.
static float XeTileResolve7e3To32(uint f10) {
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint mantissa_lzcnt = 7u - (31u - clz(mantissa));
    exponent = 1u - mantissa_lzcnt;
    mantissa = (mantissa << mantissa_lzcnt) & 0x7Fu;
  }
  return as_type<float>(f10 != 0u ? ((exponent + 124u) << 23u) |
                                        (mantissa << 16u)
                                  : 0u);
}
// resolve_full_32bpp_rgb10f_1x_direct's conversion of one pixel: the dump's
// packing, XeUnpackR10G10B10A2Float, the exponent bias and XePackR10G10B10A2
// as an unsigned fraction. The red/blue swap comes after, on the packed
// fields, which is the same as swapping the floats before packing. The color
// products are exact; alpha's are exact or above a multiple of 1/3, never at
// a rounding boundary, so contraction changes none of them.
static uint XeTileResolvePack_RGB10FloatToRGB10(
    float4 color, constant XeTileResolveConstants& c) {
  uint packed = XeTileResolvePack_RGB10Float(color, c);
  float4 pixel = float4(XeTileResolve7e3To32(packed & 0x3FFu),
                        XeTileResolve7e3To32((packed >> 10u) & 0x3FFu),
                        XeTileResolve7e3To32((packed >> 20u) & 0x3FFu),
                        float(packed >> 30u) * (1.0f / 3.0f));
  pixel *= as_type<float>(c.exp_bias_factor_bits);
  uint4 n = uint4(saturate(pixel) * float4(1023.0f, 1023.0f, 1023.0f, 3.0f) +
                  0.5f) &
            uint4(1023u, 1023u, 1023u, 3u);
  return n.x | (n.y << 10u) | (n.z << 20u) | (n.w << 30u);
}
static uint XeTileResolvePack_8888GammaAverage4(
    float4 s0, float4 s1, float4 s2, float4 s3,
    constant XeTileResolveConstants& c) {
  return XeTileResolveAverage4Exact(
      XeTileResolveWiden8(XeTileResolveBytes(XeTileResolveGammaPack(s0))) +
      XeTileResolveWiden8(XeTileResolveBytes(XeTileResolveGammaPack(s1))) +
      XeTileResolveWiden8(XeTileResolveBytes(XeTileResolveGammaPack(s2))) +
      XeTileResolveWiden8(XeTileResolveBytes(XeTileResolveGammaPack(s3))));
}

// resolve_full_32bpp_rgb10_4x_direct's average of a 2_10_10_10 render target
// (RGB10A2Unorm), in its operation order: each sample packed as
// XeResolveDirectPackRGB10A2 packs it and unpacked as XeUnpackR10G10B10A2UNorm,
// with 1/1023 and 1/3's exact bits, summed in sample order, times
// dest_exp_bias_factor * 0.5 * 0.5 (no bias), and packed as XePackFixed packs
// unsigned fractions. Each multiply and add is rounded on its own (fma with a
// zero or unit operand), as the compute shader rounds them, so the output
// matches it bit for bit.
static float4 XeTileResolveRGB10Bits(float4 sample) {
  return float4(uint4(uint(sample.x * 1023.0f + 0.5f),
                      uint(sample.y * 1023.0f + 0.5f),
                      uint(sample.z * 1023.0f + 0.5f),
                      uint(sample.w * 3.0f + 0.5f)) &
                uint4(1023u, 1023u, 1023u, 3u));
}
static uint XeTileResolvePack_RGB10Average4(float4 s0, float4 s1, float4 s2,
                                            float4 s3,
                                            constant XeTileResolveConstants& c) {
  const float4 scale =
      float4(as_type<float>(0x3A802008u), as_type<float>(0x3A802008u),
             as_type<float>(0x3A802008u), as_type<float>(0x3EAAAAABu));
  float4 sum = fma(XeTileResolveRGB10Bits(s0), scale, float4(0.0f));
  sum = fma(float4(1.0f), sum,
            fma(XeTileResolveRGB10Bits(s1), scale, float4(0.0f)));
  sum = fma(float4(1.0f), sum,
            fma(XeTileResolveRGB10Bits(s2), scale, float4(0.0f)));
  sum = fma(float4(1.0f), sum,
            fma(XeTileResolveRGB10Bits(s3), scale, float4(0.0f)));
  float4 average = sum * 0.25f;
  uint4 n = uint4(saturate(average) * float4(1023.0f, 1023.0f, 1023.0f, 3.0f) +
                  0.5f) &
            uint4(1023u, 1023u, 1023u, 3u);
  return n.x | (n.y << 10u) | (n.z << 20u) | (n.w << 30u);
}

// The pixel's one value, or its four samples (the guest 4x sample numbering
// matches the host one).
#define XE_TILE_RESOLVE_READ_PIXEL(kernel_name)                               \
  XeTileResolvePack_##kernel_name(pixels.read(pixel_in_tile).color, constants)
#define XE_TILE_RESOLVE_READ_4_SAMPLES(kernel_name)                           \
  XeTileResolvePack_##kernel_name(                                            \
      pixels.read(pixel_in_tile, 0, imageblock_data_rate::sample).color,      \
      pixels.read(pixel_in_tile, 1, imageblock_data_rate::sample).color,      \
      pixels.read(pixel_in_tile, 2, imageblock_data_rate::sample).color,      \
      pixels.read(pixel_in_tile, 3, imageblock_data_rate::sample).color,      \
      constants)

// How many texture-cache textures the resolve refreshes, 0 to 4: the first
// and its aliases, R32Uint (32bpp) or RG32Uint (64bpp) 2D array views.
constant uint xe_tile_resolve_refresh_count [[function_constant(0)]];
constant bool xe_tile_resolve_refresh_0 = xe_tile_resolve_refresh_count > 0u;
constant bool xe_tile_resolve_refresh_1 = xe_tile_resolve_refresh_count > 1u;
constant bool xe_tile_resolve_refresh_2 = xe_tile_resolve_refresh_count > 2u;
constant bool xe_tile_resolve_refresh_3 = xe_tile_resolve_refresh_count > 3u;

#define XE_TILE_RESOLVE(kernel_name, member_type, read, index)                \
  struct XeTileResolvePixel_##kernel_name##_##index {                         \
    member_type color [[color(index)]];                                       \
  };                                                                          \
  kernel void xe_tile_resolve_##kernel_name##_##index(                        \
      imageblock<XeTileResolvePixel_##kernel_name##_##index,                  \
                 imageblock_layout_implicit> pixels,                          \
      constant XeTileResolveConstants& constants [[buffer(0)]],               \
      device uint* dest [[buffer(1)]],                                        \
      texture2d_array<uint, access::write> refresh_0                          \
      [[texture(0), function_constant(xe_tile_resolve_refresh_0)]],           \
      texture2d_array<uint, access::write> refresh_1                          \
      [[texture(1), function_constant(xe_tile_resolve_refresh_1)]],           \
      texture2d_array<uint, access::write> refresh_2                          \
      [[texture(2), function_constant(xe_tile_resolve_refresh_2)]],           \
      texture2d_array<uint, access::write> refresh_3                          \
      [[texture(3), function_constant(xe_tile_resolve_refresh_3)]],           \
      ushort2 pixel_in_tile [[thread_position_in_threadgroup]],               \
      uint2 pixel [[thread_position_in_grid]]) {                              \
    uint2 p = pixel - constants.origin;                                       \
    if (p.x >= constants.size.x || p.y >= constants.size.y) {                 \
      return;                                                                 \
    }                                                                         \
    auto value =                                                              \
        XeTileResolveSwapRedBlue(constants, read(kernel_name));               \
    if (xe_tile_resolve_refresh_0) {                                          \
      uint2 texel = p + constants.refresh_offset;                             \
      if (texel.x < constants.refresh_size.x &&                               \
          texel.y < constants.refresh_size.y) {                               \
        uint4 refresh = XeTileResolveRefreshTexel(value);                     \
        refresh_0.write(refresh, texel, 0);                                   \
        if (xe_tile_resolve_refresh_1) {                                      \
          refresh_1.write(refresh, texel, 0);                                 \
        }                                                                     \
        if (xe_tile_resolve_refresh_2) {                                      \
          refresh_2.write(refresh, texel, 0);                                 \
        }                                                                     \
        if (xe_tile_resolve_refresh_3) {                                      \
          refresh_3.write(refresh, texel, 0);                                 \
        }                                                                     \
      }                                                                       \
    }                                                                         \
    XeTileResolveStore(constants, dest, p, value);                            \
  }
#define XE_TILE_RESOLVE_ALL_INDICES(kernel_name, member_type, read)           \
  XE_TILE_RESOLVE(kernel_name, member_type, read, 0)                          \
  XE_TILE_RESOLVE(kernel_name, member_type, read, 1)                          \
  XE_TILE_RESOLVE(kernel_name, member_type, read, 2)                          \
  XE_TILE_RESOLVE(kernel_name, member_type, read, 3)
XE_TILE_RESOLVE_ALL_INDICES(8888, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(16_16Float, half4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(32Float, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(16_16_16_16Float, half4,
                            XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(32_32Float, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(16_16_16_16, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(8888Average4, float4,
                            XE_TILE_RESOLVE_READ_4_SAMPLES)
XE_TILE_RESOLVE_ALL_INDICES(8888GammaAverage4, float4,
                            XE_TILE_RESOLVE_READ_4_SAMPLES)
XE_TILE_RESOLVE_ALL_INDICES(RGB10Average4, float4,
                            XE_TILE_RESOLVE_READ_4_SAMPLES)
XE_TILE_RESOLVE_ALL_INDICES(8888Gamma, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(RGB10, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(RGB10Float, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(RGB10FloatToRGB10, float4,
                            XE_TILE_RESOLVE_READ_PIXEL)
)METAL";

const char* const kTileResolveKernelNames[] = {"8888",
                                               "16_16Float",
                                               "32Float",
                                               "16_16_16_16Float",
                                               "32_32Float",
                                               "16_16_16_16",
                                               "8888Average4",
                                               "8888GammaAverage4",
                                               "RGB10Average4",
                                               "8888Gamma",
                                               "RGB10",
                                               "RGB10Float",
                                               "RGB10FloatToRGB10"};

// The largest tile area Metal allows at each sample count (Metal feature set
// tables) - a 4x pass with 32x32 tiles hung the GPU without validation.
uint32_t GetMaxTileArea(uint32_t sample_count) {
  switch (sample_count) {
    case 1:
    case 2:
      return 32 * 32;
    case 4:
      return 32 * 16;
    case 8:
      return 16 * 16;
    default:
      return 0;
  }
}

}  // namespace

MTL::RenderPipelineState* MetalResolveEncoder::GetOrCreateTileResolvePipeline(
    MTL::RenderPassDescriptor* pass_descriptor, uint32_t color_index,
    TileResolveKernel kernel, uint32_t sample_count, uint32_t refresh_count) {
  // The pass's color formats, 10 bits each, then the color index (2 bits),
  // the kernel (4 bits), the samples (4 bits) and the refresh count.
  uint64_t key = 0;
  MTL::PixelFormat formats[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MTL::Texture* texture =
        pass_descriptor->colorAttachments()->object(i)->texture();
    formats[i] = texture ? texture->pixelFormat() : MTL::PixelFormatInvalid;
    if (uint32_t(formats[i]) >= 1024) {
      return nullptr;
    }
    key |= uint64_t(formats[i]) << (10 * i);
  }
  key |= uint64_t(color_index) << 40 | uint64_t(kernel) << 42 |
         uint64_t(sample_count) << 46 | uint64_t(refresh_count) << 50;
  auto it = tile_resolve_pipelines_.find(key);
  if (it != tile_resolve_pipelines_.end()) {
    return it->second;
  }

  if (!tile_resolve_library_ && !tile_resolve_library_failed_) {
    NS::Error* error = nullptr;
    tile_resolve_library_ = device_->newLibrary(
        NS::String::string(kTileResolveLibrarySource, NS::UTF8StringEncoding),
        nullptr, &error);
    if (!tile_resolve_library_) {
      tile_resolve_library_failed_ = true;
      XELOGE("tresolve: failed to compile the tile resolve library: {}",
             error && error->localizedDescription()
                 ? error->localizedDescription()->utf8String()
                 : "unknown error");
    }
  }
  MTL::RenderPipelineState* pipeline = nullptr;
  if (tile_resolve_library_) {
    std::string function_name = std::string("xe_tile_resolve_") +
                                kTileResolveKernelNames[uint32_t(kernel)] +
                                "_" + std::to_string(color_index);
    MTL::FunctionConstantValues* constant_values =
        MTL::FunctionConstantValues::alloc()->init();
    constant_values->setConstantValue(&refresh_count, MTL::DataTypeUInt,
                                      NS::UInteger(0));
    NS::Error* function_error = nullptr;
    MTL::Function* function = tile_resolve_library_->newFunction(
        NS::String::string(function_name.c_str(), NS::UTF8StringEncoding),
        constant_values, &function_error);
    constant_values->release();
    if (!function) {
      XELOGE("tresolve: failed to specialize {}: {}", function_name,
             function_error && function_error->localizedDescription()
                 ? function_error->localizedDescription()->utf8String()
                 : "unknown error");
    }
    MTL::TileRenderPipelineDescriptor* descriptor =
        MTL::TileRenderPipelineDescriptor::alloc()->init();
    if (function && descriptor) {
      descriptor->setTileFunction(function);
      descriptor->setRasterSampleCount(sample_count);
      descriptor->setThreadgroupSizeMatchesTileSize(true);
      for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        descriptor->colorAttachments()->object(i)->setPixelFormat(formats[i]);
      }
      NS::Error* error = nullptr;
      pipeline = device_->newRenderPipelineState(
          descriptor, MTL::PipelineOptionNone, nullptr, &error);
      if (!pipeline) {
        XELOGE("tresolve: failed to create {}: {}", function_name,
               error && error->localizedDescription()
                   ? error->localizedDescription()->utf8String()
                   : "unknown error");
      }
    }
    if (descriptor) {
      descriptor->release();
    }
    if (function) {
      function->release();
    }
  }
  tile_resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
}

const char* MetalResolveEncoder::TileResolveInOpenPass(
    const draw_util::ResolveInfo& resolve_info,
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, std::span<MTL::Texture* const> refresh_textures,
    uint32_t refresh_offset_x, uint32_t refresh_offset_y,
    size_t post_pass_resolves_count, PostPassRefreshCopy* refresh_copy_out,
    bool* has_refresh_copy_out) {
  if (has_refresh_copy_out) {
    *has_refresh_copy_out = false;
  }
  if (!device_->supportsFamily(MTL::GPUFamilyApple4)) {
    return "family";
  }
  if (resolve_info.IsCopyingDepth()) {
    return "depth";
  }
  // Only set under resolution scaling, which takes the EDRAM path, but the
  // tile kernels don't fill.
  if (copy_shader_constants.dest_relative.edram_info.fill_half_pixel_offset) {
    return "half_pixel_fill";
  }
  // The dump shader's bit copies and 8_8_8_8 four-sample average, the
  // 16_16_16_16 conversion, the rgb10-4x direct copy's 2_10_10_10 four-sample
  // average and the rgb10f-1x one's 2_10_10_10_FLOAT to 2_10_10_10
  // conversion have tile variants.
  const bool rgba16_1x =
      IsFull64RGBA16OneSample(copy_shader, copy_shader_constants);
  const bool average_4x =
      IsFull32RGBA8FourSampleAverage(copy_shader, copy_shader_constants);
  const bool rgb10_4x =
      IsFull32RGB10FourSampleAverage(copy_shader, copy_shader_constants);
  const bool four_samples = average_4x || rgb10_4x;
  const bool rgb10f_1x =
      IsFull32RGB10FloatOneSample(copy_shader, copy_shader_constants);
  switch (copy_shader) {
    case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
      break;
    default:
      if (!rgba16_1x && !four_samples && !rgb10f_1x) {
        return "kernel";
      }
      break;
  }

  // One render target owning every tile of the resolve.
  using ResolveCopyDumpRectangle =
      MetalRenderTargetCache::ResolveCopyDumpRectangle;
  using RenderTargetKey = MetalRenderTargetCache::RenderTargetKey;
  std::vector<ResolveCopyDumpRectangle> rectangles;
  render_target_cache_.GetResolveCopyRectanglesToDump(
      dump_base, dump_row_length_used, dump_rows, dump_pitch, rectangles);
  if (rectangles.size() != 1 || !rectangles[0].render_target) {
    return "rectangles";
  }
  const ResolveCopyDumpRectangle& rectangle = rectangles[0];
  if (rectangle.row_first || rectangle.rows != dump_rows ||
      rectangle.row_first_start ||
      rectangle.row_last_end != dump_row_length_used) {
    return "partial_owner";
  }
  auto* rt = static_cast<MetalRenderTargetCache::MetalRenderTarget*>(
      rectangle.render_target);
  const RenderTargetKey rt_key = rt->key();
  if (rt_key.is_depth) {
    return "depth";
  }
  const xenos::MsaaSamples msaa_samples =
      four_samples ? xenos::MsaaSamples::k4X : xenos::MsaaSamples::k1X;
  if (rt_key.msaa_samples != msaa_samples) {
    return "msaa";
  }
  TileResolveKernel kernel;
  MTL::PixelFormat source_format;
  switch (rt_key.GetColorFormat()) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      kernel = average_4x ? TileResolveKernel::k8888Average4
                          : TileResolveKernel::k8888;
      source_format = MTL::PixelFormatRGBA8Unorm;
      break;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      // Linear in RGBA16Unorm (gamma_render_target_as_unorm16), which the
      // PWL encoding expects: the average, or the bit copy of one sample.
      kernel = average_4x ? TileResolveKernel::k8888GammaAverage4
                          : TileResolveKernel::k8888Gamma;
      source_format = MTL::PixelFormatRGBA16Unorm;
      break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      // The rgb10-4x average, or the bit copy of one sample.
      kernel = rgb10_4x ? TileResolveKernel::kRGB10Average4
                        : TileResolveKernel::kRGB10;
      source_format = MTL::PixelFormatRGB10A2Unorm;
      break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      // The bit copy of one sample, or the rgb10f-1x conversion.
      kernel = rgb10f_1x ? TileResolveKernel::kRGB10FloatToRGB10
                         : TileResolveKernel::kRGB10Float;
      source_format = MTL::PixelFormatRGBA16Float;
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      kernel = TileResolveKernel::k16_16Float;
      source_format = MTL::PixelFormatRG16Float;
      break;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      kernel = TileResolveKernel::k32Float;
      source_format = MTL::PixelFormatR32Float;
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      kernel = TileResolveKernel::k16_16_16_16Float;
      source_format = MTL::PixelFormatRGBA16Float;
      break;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      kernel = TileResolveKernel::k32_32Float;
      source_format = MTL::PixelFormatRG32Float;
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      // Only the conversion: the bit copy of the raw bits isn't ported.
      if (!rgba16_1x) {
        return "format";
      }
      kernel = TileResolveKernel::k16_16_16_16;
      source_format = MTL::PixelFormatRGBA16Snorm;
      break;
    default:
      return "format";
  }
  if ((rgba16_1x && kernel != TileResolveKernel::k16_16_16_16) ||
      (average_4x && kernel != TileResolveKernel::k8888Average4 &&
       kernel != TileResolveKernel::k8888GammaAverage4) ||
      (rgb10_4x && kernel != TileResolveKernel::kRGB10Average4) ||
      (rgb10f_1x && kernel != TileResolveKernel::kRGB10FloatToRGB10)) {
    return "kernel";
  }
  const bool is_64bpp = rt_key.Is64bpp();
  const uint32_t sample_count =
      MetalRenderTargetCache::MsaaSamplesToCount(msaa_samples);
  MTL::Texture* source = rt->texture();
  if (!source || source->pixelFormat() != source_format ||
      source->sampleCount() != sample_count) {
    return "host_format";
  }

  // Resolve pixel p is render target pixel p + origin while the resolve's
  // rows are the render target's rows, as in DirectResolveRenderTargets'
  // rgb10 copies - which with one owner covering the whole span is also where
  // the dump shader reads it.
  const draw_util::ResolveEdramInfo& edram_info =
      copy_shader_constants.dest_relative.edram_info;
  const draw_util::ResolveCoordinateInfo& coordinate_info =
      copy_shader_constants.dest_relative.coordinate_info;
  if (!edram_info.pitch_tiles || dump_pitch != edram_info.pitch_tiles ||
      rt_key.GetPitchTiles() != edram_info.pitch_tiles ||
      edram_info.base_tiles < rt_key.base_tiles) {
    return "layout";
  }
  if (xenos::IsColorRenderTargetFormat64bpp(
          xenos::ColorRenderTargetFormat(edram_info.format)) != is_64bpp) {
    return "format";
  }
  const uint32_t base_delta = edram_info.base_tiles - rt_key.base_tiles;
  // 64bpp tiles are half as wide in pixels, and 4x ones half as wide and half
  // as tall.
  const uint32_t edram_tile_width =
      xenos::kEdramTileWidthSamples >>
      (uint32_t(is_64bpp) + uint32_t(four_samples));
  const uint32_t edram_tile_height =
      xenos::kEdramTileHeightSamples >> uint32_t(four_samples);
  TileResolveConstants constants = {};
  constants.origin[0] = base_delta % edram_info.pitch_tiles * edram_tile_width +
                        coordinate_info.edram_offset_x_div_8 * 8;
  constants.origin[1] =
      base_delta / edram_info.pitch_tiles * edram_tile_height +
      coordinate_info.edram_offset_y_div_8 * 8;
  constants.size[0] = coordinate_info.width_div_8 * 8;
  constants.size[1] = resolve_info.height_div_8 * 8;
  if (constants.origin[0] + constants.size[0] >
          edram_info.pitch_tiles * edram_tile_width ||
      constants.origin[0] + constants.size[0] > source->width() ||
      constants.origin[1] + constants.size[1] > source->height()) {
    return "layout";
  }
  const reg::RB_COPY_DEST_INFO dest_info =
      copy_shader_constants.dest_relative.dest_info;
  const draw_util::ResolveCopyDestCoordinateInfo& dest_coordinate_info =
      copy_shader_constants.dest_relative.dest_coordinate_info;
  constants.dest_offset[0] = dest_coordinate_info.offset_x_div_8 * 8;
  constants.dest_offset[1] = dest_coordinate_info.offset_y_div_8 * 8;
  constants.dest_pitch_macro_tiles = dest_coordinate_info.pitch_aligned_div_32;
  // Rows wider than the destination pitch wrap onto the next rows' words, and
  // which pixel lands there depends on write order, which per-tile dispatch
  // order doesn't match.
  if (constants.dest_offset[0] + constants.size[0] >
      constants.dest_pitch_macro_tiles * 32) {
    return "dest_pitch";
  }
  constants.dest_slice_pitch_macro_tiles =
      dest_coordinate_info.height_aligned_div_32 << 1;
  constants.dest_base = copy_shader_constants.dest_base;
  constants.dest_array = dest_info.copy_dest_array;
  constants.dest_slice = dest_info.copy_dest_slice;
  if (dest_info.copy_dest_swap) {
    switch (xenos::ColorRenderTargetFormat(edram_info.format)) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        constants.swap = 1;
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        constants.swap = 2;
        break;
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        constants.swap = 3;
        break;
      default:
        break;
    }
  }
  xenos::Endian128 endian = dest_info.copy_dest_endian;
  // XeEndianSwap64 exchanges the dwords for 8in64, then swaps as 8in32.
  if (is_64bpp && endian == xenos::Endian128::k8in64) {
    constants.exchange_dwords = 1;
    endian = xenos::Endian128::k8in32;
  }
  constants.bytes_per_block_log2 = 2 + uint32_t(is_64bpp);
  // XeResolveGetInfo's dest_num_format and dest_exp_bias_factor.
  constants.num_format = (dest_info.value >> 13) & 0x7;
  constants.exp_bias_factor_bits =
      uint32_t((int32_t(dest_info.value << 10) >> 26 << 23) + 0x3F800000);
  if (kernel == TileResolveKernel::kRGB10Average4) {
    // The kernel's average is times 1/4 alone, and its pack the unsigned
    // fraction's.
    if (constants.exp_bias_factor_bits != 0x3F800000u || constants.num_format) {
      return "rgb10_bias";
    }
  }
  if (kernel == TileResolveKernel::k16_16_16_16) {
    // An RGBA16Snorm tile no draw of the pass touched may hold 0x8000, which
    // today's compute reads raw (-32768) and the tile read sees as -1.0, like
    // 0x8001 (-32767). Both convert to the same output except for these.
    const int32_t exp_bias = int32_t(dest_info.value << 10) >> 26;
    if ((constants.num_format == 1 && (exp_bias == -6 || exp_bias == -5)) ||
        (constants.num_format == 3 && (exp_bias == -6 || exp_bias == 10))) {
      return "rgba16_bias";
    }
  }
  constants.swap_bytes_in_16 =
      endian == xenos::Endian128::k8in16 || endian == xenos::Endian128::k8in32;
  constants.swap_halves_in_32 =
      endian == xenos::Endian128::k8in32 || endian == xenos::Endian128::k16in32;
  // The dump shader stores whole 16-byte blocks from this base.
  if (constants.dest_base & 0xF) {
    return "dest_align";
  }

  // The render target has to be an attachment of the open pass, and the
  // resolve inside the area its tiles cover.
  MTL::RenderPassDescriptor* pass_descriptor = nullptr;
  uint32_t pass_width = 0, pass_height = 0;
  MetalBufferHazards::Encoder* hazards = nullptr;
  MTL::RenderCommandEncoder* encoder = command_processor_.GetOpenRenderPass(
      pass_descriptor, pass_width, pass_height, hazards);
  if (!encoder) {
    return "no_pass";
  }
  uint32_t color_index = UINT32_MAX;
  for (uint32_t i = 0; i < 8; ++i) {
    MTL::RenderPassColorAttachmentDescriptor* attachment =
        pass_descriptor->colorAttachments()->object(i);
    if (attachment->texture() && i >= xenos::kMaxColorRenderTargets) {
      return "attachments";
    }
    if (attachment->texture() == source && !attachment->level() &&
        !attachment->slice() && !attachment->depthPlane()) {
      color_index = i;
    }
  }
  if (color_index == UINT32_MAX) {
    return "not_attached";
  }
  if (constants.origin[0] + constants.size[0] > pass_width ||
      constants.origin[1] + constants.size[1] > pass_height) {
    return "render_area";
  }
  // Every tile dispatch of a pass declares the same imageblock member type:
  // half4 for the 16-bit float formats, float4 for the rest.
  const uint32_t member_class =
      1 + uint32_t(kernel == TileResolveKernel::k16_16Float ||
                   kernel == TileResolveKernel::k16_16_16_16Float);
  const uint32_t pass_member_class =
      command_processor_.GetRenderEncoderTileMemberClass();
  if (pass_member_class && pass_member_class != member_class) {
    return "member_class";
  }

  // The tile size is the pass's own, checked against the limits rather than
  // assumed.
  const uint32_t tile_width = uint32_t(encoder->tileWidth());
  const uint32_t tile_height = uint32_t(encoder->tileHeight());
  if (!tile_width || !tile_height ||
      tile_width * tile_height > GetMaxTileArea(sample_count)) {
    return "tile_size";
  }
  // The refresh views are R32Uint at 32bpp and RG32Uint at 64bpp, all of the
  // shape of the first.
  const uint32_t refresh_count = uint32_t(refresh_textures.size());
  // What the dispatch writes, and what is copied from its scratch texture
  // after the pass.
  std::array<MTL::Texture*, 4> kernel_refresh{};
  uint32_t kernel_refresh_count = 0;
  std::array<MTL::Texture*, 4> sampled_refresh{};
  uint32_t sampled_refresh_count = 0;
  bool refresh_swap = false;
  // Back to the pool on every return before they're handed to the pass end.
  MetalTextureCache* texture_cache = command_processor_.texture_cache();
  std::array<std::unique_ptr<MetalTextureCache::ResolveRefreshStorage>, 4>
      refresh_storage;
  auto release_refresh_storage = [&]() {
    for (auto& storage : refresh_storage) {
      if (storage) {
        texture_cache->ReleaseResolveRefreshStorage(std::move(storage));
      }
    }
  };
  struct ReleaseOnReturn {
    decltype(release_refresh_storage)& release;
    ~ReleaseOnReturn() { release(); }
  } release_on_return{release_refresh_storage};
  if (refresh_count) {
    if (refresh_count > 4) {
      return "refresh_count";
    }
    const MTL::PixelFormat refresh_format =
        is_64bpp ? MTL::PixelFormatRG32Uint : MTL::PixelFormatR32Uint;
    for (MTL::Texture* refresh_texture : refresh_textures) {
      if (!refresh_texture ||
          refresh_texture->textureType() != MTL::TextureType2DArray ||
          refresh_texture->pixelFormat() != refresh_format) {
        return "refresh_format";
      }
    }
    // An earlier draw of this pass may sample them in a tile the dispatch
    // reaches first. Those get a scratch texture of their shape instead,
    // copied into them when the pass ends - after every draw of the pass, as
    // the compute resolve after ending the pass would write them.
    for (uint32_t i = 0; i < refresh_count; ++i) {
      if (command_processor_.RenderEncoderUsesAnyTexture(
              refresh_textures.subspan(i, 1))) {
        sampled_refresh[sampled_refresh_count++] = refresh_textures[i];
      } else {
        kernel_refresh[kernel_refresh_count++] = refresh_textures[i];
      }
    }
    if (sampled_refresh_count) {
      // Where the dispatch writes every texel of them, each gets an image of
      // its own instead, which it takes as its image when the pass ends.
      // Otherwise the scratch texture, bound last, is copied into all.
      refresh_swap = texture_cache && !refresh_offset_x && !refresh_offset_y &&
                     constants.size[0] >= refresh_textures[0]->width() &&
                     constants.size[1] >= refresh_textures[0]->height();
      for (uint32_t i = 0; refresh_swap && i < sampled_refresh_count; ++i) {
        refresh_storage[i] =
            texture_cache->AcquireResolveRefreshStorage(sampled_refresh[i]);
        refresh_swap = refresh_storage[i] != nullptr;
      }
      if (!refresh_swap) {
        release_refresh_storage();
      }
      kernel_refresh_count += refresh_swap ? sampled_refresh_count : 1;
    }
    constants.refresh_size[0] = uint32_t(refresh_textures[0]->width());
    constants.refresh_size[1] = uint32_t(refresh_textures[0]->height());
    constants.refresh_offset[0] = refresh_offset_x;
    constants.refresh_offset[1] = refresh_offset_y;
  }
  MTL::RenderPipelineState* pipeline = GetOrCreateTileResolvePipeline(
      pass_descriptor, color_index, kernel, sample_count, kernel_refresh_count);
  if (!pipeline) {
    return "pipeline";
  }
  if (tile_width * tile_height > pipeline->maxTotalThreadsPerThreadgroup()) {
    return "threads";
  }
  const NS::UInteger imageblock_bytes =
      std::max<NS::UInteger>(
          pipeline->imageblockSampleLength() * tile_width * tile_height,
          pipeline->imageblockMemoryLength(
              MTL::Size::Make(tile_width, tile_height, 1))) *
      sample_count;
  if (imageblock_bytes > device_->maxThreadgroupMemoryLength()) {
    return "imageblock";
  }

  auto* shared = command_processor_.shared_memory();
  MTL::Buffer* dest_buffer = shared ? shared->GetBuffer() : nullptr;
  if (!dest_buffer) {
    return "shared_memory";
  }
  // Earlier commands of this pass that touched the destination may run after
  // the dispatch in other tiles.
  if ((hazards && hazards->HasAccess(resolve_info.copy_dest_extent_start,
                                     resolve_info.copy_dest_extent_length)) ||
      command_processor_.TileResolveWritesOverlap(
          resolve_info.copy_dest_extent_start,
          resolve_info.copy_dest_extent_length)) {
    return "dest_in_pass";
  }
  // The GPU is about to overwrite this range, so any CPU-side data has to be
  // uploaded first. Without zero-copy that is a blit, which ends the pass.
  if (!shared->RequestRange(resolve_info.copy_dest_extent_start,
                            resolve_info.copy_dest_extent_length)) {
    return "request_range";
  }
  if (!command_processor_.HasActiveRenderEncoder()) {
    return "upload_ended_pass";
  }
  MTL::Texture* refresh_scratch = nullptr;
  if (refresh_swap) {
    for (uint32_t i = 0; i < sampled_refresh_count; ++i) {
      kernel_refresh[kernel_refresh_count - sampled_refresh_count + i] =
          refresh_storage[i]->write_view;
    }
  } else if (sampled_refresh_count) {
    refresh_scratch =
        render_target_cache_.AcquireRefreshScratch(refresh_textures[0]);
    if (!refresh_scratch) {
      return "refresh_scratch";
    }
    kernel_refresh[kernel_refresh_count - 1] = refresh_scratch;
  }

  encoder->setRenderPipelineState(pipeline);
  command_processor_.InvalidateRenderEncoderPipeline();
  encoder->setTileBytes(&constants, sizeof(constants), 0);
  encoder->setTileBuffer(dest_buffer, 0, 1);
  for (uint32_t i = 0; i < kernel_refresh_count; ++i) {
    encoder->setTileTexture(kernel_refresh[i], i);
    command_processor_.UseRenderEncoderResource(kernel_refresh[i],
                                                MTL::ResourceUsageWrite);
  }
  if (refresh_scratch) {
    // The texels the dispatch writes: resolve pixel p at p + the refresh
    // offset, inside the texture.
    PostPassRefreshCopy copy;
    copy.scratch = refresh_scratch;
    copy.origin_x = refresh_offset_x;
    copy.origin_y = refresh_offset_y;
    copy.width = refresh_offset_x < constants.refresh_size[0]
                     ? std::min(constants.size[0],
                                constants.refresh_size[0] - refresh_offset_x)
                     : 0;
    copy.height = refresh_offset_y < constants.refresh_size[1]
                      ? std::min(constants.size[1],
                                 constants.refresh_size[1] - refresh_offset_y)
                      : 0;
    for (uint32_t i = 0; i < sampled_refresh_count; ++i) {
      copy.destinations.push_back(NS::RetainPtr(sampled_refresh[i]));
    }
    copy.post_pass_resolves_before = post_pass_resolves_count;
    if (refresh_copy_out) {
      *refresh_copy_out = std::move(copy);
    }
    if (has_refresh_copy_out) {
      *has_refresh_copy_out = true;
    }
  } else if (refresh_swap) {
    PostPassRefreshCopy copy;
    for (uint32_t i = 0; i < sampled_refresh_count; ++i) {
      PostPassRefreshCopy::Swap& swap = copy.swaps.emplace_back();
      swap.write_view = NS::RetainPtr(sampled_refresh[i]);
      swap.storage = std::move(refresh_storage[i]);
    }
    copy.post_pass_resolves_before = post_pass_resolves_count;
    if (refresh_copy_out) {
      *refresh_copy_out = std::move(copy);
    }
    if (has_refresh_copy_out) {
      *has_refresh_copy_out = true;
    }
  }
  if (hazards) {
    hazards->Access(resolve_info.copy_dest_extent_start,
                    resolve_info.copy_dest_extent_length, true);
  }
  encoder->dispatchThreadsPerTile(MTL::Size::Make(tile_width, tile_height, 1));
  command_processor_.SetRenderEncoderTileMemberClass(member_class);
  return nullptr;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
