/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/edram_transfer_shader.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "third_party/glslang/SPIRV/GLSL.std.450.h"

#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

const EdramTransferPipelineLayoutInfo kEdramTransferPipelineLayoutInfos[size_t(
    EdramTransferPipelineLayoutIndex::kCount)] = {
    // kColor
    {kEdramTransferUsedDescriptorSetColorTextureBit,
     kEdramTransferUsedPushConstantDwordAddressBit},
    // kDepth
    {kEdramTransferUsedDescriptorSetDepthStencilTexturesBit,
     kEdramTransferUsedPushConstantDwordAddressBit},
    // kColorToStencilBit
    {kEdramTransferUsedDescriptorSetColorTextureBit,
     kEdramTransferUsedPushConstantDwordAddressBit |
         kEdramTransferUsedPushConstantDwordStencilMaskBit},
    // kDepthToStencilBit
    {kEdramTransferUsedDescriptorSetDepthStencilTexturesBit,
     kEdramTransferUsedPushConstantDwordAddressBit |
         kEdramTransferUsedPushConstantDwordStencilMaskBit},
    // kColorAndHostDepthTexture
    {kEdramTransferUsedDescriptorSetHostDepthStencilTexturesBit |
         kEdramTransferUsedDescriptorSetColorTextureBit,
     kEdramTransferUsedPushConstantDwordHostDepthAddressBit |
         kEdramTransferUsedPushConstantDwordAddressBit},
    // kColorAndHostDepthBuffer
    {kEdramTransferUsedDescriptorSetHostDepthBufferBit |
         kEdramTransferUsedDescriptorSetColorTextureBit,
     kEdramTransferUsedPushConstantDwordHostDepthAddressBit |
         kEdramTransferUsedPushConstantDwordAddressBit},
    // kDepthAndHostDepthTexture
    {kEdramTransferUsedDescriptorSetHostDepthStencilTexturesBit |
         kEdramTransferUsedDescriptorSetDepthStencilTexturesBit,
     kEdramTransferUsedPushConstantDwordHostDepthAddressBit |
         kEdramTransferUsedPushConstantDwordAddressBit},
    // kDepthAndHostDepthBuffer
    {kEdramTransferUsedDescriptorSetHostDepthBufferBit |
         kEdramTransferUsedDescriptorSetDepthStencilTexturesBit,
     kEdramTransferUsedPushConstantDwordHostDepthAddressBit |
         kEdramTransferUsedPushConstantDwordAddressBit},
};

const EdramTransferModeInfo
    kEdramTransferModes[size_t(EdramTransferMode::kCount)] = {
        // kColorToDepth
        {EdramTransferOutput::kDepth, EdramTransferPipelineLayoutIndex::kColor},
        // kColorToColor
        {EdramTransferOutput::kColor, EdramTransferPipelineLayoutIndex::kColor},
        // kDepthToDepth
        {EdramTransferOutput::kDepth, EdramTransferPipelineLayoutIndex::kDepth},
        // kDepthToColor
        {EdramTransferOutput::kColor, EdramTransferPipelineLayoutIndex::kDepth},
        // kColorToStencilBit
        {EdramTransferOutput::kStencilBit,
         EdramTransferPipelineLayoutIndex::kColorToStencilBit},
        // kDepthToStencilBit
        {EdramTransferOutput::kStencilBit,
         EdramTransferPipelineLayoutIndex::kDepthToStencilBit},
        // kColorAndHostDepthToDepth
        {EdramTransferOutput::kDepth,
         EdramTransferPipelineLayoutIndex::kColorAndHostDepthTexture},
        // kDepthAndHostDepthToDepth
        {EdramTransferOutput::kDepth,
         EdramTransferPipelineLayoutIndex::kDepthAndHostDepthTexture},
        // kColorAndHostDepthCopyToDepth
        {EdramTransferOutput::kDepth,
         EdramTransferPipelineLayoutIndex::kColorAndHostDepthBuffer},
        // kDepthAndHostDepthCopyToDepth
        {EdramTransferOutput::kDepth,
         EdramTransferPipelineLayoutIndex::kDepthAndHostDepthBuffer},
};

namespace {

// Converts a raw 8-bit gamma-encoded byte (a uint scalar) to the midpoint of
// its linear range. Used when a non-gamma EDRAM value is reinterpreted as
// k_8_8_8_8_GAMMA for a linear unorm16 host render target. The midpoint, rather
// than the exact lower edge produced by PWLGammaToLinear, keeps the value
// safely inside the byte's range across the unorm16 quantization round-trip, so
// a later linear -> gamma re-encode reproduces the original byte. Mirrors the
// piecewise constants in the D3D12 render target cache.
spv::Id GammaByteToLinearMidpoint(SpirvBuilder& builder, spv::Id byte_uint) {
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_bool = builder.makeBoolType();
  // F = float(byte) * recip + offset, with the piece selected by the byte.
  spv::Id recip = builder.makeFloatConstant(1.0f / 1023.0f);
  spv::Id offset = builder.makeFloatConstant(0.5f / 1023.0f);
  struct Piece {
    uint32_t threshold;
    float recip;
    float offset;
  };
  static const Piece kPieces[] = {
      {64, 1.0f / 511.5f, -31.5f / 511.5f},
      {96, 1.0f / 255.75f, -63.5f / 255.75f},
      {192, 1.0f / 127.875f, -127.5f / 127.875f},
  };
  for (const Piece& piece : kPieces) {
    spv::Id in_piece =
        builder.createBinOp(spv::OpUGreaterThanEqual, type_bool, byte_uint,
                            builder.makeUintConstant(piece.threshold));
    recip = builder.createTriOp(spv::OpSelect, type_float, in_piece,
                                builder.makeFloatConstant(piece.recip), recip);
    offset =
        builder.createTriOp(spv::OpSelect, type_float, in_piece,
                            builder.makeFloatConstant(piece.offset), offset);
  }
  return builder.createBinOp(
      spv::OpFAdd, type_float,
      builder.createBinOp(
          spv::OpFMul, type_float,
          builder.createUnaryOp(spv::OpConvertUToF, type_float, byte_uint),
          recip),
      offset);
}

// XeFastDivMod from the Metal transfer shaders: the quotient from a float
// reciprocal, then one correction step in each direction because the estimate
// can land either side by one. Cheaper than OpUDiv where the divisor is a push
// constant and integer division is slow.
void FastDivMod(SpirvBuilder& builder, spv::Id x, spv::Id w,
                spv::Id& quotient_out, spv::Id& remainder_out) {
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id w_float = builder.createUnaryOp(spv::OpConvertUToF, type_float, w);
  spv::Id inv_w = builder.createBinOp(spv::OpFDiv, type_float,
                                      builder.makeFloatConstant(1.0f), w_float);
  spv::Id quotient = builder.createUnaryOp(
      spv::OpConvertFToU, type_uint,
      builder.createBinOp(
          spv::OpFMul, type_float,
          builder.createUnaryOp(spv::OpConvertUToF, type_float, x), inv_w));
  spv::Id remainder = builder.createBinOp(
      spv::OpISub, type_uint, x,
      builder.createBinOp(spv::OpIMul, type_uint, quotient, w));
  // The estimate was low: the remainder still holds a whole divisor.
  spv::Id too_low =
      builder.createBinOp(spv::OpUGreaterThanEqual, type_bool, remainder, w);
  // The estimate was high: the subtraction wrapped past x.
  spv::Id too_high =
      builder.createBinOp(spv::OpUGreaterThan, type_bool, remainder, x);
  spv::Id one = builder.makeUintConstant(1);
  quotient_out = builder.createTriOp(
      spv::OpSelect, type_uint, too_low,
      builder.createBinOp(spv::OpIAdd, type_uint, quotient, one),
      builder.createTriOp(
          spv::OpSelect, type_uint, too_high,
          builder.createBinOp(spv::OpISub, type_uint, quotient, one),
          quotient));
  remainder_out = builder.createTriOp(
      spv::OpSelect, type_uint, too_low,
      builder.createBinOp(spv::OpISub, type_uint, remainder, w),
      builder.createTriOp(
          spv::OpSelect, type_uint, too_high,
          builder.createBinOp(spv::OpIAdd, type_uint, remainder, w),
          remainder));
}

// Keep quotient and remainder coupled in the exact integer path. This avoids
// asking downstream compilers to rediscover a shared divide for OpUDiv/OpUMod.
// For a nonzero divisor q * w <= x, so the multiply cannot overflow uint32.
void IntegerDivMod(SpirvBuilder& builder, spv::Id x, spv::Id w,
                   spv::Id& quotient_out, spv::Id& remainder_out) {
  spv::Id type_uint = builder.makeUintType(32);
  quotient_out = builder.createBinOp(spv::OpUDiv, type_uint, x, w);
  remainder_out = builder.createBinOp(
      spv::OpISub, type_uint, x,
      builder.createBinOp(spv::OpIMul, type_uint, quotient_out, w));
}

// Tile dimensions are known when emitting the shader. Express power-of-two
// division directly, without floating-point estimates or bounded-input
// assumptions. Non-power-of-two dimensions still use an exact integer divide.
void ConstantDivMod(SpirvBuilder& builder, spv::Id x, uint32_t divisor,
                    spv::Id& quotient_out, spv::Id& remainder_out) {
  assert_true(divisor > 1);
  if ((divisor & (divisor - 1)) == 0) {
    uint32_t shift = 0;
    for (uint32_t remaining = divisor; remaining > 1; remaining >>= 1) {
      ++shift;
    }
    spv::Id type_uint = builder.makeUintType(32);
    quotient_out = builder.createBinOp(spv::OpShiftRightLogical, type_uint, x,
                                       builder.makeUintConstant(shift));
    remainder_out = builder.createBinOp(spv::OpBitwiseAnd, type_uint, x,
                                        builder.makeUintConstant(divisor - 1));
    return;
  }
  IntegerDivMod(builder, x, builder.makeUintConstant(divisor), quotient_out,
                remainder_out);
}

struct CanonicalSampleCoords {
  spv::Id u_guest;
  spv::Id v_guest;
  // Host subpixel offsets under resolution scaling, spv::NoResult when the
  // axis isn't scaled.
  spv::Id sub_x;
  spv::Id sub_y;
};

// Indices of host samples that transfer sample remap helpers use:
// - First sample bit of 4x - horizontal sample.
// - Second sample bit of 4x - vertical sample.
// - 2x:
//   - Native 2x - top sample is 1 with the standard sample locations, bottom
//     sample is 0.
//   - 2x as 4x - top sample is 0, bottom sample is 3.

// Converts the view pixel coordinates and host sample index to canonical
// guest sample coordinates, with the host subpixel offsets carried along in
// case of scaling.
CanonicalSampleCoords CanonicalizeSample(
    SpirvBuilder& builder, spv::Id type_uint, xenos::MsaaSamples msaa_samples,
    bool msaa_2x_attachments_supported, spv::Id pixel_x, spv::Id pixel_y,
    spv::Id host_sample_id, uint32_t scale_x, uint32_t scale_y) {
  CanonicalSampleCoords result;
  result.sub_x = spv::NoResult;
  result.sub_y = spv::NoResult;
  spv::Id guest_x = pixel_x;
  spv::Id guest_y = pixel_y;
  if (scale_x > 1) {
    spv::Id const_scale_x = builder.makeUintConstant(scale_x);
    guest_x =
        builder.createBinOp(spv::OpUDiv, type_uint, pixel_x, const_scale_x);
    result.sub_x =
        builder.createBinOp(spv::OpUMod, type_uint, pixel_x, const_scale_x);
  }
  if (scale_y > 1) {
    spv::Id const_scale_y = builder.makeUintConstant(scale_y);
    guest_y =
        builder.createBinOp(spv::OpUDiv, type_uint, pixel_y, const_scale_y);
    result.sub_y =
        builder.createBinOp(spv::OpUMod, type_uint, pixel_y, const_scale_y);
  }
  spv::Id const_uint_1 = builder.makeUintConstant(1);
  spv::Id const_uint_2 = builder.makeUintConstant(2);
  spv::Id const_uint_30 = builder.makeUintConstant(30);
  if (msaa_samples >= xenos::MsaaSamples::k4X) {
    // The guest sample index is the host sample index, bit 0 horizontal
    // and bit 1 vertical for both.
    // u = ((x >> 1) << 2) | ((sample & 1) << 1) | (x & 1)
    result.u_guest = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint,
        builder.createQuadOp(spv::OpBitFieldInsert, type_uint, guest_x,
                             host_sample_id, const_uint_1, const_uint_1),
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_x,
                            const_uint_1),
        const_uint_2, const_uint_30);
    // v = ((y >> 1) << 2) | ((sample >> 1) << 1) | (y & 1)
    result.v_guest = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint,
        builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, guest_y,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                host_sample_id, const_uint_1),
            const_uint_1, const_uint_1),
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_y,
                            const_uint_1),
        const_uint_2, const_uint_30);
  } else if (msaa_samples == xenos::MsaaSamples::k2X) {
    // Guest sample 0 is the top one. Native 2x has the top at host sample
    // 1, and 2x emulated as 4x has it at host sample 0.
    spv::Id guest_sample;
    if (msaa_2x_attachments_supported) {
      guest_sample = builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                         host_sample_id, const_uint_1);
    } else {
      guest_sample = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                         host_sample_id, const_uint_1);
    }
    // u = (x & ~2) | (sample << 1)
    result.u_guest =
        builder.createQuadOp(spv::OpBitFieldInsert, type_uint, guest_x,
                             guest_sample, const_uint_1, const_uint_1);
    // v = ((y >> 1) << 2) | (x & 2) | (y & 1)
    result.v_guest = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint,
        builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, guest_y,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_x,
                                const_uint_1),
            const_uint_1, const_uint_1),
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_y,
                            const_uint_1),
        const_uint_2, const_uint_30);
  } else {
    result.u_guest = guest_x;
    result.v_guest = guest_y;
  }
  return result;
}

// Converts the canonical guest sample coordinates, along with the subpixel
// offsets in case of scaling, back to view pixels and host sample index.
// host_sample_id_out remains spv::NoResult for a single sampled view.
void DecanonicalizeSample(SpirvBuilder& builder, spv::Id type_uint,
                          xenos::MsaaSamples msaa_samples,
                          bool msaa_2x_attachments_supported,
                          const CanonicalSampleCoords& coords, uint32_t scale_x,
                          uint32_t scale_y, spv::Id& pixel_x_out,
                          spv::Id& pixel_y_out, spv::Id& host_sample_id_out) {
  spv::Id const_uint_1 = builder.makeUintConstant(1);
  spv::Id const_uint_2 = builder.makeUintConstant(2);
  spv::Id const_uint_31 = builder.makeUintConstant(31);
  spv::Id guest_x, guest_y;
  host_sample_id_out = spv::NoResult;
  if (msaa_samples >= xenos::MsaaSamples::k4X) {
    // x = ((u >> 2) << 1) | (u & 1)
    guest_x = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, coords.u_guest,
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, coords.u_guest,
                            const_uint_2),
        const_uint_1, const_uint_31);
    // y = ((v >> 2) << 1) | (v & 1)
    guest_y = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, coords.v_guest,
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, coords.v_guest,
                            const_uint_2),
        const_uint_1, const_uint_31);
    // The host sample index is the guest sample index.
    // sample = ((u >> 1) & 1) | (v & 2)
    host_sample_id_out = builder.createBinOp(
        spv::OpBitwiseOr, type_uint,
        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, coords.u_guest,
                            const_uint_1, const_uint_1),
        builder.createBinOp(spv::OpBitwiseAnd, type_uint, coords.v_guest,
                            const_uint_2));
  } else if (msaa_samples == xenos::MsaaSamples::k2X) {
    // x = (u & ~3) | (v & 2) | (u & 1)
    guest_x = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, coords.u_guest,
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, coords.v_guest,
                            const_uint_1),
        const_uint_1, const_uint_1);
    // y = ((v >> 2) << 1) | (v & 1)
    guest_y = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, coords.v_guest,
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, coords.v_guest,
                            const_uint_2),
        const_uint_1, const_uint_31);
    // Guest sample 0 is the top one.
    // sample = (u >> 1) & 1
    spv::Id guest_sample =
        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, coords.u_guest,
                            const_uint_1, const_uint_1);
    if (msaa_2x_attachments_supported) {
      host_sample_id_out = builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                               guest_sample, const_uint_1);
    } else {
      // The guest sample 1 is the host sample 3 when 2x is emulated as 4x.
      host_sample_id_out =
          builder.createQuadOp(spv::OpBitFieldInsert, type_uint, guest_sample,
                               guest_sample, const_uint_1, const_uint_1);
    }
  } else {
    guest_x = coords.u_guest;
    guest_y = coords.v_guest;
  }
  pixel_x_out = guest_x;
  if (scale_x > 1) {
    assert_true(coords.sub_x != spv::NoResult);
    pixel_x_out = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createBinOp(spv::OpIMul, type_uint, guest_x,
                            builder.makeUintConstant(scale_x)),
        coords.sub_x);
  }
  pixel_y_out = guest_y;
  if (scale_y > 1) {
    assert_true(coords.sub_y != spv::NoResult);
    pixel_y_out = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createBinOp(spv::OpIMul, type_uint, guest_y,
                            builder.makeUintConstant(scale_y)),
        coords.sub_y);
  }
}

}  // namespace

std::vector<uint32_t> BuildEdramTransferShaderSpirv(
    EdramTransferShaderKey key, const EdramTransferShaderOptions& options) {
  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;
  SpirvBuilder builder(options.spirv_version,
                       (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint4 = builder.makeVectorType(type_uint, 4);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);

  const EdramTransferModeInfo& mode = kEdramTransferModes[size_t(key.mode)];
  const EdramTransferPipelineLayoutInfo& pipeline_layout_info =
      kEdramTransferPipelineLayoutInfos[size_t(mode.pipeline_layout)];

  // If not dest_is_color, it's depth, or stencil bit - 40-sample columns are
  // swapped as opposed to color source.
  bool dest_is_color = (mode.output == EdramTransferOutput::kColor);
  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  bool dest_is_64bpp =
      dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  // If not source_is_color, it's depth / stencil - 40-sample columns are
  // swapped as opposed to color destination.
  bool source_is_color = (pipeline_layout_info.used_descriptor_sets &
                          kEdramTransferUsedDescriptorSetColorTextureBit) != 0;
  bool source_is_64bpp;
  uint32_t source_color_format_component_count;
  uint32_t source_color_texture_component_mask;
  bool source_color_is_uint;
  spv::Id source_color_component_type;
  if (source_is_color) {
    assert_zero(pipeline_layout_info.used_descriptor_sets &
                kEdramTransferUsedDescriptorSetDepthStencilTexturesBit);
    source_is_64bpp =
        xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (mode.output == EdramTransferOutput::kStencilBit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Need one component, but choosing from the two 32bpp halves of the
        // 64bpp sample.
        source_color_texture_component_mask =
            0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_texture_component_mask = 0b1;
      }
    } else {
      source_color_texture_component_mask =
          (uint32_t(1) << source_color_format_component_count) - 1;
    }
    source_color_is_uint = options.source_color_is_uint;
    source_color_component_type = source_color_is_uint ? type_uint : type_float;
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_texture_component_mask = 0;
    source_color_is_uint = false;
    source_color_component_type = spv::NoType;
  }

  std::vector<spv::Id> main_interface;

  // Outputs.
  bool shader_uses_stencil_reference_output =
      mode.output == EdramTransferOutput::kDepth &&
      options.stencil_reference_output_supported;
  bool dest_color_is_uint = false;
  uint32_t dest_color_component_count = 0;
  spv::Id type_fragment_data_component = spv::NoResult;
  spv::Id type_fragment_data = spv::NoResult;
  spv::Id output_fragment_data = spv::NoResult;
  spv::Id output_fragment_depth = spv::NoResult;
  spv::Id output_fragment_stencil_ref = spv::NoResult;
  switch (mode.output) {
    case EdramTransferOutput::kColor:
      dest_color_is_uint = options.dest_color_is_uint;
      dest_color_component_count =
          xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
      type_fragment_data_component =
          dest_color_is_uint ? type_uint : type_float;
      type_fragment_data =
          dest_color_component_count > 1
              ? builder.makeVectorType(type_fragment_data_component,
                                       dest_color_component_count)
              : type_fragment_data_component;
      output_fragment_data = builder.createVariable(
          spv::NoPrecision, spv::StorageClassOutput, type_fragment_data,
          "xe_transfer_fragment_data");
      builder.addDecoration(output_fragment_data, spv::DecorationLocation,
                            key.dest_color_rt_index);
      main_interface.push_back(output_fragment_data);
      break;
    case EdramTransferOutput::kDepth:
      output_fragment_depth =
          builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                 type_float, "gl_FragDepth");
      builder.addDecoration(output_fragment_depth, spv::DecorationBuiltIn,
                            static_cast<int>(spv::BuiltIn::FragDepth));
      main_interface.push_back(output_fragment_depth);
      if (shader_uses_stencil_reference_output) {
        builder.addExtension("SPV_EXT_shader_stencil_export");
        builder.addCapability(spv::CapabilityStencilExportEXT);
        output_fragment_stencil_ref =
            builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                   type_int, "gl_FragStencilRefARB");
        builder.addDecoration(
            output_fragment_stencil_ref, spv::DecorationBuiltIn,
            static_cast<int>(spv::BuiltIn::FragStencilRefEXT));
        main_interface.push_back(output_fragment_stencil_ref);
      }
      break;
    default:
      break;
  }

  // Bindings.
  // Generating SPIR-V 1.0, no need to add bindings to the entry point's
  // interface until SPIR-V 1.4.
  // Color source.
  bool source_is_multisampled =
      key.source_msaa_samples != xenos::MsaaSamples::k1X;
  spv::Id source_color_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kEdramTransferUsedDescriptorSetColorTextureBit) {
    source_color_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(source_color_component_type, spv::Dim2D, false,
                              false, source_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_transfer_color");
    builder.addDecoration(
        source_color_texture, spv::DecorationDescriptorSet,
        xe::bit_count(pipeline_layout_info.used_descriptor_sets &
                      (kEdramTransferUsedDescriptorSetColorTextureBit - 1)));
    builder.addDecoration(source_color_texture, spv::DecorationBinding, 0);
  }
  // Depth / stencil source.
  spv::Id source_depth_texture = spv::NoResult;
  spv::Id source_stencil_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kEdramTransferUsedDescriptorSetDepthStencilTexturesBit) {
    uint32_t source_depth_stencil_descriptor_set = xe::bit_count(
        pipeline_layout_info.used_descriptor_sets &
        (kEdramTransferUsedDescriptorSetDepthStencilTexturesBit - 1));
    // Using `depth == false` in makeImageType because comparisons are not
    // required, and other values of `depth` are causing issues in drivers.
    // https://github.com/microsoft/DirectXShaderCompiler/issues/1107
    if (mode.output != EdramTransferOutput::kStencilBit) {
      source_depth_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_float, spv::Dim2D, false, false,
                                source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_depth");
      builder.addDecoration(source_depth_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_depth_texture, spv::DecorationBinding, 0);
    }
    if (mode.output != EdramTransferOutput::kDepth ||
        shader_uses_stencil_reference_output) {
      source_stencil_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_uint, spv::Dim2D, false, false,
                                source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_stencil");
      builder.addDecoration(source_stencil_texture,
                            spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
    }
  }
  // Host depth source buffer.
  spv::Id host_depth_source_buffer = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kEdramTransferUsedDescriptorSetHostDepthBufferBit) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(type_uint));
    // Storage buffers have std430 packing, no padding to 4-component vectors.
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                          sizeof(uint32_t));
    spv::Id type_host_depth_source_buffer =
        builder.makeStructType(id_vector_temp, "XeTransferHostDepthBuffer");
    builder.addMemberName(type_host_depth_source_buffer, 0, "host_depth");
    builder.addMemberDecoration(type_host_depth_source_buffer, 0,
                                spv::DecorationNonWritable);
    builder.addMemberDecoration(type_host_depth_source_buffer, 0,
                                spv::DecorationOffset, 0);
    // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // BufferBlock.
    builder.addDecoration(type_host_depth_source_buffer,
                          spv::DecorationBufferBlock);
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    host_depth_source_buffer = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniform,
        type_host_depth_source_buffer, "xe_transfer_host_depth_buffer");
    builder.addDecoration(
        host_depth_source_buffer, spv::DecorationDescriptorSet,
        xe::bit_count(pipeline_layout_info.used_descriptor_sets &
                      (kEdramTransferUsedDescriptorSetHostDepthBufferBit - 1)));
    builder.addDecoration(host_depth_source_buffer, spv::DecorationBinding, 0);
  }
  // Host depth source texture (the depth / stencil descriptor set is reused,
  // but stencil is not needed).
  spv::Id host_depth_source_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kEdramTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
    host_depth_source_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(
            type_float, spv::Dim2D, false, false,
            key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X, 1,
            spv::ImageFormatUnknown),
        "xe_transfer_host_depth");
    builder.addDecoration(
        host_depth_source_texture, spv::DecorationDescriptorSet,
        xe::bit_count(
            pipeline_layout_info.used_descriptor_sets &
            (kEdramTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)));
    builder.addDecoration(host_depth_source_texture, spv::DecorationBinding, 0);
  }
  // Push constants.
  id_vector_temp.clear();
  uint32_t push_constants_member_host_depth_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kEdramTransferUsedPushConstantDwordHostDepthAddressBit) {
    push_constants_member_host_depth_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kEdramTransferUsedPushConstantDwordAddressBit) {
    push_constants_member_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_stencil_mask = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kEdramTransferUsedPushConstantDwordStencilMaskBit) {
    push_constants_member_stencil_mask = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  // Rides after the layout's dwords so their offsets are untouched.
  const bool sample_index_in_push_constants =
      key.dest_msaa_samples != xenos::MsaaSamples::k1X &&
      !options.sample_rate_shading_supported &&
      options.sample_index_push_constant;
  uint32_t push_constants_member_sample_index = UINT32_MAX;
  if (sample_index_in_push_constants) {
    push_constants_member_sample_index = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  spv::Id push_constants = spv::NoResult;
  if (!id_vector_temp.empty()) {
    spv::Id type_push_constants =
        builder.makeStructType(id_vector_temp, "XeTransferPushConstants");
    if (pipeline_layout_info.used_push_constant_dwords &
        kEdramTransferUsedPushConstantDwordHostDepthAddressBit) {
      assert_true(push_constants_member_host_depth_address != UINT32_MAX);
      builder.addMemberName(type_push_constants,
                            push_constants_member_host_depth_address,
                            "host_depth_address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_host_depth_address,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(
                  pipeline_layout_info.used_push_constant_dwords &
                  (kEdramTransferUsedPushConstantDwordHostDepthAddressBit -
                   1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kEdramTransferUsedPushConstantDwordAddressBit) {
      assert_true(push_constants_member_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_address,
                            "address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_address,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(
                  pipeline_layout_info.used_push_constant_dwords &
                  (kEdramTransferUsedPushConstantDwordAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kEdramTransferUsedPushConstantDwordStencilMaskBit) {
      assert_true(push_constants_member_stencil_mask != UINT32_MAX);
      builder.addMemberName(type_push_constants,
                            push_constants_member_stencil_mask, "stencil_mask");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_stencil_mask,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(
                  pipeline_layout_info.used_push_constant_dwords &
                  (kEdramTransferUsedPushConstantDwordStencilMaskBit - 1)));
    }
    if (sample_index_in_push_constants) {
      builder.addMemberName(type_push_constants,
                            push_constants_member_sample_index, "sample_index");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_sample_index,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(pipeline_layout_info.used_push_constant_dwords));
    }
    builder.addDecoration(type_push_constants, spv::DecorationBlock);
    push_constants = builder.createVariable(
        spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
        "xe_transfer_push_constants");
  }

  // Coordinate inputs.
  spv::Id input_fragment_coord = builder.createVariable(
      spv::NoPrecision, spv::StorageClassInput, type_float4, "gl_FragCoord");
  builder.addDecoration(input_fragment_coord, spv::DecorationBuiltIn,
                        static_cast<int>(spv::BuiltIn::FragCoord));
  main_interface.push_back(input_fragment_coord);
  spv::Id input_sample_id = spv::NoResult;
  spv::Id spec_const_sample_id = spv::NoResult;
  spv::Id output_sample_mask = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (options.sample_rate_shading_supported) {
      // One draw for all samples.
      builder.addCapability(spv::CapabilitySampleRateShading);
      input_sample_id = builder.createVariable(
          spv::NoPrecision, spv::StorageClassInput, type_int, "gl_SampleID");
      builder.addDecoration(input_sample_id, spv::DecorationFlat);
      builder.addDecoration(input_sample_id, spv::DecorationBuiltIn,
                            static_cast<int>(spv::BuiltIn::SampleId));
      main_interface.push_back(input_sample_id);
    } else {
      // One sample per draw, with different sample masks.
      if (!options.sample_index_push_constant) {
        spec_const_sample_id = builder.makeUintConstant(0, true);
        builder.addName(spec_const_sample_id, "xe_transfer_sample_id");
        builder.addDecoration(spec_const_sample_id, spv::DecorationSpecId, 0);
      }
      if (options.sample_mask_output) {
        output_sample_mask = builder.createVariable(
            spv::NoPrecision, spv::StorageClassOutput,
            builder.makeArrayType(type_int, builder.makeUintConstant(1), 0),
            "gl_SampleMask");
        builder.addDecoration(output_sample_mask, spv::DecorationBuiltIn,
                              static_cast<int>(spv::BuiltIn::SampleMask));
        main_interface.push_back(output_sample_mask);
      }
    }
  }

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function =
      builder.makeFunctionEntry(spv::NoPrecision, type_void, "main",
                                main_param_types, main_precisions, &main_entry);

  // Working with unsigned numbers for simplicity now, bitcasting to signed will
  // be done at texture fetch.

  // The two sides of the transfer may be in different scale classes. The host
  // depth source always has the destination's scale since native render
  // targets don't track host depth.
  uint32_t dest_scale_x =
      key.dest_scale_native ? 1 : options.resolution_scale_x;
  uint32_t dest_scale_y =
      key.dest_scale_native ? 1 : options.resolution_scale_y;
  uint32_t source_scale_x =
      key.source_scale_native ? 1 : options.resolution_scale_x;
  uint32_t source_scale_y =
      key.source_scale_native ? 1 : options.resolution_scale_y;
  uint32_t dest_tile_width_samples =
      xenos::kEdramTileWidthSamples * dest_scale_x;
  uint32_t dest_tile_height_samples =
      xenos::kEdramTileHeightSamples * dest_scale_y;
  uint32_t source_tile_width_samples =
      xenos::kEdramTileWidthSamples * source_scale_x;
  uint32_t source_tile_height_samples =
      xenos::kEdramTileHeightSamples * source_scale_y;

  // Split the destination pixel index into 32bpp tile and 32bpp-tile-relative
  // pixel index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the pixel position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  uint_vector_temp.clear();
  uint_vector_temp.push_back(0);
  uint_vector_temp.push_back(1);
  spv::Id dest_pixel_coord = builder.createUnaryOp(
      spv::OpConvertFToU, type_uint2,
      builder.createRvalueSwizzle(
          spv::NoPrecision, type_float2,
          builder.createLoad(input_fragment_coord, spv::NoPrecision),
          uint_vector_temp));
  spv::Id dest_pixel_x =
      builder.createCompositeExtract(dest_pixel_coord, type_uint, 0);
  uint32_t dest_tile_width_pixels =
      dest_tile_width_samples >>
      (uint32_t(dest_is_64bpp) +
       uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k4X));
  spv::Id const_dest_tile_width_pixels =
      builder.makeUintConstant(dest_tile_width_pixels);
  spv::Id dest_tile_index_x;
  spv::Id dest_tile_pixel_x;
  if (key.depth_identity_layout) {
    dest_tile_index_x = builder.makeUintConstant(0);
    dest_tile_pixel_x = dest_pixel_x;
  } else if (options.optimize_integer_addressing) {
    ConstantDivMod(builder, dest_pixel_x, dest_tile_width_pixels,
                   dest_tile_index_x, dest_tile_pixel_x);
  } else {
    dest_tile_index_x = builder.createBinOp(
        spv::OpUDiv, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
    dest_tile_pixel_x = builder.createBinOp(
        spv::OpUMod, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  }
  spv::Id dest_pixel_y =
      builder.createCompositeExtract(dest_pixel_coord, type_uint, 1);
  uint32_t dest_tile_height_pixels =
      dest_tile_height_samples >>
      uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k2X);
  spv::Id const_dest_tile_height_pixels =
      builder.makeUintConstant(dest_tile_height_pixels);
  spv::Id dest_tile_index_y;
  spv::Id dest_tile_pixel_y;
  if (key.depth_identity_layout) {
    dest_tile_index_y = builder.makeUintConstant(0);
    dest_tile_pixel_y = dest_pixel_y;
  } else if (options.optimize_integer_addressing) {
    ConstantDivMod(builder, dest_pixel_y, dest_tile_height_pixels,
                   dest_tile_index_y, dest_tile_pixel_y);
  } else {
    dest_tile_index_y = builder.createBinOp(
        spv::OpUDiv, type_uint, dest_pixel_y, const_dest_tile_height_pixels);
    dest_tile_pixel_y = builder.createBinOp(
        spv::OpUMod, type_uint, dest_pixel_y, const_dest_tile_height_pixels);
  }

  assert_true(push_constants_member_address != UINT32_MAX);
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.makeIntConstant(int32_t(push_constants_member_address)));
  spv::Id address_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                id_vector_temp),
      spv::NoPrecision);

  // Calculate the 32bpp tile index from its X and Y parts.
  spv::Id dest_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(
              spv::OpBitFieldUExtract, type_uint, address_constant,
              builder.makeUintConstant(0),
              builder.makeUintConstant(xenos::kEdramPitchTilesBits)),
          dest_tile_index_y),
      dest_tile_index_x);

  // Load the destination sample index.
  spv::Id dest_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (options.sample_rate_shading_supported) {
      assert_true(input_sample_id != spv::NoResult);
      dest_sample_id = builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createLoad(input_sample_id, spv::NoPrecision));
    } else {
      if (sample_index_in_push_constants) {
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(
            int32_t(push_constants_member_sample_index)));
        dest_sample_id = builder.createLoad(
            builder.createAccessChain(spv::StorageClassPushConstant,
                                      push_constants, id_vector_temp),
            spv::NoPrecision);
      } else {
        assert_true(spec_const_sample_id != spv::NoResult);
        // Already uint.
        dest_sample_id = spec_const_sample_id;
      }
      if (output_sample_mask != spv::NoResult) {
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeUintConstant(0));
        builder.createStore(
            builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                    builder.makeUintConstant(1),
                                    dest_sample_id)),
            builder.createAccessChain(spv::StorageClassOutput,
                                      output_sample_mask, id_vector_temp));
      }
    }
  }

  // Transform the destination framebuffer pixel and sample coordinates into the
  // source texture pixel and sample coordinates.

  // First sample bit at 4x with Vulkan standard locations - horizontal sample.
  // Second sample bit at 4x with Vulkan standard locations - vertical sample.
  // At 2x:
  // - Native 2x: top is 1 in Vulkan, bottom is 0.
  // - 2x as 4x: top is 0, bottom is 3.

  // If the scale classes differ, convert the tile-local pixel coordinates to
  // the source scale space - the remappings below transform between two
  // layouts of one scale. The destination-space coordinates are saved for the
  // host depth source. Sample indices don't change.
  spv::Id dest_space_tile_pixel_x = dest_tile_pixel_x;
  spv::Id dest_space_tile_pixel_y = dest_tile_pixel_y;
  if (key.source_scale_native != key.dest_scale_native) {
    if (key.dest_scale_native) {
      // Native destination reading a scaled source - take the center host
      // pixel of each guest pixel, like memexport and the resolve downscale
      // do.
      dest_tile_pixel_x = builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIMul, type_uint, dest_tile_pixel_x,
                              builder.makeUintConstant(source_scale_x)),
          builder.makeUintConstant(source_scale_x >> 1));
      dest_tile_pixel_y = builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIMul, type_uint, dest_tile_pixel_y,
                              builder.makeUintConstant(source_scale_y)),
          builder.makeUintConstant(source_scale_y >> 1));
    } else {
      // Scaled destination reading a native source - duplicate guest pixels.
      dest_tile_pixel_x =
          builder.createBinOp(spv::OpUDiv, type_uint, dest_tile_pixel_x,
                              builder.makeUintConstant(dest_scale_x));
      dest_tile_pixel_y =
          builder.createBinOp(spv::OpUDiv, type_uint, dest_tile_pixel_y,
                              builder.makeUintConstant(dest_scale_y));
    }
  }

  // The transfer remaps the destination sample to the source sample through
  // the canonical sample coordinates, the layout is described in
  // XeEdramOffsetBytes in edram.xesli.
  if (key.depth_identity_layout) {
    assert_true(key.mode == EdramTransferMode::kDepthToDepth);
    assert_true(key.source_resource_format == key.dest_resource_format);
    assert_true(source_scale_x == 1 && source_scale_y == 1 &&
                dest_scale_x == 1 && dest_scale_y == 1);
    assert_true((key.source_msaa_samples == xenos::MsaaSamples::k1X &&
                 key.dest_msaa_samples == xenos::MsaaSamples::k4X) ||
                (key.source_msaa_samples == xenos::MsaaSamples::k4X &&
                 key.dest_msaa_samples == xenos::MsaaSamples::k1X));
    // Tile widths / heights are multiples of the sample permutation period.
    // With equal base and pitch and no relative tile wrap, doing the same
    // permutation on global coordinates (dest_tile_pixel_x / y above) absorbs
    // the tile-origin arithmetic.
  }
  spv::Id source_sample_id = dest_sample_id;
  spv::Id source_tile_pixel_x = dest_tile_pixel_x;
  spv::Id source_tile_pixel_y = dest_tile_pixel_y;
  spv::Id source_color_half = spv::NoResult;
  if (key.source_msaa_samples != key.dest_msaa_samples ||
      source_is_64bpp != dest_is_64bpp) {
    // Remap the destination view sample to the source view using the canonical
    // coordinates, both views are in the source scale space here.
    CanonicalSampleCoords canonical = CanonicalizeSample(
        builder, type_uint, key.dest_msaa_samples,
        options.msaa_2x_attachments_supported, dest_tile_pixel_x,
        dest_tile_pixel_y, dest_sample_id, source_scale_x, source_scale_y);
    if (dest_is_64bpp && !source_is_64bpp) {
      // The low 32bpp half of the 64bpp destination sample is obtained from
      // the source pixel at u = 2 * u_64bpp, and the high half comes from the
      // horizontally adjacent source pixel later.
      canonical.u_guest =
          builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                              canonical.u_guest, builder.makeUintConstant(1));
    } else if (!dest_is_64bpp && source_is_64bpp) {
      // The 32bpp destination sample is one half of the 64bpp source sample
      // at u = u_32bpp >> 1.
      source_color_half =
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, canonical.u_guest,
                              builder.makeUintConstant(1));
      canonical.u_guest =
          builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                              canonical.u_guest, builder.makeUintConstant(1));
    }
    spv::Id source_host_sample_id;
    DecanonicalizeSample(builder, type_uint, key.source_msaa_samples,
                         options.msaa_2x_attachments_supported, canonical,
                         source_scale_x, source_scale_y, source_tile_pixel_x,
                         source_tile_pixel_y, source_host_sample_id);
    if (source_host_sample_id != spv::NoResult) {
      source_sample_id = source_host_sample_id;
    }
  }
  assert_true((source_color_half != spv::NoResult) ==
              (source_is_64bpp && !dest_is_64bpp));

  // Source coordinates are derived - the host depth source path below needs
  // the destination-space coordinates back.
  dest_tile_pixel_x = dest_space_tile_pixel_x;
  dest_tile_pixel_y = dest_space_tile_pixel_y;

  uint32_t source_pixel_width_dwords_log2 =
      uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k4X) +
      uint32_t(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Copying between color and depth / stencil - swap 40-32bpp-sample columns
    // in the pixel index within the source 32bpp tile.
    uint32_t source_32bpp_tile_half_pixels =
        source_tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    source_tile_pixel_x = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int,
                                  source_tile_pixel_x),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(
                    spv::OpULessThan, builder.makeBoolType(),
                    source_tile_pixel_x,
                    builder.makeUintConstant(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(int32_t(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(
                    -int32_t(source_32bpp_tile_half_pixels)))));
  }

  spv::Id source_pixel_x_int;
  spv::Id source_pixel_y_int;
  if (key.depth_identity_layout) {
    source_pixel_x_int =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_tile_pixel_x);
    source_pixel_y_int =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_tile_pixel_y);
  } else if (key.color_identity_layout) {
    assert_true(source_is_color && dest_is_color && !source_is_64bpp &&
                !dest_is_64bpp);
    assert_true(key.mode == EdramTransferMode::kColorToColor);
    assert_true(key.source_msaa_samples == key.dest_msaa_samples);
    assert_true(source_scale_x == 1 && source_scale_y == 1 &&
                dest_scale_x == 1 && dest_scale_y == 1);
    source_pixel_x_int =
        builder.createUnaryOp(spv::OpBitcast, type_int, dest_pixel_x);
    source_pixel_y_int =
        builder.createUnaryOp(spv::OpBitcast, type_int, dest_pixel_y);
  } else {
    // Transform the destination 32bpp tile index into the source, wrapped, the
    // base difference is stored wrapped too. Sources in the next period hold
    // their tiles a period of rows down.
    spv::Id source_tile_index = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createBinOp(
            spv::OpBitwiseAnd, type_uint,
            builder.createBinOp(
                spv::OpIAdd, type_uint, dest_tile_index,
                builder.createTriOp(
                    spv::OpBitFieldUExtract, type_uint, address_constant,
                    builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                    builder.makeUintConstant(xenos::kEdramBaseTilesBits))),
            builder.makeUintConstant(xenos::kEdramTileCount - 1)),
        builder.createBinOp(
            spv::OpIMul, type_uint,
            builder.createTriOp(
                spv::OpBitFieldUExtract, type_uint, address_constant,
                builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2 +
                                         xenos::kEdramBaseTilesBits),
                builder.makeUintConstant(1)),
            builder.makeUintConstant(xenos::kEdramTileCount)));
    // Split the source 32bpp tile index into X and Y tile index within the
    // source image.
    spv::Id source_pitch_tiles = builder.createTriOp(
        spv::OpBitFieldUExtract, type_uint, address_constant,
        builder.makeUintConstant(xenos::kEdramPitchTilesBits),
        builder.makeUintConstant(xenos::kEdramPitchTilesBits));
    spv::Id source_tile_index_y = spv::NoResult;
    spv::Id source_tile_index_x = spv::NoResult;
    if (key.source_pitch_16) {
      ConstantDivMod(builder, source_tile_index, 16, source_tile_index_y,
                     source_tile_index_x);
    } else if (options.fast_pitch_divmod) {
      FastDivMod(builder, source_tile_index, source_pitch_tiles,
                 source_tile_index_y, source_tile_index_x);
    } else {
      source_tile_index_y = builder.createBinOp(
          spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
      source_tile_index_x = builder.createBinOp(
          spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
    }
    // Finally calculate the source texture coordinates.
    source_pixel_x_int = builder.createUnaryOp(
        spv::OpBitcast, type_int,
        builder.createBinOp(
            spv::OpIAdd, type_uint,
            builder.createBinOp(
                spv::OpIMul, type_uint,
                builder.makeUintConstant(source_tile_width_samples >>
                                         source_pixel_width_dwords_log2),
                source_tile_index_x),
            source_tile_pixel_x));
    source_pixel_y_int = builder.createUnaryOp(
        spv::OpBitcast, type_int,
        builder.createBinOp(
            spv::OpIAdd, type_uint,
            builder.createBinOp(
                spv::OpIMul, type_uint,
                builder.makeUintConstant(source_tile_height_samples >>
                                         uint32_t(key.source_msaa_samples >=
                                                  xenos::MsaaSamples::k2X)),
                source_tile_index_y),
            source_tile_pixel_y));
  }

  // Load the source.

  spv::Builder::TextureParameters source_texture_parameters = {};
  id_vector_temp.clear();
  id_vector_temp.push_back(source_pixel_x_int);
  id_vector_temp.push_back(source_pixel_y_int);
  spv::Id source_coordinates[2] = {
      builder.createCompositeConstruct(type_int2, id_vector_temp),
  };
  spv::Id source_sample_ids_int[2] = {};
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    source_sample_ids_int[0] =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  // The high 32bpp half of a 64bpp destination sample is identical to the
  // sample of the horizontally adjacent source pixel since each canonical
  // sample column of a single 64bpp value decodes to the same sample of two
  // horizontally adjacent pixels, regardless of sample count.
  bool source_load_is_two_32bpp_halves = !source_is_64bpp && dest_is_64bpp;
  if (source_load_is_two_32bpp_halves) {
    id_vector_temp.clear();
    id_vector_temp.push_back(
        builder.createBinOp(spv::OpIAdd, type_int, source_pixel_x_int,
                            builder.makeIntConstant(int32_t(source_scale_x))));
    id_vector_temp.push_back(source_pixel_y_int);
    source_coordinates[1] =
        builder.createCompositeConstruct(type_int2, id_vector_temp);
    source_sample_ids_int[1] = source_sample_ids_int[0];
  }
  spv::Id source_color[2][4] = {};
  if (source_color_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_color_texture, spv::NoPrecision);
    assert_true(source_color_component_type != spv::NoType);
    spv::Id source_color_vec4_type =
        builder.makeVectorType(source_color_component_type, 4);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_halves); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      spv::Id source_color_vec4 = builder.createTextureCall(
          spv::NoPrecision, source_color_vec4_type, false, true, false, false,
          false, source_texture_parameters, spv::ImageOperandsMaskNone);
      uint32_t source_color_components_remaining =
          source_color_texture_component_mask;
      uint32_t source_color_component_index;
      while (xe::bit_scan_forward(source_color_components_remaining,
                                  &source_color_component_index)) {
        source_color_components_remaining &=
            ~(uint32_t(1) << source_color_component_index);
        source_color[i][source_color_component_index] =
            builder.createCompositeExtract(source_color_vec4,
                                           source_color_component_type,
                                           source_color_component_index);
      }
    }
  }
  spv::Id source_depth_float[2] = {};
  if (source_depth_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_depth_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_halves); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_depth_float[i] = builder.createCompositeExtract(
          builder.createTextureCall(
              spv::NoPrecision, type_float4, false, true, false, false, false,
              source_texture_parameters, spv::ImageOperandsMaskNone),
          type_float, 0);
    }
  }
  spv::Id source_stencil[2] = {};
  if (source_stencil_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_halves); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_stencil[i] = builder.createCompositeExtract(
          builder.createTextureCall(
              spv::NoPrecision, type_uint4, false, true, false, false, false,
              source_texture_parameters, spv::ImageOperandsMaskNone),
          type_uint, 0);
    }
  }

  // Pick the needed 32bpp half of the 64bpp color.
  if (source_is_64bpp && !dest_is_64bpp) {
    uint32_t source_color_half_component_count =
        source_color_format_component_count >> 1;
    assert_true(source_color_half != spv::NoResult);
    spv::Id source_color_is_second_half =
        builder.createBinOp(spv::OpINotEqual, type_bool, source_color_half,
                            builder.makeUintConstant(0));
    if (mode.output == EdramTransferOutput::kStencilBit) {
      source_color[0][0] = builder.createTriOp(
          spv::OpSelect, source_color_component_type,
          source_color_is_second_half,
          source_color[0][source_color_half_component_count],
          source_color[0][0]);
    } else {
      for (uint32_t i = 0; i < source_color_half_component_count; ++i) {
        source_color[0][i] = builder.createTriOp(
            spv::OpSelect, source_color_component_type,
            source_color_is_second_half,
            source_color[0][source_color_half_component_count + i],
            source_color[0][i]);
      }
    }
  }

  if (output_fragment_stencil_ref != spv::NoResult &&
      source_stencil[0] != spv::NoResult) {
    // For the depth -> depth case, write the stencil directly to the output.
    assert_true(mode.output == EdramTransferOutput::kDepth);
    builder.createStore(
        builder.createUnaryOp(spv::OpBitcast, type_int, source_stencil[0]),
        output_fragment_stencil_ref);
  }

  if (dest_is_64bpp) {
    // Construct the 64bpp color from two 32-bit samples or one 64-bit sample.
    // If `packed` (two uints) are created, use the generic path involving
    // unpacking.
    // Otherwise, the fragment data output must be written to directly by the
    // reached control flow path.
    spv::Id packed[2] = {};
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (source_color_format ==
              xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
            // Gamma source stores linear; encode RGB of both 32bpp samples to
            // gamma bytes before packing into the 64bpp destination. Only
            // loaded components are converted.
            for (uint32_t i = 0; i < 2; ++i) {
              for (uint32_t j = 0; j < 3; ++j) {
                if (source_color[i][j] == spv::NoResult) {
                  continue;
                }
                source_color[i][j] = SpirvShaderTranslator::LinearToPWLGamma(
                    &builder, source_color[i][j], true, ext_inst_glsl_std_450);
              }
            }
          }
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
          spv::Id component_width = builder.makeUintConstant(8);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        source_color[i][0], unorm_scale),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float,
                                              source_color[i][j], unorm_scale),
                          unorm_round_offset)),
                  builder.makeUintConstant(8 * j), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        source_color[i][0], unorm_scale_rgb),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(
                              spv::OpFMul, type_float, source_color[i][j],
                              j == 3 ? unorm_scale_a : unorm_scale_rgb),
                          unorm_round_offset)),
                  builder.makeUintConstant(10 * j),
                  j == 3 ? width_a : width_rgb);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::
            k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id float_0 = builder.makeFloatConstant(0.0f);
          spv::Id float_1 = builder.makeFloatConstant(1.0f);
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id offset_a = builder.makeUintConstant(30);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed[i] = SpirvShaderTranslator::UnclampedFloat32To7e3(
                builder, source_color[i][0], ext_inst_glsl_std_450);
            for (uint32_t j = 1; j < 3; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  SpirvShaderTranslator::UnclampedFloat32To7e3(
                      builder, source_color[i][j], ext_inst_glsl_std_450),
                  builder.makeUintConstant(10 * j), width_rgb);
            }
            // Saturate and convert the alpha.
            spv::Id alpha_saturated = builder.createTriBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                source_color[i][3], float_0, float_1);
            packed[i] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[i],
                builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createBinOp(
                        spv::OpFAdd, type_float,
                        builder.createBinOp(spv::OpFMul, type_float,
                                            alpha_saturated, unorm_scale_a),
                        unorm_round_offset)),
                offset_a, width_a);
          }
        } break;
        // All 64bpp formats, and all 16 bits per component formats, are
        // represented as integers in ownership transfer for safe handling of
        // NaN encodings and -32768 / -32767.
        // TODO(Triang3l): Handle the case when that's not true (no multisampled
        // sampled images, no 16-bit UNORM, no cross-packing 32bpp aliasing on a
        // portability subset device or a 64bpp format where that wouldn't help
        // anyway).
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          if (dest_color_format ==
              xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            spv::Id component_offset_width = builder.makeUintConstant(16);
            spv::Id color_16_in_32[2];
            for (uint32_t i = 0; i < 2; ++i) {
              color_16_in_32[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, source_color[i][0],
                  source_color[i][1], component_offset_width,
                  component_offset_width);
            }
            id_vector_temp.clear();
            id_vector_temp.push_back(color_16_in_32[0]);
            id_vector_temp.push_back(color_16_in_32[1]);
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[i >> 1][i & 1]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          if (dest_color_format ==
              xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            spv::Id component_offset_width = builder.makeUintConstant(16);
            spv::Id color_16_in_32[2];
            for (uint32_t i = 0; i < 2; ++i) {
              color_16_in_32[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, source_color[0][i << 1],
                  source_color[0][(i << 1) + 1], component_offset_width,
                  component_offset_width);
            }
            id_vector_temp.clear();
            id_vector_temp.push_back(color_16_in_32[0]);
            id_vector_temp.push_back(color_16_in_32[1]);
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[i][0];
            if (!source_color_is_uint) {
              packed[i] =
                  builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[0][i];
            if (!source_color_is_uint) {
              packed[i] =
                  builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
      }
    } else {
      assert_true(source_depth_texture != spv::NoResult);
      assert_true(source_stencil_texture != spv::NoResult);
      spv::Id depth_offset = builder.makeUintConstant(8);
      spv::Id depth_width = builder.makeUintConstant(24);
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Id depth24 = spv::NoResult;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the
            // correct conversion, adding +0.5 and rounding towards zero results
            // in red instead of black in the 4D5307E6 clear shader.
            depth24 = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(
                        spv::OpFMul, type_float, source_depth_float[i],
                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[i],
                !options.depth_float24_convert_in_pixel_shader &&
                    options.depth_float24_round,
                true, ext_inst_glsl_std_450);
          } break;
        }
        // Merge depth and stencil.
        packed[i] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint,
                                         source_stencil[i], depth24,
                                         depth_offset, depth_width);
      }
    }
    // Common path unless there was a specialized one - unpack two packed 32-bit
    // parts.
    if (packed[0] != spv::NoResult) {
      assert_true(packed[1] != spv::NoResult);
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        id_vector_temp.clear();
        id_vector_temp.push_back(packed[0]);
        id_vector_temp.push_back(packed[1]);
        // Multisampled sampled images are optional in Vulkan, and image views
        // of different formats can't be created separately for sampled image
        // and color attachment usages, so no multisampled integer sampled image
        // support implies no multisampled integer framebuffer attachment
        // support in Xenia.
        if (!dest_color_is_uint) {
          for (spv::Id& float32 : id_vector_temp) {
            float32 =
                builder.createUnaryOp(spv::OpBitcast, type_float, float32);
          }
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data,
                                                             id_vector_temp),
                            output_fragment_data);
      } else {
        spv::Id const_uint_0 = builder.makeUintConstant(0);
        spv::Id const_uint_16 = builder.makeUintConstant(16);
        id_vector_temp.clear();
        for (uint32_t i = 0; i < 4; ++i) {
          id_vector_temp.push_back(builder.createTriOp(
              spv::OpBitFieldUExtract, type_uint, packed[i >> 1],
              (i & 1) ? const_uint_16 : const_uint_0, const_uint_16));
        }
        // TODO(Triang3l): Handle the case when that's not true (no multisampled
        // sampled images, no 16-bit UNORM, no cross-packing 32bpp aliasing on a
        // portability subset device or a 64bpp format where that wouldn't help
        // anyway).
        builder.createStore(builder.createCompositeConstruct(type_fragment_data,
                                                             id_vector_temp),
                            output_fragment_data);
      }
    }
  } else {
    // If `packed` is created, use the generic path involving unpacking.
    // - For a color destination, the packed 32bpp color.
    // - For a depth / stencil destination, stencil in 0:7, depth in 8:31
    //   normally, or depth in 0:23 and zeros in 24:31 with packed_only_depth.
    // - For a stencil bit, stencil in 0:7.
    // Otherwise, the fragment data or fragment depth / stencil output must be
    // written to directly by the reached control flow path.
    spv::Id packed = spv::NoResult;
    bool packed_only_depth = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            // Same byte layout - only a gamma color space conversion on RGB may
            // be needed. The gamma resource format is stored as linear in the
            // unorm16 host render target, so convert when exactly one side is
            // gamma (alpha is never gamma-encoded).
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              spv::Id component = source_color[0][i];
              if (i < 3 && dest_color_format != source_color_format) {
                if (dest_color_format ==
                    xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                  // Gamma source (linear storage) -> plain dest (gamma bytes).
                  component = SpirvShaderTranslator::LinearToPWLGamma(
                      &builder, component, true, ext_inst_glsl_std_450);
                } else {
                  // Plain source (gamma bytes) -> gamma dest (linear storage).
                  component = SpirvShaderTranslator::PWLGammaToLinear(
                      &builder, component, true, ext_inst_glsl_std_450);
                }
              }
              id_vector_temp.push_back(component);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            if (source_color_format ==
                xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              // Gamma source stores linear; encode RGB to gamma bytes before
              // packing into the raw 32bpp value reinterpreted by the
              // differently-formatted destination. Only loaded components are
              // converted (stencil bit output loads red only).
              for (uint32_t j = 0; j < 3; ++j) {
                if (source_color[0][j] == spv::NoResult) {
                  continue;
                }
                source_color[0][j] = SpirvShaderTranslator::LinearToPWLGamma(
                    &builder, source_color[0][j], true, ext_inst_glsl_std_450);
              }
            }
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
            uint32_t packed_component_offset = 0;
            if (mode.output == EdramTransferOutput::kDepth) {
              // When need only depth, not stencil, skip the red component, and
              // put the depth from GBA directly in the lower bits.
              packed_component_offset = 1;
              packed_only_depth = true;
              if (output_fragment_stencil_ref != spv::NoResult) {
                builder.createStore(
                    builder.createUnaryOp(
                        spv::OpBitcast, type_int,
                        builder.createUnaryOp(
                            spv::OpConvertFToU, type_uint,
                            builder.createBinOp(
                                spv::OpFAdd, type_float,
                                builder.createBinOp(spv::OpFMul, type_float,
                                                    source_color[0][0],
                                                    unorm_scale),
                                unorm_round_offset))),
                    output_fragment_stencil_ref);
              }
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(
                        spv::OpFMul, type_float,
                        source_color[0][packed_component_offset], unorm_scale),
                    unorm_round_offset));
            if (mode.output != EdramTransferOutput::kStencilBit) {
              spv::Id component_width = builder.makeUintConstant(8);
              for (uint32_t i = 1; i < 4 - packed_component_offset; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(
                                spv::OpFMul, type_float,
                                source_color[0][packed_component_offset + i],
                                unorm_scale),
                            unorm_round_offset)),
                    builder.makeUintConstant(8 * i), component_width);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_color &&
              (dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
               dest_color_format == xenos::ColorRenderTargetFormat::
                                        k_2_10_10_10_AS_10_10_10_10)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        source_color[0][0], unorm_scale_rgb),
                    unorm_round_offset));
            if (mode.output != EdramTransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              for (uint32_t i = 1; i < 4; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(
                                spv::OpFMul, type_float, source_color[0][i],
                                i == 3 ? unorm_scale_a : unorm_scale_rgb),
                            unorm_round_offset)),
                    builder.makeUintConstant(10 * i),
                    i == 3 ? width_a : width_rgb);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::
            k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_color &&
              (dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               dest_color_format == xenos::ColorRenderTargetFormat::
                                        k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed = SpirvShaderTranslator::UnclampedFloat32To7e3(
                builder, source_color[0][0], ext_inst_glsl_std_450);
            if (mode.output != EdramTransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 1; i < 3; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    SpirvShaderTranslator::UnclampedFloat32To7e3(
                        builder, source_color[0][i], ext_inst_glsl_std_450),
                    builder.makeUintConstant(10 * i), width_rgb);
              }
              // Saturate and convert the alpha.
              spv::Id alpha_saturated = builder.createTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                  source_color[0][3], builder.makeFloatConstant(0.0f),
                  builder.makeFloatConstant(1.0f));
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float,
                                              alpha_saturated,
                                              builder.makeFloatConstant(3.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(30), builder.makeUintConstant(2));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          // All 64bpp formats, and all 16 bits per component formats, are
          // represented as integers in ownership transfer for safe handling of
          // NaN encodings and -32768 / -32767.
          // TODO(Triang3l): Handle the case when that's not true (no
          // multisampled sampled images, no 16-bit UNORM, no cross-packing
          // 32bpp aliasing on a portability subset device or a 64bpp format
          // where that wouldn't help anyway).
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_16_16_FLOAT)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 2; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            packed = source_color[0][0];
            if (mode.output != EdramTransferOutput::kStencilBit) {
              spv::Id component_offset_width = builder.makeUintConstant(16);
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed, source_color[0][1],
                  component_offset_width, component_offset_width);
            }
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          packed = source_color[0][0];
          if (!source_color_is_uint) {
            packed = builder.createUnaryOp(spv::OpBitcast, type_uint, packed);
          }
        } break;
      }
    } else if (source_depth_float[0] != spv::NoResult) {
      if (mode.output == EdramTransferOutput::kDepth &&
          dest_depth_format == source_depth_format) {
        builder.createStore(source_depth_float[0], output_fragment_depth);
      } else {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(
                        spv::OpFMul, type_float, source_depth_float[0],
                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            packed = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[0],
                !options.depth_float24_convert_in_pixel_shader &&
                    options.depth_float24_round,
                true, ext_inst_glsl_std_450);
          } break;
        }
        if (mode.output == EdramTransferOutput::kDepth) {
          packed_only_depth = true;
        } else {
          // Merge depth and stencil.
          packed = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_stencil[0], packed,
              builder.makeUintConstant(8), builder.makeUintConstant(24));
        }
      }
    }
    // For stencil bit output, use stencil directly for the discard check.
    if (packed == spv::NoResult &&
        mode.output == EdramTransferOutput::kStencilBit) {
      packed = source_stencil[0];
    }
    switch (mode.output) {
      case EdramTransferOutput::kColor: {
        // Unless a special path was taken, unpack the raw 32bpp value into the
        // 32bpp color output.
        if (packed != spv::NoResult) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              // Same-base 7e3 source, decode to [0, 1] rather than
              // bit-reinterpret. Never a gamma dest, which has no encode here.
              // See IsTransferValueConverted7e3And8888.
              if (key.value_convert && source_is_color &&
                  dest_color_format ==
                      xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
                  xenos::GetStorageColorFormat(source_color_format) ==
                      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT) {
                id_vector_temp.clear();
                for (uint32_t i = 0; i < 3; ++i) {
                  id_vector_temp.push_back(builder.createTriBuiltinCall(
                      type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                      SpirvShaderTranslator::Float7e3To32(
                          builder, packed, 10 * i, false,
                          ext_inst_glsl_std_450),
                      builder.makeFloatConstant(0.0f),
                      builder.makeFloatConstant(1.0f)));
                }
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                            packed,
                                            builder.makeUintConstant(30),
                                            builder.makeUintConstant(2))),
                    builder.makeFloatConstant(1.0f / 3.0f)));
                builder.createStore(builder.createCompositeConstruct(
                                        type_fragment_data, id_vector_temp),
                                    output_fragment_data);
                break;
              }
              // For a gamma destination stored as linear in unorm16, the raw
              // EDRAM bytes are reinterpreted as gamma and decoded to the
              // midpoint of each byte's linear range (alpha stays linear). The
              // midpoint survives the unorm16 round-trip so a later re-encode
              // reproduces the byte. Reaching the gamma format implies
              // gamma_render_target_as_unorm16.
              bool is_gamma = dest_color_format ==
                              xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
              spv::Id component_width = builder.makeUintConstant(8);
              spv::Id unorm_scale = builder.makeFloatConstant(1.0f / 255.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                spv::Id byte_uint = builder.createTriOp(
                    spv::OpBitFieldUExtract, type_uint, packed,
                    builder.makeUintConstant(8 * i), component_width);
                if (is_gamma && i < 3) {
                  id_vector_temp.push_back(
                      GammaByteToLinearMidpoint(builder, byte_uint));
                } else {
                  id_vector_temp.push_back(builder.createBinOp(
                      spv::OpFMul, type_float,
                      builder.createUnaryOp(spv::OpConvertUToF, type_float,
                                            byte_uint),
                      unorm_scale));
                }
              }
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_rgb =
                  builder.makeFloatConstant(1.0f / 1023.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              spv::Id unorm_scale_a = builder.makeFloatConstant(1.0f / 3.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                            packed,
                                            builder.makeUintConstant(10 * i),
                                            i == 3 ? width_a : width_rgb)),
                    i == 3 ? unorm_scale_a : unorm_scale_rgb));
              }
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::
                k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              id_vector_temp.clear();
              // Color.
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 0; i < 3; ++i) {
                id_vector_temp.push_back(SpirvShaderTranslator::Float7e3To32(
                    builder, packed, 10 * i, false, ext_inst_glsl_std_450));
              }
              // Alpha.
              id_vector_temp.push_back(builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                          packed, builder.makeUintConstant(30),
                                          builder.makeUintConstant(2))),
                  builder.makeFloatConstant(1.0f / 3.0f)));
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              // All 16 bits per component formats are represented as integers
              // in ownership transfer for safe handling of NaN encodings and
              // -32768 / -32767.
              // TODO(Triang3l): Handle the case when that's not true (no
              // multisampled sampled images, no 16-bit UNORM, no cross-packing
              // 32bpp aliasing on a portability subset device or a 64bpp format
              // where that wouldn't help anyway).
              spv::Id component_offset_width = builder.makeUintConstant(16);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 2; ++i) {
                id_vector_temp.push_back(builder.createTriOp(
                    spv::OpBitFieldUExtract, type_uint, packed,
                    i ? component_offset_width : builder.makeUintConstant(0),
                    component_offset_width));
              }
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // Float32 is transferred as uint32 to preserve NaN encodings.
              // However, multisampled sampled images are optional in Vulkan,
              // and image views of different formats can't be created
              // separately for sampled image and color attachment usages, so no
              // multisampled integer sampled image support implies no
              // multisampled integer framebuffer attachment support in Xenia.
              spv::Id float32 = packed;
              if (!dest_color_is_uint) {
                float32 =
                    builder.createUnaryOp(spv::OpBitcast, type_float, float32);
              }
              builder.createStore(float32, output_fragment_data);
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              assert_unhandled_case(dest_color_format);
          }
        }
      } break;
      case EdramTransferOutput::kDepth: {
        if (packed) {
          spv::Id guest_depth24 = packed;
          if (!packed_only_depth) {
            // Extract the depth bits.
            guest_depth24 =
                builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                    guest_depth24, builder.makeUintConstant(8));
          }
          // Load the host float32 depth, check if, when converted to the guest
          // format, it's the same as the guest source, thus up to date, and if
          // it is, write host float32 depth, otherwise do the guest -> host
          // conversion.
          spv::Id host_depth32 = spv::NoResult;
          if (host_depth_source_texture != spv::NoResult) {
            // Convert position and sample index from within the destination
            // tile to within the host depth source tile by remapping through
            // the canonical coordinates. Both views are 32bpp and use the
            // destination scale.
            spv::Id host_depth_source_sample_id = dest_sample_id;
            spv::Id host_depth_source_tile_pixel_x = dest_tile_pixel_x;
            spv::Id host_depth_source_tile_pixel_y = dest_tile_pixel_y;
            if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
              CanonicalSampleCoords host_depth_canonical = CanonicalizeSample(
                  builder, type_uint, key.dest_msaa_samples,
                  options.msaa_2x_attachments_supported, dest_tile_pixel_x,
                  dest_tile_pixel_y, dest_sample_id, dest_scale_x,
                  dest_scale_y);
              spv::Id host_depth_remapped_sample_id;
              DecanonicalizeSample(
                  builder, type_uint, key.host_depth_source_msaa_samples,
                  options.msaa_2x_attachments_supported, host_depth_canonical,
                  dest_scale_x, dest_scale_y, host_depth_source_tile_pixel_x,
                  host_depth_source_tile_pixel_y,
                  host_depth_remapped_sample_id);
              if (host_depth_remapped_sample_id != spv::NoResult) {
                host_depth_source_sample_id = host_depth_remapped_sample_id;
              }
            }
            assert_true(push_constants_member_host_depth_address != UINT32_MAX);
            id_vector_temp.clear();
            id_vector_temp.push_back(builder.makeIntConstant(
                int32_t(push_constants_member_host_depth_address)));
            spv::Id host_depth_address_constant = builder.createLoad(
                builder.createAccessChain(spv::StorageClassPushConstant,
                                          push_constants, id_vector_temp),
                spv::NoPrecision);
            // Transform the destination tile index into the host depth source,
            // wrapped.
            spv::Id host_depth_source_tile_index = builder.createBinOp(
                spv::OpBitwiseAnd, type_uint,
                builder.createBinOp(
                    spv::OpIAdd, type_uint, dest_tile_index,
                    builder.createTriOp(
                        spv::OpBitFieldUExtract, type_uint,
                        host_depth_address_constant,
                        builder.makeUintConstant(xenos::kEdramPitchTilesBits *
                                                 2),
                        builder.makeUintConstant(xenos::kEdramBaseTilesBits))),
                builder.makeUintConstant(xenos::kEdramTileCount - 1));
            // Split the host depth source tile index into X and Y tile index
            // within the source image.
            spv::Id host_depth_source_pitch_tiles = builder.createTriOp(
                spv::OpBitFieldUExtract, type_uint, host_depth_address_constant,
                builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                builder.makeUintConstant(xenos::kEdramPitchTilesBits));
            spv::Id host_depth_source_tile_index_y = spv::NoResult;
            spv::Id host_depth_source_tile_index_x = spv::NoResult;
            if (options.fast_pitch_divmod) {
              FastDivMod(builder, host_depth_source_tile_index,
                         host_depth_source_pitch_tiles,
                         host_depth_source_tile_index_y,
                         host_depth_source_tile_index_x);
            } else {
              host_depth_source_tile_index_y = builder.createBinOp(
                  spv::OpUDiv, type_uint, host_depth_source_tile_index,
                  host_depth_source_pitch_tiles);
              host_depth_source_tile_index_x = builder.createBinOp(
                  spv::OpUMod, type_uint, host_depth_source_tile_index,
                  host_depth_source_pitch_tiles);
            }
            // Finally calculate the host depth source texture coordinates.
            spv::Id host_depth_source_pixel_x_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(
                        spv::OpIMul, type_uint,
                        builder.makeUintConstant(
                            dest_tile_width_samples >>
                            uint32_t(key.host_depth_source_msaa_samples >=
                                     xenos::MsaaSamples::k4X)),
                        host_depth_source_tile_index_x),
                    host_depth_source_tile_pixel_x));
            spv::Id host_depth_source_pixel_y_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(
                        spv::OpIMul, type_uint,
                        builder.makeUintConstant(
                            dest_tile_height_samples >>
                            uint32_t(key.host_depth_source_msaa_samples >=
                                     xenos::MsaaSamples::k2X)),
                        host_depth_source_tile_index_y),
                    host_depth_source_tile_pixel_y));
            // Load the host depth source.
            spv::Builder::TextureParameters
                host_depth_source_texture_parameters = {};
            host_depth_source_texture_parameters.sampler =
                builder.createLoad(host_depth_source_texture, spv::NoPrecision);
            id_vector_temp.clear();
            id_vector_temp.push_back(host_depth_source_pixel_x_int);
            id_vector_temp.push_back(host_depth_source_pixel_y_int);
            host_depth_source_texture_parameters.coords =
                builder.createCompositeConstruct(type_int2, id_vector_temp);
            if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
              host_depth_source_texture_parameters.sample =
                  builder.createUnaryOp(spv::OpBitcast, type_int,
                                        host_depth_source_sample_id);
            } else {
              host_depth_source_texture_parameters.lod =
                  builder.makeIntConstant(0);
            }
            host_depth32 = builder.createCompositeExtract(
                builder.createTextureCall(spv::NoPrecision, type_float4, false,
                                          true, false, false, false,
                                          host_depth_source_texture_parameters,
                                          spv::ImageOperandsMaskNone),
                type_float, 0);
          } else if (host_depth_source_buffer != spv::NoResult) {
            // Get the address in the EDRAM scratch buffer and load from there.
            // The beginning of the buffer is (0, 0) of the destination.
            // 40-sample columns are not swapped for addressing simplicity
            // (because this is used for depth -> depth transfers, where
            // swapping isn't needed).
            // Convert samples to pixels.
            assert_true(key.host_depth_source_msaa_samples ==
                        xenos::MsaaSamples::k1X);
            spv::Id dest_tile_sample_x = dest_tile_pixel_x;
            spv::Id dest_tile_sample_y = dest_tile_pixel_y;
            if (key.dest_msaa_samples >= xenos::MsaaSamples::k2X) {
              if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                // Horizontal sample index in bit 0.
                dest_tile_sample_x = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, dest_sample_id,
                    dest_tile_pixel_x, builder.makeUintConstant(1),
                    builder.makeUintConstant(31));
              }
              // Vertical sample index as 1 or 0 in bit 0 for true 2x or as 0
              // or 1 in bit 1 for 4x or for 2x emulated as 4x.
              dest_tile_sample_y = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint,
                  builder.createBinOp(
                      (key.dest_msaa_samples == xenos::MsaaSamples::k2X &&
                       options.msaa_2x_attachments_supported)
                          ? spv::OpBitwiseXor
                          : spv::OpShiftRightLogical,
                      type_uint, dest_sample_id, builder.makeUintConstant(1)),
                  dest_tile_pixel_y, builder.makeUintConstant(1),
                  builder.makeUintConstant(31));
            }
            // Combine the tile sample index and the tile index.
            // The tile index doesn't need to be wrapped, as the host depth is
            // written to the beginning of the buffer, without the base offset.
            spv::Id host_depth_offset = builder.createBinOp(
                spv::OpIAdd, type_uint,
                builder.createBinOp(
                    spv::OpIMul, type_uint,
                    builder.makeUintConstant(dest_tile_width_samples *
                                             dest_tile_height_samples),
                    dest_tile_index),
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(
                        spv::OpIMul, type_uint,
                        builder.makeUintConstant(dest_tile_width_samples),
                        dest_tile_sample_y),
                    dest_tile_sample_x));
            id_vector_temp.clear();
            // The only SSBO structure member.
            id_vector_temp.push_back(builder.makeIntConstant(0));
            id_vector_temp.push_back(builder.createUnaryOp(
                spv::OpBitcast, type_int, host_depth_offset));
            // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is
            // generated, it's Uniform.
            host_depth32 = builder.createUnaryOp(
                spv::OpBitcast, type_float,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassUniform,
                                              host_depth_source_buffer,
                                              id_vector_temp),
                    spv::NoPrecision));
          }
          const bool branchless_host_depth =
              options.branchless_d24_host_depth &&
              dest_depth_format == xenos::DepthRenderTargetFormat::kD24S8;
          spv::Block* depth24_to_depth32_header = builder.getBuildPoint();
          spv::Block* depth24_to_depth32_merge = nullptr;
          spv::Id host_depth24 = spv::NoResult;
          spv::Id host_depth_outdated = spv::NoResult;
          if (host_depth32 != spv::NoResult) {
            // Convert the host depth value to the guest format and check if it
            // matches the value in the currently owning guest render target.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to the nearest even integer. This seems to be the
                // correct conversion, adding +0.5 and rounding towards zero
                // results in red instead of black in the 4D5307E6 clear shader.
                host_depth24 = builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createUnaryBuiltinCall(
                        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                        builder.createBinOp(
                            spv::OpFMul, type_float, host_depth32,
                            builder.makeFloatConstant(float(0xFFFFFF)))));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                host_depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                    builder, host_depth32,
                    !options.depth_float24_convert_in_pixel_shader &&
                        options.depth_float24_round,
                    true, ext_inst_glsl_std_450);
              } break;
            }
            assert_true(host_depth24 != spv::NoResult);
            // Update the header block pointer after the conversion (to avoid
            // assuming that the conversion doesn't branch).
            depth24_to_depth32_header = builder.getBuildPoint();
            host_depth_outdated = builder.createBinOp(
                spv::OpINotEqual, type_bool, guest_depth24, host_depth24);
            if (!branchless_host_depth) {
              spv::Block& depth24_to_depth32_convert_entry =
                  builder.makeNewBlock();
              {
                spv::Block& depth24_to_depth32_merge_block =
                    builder.makeNewBlock();
                depth24_to_depth32_merge = &depth24_to_depth32_merge_block;
              }
              builder.createSelectionMerge(depth24_to_depth32_merge,
                                           spv::SelectionControlMaskNone);
              builder.createConditionalBranch(host_depth_outdated,
                                              &depth24_to_depth32_convert_entry,
                                              depth24_to_depth32_merge);
              builder.setBuildPoint(&depth24_to_depth32_convert_entry);
            }
          }
          // Convert the guest 24-bit depth to float32 (in an open conditional
          // if the host depth is also loaded).
          spv::Id guest_depth32 = spv::NoResult;
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              // Multiplying by 1.0 / 0xFFFFFF produces an incorrect result (for
              // 0xC00000, for instance - which is 2_10_10_10 clear to 0001) -
              // rescale from 0...0xFFFFFF to 0...0x1000000 doing what true
              // float division followed by multiplication does (on x86-64 MSVC
              // with default SSE rounding) - values starting from 0x800000
              // become bigger by 1; then accurately bias the result's exponent.
              guest_depth32 = builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createBinOp(
                          spv::OpIAdd, type_uint, guest_depth24,
                          builder.createBinOp(spv::OpShiftRightLogical,
                                              type_uint, guest_depth24,
                                              builder.makeUintConstant(23)))),
                  builder.makeFloatConstant(1.0f / float(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              guest_depth32 = SpirvShaderTranslator::Depth20e4To32(
                  builder, guest_depth24, 0, true, false,
                  ext_inst_glsl_std_450);
            } break;
          }
          assert_true(guest_depth32 != spv::NoResult);
          spv::Id fragment_depth32 = guest_depth32;
          if (host_depth32 != spv::NoResult) {
            if (branchless_host_depth) {
              // D24S8 conversion is four ALU operations with no side effects.
              // Select, rather than interpolate, to preserve the host float's
              // exact bits when its quantized depth still matches the owner.
              fragment_depth32 = builder.createTriOp(
                  spv::OpSelect, type_float, host_depth_outdated, guest_depth32,
                  host_depth32);
            } else {
              assert_not_null(depth24_to_depth32_merge);
              spv::Id depth24_to_depth32_result_block_id =
                  builder.getBuildPoint()->getId();
              builder.createBranch(depth24_to_depth32_merge);
              builder.setBuildPoint(depth24_to_depth32_merge);
              id_vector_temp.clear();
              id_vector_temp.push_back(guest_depth32);
              id_vector_temp.push_back(depth24_to_depth32_result_block_id);
              id_vector_temp.push_back(host_depth32);
              id_vector_temp.push_back(depth24_to_depth32_header->getId());
              fragment_depth32 =
                  builder.createOp(spv::OpPhi, type_float, id_vector_temp);
            }
          }
          builder.createStore(fragment_depth32, output_fragment_depth);
          // Unpack the stencil into the stencil reference output if needed and
          // not already written.
          if (!packed_only_depth &&
              output_fragment_stencil_ref != spv::NoResult) {
            builder.createStore(
                builder.createUnaryOp(
                    spv::OpBitcast, type_int,
                    builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                        builder.makeUintConstant(UINT8_MAX))),
                output_fragment_stencil_ref);
          }
        }
      } break;
      case EdramTransferOutput::kStencilBit: {
        if (packed && !options.no_discard_stencil) {
          // Kill the sample if the needed stencil bit is not set.
          assert_true(push_constants_member_stencil_mask != UINT32_MAX);
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(
              int32_t(push_constants_member_stencil_mask)));
          spv::Id stencil_mask_constant = builder.createLoad(
              builder.createAccessChain(spv::StorageClassPushConstant,
                                        push_constants, id_vector_temp),
              spv::NoPrecision);
          SpirvBuilder::IfBuilder stencil_kill_if(
              builder.createBinOp(
                  spv::OpIEqual, type_bool,
                  builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                      stencil_mask_constant),
                  builder.makeUintConstant(0)),
              spv::SelectionControlMaskNone, builder);
          builder.createNoResultOp(spv::OpKill);
          // OpKill terminates the block.
          stencil_kill_if.makeEndIf(false);
        }
      } break;
    }
  }

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeOriginUpperLeft);
  if (output_fragment_depth != spv::NoResult) {
    builder.addExecutionMode(main_function, spv::ExecutionModeDepthReplacing);
  }
  if (output_fragment_stencil_ref != spv::NoResult) {
    builder.addExecutionMode(main_function,
                             spv::ExecutionModeStencilRefReplacingEXT);
  }
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelFragment, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);
  return shader_code;
}

}  // namespace gpu
}  // namespace xe
