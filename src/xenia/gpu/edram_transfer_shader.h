/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_EDRAM_TRANSFER_SHADER_H_
#define XENIA_GPU_EDRAM_TRANSFER_SHADER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// Emits the fragment shader that moves ownership of an EDRAM range from one
// host render target to another, reading the previous owner as a texture and
// writing the new one as an attachment. Backend-independent: the result is
// SPIR-V, which Vulkan consumes directly and Metal takes through spirv_to_dxil
// and Apple's Metal Shader Converter.
//
// The vertex half is a passthrough over rectangle positions, which the backends
// supply themselves - it carries no part of the transfer's addressing.

enum EdramTransferUsedDescriptorSet : uint32_t {
  // Ordered from the least to the most frequently changed.
  kEdramTransferUsedDescriptorSetHostDepthBuffer,
  kEdramTransferUsedDescriptorSetHostDepthStencilTextures,
  kEdramTransferUsedDescriptorSetDepthStencilTextures,
  // Mutually exclusive with
  // kEdramTransferUsedDescriptorSetDepthStencilTextures.
  kEdramTransferUsedDescriptorSetColorTexture,

  kEdramTransferUsedDescriptorSetCount,

  kEdramTransferUsedDescriptorSetHostDepthBufferBit =
      uint32_t(1) << kEdramTransferUsedDescriptorSetHostDepthBuffer,
  kEdramTransferUsedDescriptorSetHostDepthStencilTexturesBit =
      uint32_t(1) << kEdramTransferUsedDescriptorSetHostDepthStencilTextures,
  kEdramTransferUsedDescriptorSetDepthStencilTexturesBit =
      uint32_t(1) << kEdramTransferUsedDescriptorSetDepthStencilTextures,
  kEdramTransferUsedDescriptorSetColorTextureBit =
      uint32_t(1) << kEdramTransferUsedDescriptorSetColorTexture,
};

// 32-bit push constants (for simplicity of size calculation and to avoid
// std140 packing issues).
enum EdramTransferUsedPushConstantDword : uint32_t {
  kEdramTransferUsedPushConstantDwordHostDepthAddress,
  kEdramTransferUsedPushConstantDwordAddress,
  // Changed 8 times per transfer.
  kEdramTransferUsedPushConstantDwordStencilMask,

  kEdramTransferUsedPushConstantDwordCount,

  kEdramTransferUsedPushConstantDwordHostDepthAddressBit =
      uint32_t(1) << kEdramTransferUsedPushConstantDwordHostDepthAddress,
  kEdramTransferUsedPushConstantDwordAddressBit =
      uint32_t(1) << kEdramTransferUsedPushConstantDwordAddress,
  kEdramTransferUsedPushConstantDwordStencilMaskBit =
      uint32_t(1) << kEdramTransferUsedPushConstantDwordStencilMask,
};

enum class EdramTransferPipelineLayoutIndex {
  kColor,
  kDepth,
  kColorToStencilBit,
  kDepthToStencilBit,
  kColorAndHostDepthTexture,
  kColorAndHostDepthBuffer,
  kDepthAndHostDepthTexture,
  kDepthAndHostDepthBuffer,

  kCount,
};

struct EdramTransferPipelineLayoutInfo {
  uint32_t used_descriptor_sets;
  uint32_t used_push_constant_dwords;
};

extern const EdramTransferPipelineLayoutInfo
    kEdramTransferPipelineLayoutInfos[size_t(
        EdramTransferPipelineLayoutIndex::kCount)];

enum class EdramTransferMode : uint32_t {
  kColorToDepth,
  kColorToColor,

  kDepthToDepth,
  kDepthToColor,

  kColorToStencilBit,
  kDepthToStencilBit,

  // Two-source modes, using the host depth if it, when converted to the guest
  // format, matches what's in the owner source (not modified, keep host
  // precision), or the guest data otherwise (significantly modified, possibly
  // cleared). Stencil for FragStencilRef is always taken from the guest
  // source.

  kColorAndHostDepthToDepth,
  // When using different source and destination depth formats.
  kDepthAndHostDepthToDepth,

  // If host depth is fetched, but it's the same image as the destination,
  // it's copied to the EDRAM buffer (but since it's just a scratch buffer,
  // with tiles laid out linearly with the same pitch as in the original
  // render target; also no swapping of 40-sample columns as opposed to the
  // host render target - this is done only for the color source) and fetched
  // from there instead of the host depth texture.
  kColorAndHostDepthCopyToDepth,
  kDepthAndHostDepthCopyToDepth,

  kCount,
};

enum class EdramTransferOutput {
  kColor,
  kDepth,
  kStencilBit,
};

struct EdramTransferModeInfo {
  EdramTransferOutput output;
  EdramTransferPipelineLayoutIndex pipeline_layout;
};

extern const EdramTransferModeInfo
    kEdramTransferModes[size_t(EdramTransferMode::kCount)];

// Whether the previous owner is read as a colour texture rather than as a
// depth / stencil pair, and whether a host depth source is read at all - both
// implied by which descriptor sets the mode's layout declares.
inline bool EdramTransferSourceIsColor(EdramTransferMode mode) {
  return (kEdramTransferPipelineLayoutInfos
              [size_t(kEdramTransferModes[size_t(mode)].pipeline_layout)]
                  .used_descriptor_sets &
          kEdramTransferUsedDescriptorSetColorTextureBit) != 0;
}
// Whether the host depth source is read back out of the EDRAM buffer rather
// than from a texture, which is what the copy modes exist for.
inline bool EdramTransferHostDepthIsCopy(EdramTransferMode mode) {
  return (kEdramTransferPipelineLayoutInfos
              [size_t(kEdramTransferModes[size_t(mode)].pipeline_layout)]
                  .used_descriptor_sets &
          kEdramTransferUsedDescriptorSetHostDepthBufferBit) != 0;
}
inline bool EdramTransferUsesHostDepth(EdramTransferMode mode) {
  return (kEdramTransferPipelineLayoutInfos
              [size_t(kEdramTransferModes[size_t(mode)].pipeline_layout)]
                  .used_descriptor_sets &
          (kEdramTransferUsedDescriptorSetHostDepthBufferBit |
           kEdramTransferUsedDescriptorSetHostDepthStencilTexturesBit)) != 0;
}

union EdramTransferShaderKey {
  uint32_t key;
  struct {
    xenos::MsaaSamples dest_msaa_samples : xenos::kMsaaSamplesBits;
    uint32_t dest_color_rt_index : xenos::kColorRenderTargetIndexBits;
    uint32_t dest_resource_format : xenos::kRenderTargetFormatBits;
    xenos::MsaaSamples source_msaa_samples : xenos::kMsaaSamplesBits;
    // Always 1x when the host depth is a copy from a buffer rather than an
    // image, not to create the same pipeline for different MSAA sample counts
    // as it doesn't matter in this case.
    xenos::MsaaSamples host_depth_source_msaa_samples : xenos::kMsaaSamplesBits;
    uint32_t source_resource_format : xenos::kRenderTargetFormatBits;
    // Decode (not bit-reinterpret) a 7e3 -> plain 8_8_8_8 reuse. See
    // IsTransferValueConverted7e3And8888.
    uint32_t value_convert : 1;
    // Scale classes of the two sides. The shader bakes each side's tile
    // size and conversion between the scale spaces.
    uint32_t dest_scale_native : 1;
    uint32_t source_scale_native : 1;

    // The layout facts below are proven by the backend on the CPU. Backends
    // that don't prove them leave the bits zero.
    // Equal EDRAM base and pitch, unscaled depth 1x <-> 4x transfer. The
    // rasterized tile range must stay below kEdramTileCount relative to the
    // base, so the sample permutation can use full-image coordinates.
    uint32_t depth_identity_layout : 1;
    // The source pitch is 16 tiles.
    uint32_t source_pitch_16 : 1;
    // Equal EDRAM base, pitch, sample count and 32bpp color layout, within one
    // EDRAM period from the base. Source and destination texture coordinates
    // are identical.
    uint32_t color_identity_layout : 1;

    // Last bits because this affects the pipeline layout - after sorting,
    // only change it as fewer times as possible. Depth buffers have an
    // additional stencil texture.
    static_assert(size_t(EdramTransferMode::kCount) <= (size_t(1) << 4));
    EdramTransferMode mode : 4;
  };

  EdramTransferShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

  struct Hasher {
    size_t operator()(const EdramTransferShaderKey& key) const {
      return std::hash<uint32_t>{}(key.key);
    }
  };
  bool operator==(const EdramTransferShaderKey& other_key) const {
    return key == other_key.key;
  }
  bool operator!=(const EdramTransferShaderKey& other_key) const {
    return !(*this == other_key);
  }
  bool operator<(const EdramTransferShaderKey& other_key) const {
    return key < other_key.key;
  }
};

// What a kEdramTransferUsedPushConstantDwordAddress or
// kEdramTransferUsedPushConstantDwordHostDepthAddress dword holds. The used
// dwords are packed densely, in the order the enum declares them.
union EdramTransferAddressConstant {
  uint32_t constant;
  struct {
    // All in tiles.
    uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
    uint32_t source_pitch : xenos::kEdramPitchTilesBits;
    // Destination base in tiles minus source base in tiles (not vice versa
    // because this is a transform of the coordinate system, not addresses
    // themselves), wrapped, the shader wraps the sum anyway.
    // 0 for host_depth_source_is_copy (ignored in this case anyway as
    // destination == source anyway).
    uint32_t source_to_dest : xenos::kEdramBaseTilesBits;
    // The source render target is in the next period (64bpp color only).
    uint32_t source_next_period : 1;
  };
  EdramTransferAddressConstant() : constant(0) {
    static_assert_size(*this, sizeof(constant));
  }
  bool operator==(const EdramTransferAddressConstant& other_constant) const {
    return constant == other_constant.constant;
  }
  bool operator!=(const EdramTransferAddressConstant& other_constant) const {
    return !(*this == other_constant);
  }
};

// What the emitter can't derive from the key: the binding model to declare the
// resources in, and the host properties the guest layout is resolved against.
struct EdramTransferShaderOptions {
  // SPIR-V version to emit, as glslang's SpirvBuilder takes it.
  uint32_t spirv_version = 0x00010000;

  // Guest-to-host resolution scale, which the tile addressing is computed
  // against.
  uint32_t resolution_scale_x = 1;
  uint32_t resolution_scale_y = 1;

  // Whether the host can give 2x MSAA render targets real 2-sample attachments
  // rather than emulating them on 4-sample ones, which decides where guest 2x
  // samples live.
  bool msaa_2x_attachments_supported = false;

  // Whether the backend stores each side's color ownership-transfer view as an
  // integer texture, so it is sampled and written as uint rather than float.
  // Ignored for the depth sides. The backends own their format policy, so this
  // comes in rather than being derived here.
  bool source_color_is_uint = false;
  bool dest_color_is_uint = false;

  // Whether the host can write the stencil reference from the fragment shader,
  // which turns the 8 per-bit stencil passes into part of the depth draw.
  bool stencil_reference_output_supported = false;

  // Whether the host can run the fragment shader per sample, which is how a
  // multisampled destination addresses one guest sample per invocation. When
  // off, the sample index comes from specialization constant 0 and the
  // destination's samples are covered by one draw each.
  bool sample_rate_shading_supported = false;

  // Write the covered sample from the shader instead of taking it from a
  // pipeline-level sample mask, for hosts that have no such state. Only
  // meaningful on the per-draw-sample path.
  bool sample_mask_output = false;

  // Take the sample index from a dword appended after the layout's push
  // constants rather than from specialization constant 0, so one pipeline
  // covers every sample instead of one per sample. Only meaningful on the
  // per-draw-sample path.
  bool sample_index_push_constant = false;

  // float24 depth handling, from the cvars of the same name as the backend
  // resolved them: whether guest depth was already rounded to float24 by the
  // pixel shader, and whether rounding is to nearest even rather than toward
  // zero.
  bool depth_float24_round = false;
  bool depth_float24_convert_in_pixel_shader = false;

  // From the cvar of the same name: keep the stencil bit passes' discard rather
  // than letting a fully packed value through.
  bool no_discard_stencil = false;

  // Split the tile index by the EDRAM pitch with a float reciprocal and a
  // correction rather than an integer divide. The pitch is a push constant, so
  // the divide cannot be strength-reduced, and it costs a fragment each on GPUs
  // where integer division is slow.
  bool fast_pitch_divmod = false;

  // Speculate the cheap D24S8 decode and select the preserved host value
  // without control flow. D24FS8 retains its conditional conversion.
  bool branchless_d24_host_depth = false;

  // Opt-in exact integer address simplification. Keep false until the target
  // backend's correctness and performance gates have passed.
  bool optimize_integer_addressing = false;
};

// Returns the SPIR-V words of the transfer fragment shader for one key.
std::vector<uint32_t> BuildEdramTransferShaderSpirv(
    EdramTransferShaderKey key, const EdramTransferShaderOptions& options);

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_EDRAM_TRANSFER_SHADER_H_
