/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_EDRAM_DUMP_SHADER_H_
#define XENIA_GPU_EDRAM_DUMP_SHADER_H_

#include <cstdint>
#include <functional>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// Emits the compute shader that copies a host render target's samples into the
// EDRAM buffer in guest tile layout, the first half of a resolve on the host
// render target path. Backend-independent: the result is SPIR-V, which Vulkan
// consumes directly and Metal takes through spirv_to_dxil and Apple's Metal
// Shader Converter.
//
// With EdramDumpShaderKey::direct_resolve, the same shader stores into the
// resolve destination in the guest texture layout instead, doing in one pass
// what the dump and the resolve copy do in two - in the plain 1x1 layout or
// the resolution-scaled group-packed one, as native_layout selects.

union EdramDumpShaderKey {
  uint32_t key;
  struct {
    xenos::MsaaSamples msaa_samples : 2;
    uint32_t resource_format : 4;
    // Last bit because this affects the pipeline - after sorting, only change
    // it at most once. Depth buffers have an additional stencil SRV.
    uint32_t is_depth : 1;
    // Writing this native render target into a scaled destination duplicates
    // its guest pixels across the host pixels covering them.
    uint32_t source_scale_native : 1;
    // Address the destination - the EDRAM buffer, or the resolve destination
    // for direct_resolve - with the plain 1x1 layout rather than the scaled
    // one. Only meaningful with resolution scaling, where it marks a fully
    // native resolve; every source of one is source_scale_native too.
    uint32_t native_layout : 1;
    // Store into the resolve destination rather than the EDRAM buffer,
    // skipping the round trip. Only for resolves the copy would do bitwise -
    // the format-converting ones stay on the EDRAM path. A scaled destination
    // is written in the group-packed layout, out of a windowed binding, so the
    // destination base push constant is unused there.
    uint32_t direct_resolve : 1;
    // Average all four samples of an RGBA8 4x source while resolving directly.
    // Only meaningful with direct_resolve.
    uint32_t direct_resolve_4x_average : 1;
  };

  EdramDumpShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

  struct Hasher {
    size_t operator()(const EdramDumpShaderKey& key) const {
      return std::hash<uint32_t>{}(key.key);
    }
  };
  bool operator==(const EdramDumpShaderKey& other_key) const {
    return key == other_key.key;
  }
  bool operator!=(const EdramDumpShaderKey& other_key) const {
    return !(*this == other_key);
  }
  bool operator<(const EdramDumpShaderKey& other_key) const {
    return key < other_key.key;
  }

  xenos::ColorRenderTargetFormat GetColorFormat() const {
    assert_false(is_depth);
    return xenos::ColorRenderTargetFormat(resource_format);
  }
  xenos::DepthRenderTargetFormat GetDepthFormat() const {
    assert_true(is_depth);
    return xenos::DepthRenderTargetFormat(resource_format);
  }
};

// A direct resolve dispatches over destination pixels rather than EDRAM
// samples, so that the guest texture layout's contiguous runs line up with
// whole stores. Four 32bpp pixels (or two 64bpp ones) are 16 adjacent bytes;
// the next four are elsewhere in the tile, so a thread does two of them.
constexpr uint32_t kEdramDumpShaderResolvePixelsPerStore = 4;
constexpr uint32_t kEdramDumpShaderResolvePixelsPerThreadAt32bpp =
    kEdramDumpShaderResolvePixelsPerStore * 2;
// Pixels per thread, which is halved for 64bpp - a 16-byte store covers half
// as many of them.
constexpr uint32_t GetEdramDumpShaderResolvePixelsPerThread(bool is_64bpp) {
  return kEdramDumpShaderResolvePixelsPerThreadAt32bpp >> uint32_t(is_64bpp);
}
// 64 threads per group, like the resolve copy shaders.
constexpr uint32_t kEdramDumpShaderResolveThreadsPerGroupX = 8;
constexpr uint32_t kEdramDumpShaderResolveThreadsPerGroupY = 8;

// There's no strict dependency on the group size in dumping, for simplicity of
// calculations especially with resolution scaling, dividing manually (as the
// group size is not unlimited). The only restriction is that an integer
// multiple of it must be 80x16 samples (and no larger than that) for 32bpp, or
// 40x16 samples for 64bpp (because only a half of the pair of tiles may need to
// be dumped). Using 8x16 since that's 128 - the minimum required group size on
// Vulkan, and the maximum number of lanes in a subgroup on Vulkan.
constexpr uint32_t kEdramDumpShaderSamplesPerGroupX = 8;
constexpr uint32_t kEdramDumpShaderSamplesPerGroupY = 16;

// Push constants, in the order the shader declares them.
enum EdramDumpShaderPushConstant : uint32_t {
  // May be different for different sources.
  kEdramDumpShaderPushConstantPitches,
  // May be changed multiple times for the same source.
  kEdramDumpShaderPushConstantOffsets,

  // EdramDumpShaderKey::direct_resolve only, and constant across a resolve -
  // draw_util::ResolveCopyShaderConstants, plus the resolve height, which the
  // copy shaders don't need because their dispatch is exactly the resolve
  // rectangle while this one is the tiles covering it.
  kEdramDumpShaderPushConstantResolveEdramInfo,
  kEdramDumpShaderPushConstantResolveCoordinateInfo,
  kEdramDumpShaderPushConstantResolveDestInfo,
  kEdramDumpShaderPushConstantResolveDestCoordinateInfo,
  kEdramDumpShaderPushConstantResolveDestBase,
  kEdramDumpShaderPushConstantResolveHeightDiv8,
  // Where this dispatch's first tile sits in the resolve's tile grid, so the
  // threads can place themselves without dividing the linear index back out.
  kEdramDumpShaderPushConstantResolveDispatchTile,

  kEdramDumpShaderPushConstantCount,
};

// kEdramDumpShaderPushConstantResolveDispatchTile.
union EdramDumpShaderResolveDispatchTile {
  uint32_t packed;
  struct {
    uint32_t tile_x : xenos::kEdramPitchTilesBits;
    uint32_t tile_y : xenos::kEdramPitchTilesBits;
  };
  EdramDumpShaderResolveDispatchTile() : packed(0) {
    static_assert_size(*this, sizeof(packed));
  }
};

union EdramDumpShaderPitches {
  uint32_t pitches;
  struct {
    // Both in tiles.
    uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
    uint32_t source_pitch : xenos::kEdramPitchTilesBits;
  };
  EdramDumpShaderPitches() : pitches(0) {
    static_assert_size(*this, sizeof(pitches));
  }
  bool operator==(const EdramDumpShaderPitches& other_pitches) const {
    return pitches == other_pitches.pitches;
  }
  bool operator!=(const EdramDumpShaderPitches& other_pitches) const {
    return !(*this == other_pitches);
  }
};

union EdramDumpShaderOffsets {
  uint32_t offsets;
  struct {
    // May be beyond the EDRAM tile count in case of EDRAM addressing wrapping,
    // and another tile count beyond for a render target in the next period,
    // thus + 2 bits.
    uint32_t dispatch_first_tile : xenos::kEdramBaseTilesBits + 2;
    uint32_t source_base_tiles : xenos::kEdramBaseTilesBits;
  };
  EdramDumpShaderOffsets() : offsets(0) {
    static_assert_size(*this, sizeof(offsets));
  }
  bool operator==(const EdramDumpShaderOffsets& other_offsets) const {
    return offsets == other_offsets.offsets;
  }
  bool operator!=(const EdramDumpShaderOffsets& other_offsets) const {
    return !(*this == other_offsets);
  }
};

// What the emitter can't derive from the key: the binding model to declare the
// resources in, and the host properties the guest layout is resolved against.
struct EdramDumpShaderOptions {
  // SPIR-V version to emit, as glslang's SpirvBuilder takes it.
  uint32_t spirv_version = 0x00010000;

  // Descriptor sets of the destination buffer - the EDRAM buffer, or the
  // resolve destination for a direct_resolve key - and of the source render
  // target. The source's stencil, when the key is a depth one, is binding 1 of
  // the source set; everything else is binding 0 of its set.
  uint32_t descriptor_set_dest = 0;
  uint32_t descriptor_set_source = 1;

  // Guest-to-host resolution scale, which the tile addressing is computed
  // against.
  uint32_t resolution_scale_x = 1;
  uint32_t resolution_scale_y = 1;

  // Whether the host can give 2x MSAA render targets real 2-sample attachments
  // rather than emulating them on 4-sample ones, which decides where guest 2x
  // samples live.
  bool msaa_2x_attachments_supported = false;

  // Whether the backend stores this color format's ownership-transfer view as
  // an integer texture, so the source is sampled as uint rather than float.
  // Ignored for depth keys. The backends own their format policy, so this
  // comes in rather than being derived here.
  bool source_is_uint = false;

  // float24 depth handling, from the cvars of the same name as the backend
  // resolved them: whether guest depth was already rounded to float24 by the
  // pixel shader, and whether rounding is to nearest even rather than toward
  // zero.
  bool depth_float24_round = false;
  bool depth_float24_convert_in_pixel_shader = false;
};

// Returns the SPIR-V words of the dump shader for one key, or an empty vector
// if the key's format is not dumpable.
std::vector<uint32_t> BuildEdramDumpShaderSpirv(
    EdramDumpShaderKey key, const EdramDumpShaderOptions& options);

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_EDRAM_DUMP_SHADER_H_
