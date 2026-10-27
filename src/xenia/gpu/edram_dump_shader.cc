/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/edram_dump_shader.h"

#include <array>
#include <initializer_list>
#include <vector>

#include "third_party/glslang/SPIRV/GLSL.std.450.h"

#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

namespace {

// Positions of the fields the direct resolve store reads out of the packed
// draw_util::Resolve* structures, the same ones resolve.xesli unpacks, derived
// from the widths their bitfields are declared with.
constexpr uint32_t kResolveEdramInfoBaseTilesShift =
    xenos::kEdramPitchTilesBits + xenos::kMsaaSamplesBits + 1;
constexpr uint32_t kResolveEdramInfoFormatShift =
    kResolveEdramInfoBaseTilesShift + xenos::kEdramBaseTilesBits;
constexpr uint32_t kResolveEdramInfoFillHalfPixelOffsetShift =
    kResolveEdramInfoFormatShift + xenos::kRenderTargetFormatBits + 1;

constexpr uint32_t kResolveCoordinateInfoOffsetXShift = 0;
constexpr uint32_t kResolveCoordinateInfoOffsetYShift = 4;
constexpr uint32_t kResolveCoordinateInfoWidthShift = 5;
constexpr uint32_t kResolveCoordinateInfoWidthBits =
    xenos::kResolveSizeBits - xenos::kResolveAlignmentPixelsLog2;

constexpr uint32_t kResolveDestInfoEndianBits = 3;
constexpr uint32_t kResolveDestInfoIsArrayShift = 3;
constexpr uint32_t kResolveDestInfoSliceShift = 4;
constexpr uint32_t kResolveDestInfoSwapShift = 24;

constexpr uint32_t kResolveDestCoordinateInfoPitchBits =
    xenos::kTexture2DCubeMaxWidthHeightLog2 + 2 -
    xenos::kTextureTileWidthHeightLog2;
constexpr uint32_t kResolveDestCoordinateInfoHeightShift =
    kResolveDestCoordinateInfoPitchBits;
constexpr uint32_t kResolveDestCoordinateInfoOffsetXShift =
    2 * kResolveDestCoordinateInfoPitchBits;
constexpr uint32_t kResolveDestCoordinateInfoOffsetYShift =
    kResolveDestCoordinateInfoOffsetXShift + 7 -
    xenos::kResolveAlignmentPixelsLog2;
constexpr uint32_t kResolveDestCoordinateInfoSampleSelectShift =
    kResolveDestCoordinateInfoOffsetYShift + 7 -
    xenos::kResolveAlignmentPixelsLog2;

// XENOS_TEXTURE_MACRO_TILE_HEIGHT_3D_LOG2 in texture_address.xesli.
constexpr uint32_t kTextureMacroTileHeight3DLog2 = 4;

// XenosTextureTiledAddressCombine in texture_address.xesli.
constexpr uint32_t CombineTiledAddress(uint32_t outer_inner_bytes,
                                       uint32_t bank, uint32_t pipe,
                                       uint32_t y_lsb) {
  return (y_lsb << 4) | (pipe << 6) | (bank << 11) | (outer_inner_bytes & 0xF) |
         (((outer_inner_bytes >> 4) & 0x1) << 5) |
         (((outer_inner_bytes >> 5) & 0x7) << 8) |
         (outer_inner_bytes >> 8 << 12);
}

// XenosTextureTiledAddressXInMacroXor - the byte step for x pixels along X,
// which is not x << bytes_per_block_log2 because the tiled layout interleaves.
constexpr uint32_t TiledAddressXInMacroXor(uint32_t x,
                                           uint32_t bytes_per_block_log2) {
  return CombineTiledAddress((x & 0x7) << bytes_per_block_log2, 0,
                             (x >> 3) & 0x3, 0);
}

// XeniaTextureResolutionScaledGroupBlocksLog2 in texture_address.xesli - the
// host blocks a resolution-scaled group holds, sized so runs along X stay at
// least as contiguous as guest tiling makes them.
constexpr uint32_t ResolutionScaledGroupBlocksXLog2(
    uint32_t bytes_per_block_log2) {
  return bytes_per_block_log2 >= 3 ? 5 - bytes_per_block_log2 : 4;
}
constexpr uint32_t ResolutionScaledGroupBlocksYLog2(
    uint32_t bytes_per_block_log2) {
  return 3 - (bytes_per_block_log2 < 2 ? bytes_per_block_log2 : 2);
}

}  // namespace

uint32_t EmitResolveRefreshDepthTexel(SpirvBuilder& builder, uint32_t packed,
                                      bool depth_float,
                                      uint32_t ext_inst_glsl_std_450) {
  if (depth_float) {
    return SpirvShaderTranslator::Depth20e4To32(builder, packed, 8, false,
                                                false, ext_inst_glsl_std_450);
  }
  auto type_uint = builder.makeUintType(32);
  auto type_float = builder.makeFloatType(32);
  auto depth = builder.createBinOp(spv::OpShiftRightLogical, type_uint, packed,
                                   builder.makeUintConstant(8));
  // Match XeUNorm24To32 exactly rather than dividing by 2^24 - 1.
  return builder.createBinOp(
      spv::OpFMul, type_float,
      builder.createUnaryOp(
          spv::OpConvertUToF, type_float,
          builder.createBinOp(
              spv::OpIAdd, type_uint, depth,
              builder.createBinOp(spv::OpShiftRightLogical, type_uint, depth,
                                  builder.makeUintConstant(23)))),
      builder.makeFloatConstant(1.0f / 16777216.0f));
}

std::vector<uint32_t> BuildEdramDumpShaderSpirv(
    EdramDumpShaderKey key, const EdramDumpShaderOptions& options) {
  bool draw_resolution_scaled =
      options.resolution_scale_x > 1 || options.resolution_scale_y > 1;

  std::vector<spv::Id> id_vector_temp;

  SpirvBuilder builder(options.spirv_version,
                       (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_int3 = builder.makeVectorType(type_int, 3);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint3 = builder.makeVectorType(type_uint, 3);
  spv::Id type_uint4 = builder.makeVectorType(type_uint, 4);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_bool = builder.makeBoolType();

  // Bindings.
  // Destination buffer - EDRAM in whole samples, or the resolve destination
  // in the 16-byte blocks a direct resolve's runs of pixels store as.
  bool format_is_64bpp = !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(
                                              key.GetColorFormat());
  bool copy_is_depth =
      bool(key.is_depth) !=
      bool(key.direct_resolve && key.direct_resolve_cross_depth_color);
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeRuntimeArray(
      key.direct_resolve ? type_uint4
                         : (format_is_64bpp ? type_uint2 : type_uint)));
  // Storage buffers have std430 packing, no padding to 4-component vectors.
  builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                        key.direct_resolve
                            ? sizeof(uint32_t) * 4
                            : (sizeof(uint32_t) << uint32_t(format_is_64bpp)));
  spv::Id type_dest = builder.makeStructType(
      id_vector_temp, key.direct_resolve ? "XeResolveDest" : "XeEdram");
  builder.addMemberName(type_dest, 0,
                        key.direct_resolve ? "resolve_dest" : "edram");
  builder.addMemberDecoration(type_dest, 0, spv::DecorationNonReadable);
  builder.addMemberDecoration(type_dest, 0, spv::DecorationOffset, 0);
  // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // BufferBlock.
  builder.addDecoration(type_dest, spv::DecorationBufferBlock);
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  spv::Id dest_buffer = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniform, type_dest,
      key.direct_resolve ? "xe_resolve_dest" : "xe_edram");
  builder.addDecoration(dest_buffer, spv::DecorationDescriptorSet,
                        options.descriptor_set_dest);
  builder.addDecoration(dest_buffer, spv::DecorationBinding, 0);
  // Color or depth source.
  bool source_is_multisampled = key.msaa_samples != xenos::MsaaSamples::k1X;
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    source_is_uint = options.source_is_uint;
  }
  // The optional resolve texture is binding 1 of the destination set, next to
  // the guest-memory destination buffer at binding 0.
  spv::Id resolve_texture = spv::NoResult;
  std::array<spv::Id, 3> resolve_texture_aliases;
  const uint32_t resolve_texture_alias_count =
      key.direct_resolve_texture ? key.direct_resolve_texture_aliases : 0;
  if (key.direct_resolve_texture) {
    spv::Id type_resolve_texture =
        copy_is_depth
            ? builder.makeImageType(type_float, spv::Dim2D, false, true, false,
                                    2, spv::ImageFormat::R32f)
            : builder.makeImageType(type_uint, spv::Dim2D, false, true, false,
                                    2,
                                    format_is_64bpp ? spv::ImageFormat::Rg32ui
                                                    : spv::ImageFormat::R32ui);
    static const char* const kResolveTextureNames[] = {
        "xe_resolve_texture", "xe_resolve_texture_alias_0",
        "xe_resolve_texture_alias_1", "xe_resolve_texture_alias_2"};
    for (uint32_t i = 0; i <= resolve_texture_alias_count; ++i) {
      spv::Id variable = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          type_resolve_texture, kResolveTextureNames[i]);
      builder.addDecoration(variable, spv::DecorationDescriptorSet,
                            options.descriptor_set_dest);
      builder.addDecoration(variable, spv::DecorationBinding, 1 + i);
      builder.addDecoration(variable, spv::DecorationNonReadable);
      if (i) {
        resolve_texture_aliases[i - 1] = variable;
      } else {
        resolve_texture = variable;
      }
    }
  }
  spv::Id source_component_type = source_is_uint ? type_uint : type_float;
  spv::Id source_texture = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      builder.makeImageType(source_component_type, spv::Dim2D, false, false,
                            source_is_multisampled, 1, spv::ImageFormatUnknown),
      "xe_edram_dump_source");
  builder.addDecoration(source_texture, spv::DecorationDescriptorSet,
                        options.descriptor_set_source);
  builder.addDecoration(source_texture, spv::DecorationBinding, 0);
  // Stencil source.
  spv::Id source_stencil_texture = spv::NoResult;
  if (key.is_depth) {
    source_stencil_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_uint, spv::Dim2D, false, false,
                              source_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_edram_dump_stencil");
    builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                          options.descriptor_set_source);
    builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
  }
  // Push constants. The direct resolve ones are declared but unreferenced in
  // the dump shaders, which costs nothing but keeps one layout for both.
  static const char* const kPushConstantNames[] = {
      "pitches",
      "offsets",
      "resolve_edram_info",
      "resolve_coordinate_info",
      "resolve_dest_info",
      "resolve_dest_coordinate_info",
      "resolve_dest_base",
      "resolve_height_div_8",
      "resolve_dispatch_tile",
      "resolve_texture_width",
      "resolve_texture_height",
  };
  static_assert(
      xe::countof(kPushConstantNames) == kEdramDumpShaderPushConstantCount,
      "Every push constant needs a name in the shader");
  id_vector_temp.clear();
  id_vector_temp.reserve(kEdramDumpShaderPushConstantCount);
  for (uint32_t i = 0; i < kEdramDumpShaderPushConstantCount; ++i) {
    id_vector_temp.push_back(type_uint);
  }
  spv::Id type_push_constants =
      builder.makeStructType(id_vector_temp, "XeEdramDumpPushConstants");
  for (uint32_t i = 0; i < kEdramDumpShaderPushConstantCount; ++i) {
    builder.addMemberName(type_push_constants, i, kPushConstantNames[i]);
    builder.addMemberDecoration(type_push_constants, i, spv::DecorationOffset,
                                int(sizeof(uint32_t) * i));
  }
  builder.addDecoration(type_push_constants, spv::DecorationBlock);
  spv::Id push_constants = builder.createVariable(
      spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
      "xe_edram_dump_push_constants");

  // gl_GlobalInvocationID input.
  spv::Id input_global_invocation_id =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                             type_uint3, "gl_GlobalInvocationID");
  builder.addDecoration(input_global_invocation_id, spv::DecorationBuiltIn,
                        static_cast<int>(spv::BuiltIn::GlobalInvocationId));

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function =
      builder.makeFunctionEntry(spv::NoPrecision, type_void, "main",
                                main_param_types, main_precisions, &main_entry);

  auto load_push_constant = [&](EdramDumpShaderPushConstant constant) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(int(constant)));
    return builder.createLoad(
        builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                  id_vector_temp),
        spv::NoPrecision);
  };
  auto extract = [&](spv::Id source, uint32_t offset, uint32_t count) {
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, source,
                               builder.makeUintConstant(offset),
                               builder.makeUintConstant(count));
  };
  auto shift_left = [&](spv::Id value, uint32_t amount) {
    return builder.createBinOp(spv::OpShiftLeftLogical, type_uint, value,
                               builder.makeUintConstant(amount));
  };
  auto shift_right = [&](spv::Id value, uint32_t amount) {
    return builder.createBinOp(spv::OpShiftRightLogical, type_uint, value,
                               builder.makeUintConstant(amount));
  };
  auto bitwise_and = [&](spv::Id value, uint32_t mask) {
    return builder.createBinOp(spv::OpBitwiseAnd, type_uint, value,
                               builder.makeUintConstant(mask));
  };
  auto bitwise_or = [&](spv::Id a, spv::Id b) {
    return builder.createBinOp(spv::OpBitwiseOr, type_uint, a, b);
  };
  auto add = [&](spv::Id a, spv::Id b) {
    return builder.createBinOp(spv::OpIAdd, type_uint, a, b);
  };
  auto multiply = [&](spv::Id a, spv::Id b) {
    return builder.createBinOp(spv::OpIMul, type_uint, a, b);
  };
  // The scale is a compile-time constant, so this and the group addressing it
  // feeds drop out entirely at 1x.
  auto scale_up = [&](spv::Id value, uint32_t scale) {
    return scale > 1 ? multiply(value, builder.makeUintConstant(scale)) : value;
  };
  auto divide_down = [&](spv::Id value, uint32_t scale) {
    return scale > 1 ? builder.createBinOp(spv::OpUDiv, type_uint, value,
                                           builder.makeUintConstant(scale))
                     : value;
  };
  auto equals = [&](spv::Id value, uint32_t constant) {
    return builder.createBinOp(spv::OpIEqual, type_bool, value,
                               builder.makeUintConstant(constant));
  };

  // For now, as the exact addressing in 64bpp render targets relatively to
  // 32bpp is unknown, treating 64bpp tiles as storing 40x16 samples rather than
  // 80x16 for simplicity of addressing into the texture.

  // Split the destination sample index into the 32bpp tile and the
  // 32bpp-tile-relative sample index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the sample position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  spv::Id global_invocation_id =
      builder.createLoad(input_global_invocation_id, spv::NoPrecision);

  // Loads one source sample and packs it into one or two 32-bit integers, in
  // the render target's own EDRAM bit layout. The only part the dump and the
  // direct resolve share - they address their sources and destinations
  // completely differently.
  auto load_and_pack = [&](spv::Id source_pixel_x, spv::Id source_pixel_y,
                           spv::Id source_sample_id, spv::Id(&packed)[2]) {
    spv::Builder::TextureParameters source_texture_parameters = {};
    source_texture_parameters.sampler =
        builder.createLoad(source_texture, spv::NoPrecision);
    id_vector_temp.clear();
    id_vector_temp.push_back(
        builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_x));
    id_vector_temp.push_back(
        builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_y));
    source_texture_parameters.coords =
        builder.createCompositeConstruct(type_int2, id_vector_temp);
    if (source_is_multisampled) {
      source_texture_parameters.sample =
          builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
    } else {
      source_texture_parameters.lod = builder.makeIntConstant(0);
    }
    spv::Id source_vec4 = builder.createTextureCall(
        spv::NoPrecision, builder.makeVectorType(source_component_type, 4),
        false, true, false, false, false, source_texture_parameters,
        spv::ImageOperandsMaskNone);
    if (key.is_depth) {
      source_texture_parameters.sampler =
          builder.createLoad(source_stencil_texture, spv::NoPrecision);
      spv::Id source_stencil = builder.createCompositeExtract(
          builder.createTextureCall(
              spv::NoPrecision, builder.makeVectorType(type_uint, 4), false,
              true, false, false, false, source_texture_parameters,
              spv::ImageOperandsMaskNone),
          type_uint, 0);
      spv::Id source_depth32 =
          builder.createCompositeExtract(source_vec4, type_float, 0);
      switch (key.GetDepthFormat()) {
        case xenos::DepthRenderTargetFormat::kD24S8: {
          // Round to the nearest even integer. This seems to be the correct
          // conversion, adding +0.5 and rounding towards zero results in red
          // instead of black in the 4D5307E6 clear shader.
          packed[0] = builder.createUnaryOp(
              spv::OpConvertFToU, type_uint,
              builder.createUnaryBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                  builder.createBinOp(
                      spv::OpFMul, type_float, source_depth32,
                      builder.makeFloatConstant(float(0xFFFFFF)))));
        } break;
        case xenos::DepthRenderTargetFormat::kD24FS8: {
          packed[0] = SpirvShaderTranslator::PreClampedDepthTo20e4(
              builder, source_depth32,
              !options.depth_float24_convert_in_pixel_shader &&
                  options.depth_float24_round,
              true, ext_inst_glsl_std_450);
        } break;
      }
      packed[0] = builder.createQuadOp(
          spv::OpBitFieldInsert, type_uint, source_stencil, packed[0],
          builder.makeUintConstant(8), builder.makeUintConstant(24));
    } else {
      switch (key.GetColorFormat()) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          // k_8_8_8_8_GAMMA is stored as linear in the unorm16 host render
          // target, so encode RGB linear -> gamma before packing (alpha stays
          // linear). Reaching the gamma resource format implies
          // gamma_render_target_as_unorm16.
          bool is_gamma = key.GetColorFormat() ==
                          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
          spv::Id source_red =
              builder.createCompositeExtract(source_vec4, type_float, 0);
          if (is_gamma) {
            source_red = SpirvShaderTranslator::LinearToPWLGamma(
                &builder, source_red, true, ext_inst_glsl_std_450);
          }
          packed[0] = builder.createUnaryOp(
              spv::OpConvertFToU, type_uint,
              builder.createBinOp(spv::OpFAdd, type_float,
                                  builder.createBinOp(spv::OpFMul, type_float,
                                                      source_red, unorm_scale),
                                  unorm_round_offset));
          spv::Id component_width = builder.makeUintConstant(8);
          for (uint32_t i = 1; i < 4; ++i) {
            spv::Id source_component =
                builder.createCompositeExtract(source_vec4, type_float, i);
            if (is_gamma && i < 3) {
              source_component = SpirvShaderTranslator::LinearToPWLGamma(
                  &builder, source_component, true, ext_inst_glsl_std_450);
            }
            packed[0] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[0],
                builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createBinOp(
                        spv::OpFAdd, type_float,
                        builder.createBinOp(spv::OpFMul, type_float,
                                            source_component, unorm_scale),
                        unorm_round_offset)),
                builder.makeUintConstant(8 * i), component_width);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
          packed[0] = builder.createUnaryOp(
              spv::OpConvertFToU, type_uint,
              builder.createBinOp(
                  spv::OpFAdd, type_float,
                  builder.createBinOp(spv::OpFMul, type_float,
                                      builder.createCompositeExtract(
                                          source_vec4, type_float, 0),
                                      unorm_scale_rgb),
                  unorm_round_offset));
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 1; i < 4; ++i) {
            packed[0] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[0],
                builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createBinOp(
                        spv::OpFAdd, type_float,
                        builder.createBinOp(
                            spv::OpFMul, type_float,
                            builder.createCompositeExtract(source_vec4,
                                                           type_float, i),
                            i == 3 ? unorm_scale_a : unorm_scale_rgb),
                        unorm_round_offset)),
                builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::
            k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          // Float16 has a wider range for both color and alpha, also NaNs -
          // clamp and convert.
          packed[0] = SpirvShaderTranslator::UnclampedFloat32To7e3(
              builder,
              builder.createCompositeExtract(source_vec4, type_float, 0),
              ext_inst_glsl_std_450);
          spv::Id width_rgb = builder.makeUintConstant(10);
          for (uint32_t i = 1; i < 3; ++i) {
            packed[0] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[0],
                SpirvShaderTranslator::UnclampedFloat32To7e3(
                    builder,
                    builder.createCompositeExtract(source_vec4, type_float, i),
                    ext_inst_glsl_std_450),
                builder.makeUintConstant(10 * i), width_rgb);
          }
          // Saturate and convert the alpha.
          spv::Id alpha_saturated = builder.createTriBuiltinCall(
              type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
              builder.createCompositeExtract(source_vec4, type_float, 3),
              builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(
                      spv::OpFAdd, type_float,
                      builder.createBinOp(spv::OpFMul, type_float,
                                          alpha_saturated,
                                          builder.makeFloatConstant(3.0f)),
                      builder.makeFloatConstant(0.5f))),
              builder.makeUintConstant(30), builder.makeUintConstant(2));
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
          spv::Id component_offset_width = builder.makeUintConstant(16);
          for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
            packed[i] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint,
                builder.createCompositeExtract(source_vec4, type_uint, 2 * i),
                builder.createCompositeExtract(source_vec4, type_uint,
                                               2 * i + 1),
                component_offset_width, component_offset_width);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
            spv::Id& packed_ref = packed[i];
            packed_ref = builder.createCompositeExtract(
                source_vec4, source_component_type, i);
            if (!source_is_uint) {
              packed_ref =
                  builder.createUnaryOp(spv::OpBitcast, type_uint, packed_ref);
            }
          }
        } break;
      }
    }
  };

  // Tile geometry and the per-dispatch constants, shared by both paths.
  // native_layout means the destination - the EDRAM buffer when dumping, the
  // resolve destination when resolving directly - is the plain 1x1 layout,
  // which is what a fully native resolve writes whatever the draw scale is.
  uint32_t layout_scale_x = key.native_layout ? 1 : options.resolution_scale_x;
  uint32_t layout_scale_y = key.native_layout ? 1 : options.resolution_scale_y;
  // Whether the destination is the group-packed resolution-scaled layout, the
  // host bytes of a guest block packed together after it.
  bool dest_scaled = !key.native_layout && draw_resolution_scaled;
  uint32_t tile_width =
      (xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) *
      layout_scale_x;
  uint32_t tile_height = xenos::kEdramTileHeightSamples * layout_scale_y;
  spv::Id const_tile_width = builder.makeUintConstant(tile_width);
  spv::Id const_tile_height = builder.makeUintConstant(tile_height);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_edram_pitch_tiles_bits =
      builder.makeUintConstant(xenos::kEdramPitchTilesBits);
  spv::Id const_edram_base_tiles_bits_plus_2 =
      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 2);
  spv::Id pitches_constant =
      load_push_constant(kEdramDumpShaderPushConstantPitches);
  spv::Id offsets_constant =
      load_push_constant(kEdramDumpShaderPushConstantOffsets);

  if (!key.direct_resolve) {
    spv::Id rectangle_sample_x =
        builder.createCompositeExtract(global_invocation_id, type_uint, 0);
    spv::Id rectangle_tile_index_x = builder.createBinOp(
        spv::OpUDiv, type_uint, rectangle_sample_x, const_tile_width);
    spv::Id tile_sample_x = builder.createBinOp(
        spv::OpUMod, type_uint, rectangle_sample_x, const_tile_width);
    spv::Id rectangle_sample_y =
        builder.createCompositeExtract(global_invocation_id, type_uint, 1);
    spv::Id rectangle_tile_index_y = builder.createBinOp(
        spv::OpUDiv, type_uint, rectangle_sample_y, const_tile_height);
    spv::Id tile_sample_y = builder.createBinOp(
        spv::OpUMod, type_uint, rectangle_sample_y, const_tile_height);

    // Get the tile index in the EDRAM relative to the dump rectangle base tile.
    spv::Id dest_pitch_tiles = builder.createTriOp(
        spv::OpBitFieldUExtract, type_uint, pitches_constant, const_uint_0,
        const_edram_pitch_tiles_bits);
    spv::Id rectangle_tile_index = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createBinOp(spv::OpIMul, type_uint, dest_pitch_tiles,
                            rectangle_tile_index_y),
        rectangle_tile_index_x);
    // Add the base tile in the dispatch to the dispatch-local tile index, not
    // wrapping yet so in case of a wraparound, the address relative to the base
    // in the image after subtraction of the base won't be negative.
    spv::Id edram_tile_index_non_wrapped = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                            offsets_constant, const_uint_0,
                            const_edram_base_tiles_bits_plus_2),
        rectangle_tile_index);

    // Combine the tile sample index and the tile index, wrapping the tile
    // addressing, into the EDRAM sample index. A direct resolve never touches
    // EDRAM - and the depth column swap below would only be undone again by the
    // resolve copy's own addressing, so it drops out entirely.
    spv::Id edram_sample_address = spv::NoResult;
    {
      edram_sample_address = builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_width * tile_height),
              builder.createBinOp(
                  spv::OpBitwiseAnd, type_uint, edram_tile_index_non_wrapped,
                  builder.makeUintConstant(xenos::kEdramTileCount - 1))),
          builder.createBinOp(
              spv::OpIAdd, type_uint,
              builder.createBinOp(spv::OpIMul, type_uint, const_tile_width,
                                  tile_sample_y),
              tile_sample_x));
      if (key.is_depth) {
        // Swap 40-sample columns in the depth buffer in the destination address
        // to get the final address of the sample in the EDRAM.
        uint32_t tile_width_half = tile_width >> 1;
        edram_sample_address = builder.createUnaryOp(
            spv::OpBitcast, type_uint,
            builder.createBinOp(
                spv::OpIAdd, type_int,
                builder.createUnaryOp(spv::OpBitcast, type_int,
                                      edram_sample_address),
                builder.createTriOp(
                    spv::OpSelect, type_int,
                    builder.createBinOp(
                        spv::OpULessThan, builder.makeBoolType(), tile_sample_x,
                        builder.makeUintConstant(tile_width_half)),
                    builder.makeIntConstant(int32_t(tile_width_half)),
                    builder.makeIntConstant(-int32_t(tile_width_half)))));
      }
    }

    // Get the linear tile index within the source texture.
    spv::Id source_tile_index = builder.createBinOp(
        spv::OpISub, type_uint, edram_tile_index_non_wrapped,
        builder.createTriOp(
            spv::OpBitFieldUExtract, type_uint, offsets_constant,
            const_edram_base_tiles_bits_plus_2,
            builder.makeUintConstant(xenos::kEdramBaseTilesBits)));
    // Split the linear tile index in the source texture into X and Y in tiles.
    spv::Id source_pitch_tiles = builder.createTriOp(
        spv::OpBitFieldUExtract, type_uint, pitches_constant,
        const_edram_pitch_tiles_bits, const_edram_pitch_tiles_bits);
    spv::Id source_tile_index_y = builder.createBinOp(
        spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
    spv::Id source_tile_index_x = builder.createBinOp(
        spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
    // Combine the source tile offset and the sample index within the tile.
    spv::Id source_sample_x = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createBinOp(spv::OpIMul, type_uint, const_tile_width,
                            source_tile_index_x),
        tile_sample_x);
    spv::Id source_sample_y = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createBinOp(spv::OpIMul, type_uint, const_tile_height,
                            source_tile_index_y),
        tile_sample_y);
    // Get the source pixel coordinate and the sample index within the pixel.
    spv::Id source_pixel_x = source_sample_x, source_pixel_y = source_sample_y;
    spv::Id source_sample_id = spv::NoResult;
    if (source_is_multisampled) {
      // source_sample_x/y hold the canonical sample coordinates within the
      // EDRAM layout. Convert them into the pixel and the sample of the
      // multisampled view using the canonical layout formulas, at guest pixel
      // granularity when the layout is scaled.
      bool layout_scaled = layout_scale_x > 1 || layout_scale_y > 1;
      spv::Id const_uint_1 = builder.makeUintConstant(1);
      spv::Id const_uint_2 = builder.makeUintConstant(2);
      spv::Id canonical_u = source_sample_x;
      spv::Id canonical_v = source_sample_y;
      spv::Id subpixel_x = spv::NoResult, subpixel_y = spv::NoResult;
      spv::Id const_layout_scale_x = spv::NoResult;
      spv::Id const_layout_scale_y = spv::NoResult;
      if (layout_scaled) {
        const_layout_scale_x = builder.makeUintConstant(layout_scale_x);
        const_layout_scale_y = builder.makeUintConstant(layout_scale_y);
        canonical_u = builder.createBinOp(
            spv::OpUDiv, type_uint, source_sample_x, const_layout_scale_x);
        subpixel_x = builder.createBinOp(spv::OpUMod, type_uint,
                                         source_sample_x, const_layout_scale_x);
        canonical_v = builder.createBinOp(
            spv::OpUDiv, type_uint, source_sample_y, const_layout_scale_y);
        subpixel_y = builder.createBinOp(spv::OpUMod, type_uint,
                                         source_sample_y, const_layout_scale_y);
      }
      if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
        // 4x MSAA source texture sample index - bit 0 for horizontal, bit 1
        // for vertical, same as the canonical layout.
        // sample = ((u >> 1) & 1) | (((v >> 1) & 1) << 1)
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createTriOp(spv::OpBitFieldUExtract, type_uint, canonical_u,
                                const_uint_1, const_uint_1),
            builder.createTriOp(spv::OpBitFieldUExtract, type_uint, canonical_v,
                                const_uint_1, const_uint_1),
            const_uint_1, const_uint_1);
        // Guest pixel per axis = ((c >> 2) << 1) | (c & 1).
        source_pixel_x = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, canonical_u,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                canonical_u, const_uint_2),
            const_uint_1, builder.makeUintConstant(31));
        source_pixel_y = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, canonical_v,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                canonical_v, const_uint_2),
            const_uint_1, builder.makeUintConstant(31));
      } else {
        // At 2x the guest sample sits in canonical U bit 1 and one guest pixel
        // X bit sits in canonical V bit 1. Convert the guest sample index to
        // the Vulkan standard sample order.
        source_sample_id = builder.createTriOp(
            spv::OpSelect, type_uint,
            builder.createBinOp(
                spv::OpINotEqual, type_bool,
                builder.createBinOp(spv::OpBitwiseAnd, type_uint, canonical_u,
                                    const_uint_2),
                const_uint_0),
            builder.makeUintConstant(
                draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                    1, options.msaa_2x_attachments_supported)),
            builder.makeUintConstant(
                draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                    0, options.msaa_2x_attachments_supported)));
        // Guest pixel X = (u & ~2) | (v & 2).
        source_pixel_x = builder.createBinOp(
            spv::OpBitwiseOr, type_uint,
            builder.createBinOp(spv::OpBitwiseAnd, type_uint, canonical_u,
                                builder.makeUintConstant(~uint32_t(2))),
            builder.createBinOp(spv::OpBitwiseAnd, type_uint, canonical_v,
                                const_uint_2));
        // Guest pixel Y = ((v & ~3) >> 1) | (v & 1).
        source_pixel_y = builder.createBinOp(
            spv::OpBitwiseOr, type_uint,
            builder.createBinOp(
                spv::OpShiftRightLogical, type_uint,
                builder.createBinOp(spv::OpBitwiseAnd, type_uint, canonical_v,
                                    builder.makeUintConstant(~uint32_t(3))),
                const_uint_1),
            builder.createBinOp(spv::OpBitwiseAnd, type_uint, canonical_v,
                                const_uint_1));
      }
      if (!key.source_scale_native && layout_scaled) {
        // Scaled source in the scaled layout. Restore the subpixel position.
        source_pixel_x = builder.createBinOp(
            spv::OpIAdd, type_uint,
            builder.createBinOp(spv::OpIMul, type_uint, source_pixel_x,
                                const_layout_scale_x),
            subpixel_x);
        source_pixel_y = builder.createBinOp(
            spv::OpIAdd, type_uint,
            builder.createBinOp(spv::OpIMul, type_uint, source_pixel_y,
                                const_layout_scale_y),
            subpixel_y);
      }
      // With a native source and a scaled layout, the guest pixel position
      // comes directly from the source texture position, and all the scaled
      // sample slots covering one guest sample receive its value.
    } else if (key.source_scale_native && dest_scaled) {
      // Native single sampled source dumped to the scaled EDRAM layout.
      // Duplicate each pixel into all the scaled sample slots covering it.
      source_pixel_x = builder.createBinOp(
          spv::OpUDiv, type_uint, source_pixel_x,
          builder.makeUintConstant(options.resolution_scale_x));
      source_pixel_y = builder.createBinOp(
          spv::OpUDiv, type_uint, source_pixel_y,
          builder.makeUintConstant(options.resolution_scale_y));
    }

    spv::Id packed[2] = {};
    load_and_pack(source_pixel_x, source_pixel_y, source_sample_id, packed);
    // Write the packed value to the EDRAM buffer.
    spv::Id store_value = packed[0];
    if (format_is_64bpp) {
      id_vector_temp.clear();
      id_vector_temp.push_back(packed[0]);
      id_vector_temp.push_back(packed[1]);
      store_value =
          builder.createCompositeConstruct(type_uint2, id_vector_temp);
    }
    id_vector_temp.clear();
    // The only SSBO structure member.
    id_vector_temp.push_back(builder.makeIntConstant(0));
    id_vector_temp.push_back(
        builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address));
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    builder.createStore(store_value,
                        builder.createAccessChain(spv::StorageClassUniform,
                                                  dest_buffer, id_vector_temp));
  } else {
    // One thread writes a run of destination pixels, the run being what the
    // guest texture layout stores contiguously - so the loads land on
    // consecutive source texels and the stores are whole 16-byte blocks,
    // rather than one scattered dword per invocation.
    uint32_t pixels_per_store =
        kEdramDumpShaderResolvePixelsPerStore >> uint32_t(format_is_64bpp);
    uint32_t pixels_per_thread = pixels_per_store * 2;

    spv::Id resolve_edram_info =
        load_push_constant(kEdramDumpShaderPushConstantResolveEdramInfo);
    spv::Id resolve_coordinate_info =
        load_push_constant(kEdramDumpShaderPushConstantResolveCoordinateInfo);
    spv::Id resolve_dest_info =
        load_push_constant(kEdramDumpShaderPushConstantResolveDestInfo);
    spv::Id resolve_dest_coordinate_info = load_push_constant(
        kEdramDumpShaderPushConstantResolveDestCoordinateInfo);

    // Where this thread's run of pixels is. The dispatch covers one tile
    // rectangle of one render target's ownership, so the tile it starts at
    // comes in rather than being divided back out of a linear index.
    uint32_t msaa_x_log2 =
        uint32_t(key.msaa_samples >= xenos::MsaaSamples::k4X);
    uint32_t msaa_y_log2 =
        uint32_t(key.msaa_samples >= xenos::MsaaSamples::k2X);
    uint32_t tile_pixels_x = tile_width >> msaa_x_log2;
    uint32_t tile_pixels_y = tile_height >> msaa_y_log2;
    spv::Id dispatch_tile =
        load_push_constant(kEdramDumpShaderPushConstantResolveDispatchTile);
    spv::Id run_x = multiply(
        builder.createCompositeExtract(global_invocation_id, type_uint, 0),
        builder.makeUintConstant(pixels_per_thread));
    spv::Id run_y =
        builder.createCompositeExtract(global_invocation_id, type_uint, 1);

    // The tile this run falls in, and its position within it. A run never
    // crosses a tile edge - the tile is a whole number of runs wide and the
    // dispatch starts on a tile boundary. Neither axis can be reduced to a
    // shift and a mask: tiles aren't a power of two pixels wide, and an odd
    // resolution scale makes them not a power of two tall either. Both counts
    // are compile-time constants, so this is a shift and a mask again wherever
    // it can be.
    spv::Id run_tile_x = builder.createBinOp(
        spv::OpUDiv, type_uint, run_x, builder.makeUintConstant(tile_pixels_x));
    spv::Id run_tile_y = builder.createBinOp(
        spv::OpUDiv, type_uint, run_y, builder.makeUintConstant(tile_pixels_y));
    spv::Id tile_x =
        add(extract(dispatch_tile, 0, xenos::kEdramPitchTilesBits), run_tile_x);
    spv::Id tile_y = add(extract(dispatch_tile, xenos::kEdramPitchTilesBits,
                                 xenos::kEdramPitchTilesBits),
                         run_tile_y);
    spv::Id pixel_in_tile_x = builder.createBinOp(
        spv::OpUMod, type_uint, run_x, builder.makeUintConstant(tile_pixels_x));
    spv::Id pixel_in_tile_y = builder.createBinOp(
        spv::OpUMod, type_uint, run_y, builder.makeUintConstant(tile_pixels_y));

    // The destination pixel, relative to the resolve rectangle. The first tile
    // starts before it when the rectangle is offset within the tile, and those
    // pixels wrap around to fail the width comparison.
    // dest_x/dest_y are host pixels because the tile geometry above is, so
    // every guest-pixel quantity they're compared or combined with is scaled.
    spv::Id resolve_offset_x =
        scale_up(shift_left(extract(resolve_coordinate_info,
                                    kResolveCoordinateInfoOffsetXShift, 4),
                            xenos::kResolveAlignmentPixelsLog2),
                 layout_scale_x);
    spv::Id resolve_offset_y =
        scale_up(shift_left(extract(resolve_coordinate_info,
                                    kResolveCoordinateInfoOffsetYShift, 1),
                            xenos::kResolveAlignmentPixelsLog2),
                 layout_scale_y);
    spv::Id dest_x = builder.createBinOp(
        spv::OpISub, type_uint,
        add(multiply(tile_x, builder.makeUintConstant(tile_pixels_x)),
            pixel_in_tile_x),
        resolve_offset_x);
    spv::Id dest_y = builder.createBinOp(
        spv::OpISub, type_uint,
        add(multiply(tile_y, builder.makeUintConstant(tile_pixels_y)),
            pixel_in_tile_y),
        resolve_offset_y);
    spv::Id write_condition = builder.createBinOp(
        spv::OpLogicalAnd, type_bool,
        builder.createBinOp(
            spv::OpULessThan, type_bool, dest_x,
            scale_up(shift_left(extract(resolve_coordinate_info,
                                        kResolveCoordinateInfoWidthShift,
                                        kResolveCoordinateInfoWidthBits),
                                xenos::kResolveAlignmentPixelsLog2),
                     layout_scale_x)),
        builder.createBinOp(
            spv::OpULessThan, type_bool, dest_y,
            scale_up(
                shift_left(load_push_constant(
                               kEdramDumpShaderPushConstantResolveHeightDiv8),
                           xenos::kResolveAlignmentPixelsLog2),
                layout_scale_y)));

    // The half-pixel offset fill hack, which the scaled copy shaders apply and
    // a direct resolve has to reproduce: the first surely covered host column
    // and row are stretched into the gap the D3D9 half-pixel offset leaves on
    // the left and top sides of the resolve area. Expressed as a bias on the
    // source position, since the destination position is what's covered - and
    // the band is at most half the scale wide, so only the first few pixels of
    // a run can fall in it.
    uint32_t fill_extent_x = dest_scaled ? layout_scale_x >> 1 : 0;
    uint32_t fill_extent_y = dest_scaled ? layout_scale_y >> 1 : 0;
    spv::Id fill_bias_y = spv::NoResult;
    std::vector<spv::Id> fill_bias_x(fill_extent_x);
    if (fill_extent_x || fill_extent_y) {
      spv::Id fill_enabled = builder.createBinOp(
          spv::OpINotEqual, type_bool,
          bitwise_and(resolve_edram_info,
                      uint32_t(1) << kResolveEdramInfoFillHalfPixelOffsetShift),
          const_uint_0);
      // How far past a destination coordinate the fill source sits, 0 outside
      // the band. Unwritten pixels wrap around to huge values, which take the
      // same zero branch as anything past it.
      auto fill_bias = [&](spv::Id dest_coordinate, uint32_t fill_extent) {
        spv::Id fill_offset = builder.createTriOp(
            spv::OpSelect, type_uint, fill_enabled,
            builder.makeUintConstant(fill_extent), const_uint_0);
        return builder.createTriOp(
            spv::OpSelect, type_uint,
            builder.createBinOp(spv::OpULessThan, type_bool, dest_coordinate,
                                fill_offset),
            builder.createBinOp(spv::OpISub, type_uint, fill_offset,
                                dest_coordinate),
            const_uint_0);
      };
      if (fill_extent_y) {
        fill_bias_y = fill_bias(dest_y, fill_extent_y);
      }
      for (uint32_t i = 0; i < fill_extent_x; ++i) {
        fill_bias_x[i] =
            fill_bias(i ? add(dest_x, builder.makeUintConstant(i)) : dest_x,
                      fill_extent_x);
      }
    }

    // The sample this pixel resolves from. Except for the keyed 4x average,
    // eligibility guarantees a single selected sample, so
    // XeResolveFirstSampleIndex is the raw field.
    spv::Id sample_select =
        extract(resolve_dest_coordinate_info,
                kResolveDestCoordinateInfoSampleSelectShift, 3);
    // The same tile within the source render target, which has its own base
    // and pitch. The dispatch's first tile comes in non-wrapped, so a tail
    // wrapped around the end of EDRAM or a render target in the next period
    // is addressed like in a dump.
    spv::Id source_tile_index = add(
        builder.createBinOp(
            spv::OpISub, type_uint,
            extract(offsets_constant, 0, xenos::kEdramBaseTilesBits + 2),
            extract(offsets_constant, xenos::kEdramBaseTilesBits + 2,
                    xenos::kEdramBaseTilesBits)),
        add(multiply(extract(pitches_constant, 0, xenos::kEdramPitchTilesBits),
                     run_tile_y),
            run_tile_x));
    spv::Id source_pitch_tiles = builder.createTriOp(
        spv::OpBitFieldUExtract, type_uint, pitches_constant,
        const_edram_pitch_tiles_bits, const_edram_pitch_tiles_bits);

    // The canonical sample rearrangement of XeEdramOffsetBytes only permutes
    // the samples within a tile, so the source pixel is the source tile's
    // origin plus the position within the tile, and the selected sample is the
    // same for every pixel of the run.
    spv::Id source_pixel_x = add(
        multiply(builder.makeUintConstant(tile_pixels_x),
                 builder.createBinOp(spv::OpUMod, type_uint, source_tile_index,
                                     source_pitch_tiles)),
        pixel_in_tile_x);
    spv::Id source_pixel_y = add(
        multiply(builder.makeUintConstant(tile_pixels_y),
                 builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index,
                                     source_pitch_tiles)),
        pixel_in_tile_y);
    if (fill_bias_y != spv::NoResult) {
      source_pixel_y = add(source_pixel_y, fill_bias_y);
    }
    // A native render target feeding a scaled destination has one texel per
    // guest pixel, so every host pixel covering it reads the same one. X is
    // divided per pixel of the run below, after the run offset is added.
    bool source_native_in_scaled_dest = key.source_scale_native && dest_scaled;
    if (source_native_in_scaled_dest) {
      source_pixel_y = divide_down(source_pixel_y, options.resolution_scale_y);
    }
    spv::Id source_sample_id = spv::NoResult;
    if (source_is_multisampled) {
      if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
        // The guest 4x sample numbering matches the host one, bit 0 horizontal
        // and bit 1 vertical, so the selected sample passes through.
        source_sample_id = bitwise_and(sample_select, 3);
      } else {
        // 2x MSAA source texture sample index - convert from the guest to the
        // Vulkan standard sample locations.
        source_sample_id = builder.createTriOp(
            spv::OpSelect, type_uint,
            builder.createBinOp(spv::OpINotEqual, type_bool,
                                bitwise_and(sample_select, 1), const_uint_0),
            builder.makeUintConstant(
                draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                    1, options.msaa_2x_attachments_supported)),
            builder.makeUintConstant(
                draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                    0, options.msaa_2x_attachments_supported)));
      }
    }

    auto load_average_4x_rgba8 = [&](spv::Id pixel_x, spv::Id pixel_y,
                                     spv::Id& packed_out) {
      spv::Id type_float4 = builder.makeVectorType(type_float, 4);
      spv::Id sum = spv::NoResult;
      for (uint32_t sample = 0; sample < 4; ++sample) {
        spv::Id value;
        if (key.GetColorFormat() != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
          // The resolve interprets EDRAM as RGBA8, but its owner may store a
          // different format. Match the dump-and-resolve round trip before
          // averaging, including gamma encoding and per-sample quantization.
          spv::Id packed[2] = {};
          load_and_pack(pixel_x, pixel_y, builder.makeIntConstant(sample),
                        packed);
          value = builder.createUnaryBuiltinCall(
              type_float4, ext_inst_glsl_std_450, GLSLstd450UnpackUnorm4x8,
              packed[0]);
        } else {
          spv::Builder::TextureParameters source_texture_parameters = {};
          source_texture_parameters.sampler =
              builder.createLoad(source_texture, spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createUnaryOp(spv::OpBitcast, type_int, pixel_x));
          id_vector_temp.push_back(
              builder.createUnaryOp(spv::OpBitcast, type_int, pixel_y));
          source_texture_parameters.coords =
              builder.createCompositeConstruct(type_int2, id_vector_temp);
          source_texture_parameters.sample = builder.makeIntConstant(sample);
          value = builder.createTextureCall(
              spv::NoPrecision, type_float4, false, true, false, false, false,
              source_texture_parameters, spv::ImageOperandsMaskNone);
        }
        sum = sum == spv::NoResult
                  ? value
                  : builder.createBinOp(spv::OpFAdd, type_float4, sum, value);
      }
      spv::Id average =
          builder.createBinOp(spv::OpVectorTimesScalar, type_float4, sum,
                              builder.makeFloatConstant(0.25f));
      spv::Id component_width = builder.makeUintConstant(8);
      packed_out = spv::NoResult;
      for (uint32_t component = 0; component < 4; ++component) {
        spv::Id packed_component = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(
                spv::OpFAdd, type_float,
                builder.createBinOp(spv::OpFMul, type_float,
                                    builder.createCompositeExtract(
                                        average, type_float, component),
                                    builder.makeFloatConstant(255.0f)),
                builder.makeFloatConstant(0.5f)));
        packed_out =
            component == 0
                ? packed_component
                : builder.createQuadOp(spv::OpBitFieldInsert, type_uint,
                                       packed_out, packed_component,
                                       builder.makeUintConstant(8 * component),
                                       component_width);
      }
    };

    SpirvBuilder::IfBuilder if_write(write_condition,
                                     spv::SelectionControlMaskNone, builder);

    // Dense dwords - one per pixel at 32bpp, two at 64bpp - so that four of
    // them are exactly one 16-byte store either way.
    uint32_t dwords_per_pixel = uint32_t(format_is_64bpp) + 1;
    std::vector<spv::Id> run_packed(size_t(pixels_per_thread) *
                                    dwords_per_pixel);
    for (uint32_t i = 0; i < pixels_per_thread; ++i) {
      spv::Id pixel_packed[2] = {};
      spv::Id pixel_x =
          i ? add(source_pixel_x, builder.makeUintConstant(i)) : source_pixel_x;
      if (key.direct_resolve_cross_depth_color) {
        // A run can cross the middle of a tile (notably at 4x MSAA), so
        // compensate per pixel rather than once at the start of the run.
        uint32_t half = tile_pixels_x >> 1;
        auto pixel_in_tile =
            i ? add(pixel_in_tile_x, builder.makeUintConstant(i))
              : pixel_in_tile_x;
        pixel_x = add(
            pixel_x,
            builder.createTriOp(
                spv::OpSelect, type_uint,
                builder.createBinOp(spv::OpULessThan, type_bool, pixel_in_tile,
                                    builder.makeUintConstant(half)),
                builder.makeUintConstant(half),
                builder.makeUintConstant(uint32_t(-int32_t(half)))));
      }
      if (i < fill_extent_x) {
        pixel_x = add(pixel_x, fill_bias_x[i]);
      }
      if (source_native_in_scaled_dest) {
        pixel_x = divide_down(pixel_x, options.resolution_scale_x);
      }
      if (key.direct_resolve_4x_average) {
        load_average_4x_rgba8(pixel_x, source_pixel_y, pixel_packed[0]);
      } else {
        load_and_pack(pixel_x, source_pixel_y, source_sample_id, pixel_packed);
      }
      for (uint32_t j = 0; j < dwords_per_pixel; ++j) {
        run_packed[i * dwords_per_pixel + j] = pixel_packed[j];
      }
    }

    // Everything the resolve copy would have done to these bits after reading
    // them back out of EDRAM, applied per pixel.

    // Red/blue swap, keyed on the resolve's format rather than the render
    // target's - the round trip packs in the render target's layout and swaps
    // in the resolve's, and the two differ when a surface is being aliased.
    spv::Id dest_swap = builder.createBinOp(
        spv::OpINotEqual, type_bool,
        bitwise_and(resolve_dest_info, uint32_t(1)
                                           << kResolveDestInfoSwapShift),
        const_uint_0);
    spv::Id edram_format =
        extract(resolve_edram_info, kResolveEdramInfoFormatShift,
                xenos::kRenderTargetFormatBits);
    auto format_is =
        [&](std::initializer_list<xenos::ColorRenderTargetFormat> formats) {
          spv::Id result = spv::NoResult;
          for (xenos::ColorRenderTargetFormat format : formats) {
            spv::Id is_format = equals(edram_format, uint32_t(format));
            result = result == spv::NoResult
                         ? is_format
                         : builder.createBinOp(spv::OpLogicalOr, type_bool,
                                               result, is_format);
          }
          return result;
        };
    spv::Id swap_8888 =
        format_is({xenos::ColorRenderTargetFormat::k_8_8_8_8,
                   xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA});
    spv::Id swap_2101010 = format_is(
        {xenos::ColorRenderTargetFormat::k_2_10_10_10,
         xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
         xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10,
         xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16});
    spv::Id swap_64bpp = builder.createBinOp(
        spv::OpLogicalAnd, type_bool, dest_swap,
        format_is({xenos::ColorRenderTargetFormat::k_16_16_16_16,
                   xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT}));

    // Endian swap. The fast copy shaders only ever call XeEndianSwap32 and
    // XeEndianSwap64, neither of which handles 8in128, so no invocation ever
    // needs a neighbour's dwords.
    spv::Id endian = extract(resolve_dest_info, 0, kResolveDestInfoEndianBits);
    spv::Id is_8in64 = equals(endian, uint32_t(xenos::Endian128::k8in64));
    if (format_is_64bpp) {
      // XeEndianSwap64 exchanges the dwords for 8in64, then swaps as 8in32.
      endian = builder.createTriOp(
          spv::OpSelect, type_uint, is_8in64,
          builder.makeUintConstant(uint32_t(xenos::Endian128::k8in32)), endian);
    }
    // XeEndianSwap32.
    spv::Id endian_is_8in32 =
        equals(endian, uint32_t(xenos::Endian128::k8in32));
    spv::Id swap_bytes_in_16 = builder.createBinOp(
        spv::OpLogicalOr, type_bool,
        equals(endian, uint32_t(xenos::Endian128::k8in16)), endian_is_8in32);
    spv::Id swap_halves_in_32 = builder.createBinOp(
        spv::OpLogicalOr, type_bool, endian_is_8in32,
        equals(endian, uint32_t(xenos::Endian128::k16in32)));

    spv::Id texture_width = spv::NoResult;
    spv::Id texture_height = spv::NoResult;
    spv::Id texture_y = spv::NoResult;
    spv::Id loaded_resolve_texture = spv::NoResult;
    std::array<spv::Id, 3> loaded_resolve_texture_aliases;
    if (key.direct_resolve_texture) {
      spv::Id texture_width_and_offset_y =
          load_push_constant(kEdramDumpShaderPushConstantResolveTextureWidth);
      texture_width = bitwise_and(texture_width_and_offset_y, 0xFFFF);
      texture_y = add(dest_y, shift_right(texture_width_and_offset_y, 16));
      texture_height =
          load_push_constant(kEdramDumpShaderPushConstantResolveTextureHeight);
      loaded_resolve_texture =
          builder.createLoad(resolve_texture, spv::NoPrecision);
      for (uint32_t i = 0; i < resolve_texture_alias_count; ++i) {
        loaded_resolve_texture_aliases[i] =
            builder.createLoad(resolve_texture_aliases[i], spv::NoPrecision);
      }
    }
    for (uint32_t i = 0; i < pixels_per_thread; ++i) {
      spv::Id* pixel = &run_packed[i * dwords_per_pixel];
      if (!format_is_64bpp) {
        // XeResolveSwapRedBlue_8_8_8_8 / XeResolveSwapRedBlue_2_10_10_10.
        spv::Id swapped_8888 =
            bitwise_or(bitwise_or(bitwise_and(pixel[0], ~uint32_t(0xFF00FF)),
                                  shift_left(bitwise_and(pixel[0], 0xFF), 16)),
                       bitwise_and(shift_right(pixel[0], 16), 0xFF));
        spv::Id swapped_2101010 =
            bitwise_or(bitwise_or(bitwise_and(pixel[0], ~uint32_t(0x3FF003FF)),
                                  shift_left(bitwise_and(pixel[0], 0x3FF), 20)),
                       bitwise_and(shift_right(pixel[0], 20), 0x3FF));
        pixel[0] = builder.createTriOp(
            spv::OpSelect, type_uint, dest_swap,
            builder.createTriOp(
                spv::OpSelect, type_uint, swap_8888, swapped_8888,
                builder.createTriOp(spv::OpSelect, type_uint, swap_2101010,
                                    swapped_2101010, pixel[0])),
            pixel[0]);
      } else {
        // XeResolveSwap4PixelsRedBlue64bpp - the low halves of the two
        // dwords.
        spv::Id swapped_0 = bitwise_or(bitwise_and(pixel[0], ~uint32_t(0xFFFF)),
                                       bitwise_and(pixel[1], 0xFFFF));
        spv::Id swapped_1 = bitwise_or(bitwise_and(pixel[1], ~uint32_t(0xFFFF)),
                                       bitwise_and(pixel[0], 0xFFFF));
        pixel[0] = builder.createTriOp(spv::OpSelect, type_uint, swap_64bpp,
                                       swapped_0, pixel[0]);
        pixel[1] = builder.createTriOp(spv::OpSelect, type_uint, swap_64bpp,
                                       swapped_1, pixel[1]);
      }
      if (key.direct_resolve_texture) {
        // The texture-cache image holds what the ordinary loader would copy
        // from the resolved memory: the swapped, not yet endian-transformed
        // texels, as raw dwords.
        spv::Id texture_x =
            i ? add(dest_x, builder.makeUintConstant(i)) : dest_x;
        spv::Id texture_write_condition =
            builder.createBinOp(spv::OpLogicalAnd, type_bool,
                                builder.createBinOp(spv::OpULessThan, type_bool,
                                                    texture_x, texture_width),
                                builder.createBinOp(spv::OpULessThan, type_bool,
                                                    texture_y, texture_height));
        SpirvBuilder::IfBuilder if_texture_write(
            texture_write_condition, spv::SelectionControlMaskNone, builder);
        spv::Id texel;
        if (copy_is_depth) {
          auto depth = EmitResolveRefreshDepthTexel(
              builder, pixel[0], key.direct_resolve_texture_depth_float,
              ext_inst_glsl_std_450);
          auto zero = builder.makeFloatConstant(0.0f);
          texel = builder.createCompositeConstruct(
              builder.makeVectorType(type_float, 4), {depth, zero, zero, zero});
        } else {
          id_vector_temp.clear();
          id_vector_temp.push_back(pixel[0]);
          id_vector_temp.push_back(format_is_64bpp ? pixel[1] : const_uint_0);
          id_vector_temp.push_back(const_uint_0);
          id_vector_temp.push_back(const_uint_0);
          texel = builder.createCompositeConstruct(type_uint4, id_vector_temp);
        }
        id_vector_temp.clear();
        id_vector_temp.push_back(
            builder.createUnaryOp(spv::OpBitcast, type_int, texture_x));
        id_vector_temp.push_back(
            builder.createUnaryOp(spv::OpBitcast, type_int, texture_y));
        id_vector_temp.push_back(builder.makeIntConstant(0));
        spv::Id coordinates =
            builder.createCompositeConstruct(type_int3, id_vector_temp);
        builder.createNoResultOp(spv::OpImageWrite,
                                 {loaded_resolve_texture, coordinates, texel});
        for (uint32_t j = 0; j < resolve_texture_alias_count; ++j) {
          builder.createNoResultOp(
              spv::OpImageWrite,
              {loaded_resolve_texture_aliases[j], coordinates, texel});
        }
        if_texture_write.makeEndIf();
      }
      if (format_is_64bpp) {
        // The 8in64 dword exchange, before the 32-bit swaps below.
        spv::Id exchanged_0 = builder.createTriOp(spv::OpSelect, type_uint,
                                                  is_8in64, pixel[1], pixel[0]);
        spv::Id exchanged_1 = builder.createTriOp(spv::OpSelect, type_uint,
                                                  is_8in64, pixel[0], pixel[1]);
        pixel[0] = exchanged_0;
        pixel[1] = exchanged_1;
      }
      for (uint32_t j = 0; j <= uint32_t(format_is_64bpp); ++j) {
        spv::Id value = pixel[j];
        value = builder.createTriOp(
            spv::OpSelect, type_uint, swap_bytes_in_16,
            bitwise_or(shift_left(bitwise_and(value, 0x00FF00FF), 8),
                       shift_right(bitwise_and(value, 0xFF00FF00), 8)),
            value);
        value = builder.createTriOp(
            spv::OpSelect, type_uint, swap_halves_in_32,
            bitwise_or(shift_left(value, 16), shift_right(value, 16)), value);
        pixel[j] = value;
      }
    }

    // XeResolveDestPixelAddress for the start of the run, then
    // XeResolveLocalXAddressXor to step to the second block - within a run,
    // consecutive pixels are consecutive in both layouts, but in the tiled one
    // the two halves are not adjacent.
    spv::Id tiled_x = add(
        dest_x,
        scale_up(shift_left(extract(resolve_dest_coordinate_info,
                                    kResolveDestCoordinateInfoOffsetXShift, 4),
                            xenos::kResolveAlignmentPixelsLog2),
                 layout_scale_x));
    spv::Id tiled_y = add(
        dest_y,
        scale_up(shift_left(extract(resolve_dest_coordinate_info,
                                    kResolveDestCoordinateInfoOffsetYShift, 4),
                            xenos::kResolveAlignmentPixelsLog2),
                 layout_scale_y));
    uint32_t bytes_per_block_log2 = 2 + uint32_t(format_is_64bpp);
    // XeniaTextureGetResolutionScaledAddressing. A scaled destination tiles
    // whole guest groups, and packs the host bytes covering a guest group
    // together after its tiled address - so the tiled maths below runs on the
    // guest group origin, and what's left of the position becomes a byte
    // offset within the group. The run of pixels a thread stores stays inside
    // one host group: it starts group-aligned and is no wider than the group.
    spv::Id host_byte_offset_in_guest_group = spv::NoResult;
    if (dest_scaled) {
      uint32_t group_blocks_x_log2 =
          ResolutionScaledGroupBlocksXLog2(bytes_per_block_log2);
      uint32_t group_blocks_y_log2 =
          ResolutionScaledGroupBlocksYLog2(bytes_per_block_log2);
      spv::Id host_group_x = shift_right(tiled_x, group_blocks_x_log2);
      spv::Id host_group_y = shift_right(tiled_y, group_blocks_y_log2);
      spv::Id guest_group_x = divide_down(host_group_x, layout_scale_x);
      spv::Id guest_group_y = divide_down(host_group_y, layout_scale_y);
      spv::Id host_group_in_guest_x =
          builder.createBinOp(spv::OpISub, type_uint, host_group_x,
                              scale_up(guest_group_x, layout_scale_x));
      spv::Id host_group_in_guest_y =
          builder.createBinOp(spv::OpISub, type_uint, host_group_y,
                              scale_up(guest_group_y, layout_scale_y));
      // Host groups are column-major within a guest group.
      spv::Id host_group_index =
          add(scale_up(host_group_in_guest_x, layout_scale_y),
              host_group_in_guest_y);
      uint32_t group_width_bytes_log2 =
          group_blocks_x_log2 + bytes_per_block_log2;
      uint32_t group_blocks_x_mask = (uint32_t(1) << group_blocks_x_log2) - 1;
      uint32_t group_blocks_y_mask = (uint32_t(1) << group_blocks_y_log2) - 1;
      host_byte_offset_in_guest_group = bitwise_or(
          bitwise_or(shift_left(host_group_index,
                                group_width_bytes_log2 + group_blocks_y_log2),
                     shift_left(bitwise_and(tiled_y, group_blocks_y_mask),
                                group_width_bytes_log2)),
          shift_left(bitwise_and(tiled_x, group_blocks_x_mask),
                     bytes_per_block_log2));
      tiled_x = shift_left(guest_group_x, group_blocks_x_log2);
      tiled_y = shift_left(guest_group_y, group_blocks_y_log2);
    }
    spv::Id dest_pitch_macro_tiles = extract(
        resolve_dest_coordinate_info, 0, kResolveDestCoordinateInfoPitchBits);
    // XenosTextureTiledAddressCombine.
    auto combine = [&](spv::Id outer_inner_bytes, spv::Id bank, spv::Id pipe,
                       spv::Id y_lsb) {
      spv::Id address =
          bitwise_or(bitwise_or(shift_left(y_lsb, 4), shift_left(pipe, 6)),
                     shift_left(bank, 11));
      address = bitwise_or(address, bitwise_and(outer_inner_bytes, 0xF));
      address = bitwise_or(
          address,
          shift_left(bitwise_and(shift_right(outer_inner_bytes, 4), 0x1), 5));
      address = bitwise_or(
          address,
          shift_left(bitwise_and(shift_right(outer_inner_bytes, 5), 0x7), 8));
      return bitwise_or(address,
                        shift_left(shift_right(outer_inner_bytes, 8), 12));
    };
    // XenosTextureTiledAddress2D.
    spv::Id outer_blocks_2d = shift_left(
        add(multiply(shift_right(tiled_y, 5), dest_pitch_macro_tiles),
            shift_right(tiled_x, 5)),
        6);
    spv::Id inner_blocks_2d =
        bitwise_or(shift_left(bitwise_and(shift_right(tiled_y, 1), 0x7), 3),
                   bitwise_and(tiled_x, 0x7));
    spv::Id outer_inner_bytes_2d = shift_left(
        bitwise_or(outer_blocks_2d, inner_blocks_2d), bytes_per_block_log2);
    spv::Id address_2d =
        combine(outer_inner_bytes_2d, bitwise_and(shift_right(tiled_y, 4), 0x1),
                builder.createBinOp(
                    spv::OpBitwiseXor, type_uint,
                    bitwise_and(shift_right(tiled_x, 3), 0x3),
                    shift_left(bitwise_and(shift_right(tiled_y, 3), 0x1), 1)),
                bitwise_and(tiled_y, 1));
    // XenosTextureTiledAddress3D, for array destinations.
    spv::Id dest_slice =
        extract(resolve_dest_info, kResolveDestInfoSliceShift, 3);
    spv::Id dest_slice_pitch_macro_tiles = shift_left(
        extract(resolve_dest_coordinate_info,
                kResolveDestCoordinateInfoHeightShift,
                kResolveDestCoordinateInfoPitchBits),
        xenos::kTextureTileWidthHeightLog2 - kTextureMacroTileHeight3DLog2);
    spv::Id outer_blocks_3d = shift_left(
        add(multiply(add(multiply(shift_right(dest_slice, 2),
                                  dest_slice_pitch_macro_tiles),
                         shift_right(tiled_y, kTextureMacroTileHeight3DLog2)),
                     dest_pitch_macro_tiles),
            shift_right(tiled_x, 5)),
        7);
    spv::Id inner_blocks_3d = bitwise_or(
        bitwise_or(shift_left(bitwise_and(dest_slice, 0x3), 5),
                   shift_left(bitwise_and(shift_right(tiled_y, 1), 0x3), 3)),
        bitwise_and(tiled_x, 0x7));
    spv::Id outer_inner_bytes_3d = shift_left(
        bitwise_or(outer_blocks_3d, inner_blocks_3d), bytes_per_block_log2);
    spv::Id bank_3d =
        bitwise_and(builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                        shift_right(tiled_y, 3),
                                        shift_right(dest_slice, 2)),
                    0x1);
    spv::Id address_3d =
        combine(outer_inner_bytes_3d, bank_3d,
                builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                    bitwise_and(shift_right(tiled_x, 3), 0x3),
                                    shift_left(bank_3d, 1)),
                bitwise_and(tiled_y, 1));
    spv::Id dest_address = builder.createTriOp(
        spv::OpSelect, type_uint,
        builder.createBinOp(
            spv::OpINotEqual, type_bool,
            bitwise_and(resolve_dest_info, uint32_t(1)
                                               << kResolveDestInfoIsArrayShift),
            const_uint_0),
        address_3d, address_2d);
    if (dest_scaled) {
      dest_address =
          add(scale_up(dest_address, layout_scale_x * layout_scale_y),
              host_byte_offset_in_guest_group);
    } else {
      dest_address =
          add(dest_address,
              load_push_constant(kEdramDumpShaderPushConstantResolveDestBase));
    }

    for (uint32_t block = 0; block < 2; ++block) {
      spv::Id block_address =
          block ? add(dest_address,
                      builder.makeUintConstant(
                          dest_scaled
                              ? pixels_per_store << bytes_per_block_log2
                              : TiledAddressXInMacroXor(pixels_per_store,
                                                        bytes_per_block_log2)))
                : dest_address;
      spv::Id block_index = shift_right(block_address, 4);
      // Four dwords either way - four 32bpp pixels, or two 64bpp ones.
      uint32_t first_dword = block * pixels_per_store * dwords_per_pixel;
      id_vector_temp.clear();
      for (uint32_t i = 0; i < 4; ++i) {
        id_vector_temp.push_back(run_packed[first_dword + i]);
      }
      spv::Id block_value =
          builder.createCompositeConstruct(type_uint4, id_vector_temp);
      id_vector_temp.clear();
      // The only SSBO structure member.
      id_vector_temp.push_back(builder.makeIntConstant(0));
      id_vector_temp.push_back(
          builder.createUnaryOp(spv::OpBitcast, type_int, block_index));
      builder.createStore(
          block_value, builder.createAccessChain(spv::StorageClassUniform,
                                                 dest_buffer, id_vector_temp));
    }
    if_write.makeEndIf();
  }

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(
      main_function, spv::ExecutionModeLocalSize,
      key.direct_resolve ? kEdramDumpShaderResolveThreadsPerGroupX
                         : kEdramDumpShaderSamplesPerGroupX,
      key.direct_resolve ? kEdramDumpShaderResolveThreadsPerGroupY
                         : kEdramDumpShaderSamplesPerGroupY,
      1);
  spv::Instruction* entry_point = builder.addEntryPoint(
      spv::ExecutionModelGLCompute, main_function, "main");
  // Bindings only need to be added to the entry point's interface starting
  // with SPIR-V 1.4 - emitting 1.0 here, so only inputs / outputs.
  entry_point->addIdOperand(input_global_invocation_id);

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);
  return std::vector<uint32_t>(shader_code.begin(), shader_code.end());
}

}  // namespace gpu
}  // namespace xe
