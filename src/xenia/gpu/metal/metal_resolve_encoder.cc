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
#include "xenia/gpu/shaders/bytecode/metal/metal_tile_resolve_cs.h"
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

struct ResolvePipelineEntry {
  draw_util::ResolveCopyShaderIndex index;
  const uint8_t* metallib;
  size_t metallib_size;
  const char* name;
  const uint8_t* scaled_metallib;
  size_t scaled_metallib_size;
  const char* scaled_name;
};

constexpr ResolvePipelineEntry kResolvePipelineEntries[] = {
    {draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA,
     resolve_fast_32bpp_1x2xmsaa_cs_metallib,
     sizeof(resolve_fast_32bpp_1x2xmsaa_cs_metallib),
     "resolve_fast_32bpp_1x2xmsaa",
     resolve_fast_32bpp_1x2xmsaa_scaled_cs_metallib,
     sizeof(resolve_fast_32bpp_1x2xmsaa_scaled_cs_metallib),
     "resolve_fast_32bpp_1x2xmsaa_scaled"},
    {draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA,
     resolve_fast_32bpp_4xmsaa_cs_metallib,
     sizeof(resolve_fast_32bpp_4xmsaa_cs_metallib), "resolve_fast_32bpp_4xmsaa",
     resolve_fast_32bpp_4xmsaa_scaled_cs_metallib,
     sizeof(resolve_fast_32bpp_4xmsaa_scaled_cs_metallib),
     "resolve_fast_32bpp_4xmsaa_scaled"},
    {draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA,
     resolve_fast_64bpp_1x2xmsaa_cs_metallib,
     sizeof(resolve_fast_64bpp_1x2xmsaa_cs_metallib),
     "resolve_fast_64bpp_1x2xmsaa",
     resolve_fast_64bpp_1x2xmsaa_scaled_cs_metallib,
     sizeof(resolve_fast_64bpp_1x2xmsaa_scaled_cs_metallib),
     "resolve_fast_64bpp_1x2xmsaa_scaled"},
    {draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA,
     resolve_fast_64bpp_4xmsaa_cs_metallib,
     sizeof(resolve_fast_64bpp_4xmsaa_cs_metallib), "resolve_fast_64bpp_4xmsaa",
     resolve_fast_64bpp_4xmsaa_scaled_cs_metallib,
     sizeof(resolve_fast_64bpp_4xmsaa_scaled_cs_metallib),
     "resolve_fast_64bpp_4xmsaa_scaled"},
    {draw_util::ResolveCopyShaderIndex::kFull8bpp,
     resolve_full_8bpp_cs_metallib, sizeof(resolve_full_8bpp_cs_metallib),
     "resolve_full_8bpp", resolve_full_8bpp_scaled_cs_metallib,
     sizeof(resolve_full_8bpp_scaled_cs_metallib), "resolve_full_8bpp_scaled"},
    {draw_util::ResolveCopyShaderIndex::kFull16bpp,
     resolve_full_16bpp_cs_metallib, sizeof(resolve_full_16bpp_cs_metallib),
     "resolve_full_16bpp", resolve_full_16bpp_scaled_cs_metallib,
     sizeof(resolve_full_16bpp_scaled_cs_metallib),
     "resolve_full_16bpp_scaled"},
    {draw_util::ResolveCopyShaderIndex::kFull32bpp,
     resolve_full_32bpp_cs_metallib, sizeof(resolve_full_32bpp_cs_metallib),
     "resolve_full_32bpp", resolve_full_32bpp_scaled_cs_metallib,
     sizeof(resolve_full_32bpp_scaled_cs_metallib),
     "resolve_full_32bpp_scaled"},
    {draw_util::ResolveCopyShaderIndex::kFull64bpp,
     resolve_full_64bpp_cs_metallib, sizeof(resolve_full_64bpp_cs_metallib),
     "resolve_full_64bpp", resolve_full_64bpp_scaled_cs_metallib,
     sizeof(resolve_full_64bpp_scaled_cs_metallib),
     "resolve_full_64bpp_scaled"},
    {draw_util::ResolveCopyShaderIndex::kFull128bpp,
     resolve_full_128bpp_cs_metallib, sizeof(resolve_full_128bpp_cs_metallib),
     "resolve_full_128bpp", resolve_full_128bpp_scaled_cs_metallib,
     sizeof(resolve_full_128bpp_scaled_cs_metallib),
     "resolve_full_128bpp_scaled"},
};

struct DirectResolvePipelineEntry {
  const uint8_t* metallib;
  size_t metallib_size;
  const char* name;
  const uint8_t* refresh_metallib;
  size_t refresh_metallib_size;
  const char* refresh_name;
};

// Order matches MetalResolveEncoder::DirectResolveKind.
constexpr DirectResolvePipelineEntry kDirectResolvePipelineEntries[] = {
    {resolve_full_32bpp_rgb10_4x_direct_cs_metallib,
     sizeof(resolve_full_32bpp_rgb10_4x_direct_cs_metallib),
     "resolve_full_32bpp_rgb10_4x_direct",
     resolve_full_32bpp_rgb10_4x_direct_refresh_cs_metallib,
     sizeof(resolve_full_32bpp_rgb10_4x_direct_refresh_cs_metallib),
     "resolve_full_32bpp_rgb10_4x_direct_refresh"},
    {resolve_full_32bpp_1x_direct_cs_metallib,
     sizeof(resolve_full_32bpp_1x_direct_cs_metallib),
     "resolve_full_32bpp_1x_direct",
     resolve_full_32bpp_1x_direct_refresh_cs_metallib,
     sizeof(resolve_full_32bpp_1x_direct_refresh_cs_metallib),
     "resolve_full_32bpp_1x_direct_refresh"},
    {resolve_full_32bpp_rgb10f_1x_direct_cs_metallib,
     sizeof(resolve_full_32bpp_rgb10f_1x_direct_cs_metallib),
     "resolve_full_32bpp_rgb10f_1x_direct",
     resolve_full_32bpp_rgb10f_1x_direct_refresh_cs_metallib,
     sizeof(resolve_full_32bpp_rgb10f_1x_direct_refresh_cs_metallib),
     "resolve_full_32bpp_rgb10f_1x_direct_refresh"},
    {resolve_full_64bpp_rgba16_1x_direct_cs_metallib,
     sizeof(resolve_full_64bpp_rgba16_1x_direct_cs_metallib),
     "resolve_full_64bpp_rgba16_1x_direct",
     resolve_full_64bpp_rgba16_1x_direct_refresh_cs_metallib,
     sizeof(resolve_full_64bpp_rgba16_1x_direct_refresh_cs_metallib),
     "resolve_full_64bpp_rgba16_1x_direct_refresh"},
    {resolve_full_64bpp_rgb10f_rgba16_1x_direct_cs_metallib,
     sizeof(resolve_full_64bpp_rgb10f_rgba16_1x_direct_cs_metallib),
     "resolve_full_64bpp_rgb10f_rgba16_1x_direct",
     resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_cs_metallib,
     sizeof(resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh_cs_metallib),
     "resolve_full_64bpp_rgb10f_rgba16_1x_direct_refresh"},
    {resolve_full_64bpp_rgb10f_rgba16_msaa_direct_cs_metallib,
     sizeof(resolve_full_64bpp_rgb10f_rgba16_msaa_direct_cs_metallib),
     "resolve_full_64bpp_rgb10f_rgba16_msaa_direct",
     resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_cs_metallib,
     sizeof(resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh_cs_metallib),
     "resolve_full_64bpp_rgb10f_rgba16_msaa_direct_refresh"},
};

}  // namespace

MetalResolveEncoder::MetalResolveEncoder(
    MetalRenderTargetCache& render_target_cache,
    MetalCommandProcessor& command_processor)
    : render_target_cache_(render_target_cache),
      command_processor_(command_processor) {}

MetalResolveEncoder::~MetalResolveEncoder() { Shutdown(); }

bool MetalResolveEncoder::Initialize(MTL::Device* device,
                                     bool draw_resolution_scaled) {
  static_assert(std::size(kDirectResolvePipelineEntries) ==
                size_t(DirectResolveKind::kCount));
  device_ = device;
  resolve_pipelines_.fill(nullptr);
  resolve_scaled_pipelines_.fill(nullptr);
  direct_resolve_pipelines_.fill(nullptr);

  for (const ResolvePipelineEntry& entry : kResolvePipelineEntries) {
    auto* pso = CreateComputePipelineFromEmbeddedLibrary(
        device_, entry.metallib, entry.metallib_size, entry.name);
    resolve_pipelines_[size_t(entry.index)] = pso;
    if (!pso) {
      XELOGE("Metal: failed to initialize resolve compute pipelines");
      return false;
    }
  }

  for (size_t i = 0; i < std::size(kDirectResolvePipelineEntries); ++i) {
    const DirectResolvePipelineEntry& entry = kDirectResolvePipelineEntries[i];
    direct_resolve_pipelines_[i * 2] = CreateComputePipelineFromEmbeddedLibrary(
        device_, entry.metallib, entry.metallib_size, entry.name);
    direct_resolve_pipelines_[i * 2 + 1] =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, entry.refresh_metallib, entry.refresh_metallib_size,
            entry.refresh_name);
  }

  resolve_specialized_pipelines_.clear();
  resolve_specialized_pipelines_.reserve(std::size(kResolveSpecializations));
  for (const ResolveSpecialization& specialization : kResolveSpecializations) {
    resolve_specialized_pipelines_.push_back(
        CreateComputePipelineFromEmbeddedLibrary(
            device_, specialization.metallib, specialization.metallib_size,
            specialization.name));
  }

  if (draw_resolution_scaled) {
    for (const ResolvePipelineEntry& entry : kResolvePipelineEntries) {
      auto* pso = CreateComputePipelineFromEmbeddedLibrary(
          device_, entry.scaled_metallib, entry.scaled_metallib_size,
          entry.scaled_name);
      resolve_scaled_pipelines_[size_t(entry.index)] = pso;
      if (!pso) {
        XELOGE("Metal: failed to initialize scaled resolve compute pipelines");
        return false;
      }
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
  auto release_all = [](auto& pipelines) {
    for (MTL::ComputePipelineState*& pipeline : pipelines) {
      if (pipeline) {
        pipeline->release();
        pipeline = nullptr;
      }
    }
  };
  release_all(resolve_pipelines_);
  release_all(resolve_scaled_pipelines_);
  release_all(direct_resolve_pipelines_);
  release_all(resolve_specialized_pipelines_);
  resolve_specialized_pipelines_.clear();

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
  const size_t shader_index = size_t(copy_shader);
  if (shader_index >= resolve_pipelines_.size()) {
    return nullptr;
  }
  if (draw_resolution_scaled) {
    return resolve_scaled_pipelines_[shader_index];
  }
  MTL::ComputePipelineState* pipeline = resolve_pipelines_[shader_index];
  const ResolveCopyKey key = GetResolveCopyKey(copy_constants);
  for (size_t i = 0; i < resolve_specialized_pipelines_.size(); ++i) {
    const ResolveSpecialization& specialization = kResolveSpecializations[i];
    if (specialization.copy_shader == copy_shader &&
        specialization.key.edram == key.edram &&
        specialization.key.dest == key.dest &&
        specialization.key.sample == key.sample &&
        resolve_specialized_pipelines_[i]) {
      return resolve_specialized_pipelines_[i];
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
    const DirectResolveKind direct_kind =
        rgba16_1x               ? DirectResolveKind::kRgba16_1x
        : rgb10f_rgba16_average ? DirectResolveKind::kRgb10fRgba16_Msaa
        : rgb10f_rgba16_1x      ? DirectResolveKind::kRgb10fRgba16_1x
        : rgb10_4x_average      ? DirectResolveKind::kRgb10_4x
        : full32_1x             ? DirectResolveKind::kFull32_1x
                                : DirectResolveKind::kRgb10f_1x;
    rgb10_pipeline = direct_resolve_pipelines_[size_t(direct_kind) * 2 +
                                               (refresh_texture ? 1 : 0)];
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

// Laid out as XeTileResolveConstants in shaders/metal_tile_resolve.cs.metal.
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
    tile_resolve_library_ = CreateLibraryFromEmbeddedMetallib(
        device_, metal_tile_resolve_cs_metallib,
        sizeof(metal_tile_resolve_cs_metallib), "metal_tile_resolve");
    if (!tile_resolve_library_) {
      tile_resolve_library_failed_ = true;
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
