/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_pipeline_cache.h"

#include <dispatch/dispatch.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/metal-cpp/Foundation/NSProcessInfo.hpp"
#include "third_party/metal-cpp/Foundation/NSURL.hpp"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_shader_cache.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"

#include "xenia/gpu/metal/metal_shader_converter.h"
using BYTE = uint8_t;
#include "xenia/gpu/metal/d3d12_5_1_bytecode/adaptive_quad_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/adaptive_triangle_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/continuous_quad_1cp_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/continuous_quad_4cp_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/continuous_triangle_1cp_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/continuous_triangle_3cp_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/discrete_quad_1cp_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/discrete_quad_4cp_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/discrete_triangle_1cp_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/discrete_triangle_3cp_hs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/tessellation_adaptive_vs.h"
#include "xenia/gpu/metal/d3d12_5_1_bytecode/tessellation_indexed_vs.h"
// Metal IR Converter Runtime - defines IRDescriptorTableEntry and bind points.
#ifndef IR_RUNTIME_METALCPP
#define IR_RUNTIME_METALCPP
#endif
#include "third_party/metal-shader-converter/include/metal_irconverter_runtime.h"

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

DECLARE_bool(async_shader_compilation);
DECLARE_bool(depth_float24_convert_in_pixel_shader);
DECLARE_bool(depth_float24_round);

DEFINE_int32(metal_pipeline_creation_threads, -1,
             "Number of threads for background pipeline compilation. "
             "-1 = auto (75% of cores), 0 = disabled (synchronous).",
             "Metal");

DEFINE_bool(metal_native_msl_guest_shaders, false,
            "Experimental: translate guest shaders directly to native Metal "
            "Shading Language. This is compile/dump-only by default.",
            "Metal");

DEFINE_bool(metal_native_msl_compile_only, true,
            "When native guest MSL is enabled, compile native MSL for "
            "diagnostics but keep rendering on the DXIL/MSC path.",
            "Metal");

DEFINE_bool(metal_native_msl_render, false,
            "Experimental: use native guest MSL for standard vertex/pixel "
            "render pipelines instead of DXIL/MSC. Geometry and tessellation "
            "emulation remain on the existing path.",
            "Metal");

DEFINE_bool(metal_native_msl_fallback_to_msc, true,
            "If native guest MSL translation/compilation fails, keep using "
            "the DXIL/MSC path. Disable to make native MSL misses fail loudly.",
            "Metal");

DEFINE_bool(metal_native_msl_helper_msc, true,
            "Temporary native-MSL bring-up path: keep geometry/tessellation "
            "helper-stage pipelines on the existing DXIL/Metal Shader "
            "Converter emulation path while standard guest VS/PS render "
            "pipelines use native MSL.",
            "Metal");

DEFINE_bool(metal_native_msl_expand_rectangle_lists, true,
            "Expand Xenos rectangle lists in native MSL vertex shaders instead "
            "of routing them through the temporary geometry helper path. This "
            "must stay enabled for native MSL on systems where DXBC to DXIL "
            "conversion is unavailable.",
            "Metal");

namespace xe {
namespace gpu {
namespace metal {

namespace {

void AtomicMax(std::atomic<uint64_t>& target, uint64_t value) {
  uint64_t current = target.load(std::memory_order_relaxed);
  while (current < value && !target.compare_exchange_weak(
                                current, value, std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
  }
}

bool NativeGuestMslRenderEnabled() {
  return cvars::metal_native_msl_render;
}

uint64_t NativeMslTranslationCacheKey(uint64_t shader_modification,
                                      uint64_t texture_sign_key) {
  if (!texture_sign_key) {
    return shader_modification;
  }
  uint64_t key_data[] = {
      shader_modification,
      texture_sign_key,
      UINT64_C(0x4E4D534C53676E31),  // "NMSLSgn1"
  };
  uint64_t key = XXH3_64bits(key_data, sizeof(key_data));
  return key ? key : UINT64_C(0x4E4D534C53676E31);
}

DxbcShader::TextureSignComponentMasks NativeMslTextureSignMasksFromStorage(
    const uint8_t* stored_masks) {
  DxbcShader::TextureSignComponentMasks masks = {};
  std::memcpy(masks.data(), stored_masks, masks.size());
  return masks;
}

bool NativePrimitiveMeshPipelineKind(
    MetalPipelineCache::PipelineKind kind) {
  switch (kind) {
    case MetalPipelineCache::PipelineKind::kNativeRectangleMesh:
    case MetalPipelineCache::PipelineKind::kNativePointMesh:
    case MetalPipelineCache::PipelineKind::kNativeQuadMesh:
      return true;
    default:
      return false;
  }
}

MetalPipelineCache::PipelineKind NativePrimitiveMeshPipelineKindForType(
    MetalPipelineCache::NativeMeshPipelineType type) {
  switch (type) {
    case MetalPipelineCache::NativeMeshPipelineType::kPointList:
      return MetalPipelineCache::PipelineKind::kNativePointMesh;
    case MetalPipelineCache::NativeMeshPipelineType::kQuadList:
      return MetalPipelineCache::PipelineKind::kNativeQuadMesh;
    case MetalPipelineCache::NativeMeshPipelineType::kRectangleList:
    default:
      return MetalPipelineCache::PipelineKind::kNativeRectangleMesh;
  }
}

MetalPipelineCache::NativeMeshPipelineType NativePrimitiveMeshTypeForKind(
    MetalPipelineCache::PipelineKind kind) {
  switch (kind) {
    case MetalPipelineCache::PipelineKind::kNativePointMesh:
      return MetalPipelineCache::NativeMeshPipelineType::kPointList;
    case MetalPipelineCache::PipelineKind::kNativeQuadMesh:
      return MetalPipelineCache::NativeMeshPipelineType::kQuadList;
    case MetalPipelineCache::PipelineKind::kNativeRectangleMesh:
    default:
      return MetalPipelineCache::NativeMeshPipelineType::kRectangleList;
  }
}

const char* NativePrimitiveMeshEntryPoint(
    MetalPipelineCache::NativeMeshPipelineType type) {
  switch (type) {
    case MetalPipelineCache::NativeMeshPipelineType::kPointList:
      return "main_point_mesh";
    case MetalPipelineCache::NativeMeshPipelineType::kQuadList:
      return "main_quad_mesh";
    case MetalPipelineCache::NativeMeshPipelineType::kRectangleList:
    default:
      return "main_rect_mesh";
  }
}

const char* NativePrimitiveMeshLabel(
    MetalPipelineCache::NativeMeshPipelineType type) {
  switch (type) {
    case MetalPipelineCache::NativeMeshPipelineType::kPointList:
      return "point-list";
    case MetalPipelineCache::NativeMeshPipelineType::kQuadList:
      return "quad-list";
    case MetalPipelineCache::NativeMeshPipelineType::kRectangleList:
    default:
      return "rectangle-list";
  }
}

uint64_t ElapsedMs(std::chrono::steady_clock::time_point start,
                   std::chrono::steady_clock::time_point end) {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
          .count());
}

void LogMetalErrorDetails(const char* label, NS::Error* error) {
  if (!error) {
    return;
  }
  const char* desc = error->localizedDescription()
                         ? error->localizedDescription()->utf8String()
                         : nullptr;
  const char* failure = error->localizedFailureReason()
                            ? error->localizedFailureReason()->utf8String()
                            : nullptr;
  const char* recovery =
      error->localizedRecoverySuggestion()
          ? error->localizedRecoverySuggestion()->utf8String()
          : nullptr;
  const char* domain =
      error->domain() ? error->domain()->utf8String() : nullptr;
  int64_t code = error->code();
  XELOGE("{}: domain={} code={} desc='{}' failure='{}' recovery='{}'", label,
         domain ? domain : "<null>", code, desc ? desc : "<null>",
         failure ? failure : "<null>", recovery ? recovery : "<null>");
  NS::Dictionary* user_info = error->userInfo();
  if (user_info) {
    auto* info_desc = user_info->description();
    XELOGE("{}: userInfo={}", label,
           info_desc ? info_desc->utf8String() : "<null>");
  }
}

void EnsureDepthFormatForDepthWritingFragment(const char* pipeline_name,
                                              bool fragment_writes_depth,
                                              MTL::PixelFormat* depth_format) {
  if (!fragment_writes_depth || !depth_format ||
      *depth_format != MTL::PixelFormatInvalid) {
    return;
  }
  *depth_format = MTL::PixelFormatDepth32Float;
  static bool logged = false;
  if (!logged) {
    logged = true;
    XELOGW(
        "{}: fragment writes depth without a bound depth attachment; "
        "using Depth32Float pipeline fallback",
        pipeline_name);
  }
}

bool ShaderUsesVertexFetch(const Shader& shader) {
  if (!shader.vertex_bindings().empty()) {
    return true;
  }
  const Shader::ConstantRegisterMap& constant_map =
      shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    if (constant_map.vertex_fetch_bitmap[i] != 0) {
      return true;
    }
  }
  return false;
}

MTL::CompareFunction ToMetalCompareFunction(xenos::CompareFunction compare) {
  static const MTL::CompareFunction kCompareMap[8] = {
      MTL::CompareFunctionNever,         // 0
      MTL::CompareFunctionLess,          // 1
      MTL::CompareFunctionEqual,         // 2
      MTL::CompareFunctionLessEqual,     // 3
      MTL::CompareFunctionGreater,       // 4
      MTL::CompareFunctionNotEqual,      // 5
      MTL::CompareFunctionGreaterEqual,  // 6
      MTL::CompareFunctionAlways,        // 7
  };
  return kCompareMap[uint32_t(compare) & 0x7];
}

MTL::StencilOperation ToMetalStencilOperation(xenos::StencilOp op) {
  static const MTL::StencilOperation kStencilOpMap[8] = {
      MTL::StencilOperationKeep,            // 0
      MTL::StencilOperationZero,            // 1
      MTL::StencilOperationReplace,         // 2
      MTL::StencilOperationIncrementClamp,  // 3
      MTL::StencilOperationDecrementClamp,  // 4
      MTL::StencilOperationInvert,          // 5
      MTL::StencilOperationIncrementWrap,   // 6
      MTL::StencilOperationDecrementWrap,   // 7
  };
  return kStencilOpMap[uint32_t(op) & 0x7];
}

MTL::ColorWriteMask ToMetalColorWriteMask(uint32_t write_mask) {
  MTL::ColorWriteMask mtl_mask = MTL::ColorWriteMaskNone;
  if (write_mask & 0x1) {
    mtl_mask |= MTL::ColorWriteMaskRed;
  }
  if (write_mask & 0x2) {
    mtl_mask |= MTL::ColorWriteMaskGreen;
  }
  if (write_mask & 0x4) {
    mtl_mask |= MTL::ColorWriteMaskBlue;
  }
  if (write_mask & 0x8) {
    mtl_mask |= MTL::ColorWriteMaskAlpha;
  }
  return mtl_mask;
}

MTL::BlendOperation ToMetalBlendOperation(xenos::BlendOp blend_op) {
  static const MTL::BlendOperation kBlendOpMap[8] = {
      MTL::BlendOperationAdd,              // 0
      MTL::BlendOperationSubtract,         // 1
      MTL::BlendOperationMin,              // 2
      MTL::BlendOperationMax,              // 3
      MTL::BlendOperationReverseSubtract,  // 4
      MTL::BlendOperationAdd,              // 5
      MTL::BlendOperationAdd,              // 6
      MTL::BlendOperationAdd,              // 7
  };
  return kBlendOpMap[uint32_t(blend_op) & 0x7];
}

MTL::BlendFactor ToMetalBlendFactorRgb(xenos::BlendFactor blend_factor) {
  static const MTL::BlendFactor kBlendFactorMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,  // ?
      /*  3 */ MTL::BlendFactorZero,  // ?
      /*  4 */ MTL::BlendFactorSourceColor,
      /*  5 */ MTL::BlendFactorOneMinusSourceColor,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationColor,
      /*  9 */ MTL::BlendFactorOneMinusDestinationColor,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendColor,  // CONSTANT_COLOR
      /* 13 */ MTL::BlendFactorOneMinusBlendColor,
      /* 14 */ MTL::BlendFactorBlendAlpha,  // CONSTANT_ALPHA
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorMap[uint32_t(blend_factor) & 0x1F];
}

MTL::BlendFactor ToMetalBlendFactorAlpha(xenos::BlendFactor blend_factor) {
  static const MTL::BlendFactor kBlendFactorAlphaMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,  // ?
      /*  3 */ MTL::BlendFactorZero,  // ?
      /*  4 */ MTL::BlendFactorSourceAlpha,
      /*  5 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationAlpha,
      /*  9 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendAlpha,
      /* 13 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 14 */ MTL::BlendFactorBlendAlpha,
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorAlphaMap[uint32_t(blend_factor) & 0x1F];
}

void ApplyBlendStateToDescriptor(
    MTL::RenderPipelineColorAttachmentDescriptorArray* color_attachments,
    uint32_t normalized_color_mask, const uint32_t blendcontrol[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = color_attachments->object(i);
    if (color_attachment->pixelFormat() == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    uint32_t rt_write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    reg::RB_BLENDCONTROL bc = {};
    bc.value = blendcontrol[i];
    MTL::BlendFactor src_rgb = ToMetalBlendFactorRgb(bc.color_srcblend);
    MTL::BlendFactor dst_rgb = ToMetalBlendFactorRgb(bc.color_destblend);
    MTL::BlendOperation op_rgb = ToMetalBlendOperation(bc.color_comb_fcn);
    MTL::BlendFactor src_alpha = ToMetalBlendFactorAlpha(bc.alpha_srcblend);
    MTL::BlendFactor dst_alpha = ToMetalBlendFactorAlpha(bc.alpha_destblend);
    MTL::BlendOperation op_alpha = ToMetalBlendOperation(bc.alpha_comb_fcn);

    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    color_attachment->setBlendingEnabled(blending_enabled);
    if (blending_enabled) {
      color_attachment->setSourceRGBBlendFactor(src_rgb);
      color_attachment->setDestinationRGBBlendFactor(dst_rgb);
      color_attachment->setRgbBlendOperation(op_rgb);
      color_attachment->setSourceAlphaBlendFactor(src_alpha);
      color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
      color_attachment->setAlphaBlendOperation(op_alpha);
    }
  }
}

IRFormat MapVertexFormatToIRFormat(
    const ParsedVertexFetchInstruction::Attributes& attrs) {
  using xenos::VertexFormat;
  switch (attrs.data_format) {
    case VertexFormat::k_8_8_8_8:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR8G8B8A8Sint : IRFormatR8G8B8A8Uint;
      }
      return attrs.is_signed ? IRFormatR8G8B8A8Snorm : IRFormatR8G8B8A8Unorm;
    case VertexFormat::k_2_10_10_10:
      if (attrs.is_integer) {
        return IRFormatR10G10B10A2Uint;
      }
      return IRFormatR10G10B10A2Unorm;
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
      return IRFormatR11G11B10Float;
    case VertexFormat::k_16_16:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR16G16Sint : IRFormatR16G16Uint;
      }
      return attrs.is_signed ? IRFormatR16G16Snorm : IRFormatR16G16Unorm;
    case VertexFormat::k_16_16_16_16:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR16G16B16A16Sint
                               : IRFormatR16G16B16A16Uint;
      }
      return attrs.is_signed ? IRFormatR16G16B16A16Snorm
                             : IRFormatR16G16B16A16Unorm;
    case VertexFormat::k_16_16_FLOAT:
      return IRFormatR16G16Float;
    case VertexFormat::k_16_16_16_16_FLOAT:
      return IRFormatR16G16B16A16Float;
    case VertexFormat::k_32:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR32Sint : IRFormatR32Uint;
      }
      return IRFormatR32Float;
    case VertexFormat::k_32_32:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR32G32Sint : IRFormatR32G32Uint;
      }
      return IRFormatR32G32Float;
    case VertexFormat::k_32_FLOAT:
      return IRFormatR32Float;
    case VertexFormat::k_32_32_FLOAT:
      return IRFormatR32G32Float;
    case VertexFormat::k_32_32_32_FLOAT:
      return IRFormatR32G32B32Float;
    case VertexFormat::k_32_32_32_32:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR32G32B32A32Sint
                               : IRFormatR32G32B32A32Uint;
      }
      return IRFormatR32G32B32A32Float;
    case VertexFormat::k_32_32_32_32_FLOAT:
      return IRFormatR32G32B32A32Float;
    default:
      return IRFormatUnknown;
  }
}

IRInputTopology GetGeometryStageInputTopology(GeometryShaderKey key) {
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      return IRInputTopologyPoint;
    case PipelineGeometryShader::kRectangleList:
      return IRInputTopologyTriangle;
    case PipelineGeometryShader::kQuadList:
    default:
      return IRInputTopologyUndefined;
  }
}

struct StageInInputAttribute {
  uint32_t input_slot = 0;
  uint32_t offset = 0;
  IRFormat format = IRFormatUnknown;
};

struct StageInInputLayout {
  IRVersionedInputLayoutDescriptor descriptor = {};
  std::vector<std::string> semantic_names_storage;
};

StageInInputLayout BuildStageInInputLayout(
    const Shader& vertex_shader,
    const MetalShaderReflectionInfo& vertex_reflection,
    const char* log_prefix) {
  std::vector<StageInInputAttribute> attribute_map;
  attribute_map.reserve(32);

  uint32_t attr_index = 0;
  for (const auto& binding : vertex_shader.vertex_bindings()) {
    for (const auto& attr : binding.attributes) {
      if (attr_index >= 31) {
        break;
      }
      StageInInputAttribute mapped = {};
      mapped.input_slot = static_cast<uint32_t>(binding.binding_index);
      mapped.offset =
          static_cast<uint32_t>(attr.fetch_instr.attributes.offset * 4);
      mapped.format = MapVertexFormatToIRFormat(attr.fetch_instr.attributes);
      attribute_map.push_back(mapped);
      ++attr_index;
    }
    if (attr_index >= 31) {
      break;
    }
  }

  StageInInputLayout input_layout = {};
  input_layout.descriptor.version = IRInputLayoutDescriptorVersion_1;
  input_layout.descriptor.desc_1_0.numElements = 0;
  if (vertex_reflection.vertex_inputs.empty()) {
    return input_layout;
  }

  input_layout.semantic_names_storage.reserve(
      vertex_reflection.vertex_inputs.size());
  uint32_t element_count = 0;
  for (const auto& input : vertex_reflection.vertex_inputs) {
    if (element_count >= 31) {
      break;
    }
    if (input.attribute_index >= attribute_map.size()) {
      XELOGW("{}: vertex input {} out of range (max {})", log_prefix,
             input.attribute_index, attribute_map.size());
      continue;
    }
    const StageInInputAttribute& mapped = attribute_map[input.attribute_index];
    if (mapped.format == IRFormatUnknown) {
      XELOGW("{}: unknown IRFormat for vertex input {}", log_prefix,
             input.attribute_index);
      continue;
    }

    std::string semantic_base = input.name;
    uint32_t semantic_index = 0;
    if (!semantic_base.empty()) {
      size_t digit_pos = semantic_base.size();
      while (digit_pos > 0 && std::isdigit(static_cast<unsigned char>(
                                  semantic_base[digit_pos - 1]))) {
        --digit_pos;
      }
      if (digit_pos < semantic_base.size()) {
        semantic_index = static_cast<uint32_t>(
            std::strtoul(semantic_base.c_str() + digit_pos, nullptr, 10));
        semantic_base.resize(digit_pos);
      }
    }
    if (semantic_base.empty()) {
      semantic_base = "TEXCOORD";
    }

    input_layout.semantic_names_storage.push_back(std::move(semantic_base));
    input_layout.descriptor.desc_1_0.semanticNames[element_count] =
        input_layout.semantic_names_storage.back().c_str();
    IRInputElementDescriptor1& element =
        input_layout.descriptor.desc_1_0.inputElementDescs[element_count];
    element.semanticIndex = semantic_index;
    element.format = mapped.format;
    element.inputSlot = mapped.input_slot;
    element.alignedByteOffset = mapped.offset;
    element.instanceDataStepRate = 0;
    element.inputSlotClass = IRInputClassificationPerVertexData;
    ++element_count;
  }
  input_layout.descriptor.desc_1_0.numElements = element_count;
  return input_layout;
}

constexpr uint32_t kMetalPipelineStorageMagic = 0x4C544D58;  // 'XMTL'
constexpr uint32_t kGeneratedStageGeometryVertex = 0x100;
constexpr uint32_t kGeneratedStageGeometryShader = 0x101;
constexpr uint32_t kGeneratedStageTessellationVertex = 0x200;
constexpr uint32_t kGeneratedStageTessellationHull = 0x201;
constexpr uint32_t kGeneratedStageTessellationDomain = 0x202;

uint32_t NativeMslTessellationControlPointCount(
    const PrimitiveProcessor::ProcessingResult& primitive) {
  switch (primitive.host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      return 3;
    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      return 4;
    case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      return primitive.tessellation_mode == xenos::TessellationMode::kAdaptive
                 ? 3
                 : 1;
    case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      return primitive.tessellation_mode == xenos::TessellationMode::kAdaptive
                 ? 4
                 : 1;
    default:
      return 0;
  }
}

}  // namespace

class MetalPipelineCache::GeneratedStageCache {
 public:
  struct GeometryVertexStageState {
    MTL::Library* library = nullptr;
    MTL::Library* stage_in_library = nullptr;
    std::string function_name;
    uint32_t vertex_output_size_in_bytes = 0;
  };

  struct GeometryShaderStageState {
    MTL::Library* library = nullptr;
    std::string function_name;
    uint32_t max_input_primitives_per_mesh_threadgroup = 0;
    std::vector<MetalShaderFunctionConstant> function_constants;
  };

  struct TessellationVertexStageState {
    MTL::Library* library = nullptr;
    MTL::Library* stage_in_library = nullptr;
    std::string function_name;
    uint32_t vertex_output_size_in_bytes = 0;
  };

  struct TessellationHullStageState {
    MTL::Library* library = nullptr;
    std::string function_name;
    MetalShaderReflectionInfo reflection;
  };

  struct TessellationDomainStageState {
    MTL::Library* library = nullptr;
    std::string function_name;
    MetalShaderReflectionInfo reflection;
  };

  GeneratedStageCache(MetalPipelineCache& owner, MTL::Device* device,
                      DxbcToDxilConverter& dxbc_to_dxil_converter,
                      MetalShaderConverter& metal_shader_converter)
      : owner_(owner),
        device_(device),
        dxbc_to_dxil_converter_(dxbc_to_dxil_converter),
        metal_shader_converter_(metal_shader_converter) {}

  ~GeneratedStageCache() {
    for (auto& pair : geometry_vertex_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
      if (pair.second.stage_in_library) {
        pair.second.stage_in_library->release();
      }
    }
    for (auto& pair : geometry_shader_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
    }
    for (auto& pair : tessellation_vertex_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
      if (pair.second.stage_in_library) {
        pair.second.stage_in_library->release();
      }
    }
    for (auto& pair : tessellation_hull_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
    }
    for (auto& pair : tessellation_domain_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
    }
  }

  std::shared_ptr<const MetalStageCompileResult> CompileStage(
      MetalShaderStage stage, std::shared_ptr<const std::vector<uint8_t>> dxil,
      bool enable_geometry_emulation, int input_topology,
      const IRVersionedInputLayoutDescriptor* input_layout = nullptr,
      uint64_t stage_in_layout_key = 0,
      std::function<const IRVersionedInputLayoutDescriptor*(
          const MetalShaderReflectionInfo&)>
          stage_in_layout_builder = nullptr) {
    MetalStageCompileRequest request;
    request.stage = stage;
    request.dxil_data = std::move(dxil);
    request.enable_geometry_emulation = enable_geometry_emulation;
    request.input_topology = input_topology;
    request.input_layout = input_layout;
    request.stage_in_layout_key = stage_in_layout_key;
    request.stage_in_layout_builder = std::move(stage_in_layout_builder);
    request.requested_outputs = kMetalStageCompileOutputMetallib;
    if (input_layout || request.stage_in_layout_builder) {
      request.requested_outputs |= kMetalStageCompileOutputStageInMetallib;
    }
    return metal_shader_converter_.CompileStage(request);
  }

  GeometryVertexStageState* GetGeometryVertexStage(
      MetalShader::MetalTranslation* vertex_translation,
      GeometryShaderKey geometry_shader_key) {
    const uint64_t geometry_vertex_stage_key_data[] = {
        uint64_t(reinterpret_cast<uintptr_t>(vertex_translation)),
        uint64_t(geometry_shader_key.key)};
    uint64_t geometry_vertex_stage_key =
        XXH3_64bits(geometry_vertex_stage_key_data,
                    sizeof(geometry_vertex_stage_key_data));
    auto vertex_it =
        geometry_vertex_stage_cache_.find(geometry_vertex_stage_key);
    if (vertex_it != geometry_vertex_stage_cache_.end()) {
      return &vertex_it->second;
    }

    if (!owner_.EnsureDxilTranslationReady(vertex_translation,
                                           "geometry helper vertex",
                                           /*helper_stage_pipeline=*/true)) {
      XELOGE("Geometry VS: failed to create a valid DXIL translation");
      return nullptr;
    }
    std::vector<uint8_t> dxil_data = vertex_translation->GetDxilDataCopy();

    IRInputTopology input_topology =
        GetGeometryStageInputTopology(geometry_shader_key);
    auto dxil = std::make_shared<std::vector<uint8_t>>(std::move(dxil_data));
    const uint64_t stage_in_key_data[] = {
        vertex_translation->shader().ucode_data_hash(),
        vertex_translation->modification(), uint64_t(geometry_shader_key.key)};
    uint64_t stage_in_layout_key =
        XXH3_64bits(stage_in_key_data, sizeof(stage_in_key_data));
    auto input_layout = std::make_shared<StageInInputLayout>();
    std::shared_ptr<const MetalStageCompileResult> vertex_result = CompileStage(
        MetalShaderStage::kVertex, dxil, true, static_cast<int>(input_topology),
        nullptr, stage_in_layout_key,
        [input_layout,
         vertex_translation](const MetalShaderReflectionInfo& reflection)
            -> const IRVersionedInputLayoutDescriptor* {
          *input_layout = BuildStageInInputLayout(vertex_translation->shader(),
                                                  reflection, "Geometry VS");
          return &input_layout->descriptor;
        });
    if (!vertex_result || !vertex_result->success) {
      XELOGE("Geometry VS: DXIL to Metal conversion failed: {}",
             vertex_result ? vertex_result->error_message : "unknown error");
      return nullptr;
    }
    const MetalShaderReflectionInfo& vertex_reflection =
        vertex_result->reflection;
    if (vertex_result->stage_in_metallib.empty()) {
      XELOGE(
          "Geometry VS: Failed to synthesize stage-in function "
          "(vertex_inputs={}, output_size={})",
          vertex_reflection.vertex_input_count,
          vertex_reflection.vertex_output_size_in_bytes);
      return nullptr;
    }

    MTL::Library* vertex_library =
        owner_.NewLibraryFromBytes(vertex_result->metallib_data, "Geometry VS");
    if (!vertex_library) {
      return nullptr;
    }
    MTL::Library* stage_in_library = owner_.NewLibraryFromBytes(
        vertex_result->stage_in_metallib, "Geometry VS stage-in");
    if (!stage_in_library) {
      vertex_library->release();
      return nullptr;
    }

    GeometryVertexStageState state;
    state.library = vertex_library;
    state.stage_in_library = stage_in_library;
    state.function_name = vertex_result->function_name;
    state.vertex_output_size_in_bytes =
        vertex_reflection.vertex_output_size_in_bytes;
    if (state.vertex_output_size_in_bytes == 0) {
      XELOGE(
          "Geometry VS: reflection returned zero output size "
          "(vertex_inputs={})",
          vertex_reflection.vertex_input_count);
    }

    auto [inserted_it, inserted] = geometry_vertex_stage_cache_.emplace(
        geometry_vertex_stage_key, std::move(state));
    return &inserted_it->second;
  }

  GeometryShaderStageState* GetGeometryShaderStage(
      GeometryShaderKey geometry_shader_key) {
    auto geom_it = geometry_shader_stage_cache_.find(geometry_shader_key);
    if (geom_it != geometry_shader_stage_cache_.end()) {
      return &geom_it->second;
    }

    std::vector<uint8_t> dxil_data;
    const std::vector<uint32_t>& dxbc_dwords =
        GetGeometryShader(geometry_shader_key);
    std::vector<uint8_t> dxbc_bytes(dxbc_dwords.size() * sizeof(uint32_t));
    std::memcpy(dxbc_bytes.data(), dxbc_dwords.data(), dxbc_bytes.size());

    if (!dxbc_to_dxil_converter_.IsAvailable()) {
      const uint64_t key_data[] = {kGeneratedStageGeometryShader,
                                   uint64_t(geometry_shader_key.key)};
      owner_.LogHelperDxilUnavailableOnce(
          XXH3_64bits(key_data, sizeof(key_data)),
          "geometry helper generated mesh shader");
      return nullptr;
    }
    std::string dxil_error;
    if (!owner_.ConvertDxbcToDxil(dxbc_bytes, dxil_data, &dxil_error)) {
      XELOGE("Geometry GS: DXBC to DXIL conversion failed: {}", dxil_error);
      return nullptr;
    }

    IRInputTopology input_topology =
        GetGeometryStageInputTopology(geometry_shader_key);
    std::shared_ptr<const MetalStageCompileResult> geometry_result =
        CompileStage(
            MetalShaderStage::kGeometry,
            std::make_shared<std::vector<uint8_t>>(std::move(dxil_data)), true,
            static_cast<int>(input_topology));
    if (!geometry_result || !geometry_result->success) {
      XELOGE(
          "Geometry GS: DXIL to Metal conversion failed: {}",
          geometry_result ? geometry_result->error_message : "unknown error");
      return nullptr;
    }
    const MetalShaderReflectionInfo& geometry_reflection =
        geometry_result->reflection;
    if (!geometry_result->has_mesh_stage &&
        !geometry_result->has_geometry_stage) {
      XELOGE(
          "Geometry GS: MSC did not emit mesh or geometry stage (mesh={}, "
          "geometry={})",
          geometry_result->has_mesh_stage, geometry_result->has_geometry_stage);
      return nullptr;
    }
    if (!geometry_result->has_mesh_stage) {
      static bool mesh_missing_logged = false;
      if (!mesh_missing_logged) {
        mesh_missing_logged = true;
        XELOGW(
            "Geometry GS: MSC did not emit mesh stage; using geometry stage "
            "library");
      }
    }

    MTL::Library* geometry_library = owner_.NewLibraryFromBytes(
        geometry_result->metallib_data, "Geometry GS");
    if (!geometry_library) {
      return nullptr;
    }

    GeometryShaderStageState state;
    state.library = geometry_library;
    state.function_name = geometry_result->function_name;
    state.max_input_primitives_per_mesh_threadgroup =
        geometry_reflection.gs_max_input_primitives_per_mesh_threadgroup;
    state.function_constants = geometry_reflection.function_constants;
    if (state.max_input_primitives_per_mesh_threadgroup == 0) {
      XELOGE("Geometry GS: reflection returned zero max input primitives");
    }

    auto [inserted_it, inserted] = geometry_shader_stage_cache_.emplace(
        geometry_shader_key, std::move(state));
    return &inserted_it->second;
  }

  TessellationVertexStageState* GetTessellationVertexStage(
      MetalShader::MetalTranslation* domain_translation,
      xenos::TessellationMode tessellation_mode) {
    struct VertexStageKey {
      const void* shader;
      uint32_t tessellation_mode;
    } vertex_key = {domain_translation, uint32_t(tessellation_mode)};
    uint64_t vertex_key_hash = XXH3_64bits(&vertex_key, sizeof(vertex_key));
    auto vertex_it = tessellation_vertex_stage_cache_.find(vertex_key_hash);
    if (vertex_it != tessellation_vertex_stage_cache_.end()) {
      return &vertex_it->second;
    }

    const uint64_t stable_vertex_key[] = {
        domain_translation->shader().ucode_data_hash(),
        domain_translation->modification(), uint64_t(tessellation_mode)};
    uint64_t stage_in_layout_key =
        XXH3_64bits(stable_vertex_key, sizeof(stable_vertex_key));

    const uint8_t* vs_bytes = nullptr;
    size_t vs_size = 0;
    if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
      vs_bytes = ::tessellation_adaptive_vs;
      vs_size = sizeof(::tessellation_adaptive_vs);
    } else {
      vs_bytes = ::tessellation_indexed_vs;
      vs_size = sizeof(::tessellation_indexed_vs);
    }
    std::vector<uint8_t> dxbc_bytes(vs_bytes, vs_bytes + vs_size);
    std::vector<uint8_t> dxil_data;
    std::string dxil_error;
    if (!dxbc_to_dxil_converter_.IsAvailable()) {
      const uint64_t key_data[] = {
          kGeneratedStageTessellationVertex,
          domain_translation->shader().ucode_data_hash(),
          domain_translation->modification(), uint64_t(tessellation_mode)};
      owner_.LogHelperDxilUnavailableOnce(
          XXH3_64bits(key_data, sizeof(key_data)),
          "tessellation helper generated vertex");
      return nullptr;
    }
    if (!owner_.ConvertDxbcToDxil(dxbc_bytes, dxil_data, &dxil_error)) {
      XELOGE("Tessellation VS: DXBC to DXIL conversion failed: {}", dxil_error);
      return nullptr;
    }

    auto dxil = std::make_shared<std::vector<uint8_t>>(std::move(dxil_data));
    auto input_layout = std::make_shared<StageInInputLayout>();
    std::shared_ptr<const MetalStageCompileResult> vertex_result = CompileStage(
        MetalShaderStage::kVertex, dxil, true,
        static_cast<int>(IRInputTopologyUndefined), nullptr,
        stage_in_layout_key,
        [input_layout,
         domain_translation](const MetalShaderReflectionInfo& reflection)
            -> const IRVersionedInputLayoutDescriptor* {
          *input_layout = BuildStageInInputLayout(
              domain_translation->shader(), reflection, "Tessellation VS");
          return &input_layout->descriptor;
        });
    if (!vertex_result || !vertex_result->success) {
      XELOGE("Tessellation VS: DXIL to Metal conversion failed: {}",
             vertex_result ? vertex_result->error_message : "unknown error");
      return nullptr;
    }
    const MetalShaderReflectionInfo& vertex_reflection =
        vertex_result->reflection;
    if (vertex_result->stage_in_metallib.empty()) {
      XELOGE("Tessellation VS: Failed to synthesize stage-in function");
      return nullptr;
    }

    MTL::Library* vertex_library = owner_.NewLibraryFromBytes(
        vertex_result->metallib_data, "Tessellation VS");
    if (!vertex_library) {
      return nullptr;
    }
    MTL::Library* stage_in_library = owner_.NewLibraryFromBytes(
        vertex_result->stage_in_metallib, "Tessellation VS stage-in");
    if (!stage_in_library) {
      vertex_library->release();
      return nullptr;
    }

    TessellationVertexStageState state;
    state.library = vertex_library;
    state.stage_in_library = stage_in_library;
    state.function_name = vertex_result->function_name;
    state.vertex_output_size_in_bytes =
        vertex_reflection.vertex_output_size_in_bytes;
    if (state.vertex_output_size_in_bytes == 0) {
      XELOGE("Tessellation VS: reflection returned zero output size");
    }

    auto [inserted_it, inserted] = tessellation_vertex_stage_cache_.emplace(
        vertex_key_hash, std::move(state));
    return &inserted_it->second;
  }

  TessellationHullStageState* GetTessellationHullStage(
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      xenos::TessellationMode tessellation_mode) {
    struct HullStageKey {
      uint32_t host_vs_type;
      uint32_t tessellation_mode;
    } hull_key = {uint32_t(primitive_processing_result.host_vertex_shader_type),
                  uint32_t(tessellation_mode)};
    uint64_t hull_key_hash = XXH3_64bits(&hull_key, sizeof(hull_key));
    auto hull_it = tessellation_hull_stage_cache_.find(hull_key_hash);
    if (hull_it != tessellation_hull_stage_cache_.end()) {
      return &hull_it->second;
    }

    const uint8_t* hs_bytes = nullptr;
    size_t hs_size = 0;
    switch (tessellation_mode) {
      case xenos::TessellationMode::kDiscrete:
        switch (primitive_processing_result.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            hs_bytes = ::discrete_triangle_3cp_hs;
            hs_size = sizeof(::discrete_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            hs_bytes = ::discrete_triangle_1cp_hs;
            hs_size = sizeof(::discrete_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            hs_bytes = ::discrete_quad_4cp_hs;
            hs_size = sizeof(::discrete_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            hs_bytes = ::discrete_quad_1cp_hs;
            hs_size = sizeof(::discrete_quad_1cp_hs);
            break;
          default:
            XELOGE(
                "Tessellation HS: unsupported host vertex shader type {}",
                uint32_t(primitive_processing_result.host_vertex_shader_type));
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kContinuous:
        switch (primitive_processing_result.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            hs_bytes = ::continuous_triangle_3cp_hs;
            hs_size = sizeof(::continuous_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            hs_bytes = ::continuous_triangle_1cp_hs;
            hs_size = sizeof(::continuous_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            hs_bytes = ::continuous_quad_4cp_hs;
            hs_size = sizeof(::continuous_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            hs_bytes = ::continuous_quad_1cp_hs;
            hs_size = sizeof(::continuous_quad_1cp_hs);
            break;
          default:
            XELOGE(
                "Tessellation HS: unsupported host vertex shader type {}",
                uint32_t(primitive_processing_result.host_vertex_shader_type));
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kAdaptive:
        switch (primitive_processing_result.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            hs_bytes = ::adaptive_triangle_hs;
            hs_size = sizeof(::adaptive_triangle_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            hs_bytes = ::adaptive_quad_hs;
            hs_size = sizeof(::adaptive_quad_hs);
            break;
          default:
            XELOGE(
                "Tessellation HS: unsupported host vertex shader type {}",
                uint32_t(primitive_processing_result.host_vertex_shader_type));
            return nullptr;
        }
        break;
      default:
        XELOGE("Tessellation HS: unsupported tessellation mode {}",
               uint32_t(tessellation_mode));
        return nullptr;
    }

    std::vector<uint8_t> dxbc_bytes(hs_bytes, hs_bytes + hs_size);
    std::vector<uint8_t> dxil_data;
    std::string dxil_error;
    if (!dxbc_to_dxil_converter_.IsAvailable()) {
      const uint64_t key_data[] = {
          kGeneratedStageTessellationHull,
          uint64_t(primitive_processing_result.host_vertex_shader_type),
          uint64_t(tessellation_mode)};
      owner_.LogHelperDxilUnavailableOnce(
          XXH3_64bits(key_data, sizeof(key_data)),
          "tessellation helper generated hull");
      return nullptr;
    }
    if (!owner_.ConvertDxbcToDxil(dxbc_bytes, dxil_data, &dxil_error)) {
      XELOGE("Tessellation HS: DXBC to DXIL conversion failed: {}", dxil_error);
      return nullptr;
    }

    std::shared_ptr<const MetalStageCompileResult> hull_result = CompileStage(
        MetalShaderStage::kHull,
        std::make_shared<std::vector<uint8_t>>(std::move(dxil_data)), true,
        static_cast<int>(IRInputTopologyUndefined));
    if (!hull_result || !hull_result->success) {
      XELOGE("Tessellation HS: DXIL to Metal conversion failed: {}",
             hull_result ? hull_result->error_message : "unknown error");
      return nullptr;
    }

    MTL::Library* hull_library = owner_.NewLibraryFromBytes(
        hull_result->metallib_data, "Tessellation HS");
    if (!hull_library) {
      return nullptr;
    }

    TessellationHullStageState state;
    state.library = hull_library;
    state.function_name = hull_result->function_name;
    state.reflection = hull_result->reflection;
    if (!state.reflection.has_hull_info) {
      XELOGE("Tessellation HS: reflection missing hull info");
    }

    auto [inserted_it, inserted] =
        tessellation_hull_stage_cache_.emplace(hull_key_hash, std::move(state));
    return &inserted_it->second;
  }

  TessellationDomainStageState* GetTessellationDomainStage(
      MetalShader::MetalTranslation* domain_translation) {
    uint64_t domain_key =
        XXH3_64bits(&domain_translation, sizeof(domain_translation));
    auto domain_it = tessellation_domain_stage_cache_.find(domain_key);
    if (domain_it != tessellation_domain_stage_cache_.end()) {
      return &domain_it->second;
    }

    if (!owner_.EnsureDxilTranslationReady(domain_translation,
                                           "tessellation helper domain",
                                           /*helper_stage_pipeline=*/true)) {
      return nullptr;
    }
    std::vector<uint8_t> dxil_data = domain_translation->GetDxilDataCopy();

    std::shared_ptr<const MetalStageCompileResult> domain_result = CompileStage(
        MetalShaderStage::kDomain,
        std::make_shared<std::vector<uint8_t>>(std::move(dxil_data)), true,
        static_cast<int>(IRInputTopologyUndefined));
    if (!domain_result || !domain_result->success) {
      XELOGE("Tessellation DS: DXIL to Metal conversion failed: {}",
             domain_result ? domain_result->error_message : "unknown error");
      return nullptr;
    }

    MTL::Library* domain_library = owner_.NewLibraryFromBytes(
        domain_result->metallib_data, "Tessellation DS");
    if (!domain_library) {
      return nullptr;
    }

    TessellationDomainStageState state;
    state.library = domain_library;
    state.function_name = domain_result->function_name;
    state.reflection = domain_result->reflection;
    if (!state.reflection.has_domain_info) {
      XELOGE("Tessellation DS: reflection missing domain info");
    }

    auto [inserted_it, inserted] =
        tessellation_domain_stage_cache_.emplace(domain_key, std::move(state));
    return &inserted_it->second;
  }

 private:
  MetalPipelineCache& owner_;
  MTL::Device* device_ = nullptr;
  DxbcToDxilConverter& dxbc_to_dxil_converter_;
  MetalShaderConverter& metal_shader_converter_;

  std::unordered_map<uint64_t, GeometryVertexStageState>
      geometry_vertex_stage_cache_;
  std::unordered_map<GeometryShaderKey, GeometryShaderStageState,
                     GeometryShaderKey::Hasher>
      geometry_shader_stage_cache_;
  std::unordered_map<uint64_t, TessellationVertexStageState>
      tessellation_vertex_stage_cache_;
  std::unordered_map<uint64_t, TessellationHullStageState>
      tessellation_hull_stage_cache_;
  std::unordered_map<uint64_t, TessellationDomainStageState>
      tessellation_domain_stage_cache_;
};

// ---------------------------------------------------------------------------
// ResolvePipelineRenderingKey (shared fixed-function key derivation)
// ---------------------------------------------------------------------------

PipelineRenderingKey ResolvePipelineRenderingKey(
    const RegisterFile& regs,
    const MetalShader::MetalTranslation* pixel_translation,
    bool use_fallback_pixel_shader) {
  PipelineRenderingKey key = {};

  uint32_t pixel_shader_writes_color_targets =
      (use_fallback_pixel_shader || !pixel_translation)
          ? 0
          : pixel_translation->shader().writes_color_targets();
  key.normalized_color_mask = pixel_shader_writes_color_targets
                                  ? draw_util::GetNormalizedColorMask(
                                        regs, pixel_shader_writes_color_targets)
                                  : 0;
  // Xenos alpha-to-mask is implemented by the translated pixel shader via
  // OMask / sample-mask output. Enabling Metal fixed-function alpha-to-coverage
  // would apply a second coverage mask on top of the shader result.
  key.alpha_to_mask_enable = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key.blendcontrol[i] = regs.Get<reg::RB_BLENDCONTROL>(
                                  reg::RB_BLENDCONTROL::rt_register_indices[i])
                              .value;
  }
  return key;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MetalPipelineCache::MetalPipelineCache(MTL::Device* device,
                                       const RegisterFile& register_file)
    : device_(device), register_file_(register_file) {}

MetalPipelineCache::~MetalPipelineCache() {
  // Shut down async pipeline creation threads.
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    creation_threads_shutdown_ = true;
  }
  creation_request_cond_.notify_all();
  for (auto& thread : creation_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  creation_threads_.clear();

  // Release MSC pipeline caches.
  for (auto& pair : pipeline_cache_) {
    if (pair.second) {
      auto* ps = pair.second->state.load(std::memory_order_relaxed);
      if (ps) {
        ps->release();
      }
    }
  }
  pipeline_cache_.clear();

  bindless_sampler_layout_map_.clear();
  bindless_sampler_layouts_.clear();
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();

  for (auto& pair : geometry_pipeline_cache_) {
    if (pair.second.pipeline) {
      pair.second.pipeline->release();
    }
  }
  geometry_pipeline_cache_.clear();

  for (auto& pair : native_mesh_pipeline_cache_) {
    if (pair.second.pipeline) {
      pair.second.pipeline->release();
    }
  }
  native_mesh_pipeline_cache_.clear();

  for (auto& pair : tessellation_pipeline_cache_) {
    if (pair.second.pipeline) {
      pair.second.pipeline->release();
    }
  }
  tessellation_pipeline_cache_.clear();

  generated_stages_.reset();

  if (depth_only_pixel_library_) {
    depth_only_pixel_library_->release();
    depth_only_pixel_library_ = nullptr;
  }
  depth_only_pixel_function_name_.clear();
  if (native_depth_only_pixel_library_) {
    native_depth_only_pixel_library_->release();
    native_depth_only_pixel_library_ = nullptr;
  }
  native_depth_only_pixel_function_name_.clear();

  ShutdownShaderStorage();

  shader_cache_.clear();
  shader_translator_.reset();
  dxbc_to_dxil_converter_.reset();
  metal_shader_converter_.reset();
  native_msl_translator_.reset();
}

// ---------------------------------------------------------------------------
// Shader translation initialization
// ---------------------------------------------------------------------------

bool MetalPipelineCache::InitializeShaderTranslation(
    bool gamma_render_target_as_unorm8, bool msaa_2x_supported,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y) {
  bool edram_rov_used = false;
  generated_stages_.reset();

  XELOGI(
      "DxbcShaderTranslator init: gamma_as_unorm8={}, msaa_2x={}, scale={}x{}",
      gamma_render_target_as_unorm8, msaa_2x_supported, draw_resolution_scale_x,
      draw_resolution_scale_y);

  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      ui::GraphicsProvider::GpuVendorID::kApple, true, edram_rov_used,
      gamma_render_target_as_unorm8, msaa_2x_supported, draw_resolution_scale_x,
      draw_resolution_scale_y,
      false);  // force_emit_source_map

  dxbc_to_dxil_converter_ = std::make_unique<DxbcToDxilConverter>();
  if (!dxbc_to_dxil_converter_->Initialize()) {
    // The DXBC->DXIL path needs the dxilconv library. Native MSL rendering
    // doesn't need it for standard guest vertex/pixel shaders, but generated
    // helper/system stages that still use MSC may fail until they are moved
    // over too. Without an alternate guest shader path, there is no shader
    // path, so fail hard.
    if (NativeGuestMslRenderEnabled()) {
      XELOGW(
          "Metal: DXBC->DXIL converter unavailable; alternate guest shader "
          "path enabled, but generated DXBC helper stages may fail to compile");
    } else {
      XELOGE("Failed to initialize DXBC to DXIL converter");
      return false;
    }
  }

  metal_shader_converter_ = std::make_unique<MetalShaderConverter>();
  if (!metal_shader_converter_->Initialize()) {
    XELOGE("Failed to initialize Metal Shader Converter");
    return false;
  }

  const bool helper_stage_msc_enabled =
      !NativeGuestMslRenderEnabled() || cvars::metal_native_msl_helper_msc;
  if (helper_stage_msc_enabled) {
    generated_stages_ = std::make_unique<GeneratedStageCache>(
        *this, device_, *dxbc_to_dxil_converter_, *metal_shader_converter_);
  } else {
    generated_stages_.reset();
  }

  if (NativeGuestMslRenderEnabled() || cvars::metal_native_msl_guest_shaders) {
    native_msl_translator_ = std::make_unique<MslShaderTranslator>(
        ui::GraphicsProvider::GpuVendorID::kApple,
        /*bindless_resources_used=*/true, edram_rov_used,
        draw_resolution_scale_x, draw_resolution_scale_y);
    if (NativeGuestMslRenderEnabled()) {
      XELOGW(
          "metal_native_msl_render: native guest MSL render path enabled for "
          "standard vertex/pixel pipelines; generated geometry/tess helper "
          "stages are {}",
          helper_stage_msc_enabled ? "enabled" : "disabled");
    } else {
      XELOGI(
          "metal_native_msl_guest_shaders: native MSL compile diagnostics "
          "enabled");
    }
  }

  // Configure MSC minimum targets.
  if (device_) {
    IRGPUFamily min_family = IRGPUFamilyMetal3;
    if (cvars::metal_msc_debug_validation) {
      min_family = IRGPUFamilyMetal3;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple10)) {
      min_family = IRGPUFamilyApple10;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple9)) {
      min_family = IRGPUFamilyApple9;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple8)) {
      min_family = IRGPUFamilyApple8;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple7)) {
      min_family = IRGPUFamilyApple7;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple6)) {
      min_family = IRGPUFamilyApple6;
    } else if (device_->supportsFamily(MTL::GPUFamilyMac2) ||
               device_->supportsFamily(MTL::GPUFamilyMetal4) ||
               device_->supportsFamily(MTL::GPUFamilyMetal3)) {
      min_family = IRGPUFamilyMetal3;
    }

    NS::OperatingSystemVersion os_version =
        NS::ProcessInfo::processInfo()->operatingSystemVersion();
    std::ostringstream version_stream;
    version_stream << os_version.majorVersion << "." << os_version.minorVersion
                   << "." << os_version.patchVersion;
    metal_shader_converter_->SetMinimumTarget(
        min_family, IROperatingSystem_macOS, version_stream.str());
  }

  // Spawn async pipeline creation threads if enabled.
  int32_t thread_count_config = cvars::metal_pipeline_creation_threads;
  uint32_t logical_cores = std::thread::hardware_concurrency();
  uint32_t thread_count = 0;
  if (thread_count_config < 0) {
    thread_count = std::max(1u, logical_cores * 3 / 4);
  } else {
    thread_count =
        std::min(static_cast<uint32_t>(thread_count_config), logical_cores);
  }
  if (thread_count > 0 && cvars::async_shader_compilation) {
    creation_threads_.reserve(thread_count);
    for (uint32_t i = 0; i < thread_count; ++i) {
      creation_threads_.emplace_back(&MetalPipelineCache::CreationThread, this,
                                     i);
    }
    XELOGI("MetalPipelineCache: spawned {} async pipeline creation threads",
           thread_count);
  }

  return true;
}

// ---------------------------------------------------------------------------
// Shader storage lifecycle
// ---------------------------------------------------------------------------

void MetalPipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  InitializeShaderStorageInternal(cache_root, title_id, blocking);
}

bool MetalPipelineCache::InitializeShaderStorageInternal(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  ShutdownShaderStorage();

  if (!device_) {
    XELOGW("Metal shader storage init skipped (no device)");
    return false;
  }

  shader_storage_local_root_ = GetShaderStorageLocalRoot(cache_root) / "metal" /
                               GetShaderStorageDeviceTag() /
                               GetShaderStorageAbiTag();
  shader_storage_title_root_ =
      shader_storage_local_root_ / fmt::format("{:08X}", title_id);

  std::error_code ec;
  std::filesystem::create_directories(shader_storage_title_root_, ec);
  if (ec) {
    XELOGW("Metal shader storage: Failed to create {}: {}",
           shader_storage_title_root_.string(), ec.message());
    return false;
  }

  artifact_store_path_ = shader_storage_title_root_ /
                         fmt::format("{:08X}.metal.artifacts", title_id);
  if (::cvars::metal_shader_disk_cache && g_metal_artifact_store) {
    g_metal_artifact_store->Initialize(artifact_store_path_);
  }

  ShaderStorageWriter<MetalPipelineStoredDescription>::PipelineStorageConfig
      pipeline_config;
  pipeline_config.file_suffix = ".metal.xpso";
  pipeline_config.api_magic = kMetalPipelineStorageMagic;
  pipeline_config.version =
      std::max(MetalPipelineDescription::kVersion,
               DxbcShaderTranslator::Modification::kVersion);

  uint32_t storage_index = storage_writer_.storage_index() + 1;
  std::vector<MetalPipelineStoredDescription> stored_descriptions;
  if (!storage_writer_.InitializeShaderStorage(
          cache_root, title_id, pipeline_config,
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
              uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            MetalShader* shader = LoadShader(
                type, ucode_dwords, ucode_dword_count, ucode_data_hash);
            if (shader->ucode_storage_index() == storage_index) {
              return true;
            }
            shader->set_ucode_storage_index(storage_index);
            return true;
          },
          [&](const std::set<std::pair<uint64_t, uint64_t>>
                  & translations_needed) {
            XELOGI(
                "Metal shader storage: indexed {} cached shader "
                "translations",
                translations_needed.size());
          },
          stored_descriptions)) {
    return false;
  }
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;
  shader_storage_title_id_ = title_id;
  {
    std::lock_guard<std::mutex> lock(stored_pipeline_mutex_);
    stored_pipeline_hashes_.clear();
    for (const MetalPipelineStoredDescription& desc : stored_descriptions) {
      stored_pipeline_hashes_.insert(desc.description_hash);
    }
  }

  const char* path_suffix = "rtv-bindless";
  std::string storage_suffix =
      fmt::format("{}.{}", path_suffix, GetShaderStorageAbiTag());
  pipeline_binary_archive_path_ =
      shader_storage_title_root_ /
      fmt::format("{:08X}.{}.metal.binarchive", title_id, storage_suffix);

  if (::cvars::metal_pipeline_binary_archive) {
    InitializePipelineBinaryArchive(pipeline_binary_archive_path_);
  }

  bool prewarm_binary_archive = false;
  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    prewarm_binary_archive = pipeline_binary_archive_ != nullptr;
  }
  if (prewarm_binary_archive && !stored_descriptions.empty()) {
    PrewarmStoredPipelines(stored_descriptions, blocking);
  }

  return true;
}

void MetalPipelineCache::ShutdownShaderStorage() {
  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    if (pipeline_binary_archive_) {
      if (pipeline_binary_archive_dirty_) {
        NS::String* path_string =
            NS::String::string(pipeline_binary_archive_path_.string().c_str(),
                               NS::UTF8StringEncoding);
        NS::URL* url = NS::URL::fileURLWithPath(path_string);
        NS::Error* error = nullptr;
        if (!pipeline_binary_archive_->serializeToURL(url, &error)) {
          if (error) {
            XELOGW("Metal binary archive serialize failed: {}",
                   error->localizedDescription()->utf8String());
          }
        }
        pipeline_binary_archive_dirty_ = false;
      }
      pipeline_binary_archive_->release();
      pipeline_binary_archive_ = nullptr;
    }
  }
  storage_writer_.ShutdownShaderStorage();
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;
  shader_storage_title_id_ = 0;
  {
    std::lock_guard<std::mutex> lock(stored_pipeline_mutex_);
    stored_pipeline_hashes_.clear();
  }

  if (g_metal_artifact_store) {
    g_metal_artifact_store->Shutdown();
  }
}

void MetalPipelineCache::EndSubmission() {
  const bool flush_shaders = shader_storage_file_flush_needed_.exchange(false);
  const bool flush_pipelines =
      pipeline_storage_file_flush_needed_.exchange(false);
  if (flush_shaders || flush_pipelines) {
    storage_writer_.RequestFlush(flush_shaders, flush_pipelines);
  }
  if (!creation_threads_.empty()) {
    creation_request_cond_.notify_one();
  }
}

bool MetalPipelineCache::ConvertDxbcToDxil(
    const std::vector<uint8_t>& dxbc_data, std::vector<uint8_t>& dxil_data,
    std::string* error_message) {
  dxil_convert_requests_.fetch_add(1, std::memory_order_relaxed);
  dxil_convert_dxbc_bytes_.fetch_add(dxbc_data.size(),
                                     std::memory_order_relaxed);
  MetalDxilBytecodeKey dxil_key = {};
  if (!dxbc_data.empty() && dxbc_to_dxil_converter_) {
    XXH128_hash_t dxbc_hash = XXH3_128bits(dxbc_data.data(), dxbc_data.size());
    dxil_key.dxbc_size = dxbc_data.size();
    dxil_key.dxbc_hash_low = dxbc_hash.low64;
    dxil_key.dxbc_hash_high = dxbc_hash.high64;
    dxil_key.converter_options_hash = dxbc_to_dxil_converter_->GetOptionsHash();
    dxil_key.converter_version = DxbcToDxilConverter::kCacheVersion;
    if (::cvars::metal_shader_disk_cache && g_metal_artifact_store &&
        g_metal_artifact_store->LoadDxilBytecode(dxil_key, &dxil_data)) {
      dxil_cache_hits_.fetch_add(1, std::memory_order_relaxed);
      dxil_convert_dxil_bytes_.fetch_add(dxil_data.size(),
                                         std::memory_order_relaxed);
      return true;
    }
  }
  dxil_cache_misses_.fetch_add(1, std::memory_order_relaxed);

  auto start = std::chrono::steady_clock::now();
  bool ok = dxbc_to_dxil_converter_ && dxbc_to_dxil_converter_->Convert(
                                           dxbc_data, dxil_data, error_message);
  auto end = std::chrono::steady_clock::now();
  uint64_t ms = ElapsedMs(start, end);
  dxil_convert_ms_total_.fetch_add(ms, std::memory_order_relaxed);
  AtomicMax(dxil_convert_ms_max_, ms);
  if (ok) {
    dxil_convert_dxil_bytes_.fetch_add(dxil_data.size(),
                                       std::memory_order_relaxed);
    if (::cvars::metal_shader_disk_cache && g_metal_artifact_store &&
        !dxbc_data.empty()) {
      g_metal_artifact_store->StoreDxilBytecode(dxil_key, dxil_data);
    }
  } else {
    dxil_convert_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  return ok;
}

MTL::Library* MetalPipelineCache::NewLibraryFromBytes(
    const std::vector<uint8_t>& bytes, const char* label) {
  if (!device_ || bytes.empty()) {
    RecordLibraryCreation(bytes.size(), false, 0);
    return nullptr;
  }
  NS::Error* error = nullptr;
  dispatch_data_t data = dispatch_data_create(
      bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  auto start = std::chrono::steady_clock::now();
  MTL::Library* library = device_->newLibrary(data, &error);
  auto end = std::chrono::steady_clock::now();
  dispatch_release(data);
  RecordLibraryCreation(bytes.size(), library != nullptr,
                        ElapsedMs(start, end));
  if (!library) {
    XELOGE(
        "{}: Failed to create Metal library: {}",
        label ? label : "Metal library",
        error ? error->localizedDescription()->utf8String() : "unknown error");
  }
  return library;
}

void MetalPipelineCache::RecordLibraryCreation(size_t byte_count, bool success,
                                               uint64_t ms) {
  library_requests_.fetch_add(1, std::memory_order_relaxed);
  library_bytes_.fetch_add(byte_count, std::memory_order_relaxed);
  if (!success) {
    library_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  library_ms_total_.fetch_add(ms, std::memory_order_relaxed);
  AtomicMax(library_ms_max_, ms);
}

void MetalPipelineCache::RecordRenderPipelineCreation(bool success,
                                                      uint64_t ms) {
  render_pipeline_requests_.fetch_add(1, std::memory_order_relaxed);
  if (!success) {
    render_pipeline_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  render_pipeline_ms_total_.fetch_add(ms, std::memory_order_relaxed);
  AtomicMax(render_pipeline_ms_max_, ms);
}

std::string MetalPipelineCache::GetShaderStorageDeviceTag() const {
  std::string tag = "unknown";
  if (device_ && device_->name()) {
    tag = device_->name()->utf8String();
  }

  for (char& ch : tag) {
    if (!std::isalnum(static_cast<unsigned char>(ch))) {
      ch = '_';
    }
  }
  return tag;
}

std::string MetalPipelineCache::GetShaderStorageAbiTag() const {
  return fmt::format("msc{:08X}-ps{:08X}-sh{:08X}",
                     DxbcShaderTranslator::Modification::kVersion,
                     MetalPipelineDescription::kVersion,
                     MetalArtifactStore::kStorageVersion);
}

// ---------------------------------------------------------------------------
// Pipeline binary archive
// ---------------------------------------------------------------------------

bool MetalPipelineCache::InitializePipelineBinaryArchive(
    const std::filesystem::path& archive_path) {
  if (!device_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
  if (pipeline_binary_archive_) {
    pipeline_binary_archive_->release();
    pipeline_binary_archive_ = nullptr;
  }

  MTL::BinaryArchiveDescriptor* desc =
      MTL::BinaryArchiveDescriptor::alloc()->init();
  std::error_code ec;
  bool archive_exists = std::filesystem::exists(archive_path, ec);
  if (ec) {
    XELOGW("Metal binary archive existence check failed for {}: {}",
           archive_path.string(), ec.message());
    archive_exists = false;
  }
  if (archive_exists) {
    NS::String* path_string = NS::String::string(archive_path.string().c_str(),
                                                 NS::UTF8StringEncoding);
    NS::URL* url = NS::URL::fileURLWithPath(path_string);
    desc->setUrl(url);
  }

  NS::Error* error = nullptr;
  pipeline_binary_archive_ = device_->newBinaryArchive(desc, &error);
  if (!pipeline_binary_archive_ && archive_exists) {
    if (error) {
      XELOGW(
          "Metal binary archive load failed for existing file {}; retrying "
          "with a fresh archive: {}",
          archive_path.string(), error->localizedDescription()->utf8String());
    }
    desc->setUrl(nullptr);
    error = nullptr;
    pipeline_binary_archive_ = device_->newBinaryArchive(desc, &error);
  }
  desc->release();
  if (!pipeline_binary_archive_) {
    if (error) {
      XELOGW("Metal binary archive init failed: {}",
             error->localizedDescription()->utf8String());
    }
    return false;
  }
  pipeline_binary_archive_path_ = archive_path;
  pipeline_binary_archive_dirty_ = false;
  return true;
}

void MetalPipelineCache::SerializePipelineBinaryArchive() {
  std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
  if (!pipeline_binary_archive_ || !pipeline_binary_archive_dirty_) {
    return;
  }
  NS::String* path_string = NS::String::string(
      pipeline_binary_archive_path_.string().c_str(), NS::UTF8StringEncoding);
  NS::URL* url = NS::URL::fileURLWithPath(path_string);
  NS::Error* error = nullptr;
  if (!pipeline_binary_archive_->serializeToURL(url, &error)) {
    if (error) {
      XELOGW("Metal binary archive serialize failed: {}",
             error->localizedDescription()->utf8String());
    }
  }
  pipeline_binary_archive_dirty_ = false;
}

void MetalPipelineCache::PrewarmStoredPipelines(
    const std::vector<MetalPipelineStoredDescription>& descriptions,
    bool blocking) {
  if (descriptions.empty()) {
    return;
  }
  size_t queued = 0;
  size_t skipped = 0;
  for (const MetalPipelineStoredDescription& stored : descriptions) {
    const MetalPipelineDescription& desc = stored.description;
    const bool native_pipeline =
        desc.shader_backend ==
        uint32_t(MetalShader::MetalTranslation::Backend::kNativeMsl);
    if (native_pipeline && !NativeGuestMslRenderEnabled()) {
      ++skipped;
      continue;
    }
    const bool standard_pipeline =
        desc.kind == uint32_t(PipelineKind::kStandard);
    if (!native_pipeline && standard_pipeline && NativeGuestMslRenderEnabled()) {
      ++skipped;
      continue;
    }
    auto vs_it = shader_cache_.find(desc.vertex_shader_hash);
    if (vs_it == shader_cache_.end()) {
      ++skipped;
      continue;
    }
    uint64_t vertex_translation_key = desc.vertex_shader_modification;
    if (native_pipeline) {
      vertex_translation_key = NativeMslTranslationCacheKey(
          desc.vertex_shader_modification,
          desc.vertex_shader_native_msl_texture_sign_key);
    }
    auto* vertex_translation = static_cast<MetalShader::MetalTranslation*>(
        vs_it->second->GetOrCreateTranslation(vertex_translation_key));
    const PipelineKind pipeline_kind = PipelineKind(desc.kind);
    const bool helper_pipeline =
        pipeline_kind == PipelineKind::kGeometry ||
        pipeline_kind == PipelineKind::kTessellation;
    const bool native_primitive_mesh_pipeline =
        NativePrimitiveMeshPipelineKind(pipeline_kind);
    const bool native_tessellation_mesh_pipeline =
        pipeline_kind == PipelineKind::kNativeTessellationMesh;
    if (!vertex_translation) {
      ++skipped;
      continue;
    }
    if (native_pipeline &&
        !vertex_translation->ConfigureNativeMslVariant(
            desc.vertex_shader_modification,
            desc.vertex_shader_native_msl_texture_sign_key,
            NativeMslTextureSignMasksFromStorage(
                desc.vertex_shader_native_msl_texture_sign_component_masks),
            NativeMslTextureSignMasksFromStorage(
                desc.vertex_shader_native_msl_texture_sign_values))) {
      ++skipped;
      continue;
    }
    const bool async_native_standard_pipeline =
        native_pipeline && standard_pipeline &&
        cvars::async_shader_compilation && !creation_threads_.empty();
    if (standard_pipeline || native_primitive_mesh_pipeline ||
        native_tessellation_mesh_pipeline) {
      if (!async_native_standard_pipeline &&
          !EnsureMetalTranslationReady(vertex_translation,
                                       /*native_guest_allowed=*/true,
                                       "stored vertex")) {
        ++skipped;
        continue;
      }
    } else if (helper_pipeline) {
      if (!EnsureDxilTranslationReady(
              vertex_translation,
              desc.kind == uint32_t(PipelineKind::kTessellation)
                  ? "stored tessellation helper domain"
                  : "stored geometry helper vertex",
              /*helper_stage_pipeline=*/true)) {
        ++skipped;
        continue;
      }
    } else if (!EnsureMetalTranslationReady(vertex_translation,
                                            /*native_guest_allowed=*/false,
                                            "stored system vertex")) {
      ++skipped;
      continue;
    }

    MetalShader::MetalTranslation* pixel_translation = nullptr;
    if (desc.pixel_shader_hash) {
      auto ps_it = shader_cache_.find(desc.pixel_shader_hash);
      if (ps_it == shader_cache_.end()) {
        ++skipped;
        continue;
      }
      uint64_t pixel_translation_key = desc.pixel_shader_modification;
      if (native_pipeline) {
        pixel_translation_key = NativeMslTranslationCacheKey(
            desc.pixel_shader_modification,
            desc.pixel_shader_native_msl_texture_sign_key);
      }
      pixel_translation = static_cast<MetalShader::MetalTranslation*>(
          ps_it->second->GetOrCreateTranslation(pixel_translation_key));
      if (!pixel_translation) {
        ++skipped;
        continue;
      }
      if (native_pipeline &&
          !pixel_translation->ConfigureNativeMslVariant(
              desc.pixel_shader_modification,
              desc.pixel_shader_native_msl_texture_sign_key,
              NativeMslTextureSignMasksFromStorage(
                  desc.pixel_shader_native_msl_texture_sign_component_masks),
              NativeMslTextureSignMasksFromStorage(
                  desc.pixel_shader_native_msl_texture_sign_values))) {
        ++skipped;
        continue;
      }
      if (!async_native_standard_pipeline &&
          !EnsureMetalTranslationReady(
              pixel_translation,
              standard_pipeline || native_primitive_mesh_pipeline ||
                  native_tessellation_mesh_pipeline,
              helper_pipeline ? "stored helper pixel" : "stored pixel",
              helper_pipeline)) {
        ++skipped;
        continue;
      }
    }

    PipelineAttachmentFormats formats = {};
    formats.sample_count = desc.sample_count;
    formats.depth_format = static_cast<MTL::PixelFormat>(desc.depth_format);
    formats.stencil_format = static_cast<MTL::PixelFormat>(desc.stencil_format);
    for (uint32_t i = 0; i < 4; ++i) {
      formats.color_formats[i] =
          static_cast<MTL::PixelFormat>(desc.color_formats[i]);
    }
    PipelineRenderingKey rendering = {};
    rendering.normalized_color_mask = desc.normalized_color_mask;
    rendering.alpha_to_mask_enable = desc.alpha_to_mask_enable;
    std::memcpy(rendering.blendcontrol, desc.blendcontrol,
                sizeof(rendering.blendcontrol));

    if (standard_pipeline) {
      PipelineHandle* handle = GetOrCreatePipelineState(
          vertex_translation, pixel_translation, formats, rendering);
      if (handle) {
        ++queued;
      }
    } else if (desc.kind == uint32_t(PipelineKind::kGeometry)) {
      GeometryShaderKey geometry_key;
      geometry_key.key = uint32_t(desc.auxiliary_hash);
      if (GetOrCreateGeometryPipelineState(vertex_translation,
                                           pixel_translation, geometry_key,
                                           formats, rendering)) {
        ++queued;
      }
    } else if (desc.kind == uint32_t(PipelineKind::kTessellation)) {
      PrimitiveProcessor::ProcessingResult primitive = {};
      primitive.host_vertex_shader_type =
          Shader::HostVertexShaderType(desc.auxiliary0);
      primitive.tessellation_mode = xenos::TessellationMode(desc.auxiliary1);
      primitive.host_primitive_type = xenos::PrimitiveType(desc.auxiliary2);
      if (GetOrCreateTessellationPipelineState(vertex_translation,
                                               pixel_translation, primitive,
                                               formats, rendering)) {
        ++queued;
      }
    } else if (native_primitive_mesh_pipeline) {
      if (GetOrCreateNativeMslPrimitiveMeshPipelineState(
              vertex_translation, pixel_translation,
              NativePrimitiveMeshTypeForKind(pipeline_kind), formats,
              rendering)) {
        ++queued;
      }
    } else if (native_tessellation_mesh_pipeline) {
      PrimitiveProcessor::ProcessingResult primitive = {};
      primitive.host_vertex_shader_type =
          Shader::HostVertexShaderType(desc.auxiliary0);
      primitive.tessellation_mode = xenos::TessellationMode(desc.auxiliary1);
      primitive.host_primitive_type = xenos::PrimitiveType(desc.auxiliary2);
      if (GetOrCreateNativeMslTessellationPipelineState(
              vertex_translation, pixel_translation, primitive, formats,
              rendering)) {
        ++queued;
      }
    } else {
      ++skipped;
    }
  }
  if (blocking && !creation_threads_.empty()) {
    while (IsCreatingPipelines()) {
      creation_request_cond_.notify_one();
      std::this_thread::yield();
    }
  }
  XELOGI("Metal shader storage: queued {} warm pipelines, skipped {}", queued,
         skipped);
}

// ---------------------------------------------------------------------------
// LoadShader
// ---------------------------------------------------------------------------

Shader* MetalPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                       const uint32_t* host_address,
                                       uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  return LoadShader(shader_type, host_address, dword_count, hash);
}

MetalShader* MetalPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                            const uint32_t* host_address,
                                            uint32_t dword_count,
                                            uint64_t hash) {
  auto it = shader_cache_.find(hash);
  if (it != shader_cache_.end()) {
    return it->second.get();
  }

  auto shader = std::make_unique<MetalShader>(shader_type, hash, host_address,
                                              dword_count);

  MetalShader* result = shader.get();
  shader_cache_[hash] = std::move(shader);

  XELOGD("Loaded {} shader ({} dwords, hash {:016X})",
         shader_type == xenos::ShaderType::kVertex ? "vertex" : "pixel",
         dword_count, hash);

  return result;
}

void MetalPipelineCache::SetupShaderBindingLayoutUserUIDs(MetalShader& shader) {
  if (!shader.EnterBindingLayoutUserUIDSetup()) {
    return;
  }

  const std::vector<MetalShader::TextureBinding>& texture_bindings =
      shader.GetTextureBindingsAfterTranslation();
  const size_t texture_binding_count = texture_bindings.size();
  const std::vector<MetalShader::SamplerBinding>& sampler_bindings =
      shader.GetSamplerBindingsAfterTranslation();
  const size_t sampler_binding_count = sampler_bindings.size();

  const size_t texture_binding_layout_bytes =
      texture_binding_count * sizeof(*texture_bindings.data());
  uint64_t texture_binding_layout_hash = 0;
  if (texture_binding_count) {
    texture_binding_layout_hash =
        XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
  }

  uint64_t bindless_sampler_layout_hash = 0;
  if (sampler_binding_count) {
    XXH3_state_t hash_state;
    XXH3_64bits_reset(&hash_state);
    for (size_t i = 0; i < sampler_binding_count; ++i) {
      XXH3_64bits_update(&hash_state,
                         &sampler_bindings[i].bindless_descriptor_index,
                         sizeof(sampler_bindings[i].bindless_descriptor_index));
    }
    bindless_sampler_layout_hash = XXH3_64bits_digest(&hash_state);
  }

  size_t texture_binding_layout_uid = kLayoutUIDEmpty;
  size_t sampler_binding_layout_uid = kLayoutUIDEmpty;
  if (texture_binding_count || sampler_binding_count) {
    std::lock_guard<std::mutex> layouts_lock(layouts_mutex_);
    if (texture_binding_count) {
      auto found_range =
          texture_binding_layout_map_.equal_range(texture_binding_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length == texture_binding_count &&
            !std::memcmp(
                texture_binding_layouts_.data() + it->second.vector_span_offset,
                texture_bindings.data(), texture_binding_layout_bytes)) {
          texture_binding_layout_uid = it->second.uid;
          break;
        }
      }
      if (texture_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(
            kLayoutUIDEmpty == 0,
            "Layout UID is size + 1 because 0 is the empty layout UID");
        texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
        LayoutUID new_uid;
        new_uid.uid = texture_binding_layout_uid;
        new_uid.vector_span_offset = texture_binding_layouts_.size();
        new_uid.vector_span_length = texture_binding_count;
        texture_binding_layouts_.resize(new_uid.vector_span_offset +
                                        texture_binding_count);
        std::memcpy(
            texture_binding_layouts_.data() + new_uid.vector_span_offset,
            texture_bindings.data(), texture_binding_layout_bytes);
        texture_binding_layout_map_.emplace(texture_binding_layout_hash,
                                            new_uid);
      }
    }

    if (sampler_binding_count) {
      auto found_range = bindless_sampler_layout_map_.equal_range(
          bindless_sampler_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length != sampler_binding_count) {
          continue;
        }
        sampler_binding_layout_uid = it->second.uid;
        const uint32_t* vector_bindless_sampler_layout =
            bindless_sampler_layouts_.data() + it->second.vector_span_offset;
        for (size_t i = 0; i < sampler_binding_count; ++i) {
          if (vector_bindless_sampler_layout[i] !=
              sampler_bindings[i].bindless_descriptor_index) {
            sampler_binding_layout_uid = kLayoutUIDEmpty;
            break;
          }
        }
        if (sampler_binding_layout_uid != kLayoutUIDEmpty) {
          break;
        }
      }
      if (sampler_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(
            kLayoutUIDEmpty == 0,
            "Layout UID is size + 1 because 0 is the empty layout UID");
        LayoutUID new_uid;
        new_uid.uid = bindless_sampler_layout_map_.size() + 1;
        sampler_binding_layout_uid = new_uid.uid;
        new_uid.vector_span_offset = bindless_sampler_layouts_.size();
        new_uid.vector_span_length = sampler_binding_count;
        bindless_sampler_layouts_.resize(new_uid.vector_span_offset +
                                         sampler_binding_count);
        uint32_t* vector_bindless_sampler_layout =
            bindless_sampler_layouts_.data() + new_uid.vector_span_offset;
        for (size_t i = 0; i < sampler_binding_count; ++i) {
          vector_bindless_sampler_layout[i] =
              sampler_bindings[i].bindless_descriptor_index;
        }
        bindless_sampler_layout_map_.emplace(bindless_sampler_layout_hash,
                                             new_uid);
      }
    }
  }

  shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);
  shader.SetSamplerBindingLayoutUserUID(sampler_binding_layout_uid);
}

// ---------------------------------------------------------------------------
// EnsureDepthOnlyPixelShader
// ---------------------------------------------------------------------------

bool MetalPipelineCache::EnsureDepthOnlyPixelShader() {
  if (depth_only_pixel_library_) {
    return true;
  }

  if (!shader_translator_ || !dxbc_to_dxil_converter_ ||
      !metal_shader_converter_) {
    XELOGE("Depth-only PS: shader translation not initialized");
    return false;
  }

  std::vector<uint8_t> dxbc_data =
      shader_translator_->CreateDepthOnlyPixelShader();
  if (dxbc_data.empty()) {
    XELOGE("Depth-only PS: failed to create DXBC");
    return false;
  }

  std::vector<uint8_t> dxil_data;
  std::string dxil_error;
  if (!ConvertDxbcToDxil(dxbc_data, dxil_data, &dxil_error)) {
    XELOGE("Depth-only PS: DXBC to DXIL conversion failed: {}", dxil_error);
    return false;
  }

  MetalShaderConversionResult result;
  if (!metal_shader_converter_->ConvertWithStage(MetalShaderStage::kFragment,
                                                 dxil_data, result)) {
    XELOGE("Depth-only PS: DXIL to Metal conversion failed: {}",
           result.error_message);
    return false;
  }

  depth_only_pixel_library_ =
      NewLibraryFromBytes(result.metallib_data, "Depth-only PS");
  if (!depth_only_pixel_library_) {
    return false;
  }
  depth_only_pixel_function_name_ = result.function_name;
  if (depth_only_pixel_function_name_.empty()) {
    XELOGE("Depth-only PS: missing function name");
    return false;
  }
  return true;
}

bool MetalPipelineCache::EnsureNativeDepthOnlyPixelShader() {
  if (native_depth_only_pixel_library_) {
    return true;
  }

  static constexpr char kNativeDepthOnlyPs[] =
      "#include <metal_stdlib>\n"
      "using namespace metal;\n"
      "fragment void xe_native_depth_only_ps() {}\n";

  NS::Error* error = nullptr;
  NS::String* source_string =
      NS::String::string(kNativeDepthOnlyPs, NS::UTF8StringEncoding);
  MTL::CompileOptions* compile_options = nullptr;
  if (cvars::metal_native_msl_debug_validation ||
      !cvars::metal_native_msl_fast_math) {
    compile_options = MTL::CompileOptions::alloc()->init();
    compile_options->setMathMode(MTL::MathModeSafe);
    compile_options->setMathFloatingPointFunctions(
        MTL::MathFloatingPointFunctionsFast);
    if (cvars::metal_native_msl_debug_validation) {
      compile_options->setEnableLogging(true);
      compile_options->setPreserveInvariance(true);
    }
    compile_options->setOptimizationLevel(MTL::LibraryOptimizationLevelDefault);
  }

  auto library_start = std::chrono::steady_clock::now();
  native_depth_only_pixel_library_ =
      device_->newLibrary(source_string, compile_options, &error);
  auto library_end = std::chrono::steady_clock::now();
  if (compile_options) {
    compile_options->release();
  }
  RecordLibraryCreation(std::strlen(kNativeDepthOnlyPs),
                        native_depth_only_pixel_library_ != nullptr,
                        ElapsedMs(library_start, library_end));

  if (!native_depth_only_pixel_library_) {
    XELOGE("Native depth-only PS: MSL compile failed: {}",
           error ? error->localizedDescription()->utf8String()
                 : "unknown error");
    return false;
  }
  if (error) {
    XELOGW("Native depth-only PS: MSL compile diagnostics: {}",
           error->localizedDescription()->utf8String());
  }

  native_depth_only_pixel_function_name_ = "xe_native_depth_only_ps";
  NS::String* function_name = NS::String::string(
      native_depth_only_pixel_function_name_.c_str(), NS::UTF8StringEncoding);
  MTL::Function* function =
      native_depth_only_pixel_library_->newFunction(function_name);
  if (!function) {
    XELOGE("Native depth-only PS: missing function");
    return false;
  }
  function->release();
  return true;
}

// ---------------------------------------------------------------------------
// Shader modification selection
// ---------------------------------------------------------------------------

DxbcShaderTranslator::Modification
MetalPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool native_guest_allowed) const {
  const auto& regs = register_file_;
  const ShaderTranslator* modification_translator = shader_translator_.get();
  if (native_guest_allowed && NativeGuestMslRenderEnabled() &&
      native_msl_translator_) {
    modification_translator = native_msl_translator_.get();
  }

  DxbcShaderTranslator::Modification modification(
      modification_translator->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);

  modification.vertex.output_point_size =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                   xenos::PrimitiveType::kPointList);

  return modification;
}

DxbcShaderTranslator::Modification
MetalPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    bool native_guest_allowed) const {
  const auto& regs = register_file_;
  const ShaderTranslator* modification_translator = shader_translator_.get();
  if (native_guest_allowed && NativeGuestMslRenderEnabled() &&
      native_msl_translator_) {
    modification_translator = native_msl_translator_.get();
  }

  DxbcShaderTranslator::Modification modification(
      modification_translator->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask &
      ~xenos::GetInterpolatorSamplingPattern(
          regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
          regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
          regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                 xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
  if (cvars::depth_float24_convert_in_pixel_shader &&
      normalized_depth_control.z_enable &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
          xenos::DepthRenderTargetFormat::kD24FS8) {
    modification.pixel.depth_stencil_mode =
        cvars::depth_float24_round ? DepthStencilMode::kFloat24Rounding
                                   : DepthStencilMode::kFloat24Truncating;
  } else if (shader.implicit_early_z_write_allowed() &&
             (!shader.writes_color_target(0) ||
              !draw_util::DoesCoverageDependOnAlpha(
                  regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }

  // Check if MIN/MAX blend is used with non-trivial source factors.
  // Fixed-function blend ignores factors for MIN/MAX, but Xbox 360 applies
  // them. If the destination factor is ONE, we can pre-multiply the shader
  // output by the source factor to emulate this. Only RT0 is supported.
  modification.pixel.rt0_blend_rgb_factor_for_premult =
      xenos::BlendFactor::kOne;
  modification.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;

  if (shader.writes_color_target(0)) {
    auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[0]);

    // Pre-multiply by kSrcAlpha for MIN/MAX blend ops when dstFactor is ONE.
    if ((blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.color_destblend == xenos::BlendFactor::kOne) {
      modification.pixel.rt0_blend_rgb_factor_for_premult =
          xenos::BlendFactor::kSrcAlpha;
    }

    if ((blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.alpha_destblend == xenos::BlendFactor::kOne) {
      modification.pixel.rt0_blend_a_factor_for_premult =
          xenos::BlendFactor::kSrcAlpha;
    }
  }

  return modification;
}

// ---------------------------------------------------------------------------
// GetOrCreatePipelineState (standard render pipeline)
// ---------------------------------------------------------------------------

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildPipelineDescription(
    PipelineKind kind, MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key,
    bool force_native_msl_backend) const {
  MetalPipelineDescription desc = {};
  desc.kind = uint32_t(kind);
  desc.vertex_shader_hash = vertex_translation->shader().ucode_data_hash();
  desc.vertex_shader_modification = vertex_translation->shader_modification();
  desc.shader_backend =
      uint32_t(MetalShader::MetalTranslation::Backend::kMetalShaderConverter);
  if (pixel_translation) {
    desc.pixel_shader_hash = pixel_translation->shader().ucode_data_hash();
    desc.pixel_shader_modification = pixel_translation->shader_modification();
  }
  if (force_native_msl_backend ||
      (vertex_translation->is_native_msl() &&
       (!pixel_translation || pixel_translation->is_native_msl()))) {
    desc.shader_backend =
        uint32_t(MetalShader::MetalTranslation::Backend::kNativeMsl);
    desc.vertex_shader_native_msl_texture_sign_key =
        vertex_translation->native_msl_texture_sign_key();
    std::memcpy(desc.vertex_shader_native_msl_texture_sign_component_masks,
                vertex_translation->native_msl_texture_sign_component_masks()
                    .data(),
                xenos::kTextureFetchConstantCount);
    std::memcpy(desc.vertex_shader_native_msl_texture_sign_values,
                vertex_translation->native_msl_texture_sign_values().data(),
                xenos::kTextureFetchConstantCount);
    if (pixel_translation) {
      desc.pixel_shader_native_msl_texture_sign_key =
          pixel_translation->native_msl_texture_sign_key();
      std::memcpy(desc.pixel_shader_native_msl_texture_sign_component_masks,
                  pixel_translation->native_msl_texture_sign_component_masks()
                      .data(),
                  xenos::kTextureFetchConstantCount);
      std::memcpy(desc.pixel_shader_native_msl_texture_sign_values,
                  pixel_translation->native_msl_texture_sign_values().data(),
                  xenos::kTextureFetchConstantCount);
    }
  }
  desc.sample_count = attachment_formats.sample_count;
  desc.depth_format = uint32_t(attachment_formats.depth_format);
  desc.stencil_format = uint32_t(attachment_formats.stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    desc.color_formats[i] = uint32_t(attachment_formats.color_formats[i]);
  }
  desc.normalized_color_mask = rendering_key.normalized_color_mask;
  desc.alpha_to_mask_enable = rendering_key.alpha_to_mask_enable;
  std::memcpy(desc.blendcontrol, rendering_key.blendcontrol,
              sizeof(desc.blendcontrol));
  return desc;
}

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildStandardPipelineDescription(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key,
    bool force_native_msl_backend) const {
  return BuildPipelineDescription(PipelineKind::kStandard, vertex_translation,
                                  pixel_translation, attachment_formats,
                                  rendering_key, force_native_msl_backend);
}

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildGeometryPipelineDescription(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    GeometryShaderKey geometry_shader_key,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) const {
  MetalPipelineDescription desc = BuildPipelineDescription(
      PipelineKind::kGeometry, vertex_translation, pixel_translation,
      attachment_formats, rendering_key);
  desc.auxiliary_hash = geometry_shader_key.key;
  return desc;
}

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildTessellationPipelineDescription(
    MetalShader::MetalTranslation* domain_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    xenos::TessellationMode tessellation_mode,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) const {
  MetalPipelineDescription desc = BuildPipelineDescription(
      PipelineKind::kTessellation, domain_translation, pixel_translation,
      attachment_formats, rendering_key);
  desc.auxiliary0 =
      uint32_t(primitive_processing_result.host_vertex_shader_type);
  desc.auxiliary1 = uint32_t(tessellation_mode);
  desc.auxiliary2 = uint32_t(primitive_processing_result.host_primitive_type);
  return desc;
}

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildNativePrimitiveMeshPipelineDescription(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    NativeMeshPipelineType mesh_type,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) const {
  return BuildPipelineDescription(
      NativePrimitiveMeshPipelineKindForType(mesh_type), vertex_translation,
      pixel_translation, attachment_formats, rendering_key);
}

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildNativeRectangleMeshPipelineDescription(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) const {
  return BuildNativePrimitiveMeshPipelineDescription(
      vertex_translation, pixel_translation,
      NativeMeshPipelineType::kRectangleList, attachment_formats, rendering_key);
}

void MetalPipelineCache::QueueStoredShader(MetalShader& shader) {
  if (!storage_writer_.is_active()) {
    return;
  }
  if (shader.try_set_ucode_storage_index(storage_writer_.storage_index())) {
    shader_storage_file_flush_needed_ = true;
    storage_writer_.QueueShaderWrite(&shader);
  }
}

void MetalPipelineCache::QueueStoredPipeline(
    const MetalPipelineDescription& description, uint64_t description_hash) {
  if (!storage_writer_.is_active()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(stored_pipeline_mutex_);
    if (!stored_pipeline_hashes_.insert(description_hash).second) {
      return;
    }
  }
  MetalPipelineStoredDescription stored = {};
  stored.description_hash = description_hash;
  std::memcpy(&stored.description, &description, sizeof(description));
  pipeline_storage_file_flush_needed_ = true;
  storage_writer_.QueuePipelineWrite(stored);
}

bool MetalPipelineCache::EnsureDxbcTranslationReadyLocked(
    MetalShader::MetalTranslation* translation, const char* stage_name) {
  auto dxbc_translation =
      static_cast<const DxbcShader::DxbcTranslation*>(translation);
  if (translation->is_translated() && dxbc_translation->is_valid() &&
      !translation->translated_binary().empty()) {
    return true;
  }

  if (!shader_translator_->TranslateAnalyzedShader(*translation)) {
    XELOGE("Failed to translate {} shader {:016X} to DXBC",
           stage_name ? stage_name : "unknown",
           translation->shader().ucode_data_hash());
    return false;
  }
  QueueStoredShader(static_cast<MetalShader&>(translation->shader()));
  return true;
}

bool MetalPipelineCache::IsDxbcToDxilConverterAvailable() const {
  return dxbc_to_dxil_converter_ && dxbc_to_dxil_converter_->IsAvailable();
}

void MetalPipelineCache::LogHelperDxilUnavailableOnce(uint64_t key,
                                                      const char* stage_name,
                                                      uint64_t shader_hash) {
  {
    std::lock_guard<std::mutex> lock(helper_dxil_unavailable_log_mutex_);
    if (!helper_dxil_unavailable_log_keys_.insert(key).second) {
      return;
    }
  }
  if (shader_hash) {
    XELOGE(
        "metal_helper_msc: {} shader {:016X} requires DXBC->DXIL helper "
        "conversion, but dxilconv is unavailable; native geometry/tess helper "
        "emulation is not implemented yet",
        stage_name ? stage_name : "guest", shader_hash);
  } else {
    XELOGE(
        "metal_helper_msc: {} requires DXBC->DXIL helper conversion, but "
        "dxilconv is unavailable; native geometry/tess helper emulation is not "
        "implemented yet",
        stage_name ? stage_name : "generated helper stage");
  }
}

bool MetalPipelineCache::EnsureDxilTranslationReady(
    MetalShader::MetalTranslation* translation, const char* stage_name,
    bool helper_stage_pipeline) {
  if (!translation) {
    return true;
  }
  Shader& shader = translation->shader();
  if (!shader.is_ucode_analyzed()) {
    shader.AnalyzeUcode(ucode_disasm_buffer());
  }
  if (helper_stage_pipeline && NativeGuestMslRenderEnabled() &&
      !cvars::metal_native_msl_helper_msc) {
    XELOGE(
        "metal_helper_msc: {} shader {:016X} reached DXIL preparation for a "
        "geometry/tess helper pipeline, but the temporary MSC helper island is "
        "disabled and native helper emulation is not implemented yet",
        stage_name ? stage_name : "guest", shader.ucode_data_hash());
    return false;
  }
  if (helper_stage_pipeline && NativeGuestMslRenderEnabled() &&
      !IsDxbcToDxilConverterAvailable()) {
    const uint64_t stage_hash =
        stage_name ? XXH3_64bits(stage_name, std::strlen(stage_name)) : 0;
    const uint64_t key_data[] = {
        uint64_t(reinterpret_cast<uintptr_t>(translation)), stage_hash};
    LogHelperDxilUnavailableOnce(
        XXH3_64bits(key_data, sizeof(key_data)), stage_name,
        shader.ucode_data_hash());
    return false;
  }
  const uint32_t memexport_eM_mask =
      uint32_t(shader.memexport_eM_written()) |
      uint32_t(shader.memexport_eM_potentially_written_before_end());
  if (memexport_eM_mask && NativeGuestMslRenderEnabled()) {
    if (helper_stage_pipeline) {
      XELOGE(
          "metal_helper_msc: {} shader {:016X} uses memexport (eM mask "
          "0x{:02X}); the temporary geometry/tess MSC helper island cannot "
          "translate memexport until the helper object/mesh path is native MSL",
          stage_name ? stage_name : "guest", shader.ucode_data_hash(),
          memexport_eM_mask);
      return false;
    }
    XELOGE(
        "metal_native_msl_render: {} shader {:016X} uses memexport (eM mask "
        "0x{:02X}); standard guest memexport must stay on the native MSL path",
        stage_name ? stage_name : "guest", shader.ucode_data_hash(),
        memexport_eM_mask);
    return false;
  }
  if (!translation->GetDxilDataCopy().empty()) {
    return true;
  }
  std::lock_guard<std::mutex> lock(shader_translation_mutex_);
  if (!translation->GetDxilDataCopy().empty()) {
    return true;
  }

  if (!EnsureDxbcTranslationReadyLocked(translation, stage_name)) {
    return false;
  }
  std::vector<uint8_t> dxil_data;
  std::string dxil_error;
  if (!ConvertDxbcToDxil(translation->translated_binary(), dxil_data,
                         &dxil_error)) {
    XELOGE("{} shader {:016X}: DXBC to DXIL conversion failed: {}",
           stage_name ? stage_name : "guest",
           translation->shader().ucode_data_hash(), dxil_error);
    return false;
  }
  translation->SetDxilData(std::move(dxil_data));
  return true;
}

bool MetalPipelineCache::TryCompileNativeMslForDiagnostics(
    MetalShader::MetalTranslation* translation, const char* stage_name) {
  if (!translation || !native_msl_translator_) {
    return true;
  }
  if (!translation->TryClaimNativeMslAttempt()) {
    return true;
  }

  if (cvars::async_shader_compilation && !creation_threads_.empty()) {
    PipelineCreationRequest request = {};
    request.type = PipelineCreationRequest::Type::kNativeMslDiagnostics;
    request.native_msl_translation = translation;
    request.native_msl_stage_name = stage_name;
    request.priority = 0;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_queue_.push(request);
    }
    creation_request_cond_.notify_one();
    return true;
  }

  return CompileNativeMslForDiagnostics(translation, stage_name);
}

bool MetalPipelineCache::CompileNativeMslForDiagnostics(
    MetalShader::MetalTranslation* translation, const char* stage_name) {
  if (!translation || !native_msl_translator_) {
    return true;
  }

  Shader& shader = translation->shader();
  MetalShader::MetalTranslation native_translation(
      static_cast<MetalShader&>(shader), translation->modification());
  const uint64_t shader_hash = shader.ucode_data_hash();
  uint64_t translate_us = 0;
  std::string source;
  std::string entry_point_name;
  DxbcShader::TranslationMetadata native_metadata = {};
  size_t source_size = 0;
  {
    std::lock_guard<std::mutex> lock(shader_translation_mutex_);
    if (!shader.is_ucode_analyzed()) {
      shader.AnalyzeUcode(ucode_disasm_buffer());
    }

    const auto translate_start = std::chrono::steady_clock::now();
    bool translated =
        native_msl_translator_->TranslateAnalyzedShader(native_translation);
    const auto translate_end = std::chrono::steady_clock::now();
    translate_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     translate_end - translate_start)
                     .count());
    if (!translated) {
      XELOGW(
          "metal_native_msl: {} shader {:016X} native MSL translation failed "
          "after {} us; {}",
          stage_name ? stage_name : "guest", shader_hash, translate_us,
          cvars::metal_native_msl_fallback_to_msc ? "using DXIL/MSC"
                                                  : "native fallback disabled");
      return cvars::metal_native_msl_fallback_to_msc;
    }

    const std::vector<uint8_t>& source_bytes =
        native_translation.translated_binary();
    source.assign(source_bytes.begin(), source_bytes.end());
    source_size = source_bytes.size();
    entry_point_name = native_msl_translator_->GetEntryPointName();
    native_metadata = native_msl_translator_->GetNativeMetadata();
  }

  uint64_t library_ms = 0;
  bool library_created = false;
  bool library_cache_hit = false;
  const auto compile_start = std::chrono::steady_clock::now();
  bool installed = native_translation.InstallNativeMslSource(
      device_, source, entry_point_name, native_metadata, &library_ms,
      &library_created, &library_cache_hit);
  const auto compile_end = std::chrono::steady_clock::now();
  const uint64_t compile_us =
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                   compile_end - compile_start)
                   .count());
  if (library_created) {
    RecordLibraryCreation(source_size, installed, library_ms);
  }
  if (!installed) {
    XELOGW(
        "metal_native_msl: {} shader {:016X} native MSL compile failed "
        "after {} us; {}",
        stage_name ? stage_name : "guest", shader_hash, compile_us,
        cvars::metal_native_msl_fallback_to_msc ? "using DXIL/MSC"
                                                : "native fallback disabled");
    return cvars::metal_native_msl_fallback_to_msc;
  }

  XELOGI(
      "metal_native_msl: {} shader {:016X} compile-only success "
      "translate_us={} compile_us={} source_bytes={} library_cache_hit={}",
      stage_name ? stage_name : "guest", shader_hash, translate_us, compile_us,
      source_size, library_cache_hit);
  return true;
}

bool MetalPipelineCache::EnsureNativeMslTranslationReady(
    MetalShader::MetalTranslation* translation, const char* stage_name) {
  if (!translation) {
    return true;
  }
  if (translation->is_valid()) {
    if (!translation->is_native_msl()) {
      XELOGE(
          "metal_native_msl: {} shader {:016X} was already compiled through "
          "the DXIL/MSC backend",
          stage_name ? stage_name : "guest",
          translation->shader().ucode_data_hash());
      return false;
    }
    return true;
  }
  if (!native_msl_translator_) {
    XELOGE("metal_native_msl: native MSL translator is not initialized");
    return false;
  }

  uint64_t translate_us = 0;
  uint64_t shader_hash = 0;
  std::string source;
  std::string entry_point_name;
  DxbcShader::TranslationMetadata native_metadata = {};
  size_t source_size = 0;
  {
    std::lock_guard<std::mutex> lock(shader_translation_mutex_);
    if (translation->is_valid()) {
      return translation->is_native_msl();
    }

    Shader& shader = translation->shader();
    shader_hash = shader.ucode_data_hash();
    if (!shader.is_ucode_analyzed()) {
      shader.AnalyzeUcode(ucode_disasm_buffer());
    }

    const auto translate_start = std::chrono::steady_clock::now();
    bool translated =
        native_msl_translator_->TranslateAnalyzedShader(*translation);
    const auto translate_end = std::chrono::steady_clock::now();
    translate_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     translate_end - translate_start)
                     .count());
    if (!translated) {
      XELOGE(
          "metal_native_msl: {} shader {:016X} native MSL translation failed "
          "after {} us",
          stage_name ? stage_name : "guest", shader_hash, translate_us);
      return false;
    }

    const std::vector<uint8_t>& source_bytes = translation->translated_binary();
    source.assign(source_bytes.begin(), source_bytes.end());
    source_size = source_bytes.size();
    entry_point_name = native_msl_translator_->GetEntryPointName();
    native_metadata = native_msl_translator_->GetNativeMetadata();
  }

  uint64_t library_ms = 0;
  bool library_created = false;
  bool library_cache_hit = false;
  const auto compile_start = std::chrono::steady_clock::now();
  bool installed = translation->InstallNativeMslSource(
      device_, source, entry_point_name, native_metadata, &library_ms,
      &library_created, &library_cache_hit);
  const auto compile_end = std::chrono::steady_clock::now();
  const uint64_t compile_us =
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                   compile_end - compile_start)
                   .count());
  if (library_created) {
    RecordLibraryCreation(source_size, installed, library_ms);
  }
  if (!installed) {
    XELOGE(
        "metal_native_msl: {} shader {:016X} native MSL compile failed "
        "after {} us",
        stage_name ? stage_name : "guest", shader_hash, compile_us);
    return false;
  }

  XELOGI(
      "metal_native_msl: {} shader {:016X} native render translation ready "
      "translate_us={} compile_us={} source_bytes={} library_cache_hit={}",
      stage_name ? stage_name : "guest", shader_hash, translate_us, compile_us,
      source_size, library_cache_hit);
  return true;
}

bool MetalPipelineCache::EnsureMetalTranslationReady(
    MetalShader::MetalTranslation* translation, bool native_guest_allowed,
    const char* stage_name, bool helper_stage_pipeline) {
  if (!translation) {
    return true;
  }
  if (translation->is_valid()) {
    if (helper_stage_pipeline && translation->is_native_msl()) {
      XELOGE(
          "metal_helper_msc: {} shader {:016X} is native MSL, but geometry/"
          "tess helper pipelines currently require the MSC helper ABI",
          stage_name ? stage_name : "guest",
          translation->shader().ucode_data_hash());
      return false;
    }
    if (native_guest_allowed && NativeGuestMslRenderEnabled() &&
        !translation->is_native_msl()) {
      XELOGE(
          "metal_native_msl_render: guest shader {:016X} was already "
          "compiled through the MSC backend",
          translation->shader().ucode_data_hash());
      return false;
    }
    return true;
  }

  if (native_guest_allowed && NativeGuestMslRenderEnabled()) {
    return EnsureNativeMslTranslationReady(translation, stage_name);
  }

  // Helper/system stages still use DXIL -> Metal Shader Converter until the
  // remaining generated pipelines are ported to native MSL too.

  if (helper_stage_pipeline && NativeGuestMslRenderEnabled() &&
      !cvars::metal_native_msl_helper_msc) {
    XELOGE(
        "metal_helper_msc: {} shader {:016X} reached a geometry/tess helper "
        "pipeline, but the temporary MSC helper island is disabled and native "
        "helper emulation is not implemented yet",
        stage_name ? stage_name : "guest",
        translation->shader().ucode_data_hash());
    return false;
  }

  if (!EnsureDxilTranslationReady(translation, stage_name,
                                  helper_stage_pipeline)) {
    return false;
  }

  MetalShaderStage stage;
  switch (translation->shader().type()) {
    case xenos::ShaderType::kVertex:
      stage = MetalShaderStage::kVertex;
      break;
    case xenos::ShaderType::kPixel:
      stage = MetalShaderStage::kFragment;
      break;
    default:
      XELOGE("Unsupported Metal shader type {}",
             uint32_t(translation->shader().type()));
      return false;
  }

  MetalStageCompileRequest request;
  request.stage = stage;
  request.dxil_data =
      std::make_shared<std::vector<uint8_t>>(translation->GetDxilDataCopy());
  request.requested_outputs = kMetalStageCompileOutputMetallib;
  std::shared_ptr<const MetalStageCompileResult> result =
      metal_shader_converter_->CompileStage(request);
  if (!result || !result->success) {
    XELOGE("Failed to translate shader {:016X} to Metal: {}",
           translation->shader().ucode_data_hash(),
           result ? result->error_message : "unknown error");
    return false;
  }
  uint64_t library_ms = 0;
  bool library_created = false;
  bool installed = translation->InstallMetal(device_, *result, &library_ms,
                                             &library_created);
  if (library_created) {
    RecordLibraryCreation(result->metallib_data.size(),
                          translation->metal_library() != nullptr, library_ms);
  }
  if (!installed) {
    return false;
  }

  return true;
}

MetalPipelineCache::PipelineHandle*
MetalPipelineCache::GetOrCreatePipelineState(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  if (!vertex_translation) {
    XELOGE("No vertex shader translation");
    return nullptr;
  }
  const bool requires_native_msl = NativeGuestMslRenderEnabled();
  if (requires_native_msl && !native_msl_translator_) {
    XELOGE(
        "metal_native_msl_render: standard guest pipeline requires native MSL, "
        "but the native MSL translator is unavailable");
    return nullptr;
  }
  const bool async_native_msl =
      requires_native_msl && cvars::async_shader_compilation &&
      !creation_threads_.empty();
  if (native_msl_translator_) {
    if (requires_native_msl) {
      if (!async_native_msl) {
        bool native_ready =
            EnsureNativeMslTranslationReady(vertex_translation, "vertex") &&
            EnsureNativeMslTranslationReady(pixel_translation, "pixel");
        if (!native_ready) {
          return nullptr;
        }
      }
    } else {
      if (!TryCompileNativeMslForDiagnostics(vertex_translation, "vertex")) {
        return nullptr;
      }
      if (pixel_translation &&
          !TryCompileNativeMslForDiagnostics(pixel_translation, "pixel")) {
        return nullptr;
      }
    }
  }
  MetalPipelineDescription description = BuildStandardPipelineDescription(
      vertex_translation, pixel_translation, attachment_formats, rendering_key,
      requires_native_msl);
  uint64_t key = XXH3_64bits(&description, sizeof(description));

  // Check cache.
  auto it = pipeline_cache_.find(key);
  if (it != pipeline_cache_.end()) {
    if (it->second->creation_failed.load(std::memory_order_acquire)) {
      return nullptr;
    }
    return it->second.get();
  }

  // Create a new handle.
  auto handle = std::make_unique<PipelineHandle>();
  handle->description_hash = key;
  handle->description = description;
  handle->pending_vertex_translation = vertex_translation;
  handle->pending_pixel_translation = pixel_translation;
  handle->pending_formats = attachment_formats;
  handle->pending_normalized_color_mask = description.normalized_color_mask;
  std::memcpy(handle->pending_blendcontrol, description.blendcontrol,
              sizeof(description.blendcontrol));
  handle->pending_alpha_to_mask = (description.alpha_to_mask_enable != 0);
  handle->storage_write_pending = true;

  PipelineHandle* raw_handle = handle.get();
  pipeline_cache_.emplace(key, std::move(handle));

  // Async path: enqueue for background compilation.
  if (cvars::async_shader_compilation && !creation_threads_.empty()) {
    PipelineCreationRequest request = {};
    request.type = PipelineCreationRequest::Type::kPipeline;
    request.handle = raw_handle;
    request.priority = raw_handle->priority;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_queue_.push(request);
    }
    creation_request_cond_.notify_one();
    return raw_handle;
  }

  // Synchronous path: create immediately.
  MTL::RenderPipelineState* pipeline = CreatePipelineFromHandle(raw_handle);
  raw_handle->state.store(pipeline, std::memory_order_release);
  raw_handle->pending_vertex_translation = nullptr;
  raw_handle->pending_pixel_translation = nullptr;

  if (!pipeline) {
    raw_handle->creation_failed.store(true, std::memory_order_release);
    return nullptr;
  }

  if (raw_handle->storage_write_pending) {
    QueueStoredShader(static_cast<MetalShader&>(vertex_translation->shader()));
    if (pixel_translation) {
      QueueStoredShader(static_cast<MetalShader&>(pixel_translation->shader()));
    }
    QueueStoredPipeline(raw_handle->description, raw_handle->description_hash);
    raw_handle->storage_write_pending = false;
  }

  return raw_handle;
}

// ---------------------------------------------------------------------------
// CreatePipelineFromHandle (builds a MTL::RenderPipelineState from a handle)
// ---------------------------------------------------------------------------

MTL::RenderPipelineState* MetalPipelineCache::CreatePipelineFromHandle(
    const PipelineHandle* handle) {
  MetalShader::MetalTranslation* vertex_translation =
      handle->pending_vertex_translation;
  MetalShader::MetalTranslation* pixel_translation =
      handle->pending_pixel_translation;
  const PipelineAttachmentFormats& formats = handle->pending_formats;

  if (!EnsureMetalTranslationReady(vertex_translation) ||
      !EnsureMetalTranslationReady(pixel_translation)) {
    return nullptr;
  }

  // Create pipeline descriptor.
  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();

  desc->setVertexFunction(vertex_translation->metal_function());

  if (pixel_translation && pixel_translation->metal_function()) {
    desc->setFragmentFunction(pixel_translation->metal_function());
  }

  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(
        formats.color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(formats.depth_format);
  desc->setStencilAttachmentPixelFormat(formats.stencil_format);
  desc->setSampleCount(formats.sample_count);
  desc->setAlphaToCoverageEnabled(false);

  ApplyBlendStateToDescriptor(desc->colorAttachments(),
                              handle->pending_normalized_color_mask,
                              handle->pending_blendcontrol);

  // Configure vertex fetch layout for MSC stage-in.
  const Shader& vertex_shader_ref = vertex_translation->shader();
  const auto& vertex_bindings = vertex_shader_ref.vertex_bindings();
  if (!ShaderUsesVertexFetch(vertex_shader_ref) && !vertex_bindings.empty()) {
    auto map_vertex_format =
        [](const ParsedVertexFetchInstruction::Attributes& attrs)
        -> MTL::VertexFormat {
      using xenos::VertexFormat;
      switch (attrs.data_format) {
        case VertexFormat::k_8_8_8_8:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatChar4
                                   : MTL::VertexFormatUChar4;
          }
          return attrs.is_signed ? MTL::VertexFormatChar4Normalized
                                 : MTL::VertexFormatUChar4Normalized;
        case VertexFormat::k_2_10_10_10:
          return attrs.is_signed ? MTL::VertexFormatInt1010102Normalized
                                 : MTL::VertexFormatUInt1010102Normalized;
        case VertexFormat::k_10_11_11:
        case VertexFormat::k_11_11_10:
          return MTL::VertexFormatFloatRG11B10;
        case VertexFormat::k_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatShort2
                                   : MTL::VertexFormatUShort2;
          }
          return attrs.is_signed ? MTL::VertexFormatShort2Normalized
                                 : MTL::VertexFormatUShort2Normalized;
        case VertexFormat::k_16_16_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatShort4
                                   : MTL::VertexFormatUShort4;
          }
          return attrs.is_signed ? MTL::VertexFormatShort4Normalized
                                 : MTL::VertexFormatUShort4Normalized;
        case VertexFormat::k_16_16_FLOAT:
          return MTL::VertexFormatHalf2;
        case VertexFormat::k_16_16_16_16_FLOAT:
          return MTL::VertexFormatHalf4;
        case VertexFormat::k_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatInt
                                   : MTL::VertexFormatUInt;
          }
          return MTL::VertexFormatFloat;
        case VertexFormat::k_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatInt2
                                   : MTL::VertexFormatUInt2;
          }
          return MTL::VertexFormatFloat2;
        case VertexFormat::k_32_FLOAT:
          return MTL::VertexFormatFloat;
        case VertexFormat::k_32_32_FLOAT:
          return MTL::VertexFormatFloat2;
        case VertexFormat::k_32_32_32_FLOAT:
          return MTL::VertexFormatFloat3;
        case VertexFormat::k_32_32_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatInt4
                                   : MTL::VertexFormatUInt4;
          }
          return MTL::VertexFormatFloat4;
        case VertexFormat::k_32_32_32_32_FLOAT:
          return MTL::VertexFormatFloat4;
        default:
          return MTL::VertexFormatInvalid;
      }
    };

    MTL::VertexDescriptor* vertex_desc = MTL::VertexDescriptor::alloc()->init();

    uint32_t attr_index = static_cast<uint32_t>(kIRStageInAttributeStartIndex);
    for (const auto& binding : vertex_bindings) {
      uint64_t buffer_index =
          kIRVertexBufferBindPoint + uint64_t(binding.binding_index);
      bool used_any_attribute = false;

      for (const auto& attr : binding.attributes) {
        MTL::VertexFormat fmt = map_vertex_format(attr.fetch_instr.attributes);
        if (fmt == MTL::VertexFormatInvalid) {
          ++attr_index;
          continue;
        }
        auto attr_desc = vertex_desc->attributes()->object(attr_index);
        attr_desc->setFormat(fmt);
        attr_desc->setOffset(
            static_cast<NS::UInteger>(attr.fetch_instr.attributes.offset * 4));
        attr_desc->setBufferIndex(static_cast<NS::UInteger>(buffer_index));
        used_any_attribute = true;
        ++attr_index;
      }

      if (used_any_attribute) {
        auto layout = vertex_desc->layouts()->object(buffer_index);
        layout->setStride(binding.stride_words * 4);
        layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        layout->setStepRate(1);
      }
    }

    desc->setVertexDescriptor(vertex_desc);
    vertex_desc->release();
  }

  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    if (pipeline_binary_archive_) {
      NS::Array* archives = NS::Array::array(pipeline_binary_archive_);
      desc->setBinaryArchives(archives);
      NS::Error* archive_error = nullptr;
      if (pipeline_binary_archive_->addRenderPipelineFunctions(
              desc, &archive_error)) {
        pipeline_binary_archive_dirty_ = true;
      }
    }
  }

  // Create pipeline state.
  NS::Error* error = nullptr;
  auto pipeline_start = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);
  auto pipeline_end = std::chrono::steady_clock::now();
  RecordRenderPipelineCreation(pipeline != nullptr,
                               ElapsedMs(pipeline_start, pipeline_end));
  desc->release();

  if (!pipeline) {
    if (error) {
      XELOGE("Failed to create pipeline state: {}",
             error->localizedDescription()->utf8String());
    } else {
      XELOGE("Failed to create pipeline state (unknown error)");
    }
    return nullptr;
  }

  return pipeline;
}

// ---------------------------------------------------------------------------
// CreationThread (background pipeline compilation)
// ---------------------------------------------------------------------------

void MetalPipelineCache::CreationThread(size_t thread_index) {
  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

  while (true) {
    PipelineCreationRequest request;
    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      while (creation_queue_.empty() && !creation_threads_shutdown_) {
        creation_request_cond_.wait(lock);
      }
      if (creation_threads_shutdown_ && creation_queue_.empty()) {
        break;
      }
      request = creation_queue_.top();
      creation_queue_.pop();
      ++creation_threads_busy_;
    }

    // Drain and recreate autorelease pool per pipeline.
    pool->drain();
    pool = NS::AutoreleasePool::alloc()->init();

    if (request.type ==
        PipelineCreationRequest::Type::kNativeMslDiagnostics) {
      CompileNativeMslForDiagnostics(request.native_msl_translation,
                                     request.native_msl_stage_name);
      --creation_threads_busy_;
      continue;
    }

    // Create the pipeline.
    PipelineHandle* handle = request.handle;
    MTL::RenderPipelineState* pipeline = CreatePipelineFromHandle(handle);
    handle->state.store(pipeline, std::memory_order_release);
    if (!pipeline) {
      handle->creation_failed.store(true, std::memory_order_release);
    }
    if (pipeline && handle->storage_write_pending) {
      if (handle->pending_vertex_translation) {
        QueueStoredShader(static_cast<MetalShader&>(
            handle->pending_vertex_translation->shader()));
      }
      if (handle->pending_pixel_translation) {
        QueueStoredShader(static_cast<MetalShader&>(
            handle->pending_pixel_translation->shader()));
      }
      QueueStoredPipeline(handle->description, handle->description_hash);
      handle->storage_write_pending = false;
    }

    // Clear pending data.
    handle->pending_vertex_translation = nullptr;
    handle->pending_pixel_translation = nullptr;

    --creation_threads_busy_;
  }

  pool->drain();
}

// ---------------------------------------------------------------------------
// IsCreatingPipelines
// ---------------------------------------------------------------------------

bool MetalPipelineCache::IsCreatingPipelines() {
  if (creation_threads_.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(creation_request_lock_);
  return !creation_queue_.empty() || creation_threads_busy_ != 0;
}

MetalStageCompileCacheStats MetalPipelineCache::GetAndResetStageCompileStats() {
  if (!metal_shader_converter_ ||
      !metal_shader_converter_->stage_compile_cache()) {
    return {};
  }
  return metal_shader_converter_->stage_compile_cache()->GetAndResetStats();
}

MetalPipelineRuntimeStats MetalPipelineCache::GetAndResetRuntimeStats() {
  MetalPipelineRuntimeStats stats;
  stats.dxil_convert_requests =
      dxil_convert_requests_.exchange(0, std::memory_order_relaxed);
  stats.dxil_cache_hits =
      dxil_cache_hits_.exchange(0, std::memory_order_relaxed);
  stats.dxil_cache_misses =
      dxil_cache_misses_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_failures =
      dxil_convert_failures_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_dxbc_bytes =
      dxil_convert_dxbc_bytes_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_dxil_bytes =
      dxil_convert_dxil_bytes_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_ms_total =
      dxil_convert_ms_total_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_ms_max =
      dxil_convert_ms_max_.exchange(0, std::memory_order_relaxed);
  stats.library_requests =
      library_requests_.exchange(0, std::memory_order_relaxed);
  stats.library_failures =
      library_failures_.exchange(0, std::memory_order_relaxed);
  stats.library_bytes = library_bytes_.exchange(0, std::memory_order_relaxed);
  stats.library_ms_total =
      library_ms_total_.exchange(0, std::memory_order_relaxed);
  stats.library_ms_max = library_ms_max_.exchange(0, std::memory_order_relaxed);
  stats.render_pipeline_requests =
      render_pipeline_requests_.exchange(0, std::memory_order_relaxed);
  stats.render_pipeline_failures =
      render_pipeline_failures_.exchange(0, std::memory_order_relaxed);
  stats.render_pipeline_ms_total =
      render_pipeline_ms_total_.exchange(0, std::memory_order_relaxed);
  stats.render_pipeline_ms_max =
      render_pipeline_ms_max_.exchange(0, std::memory_order_relaxed);
  return stats;
}

// ---------------------------------------------------------------------------
// GetOrCreateGeometryPipelineState
// ---------------------------------------------------------------------------

MetalPipelineCache::GeometryPipelineState*
MetalPipelineCache::GetOrCreateGeometryPipelineState(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    GeometryShaderKey geometry_shader_key,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  if (!vertex_translation) {
    XELOGE("No valid vertex shader translation for geometry pipeline");
    return nullptr;
  }
  bool use_fallback_pixel_shader = (pixel_translation == nullptr);
  MTL::Library* pixel_library =
      use_fallback_pixel_shader ? nullptr : pixel_translation->metal_library();
  const char* pixel_function = use_fallback_pixel_shader
                                   ? nullptr
                                   : pixel_translation->function_name().c_str();
  if (use_fallback_pixel_shader) {
    if (!EnsureDepthOnlyPixelShader()) {
      XELOGE("Geometry pipeline: failed to create depth-only PS");
      return nullptr;
    }
    pixel_library = depth_only_pixel_library_;
    pixel_function = depth_only_pixel_function_name_.c_str();
  } else if (!pixel_library) {
    XELOGE("No valid pixel shader translation for geometry pipeline");
    return nullptr;
  }

  uint32_t sample_count = attachment_formats.sample_count;
  MTL::PixelFormat color_formats[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_formats[i] = attachment_formats.color_formats[i];
  }
  MTL::PixelFormat depth_format = attachment_formats.depth_format;
  MTL::PixelFormat stencil_format = attachment_formats.stencil_format;

  struct GeometryPipelineKey {
    const void* vs;
    const void* ps;
    uint32_t geometry_key;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};

  key_data.vs = vertex_translation;
  key_data.ps = use_fallback_pixel_shader
                    ? static_cast<const void*>(pixel_library)
                    : static_cast<const void*>(pixel_translation);
  key_data.geometry_key = geometry_shader_key.key;
  key_data.sample_count = sample_count;
  key_data.depth_format = uint32_t(depth_format);
  key_data.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(color_formats[i]);
  }
  key_data.normalized_color_mask = rendering_key.normalized_color_mask;
  key_data.alpha_to_mask_enable = rendering_key.alpha_to_mask_enable;
  std::memcpy(key_data.blendcontrol, rendering_key.blendcontrol,
              sizeof(key_data.blendcontrol));
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));

  auto it = geometry_pipeline_cache_.find(key);
  if (it != geometry_pipeline_cache_.end()) {
    return &it->second;
  }

  if (!generated_stages_) {
    XELOGE("Geometry pipeline: generated stage cache is not initialized");
    return nullptr;
  }

  auto* vertex_stage = generated_stages_->GetGeometryVertexStage(
      vertex_translation, geometry_shader_key);
  if (!vertex_stage || !vertex_stage->library ||
      !vertex_stage->stage_in_library) {
    return nullptr;
  }
  auto* geometry_stage =
      generated_stages_->GetGeometryShaderStage(geometry_shader_key);
  if (!geometry_stage || !geometry_stage->library) {
    return nullptr;
  }

  MTL::MeshRenderPipelineDescriptor* desc =
      MTL::MeshRenderPipelineDescriptor::alloc()->init();

  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(depth_format);
  desc->setStencilAttachmentPixelFormat(stencil_format);
  desc->setRasterSampleCount(sample_count);
  desc->setAlphaToCoverageEnabled(false);

  ApplyBlendStateToDescriptor(desc->colorAttachments(),
                              key_data.normalized_color_mask,
                              key_data.blendcontrol);
  if (!vertex_stage->vertex_output_size_in_bytes ||
      !geometry_stage->max_input_primitives_per_mesh_threadgroup) {
    XELOGE(
        "Geometry pipeline: invalid reflection (vs_output={}, gs_max_input={})",
        vertex_stage->vertex_output_size_in_bytes,
        geometry_stage->max_input_primitives_per_mesh_threadgroup);
    return nullptr;
  }

  IRGeometryEmulationPipelineDescriptor ir_desc = {};
  ir_desc.stageInLibrary = vertex_stage->stage_in_library;
  ir_desc.vertexLibrary = vertex_stage->library;
  ir_desc.vertexFunctionName = vertex_stage->function_name.c_str();
  ir_desc.geometryLibrary = geometry_stage->library;
  ir_desc.geometryFunctionName = geometry_stage->function_name.c_str();
  ir_desc.fragmentLibrary = pixel_library;
  ir_desc.fragmentFunctionName = pixel_function;
  ir_desc.basePipelineDescriptor = desc;
  ir_desc.pipelineConfig.gsVertexSizeInBytes =
      vertex_stage->vertex_output_size_in_bytes;
  ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup =
      geometry_stage->max_input_primitives_per_mesh_threadgroup;

  NS::Error* error = nullptr;
  auto pipeline_start = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline =
      IRRuntimeNewGeometryEmulationPipeline(device_, &ir_desc, &error);
  auto pipeline_end = std::chrono::steady_clock::now();
  RecordRenderPipelineCreation(pipeline != nullptr,
                               ElapsedMs(pipeline_start, pipeline_end));
  desc->release();

  if (!pipeline) {
    XELOGE(
        "Failed to create geometry pipeline state: {}",
        error ? error->localizedDescription()->utf8String() : "unknown error");
    XELOGE(
        "Geometry pipeline details: vs_fn='{}' gs_fn='{}' ps_fn='{}' "
        "depth_format={} stencil_format={} samples={}",
        vertex_stage->function_name, geometry_stage->function_name,
        pixel_function ? pixel_function : "<null>", uint32_t(depth_format),
        uint32_t(stencil_format), sample_count);
    LogMetalErrorDetails("Geometry pipeline error", error);
    return nullptr;
  }

  GeometryPipelineState state;
  state.pipeline = pipeline;
  state.gs_vertex_size_in_bytes = ir_desc.pipelineConfig.gsVertexSizeInBytes;
  state.gs_max_input_primitives_per_mesh_threadgroup =
      ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup;

  auto [inserted_it, inserted] =
      geometry_pipeline_cache_.emplace(key, std::move(state));
  MetalPipelineDescription stored_description =
      BuildGeometryPipelineDescription(vertex_translation, pixel_translation,
                                       geometry_shader_key, attachment_formats,
                                       rendering_key);
  QueueStoredShader(static_cast<MetalShader&>(vertex_translation->shader()));
  if (pixel_translation) {
    QueueStoredShader(static_cast<MetalShader&>(pixel_translation->shader()));
  }
  QueueStoredPipeline(
      stored_description,
      XXH3_64bits(&stored_description, sizeof(stored_description)));
  return &inserted_it->second;
}

// ---------------------------------------------------------------------------
// GetOrCreateNativeMslPrimitiveMeshPipelineState
// ---------------------------------------------------------------------------

MetalPipelineCache::NativeMeshPipelineState*
MetalPipelineCache::GetOrCreateNativeMslPrimitiveMeshPipelineState(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    NativeMeshPipelineType mesh_type,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  const char* mesh_label = NativePrimitiveMeshLabel(mesh_type);
  const char* mesh_entry_point = NativePrimitiveMeshEntryPoint(mesh_type);
  if (!vertex_translation || !vertex_translation->is_native_msl() ||
      !vertex_translation->metal_library()) {
    XELOGE("Native {} mesh pipeline requires a native MSL vertex shader",
           mesh_label);
    return nullptr;
  }
  if (pixel_translation && (!pixel_translation->is_native_msl() ||
                            !pixel_translation->metal_function())) {
    XELOGE("Native {} mesh pipeline requires a native MSL pixel shader",
           mesh_label);
    return nullptr;
  }

  bool use_fallback_pixel_shader = (pixel_translation == nullptr);
  MTL::Function* pixel_function =
      use_fallback_pixel_shader ? nullptr : pixel_translation->metal_function();
  if (use_fallback_pixel_shader) {
    if (cvars::metal_native_msl_render) {
      // Mesh render pipelines still require a fragment function. Keep the
      // no-guest-PS case native so primitive expansion does not touch the
      // generated helper island.
      if (!EnsureNativeDepthOnlyPixelShader()) {
        XELOGE(
            "Native {} mesh pipeline: failed to create native depth-only PS",
            mesh_label);
        return nullptr;
      }
      NS::String* fallback_name = NS::String::string(
          native_depth_only_pixel_function_name_.c_str(),
          NS::UTF8StringEncoding);
      pixel_function =
          native_depth_only_pixel_library_->newFunction(fallback_name);
      if (!pixel_function) {
        XELOGE(
            "Native {} mesh pipeline: missing native depth-only PS function",
            mesh_label);
        return nullptr;
      }
    } else {
      if (!EnsureDepthOnlyPixelShader()) {
        XELOGE("Native {} mesh pipeline: failed to create depth-only PS",
               mesh_label);
        return nullptr;
      }
      NS::String* fallback_name = NS::String::string(
          depth_only_pixel_function_name_.c_str(), NS::UTF8StringEncoding);
      pixel_function = depth_only_pixel_library_->newFunction(fallback_name);
      if (!pixel_function) {
        XELOGE("Native {} mesh pipeline: missing depth-only PS function",
               mesh_label);
        return nullptr;
      }
    }
  }

  MetalPipelineDescription description =
      BuildNativePrimitiveMeshPipelineDescription(
          vertex_translation, pixel_translation, mesh_type, attachment_formats,
          rendering_key);
  uint64_t key = XXH3_64bits(&description, sizeof(description));
  auto it = native_mesh_pipeline_cache_.find(key);
  if (it != native_mesh_pipeline_cache_.end()) {
    if (use_fallback_pixel_shader && pixel_function) {
      pixel_function->release();
    }
    return &it->second;
  }

  NS::String* mesh_name =
      NS::String::string(mesh_entry_point, NS::UTF8StringEncoding);
  MTL::Function* mesh_function =
      vertex_translation->metal_library()->newFunction(mesh_name);
  if (!mesh_function) {
    XELOGE(
        "Native {} mesh pipeline: vertex shader {:016X} does not export {}",
        mesh_label, vertex_translation->shader().ucode_data_hash(),
        mesh_entry_point);
    if (use_fallback_pixel_shader && pixel_function) {
      pixel_function->release();
    }
    return nullptr;
  }

  MTL::MeshRenderPipelineDescriptor* desc =
      MTL::MeshRenderPipelineDescriptor::alloc()->init();
  desc->setMeshFunction(mesh_function);
  if (pixel_function) {
    desc->setFragmentFunction(pixel_function);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(
        attachment_formats.color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(attachment_formats.depth_format);
  desc->setStencilAttachmentPixelFormat(attachment_formats.stencil_format);
  desc->setRasterSampleCount(attachment_formats.sample_count);
  desc->setAlphaToCoverageEnabled(false);
  desc->setMaxTotalThreadsPerMeshThreadgroup(1);
  desc->setRequiredThreadsPerMeshThreadgroup(MTL::Size::Make(1, 1, 1));

  ApplyBlendStateToDescriptor(desc->colorAttachments(),
                              rendering_key.normalized_color_mask,
                              rendering_key.blendcontrol);

  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    if (pipeline_binary_archive_) {
      NS::Array* archives = NS::Array::array(pipeline_binary_archive_);
      desc->setBinaryArchives(archives);
      NS::Error* archive_error = nullptr;
      if (pipeline_binary_archive_->addMeshRenderPipelineFunctions(
              desc, &archive_error)) {
        pipeline_binary_archive_dirty_ = true;
      }
    }
  }

  NS::Error* error = nullptr;
  auto pipeline_start = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline = device_->newRenderPipelineState(
      desc, MTL::PipelineOptionNone, nullptr, &error);
  auto pipeline_end = std::chrono::steady_clock::now();
  RecordRenderPipelineCreation(pipeline != nullptr,
                               ElapsedMs(pipeline_start, pipeline_end));
  desc->release();
  mesh_function->release();
  if (use_fallback_pixel_shader && pixel_function) {
    pixel_function->release();
  }

  if (!pipeline) {
    XELOGE("Failed to create native {} mesh pipeline state: {}", mesh_label,
           error ? error->localizedDescription()->utf8String()
                 : "unknown error");
    LogMetalErrorDetails("Native primitive mesh pipeline error", error);
    return nullptr;
  }

  NativeMeshPipelineState state;
  state.pipeline = pipeline;
  state.type = mesh_type;
  auto [inserted_it, inserted] =
      native_mesh_pipeline_cache_.emplace(key, std::move(state));
  QueueStoredShader(static_cast<MetalShader&>(vertex_translation->shader()));
  if (pixel_translation) {
    QueueStoredShader(static_cast<MetalShader&>(pixel_translation->shader()));
  }
  QueueStoredPipeline(description, key);
  return &inserted_it->second;
}

MetalPipelineCache::NativeMeshPipelineState*
MetalPipelineCache::GetOrCreateNativeMslRectanglePipelineState(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  return GetOrCreateNativeMslPrimitiveMeshPipelineState(
      vertex_translation, pixel_translation,
      NativeMeshPipelineType::kRectangleList, attachment_formats, rendering_key);
}

// ---------------------------------------------------------------------------
// GetOrCreateTessellationPipelineState
// ---------------------------------------------------------------------------

MetalPipelineCache::TessellationPipelineState*
MetalPipelineCache::GetOrCreateTessellationPipelineState(
    MetalShader::MetalTranslation* domain_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  if (!domain_translation) {
    XELOGE("No valid domain shader translation for tessellation pipeline");
    return nullptr;
  }
  bool use_fallback_pixel_shader = (pixel_translation == nullptr);
  MTL::Library* pixel_library =
      use_fallback_pixel_shader ? nullptr : pixel_translation->metal_library();
  const char* pixel_function = use_fallback_pixel_shader
                                   ? nullptr
                                   : pixel_translation->function_name().c_str();
  if (use_fallback_pixel_shader) {
    if (!EnsureDepthOnlyPixelShader()) {
      XELOGE("Tessellation pipeline: failed to create depth-only PS");
      return nullptr;
    }
    pixel_library = depth_only_pixel_library_;
    pixel_function = depth_only_pixel_function_name_.c_str();
  } else if (!pixel_library) {
    XELOGE("No valid pixel shader translation for tessellation pipeline");
    return nullptr;
  }

  uint32_t sample_count = attachment_formats.sample_count;
  MTL::PixelFormat color_formats[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_formats[i] = attachment_formats.color_formats[i];
  }
  MTL::PixelFormat depth_format = attachment_formats.depth_format;
  MTL::PixelFormat stencil_format = attachment_formats.stencil_format;

  struct TessellationPipelineKey {
    const void* ds;
    const void* ps;
    uint32_t host_vs_type;
    uint32_t tessellation_mode;
    uint32_t host_prim;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};

  key_data.ds = domain_translation;
  key_data.ps = use_fallback_pixel_shader
                    ? static_cast<const void*>(pixel_library)
                    : static_cast<const void*>(pixel_translation);
  key_data.host_vs_type =
      uint32_t(primitive_processing_result.host_vertex_shader_type);
  key_data.tessellation_mode =
      uint32_t(primitive_processing_result.tessellation_mode);
  key_data.host_prim =
      uint32_t(primitive_processing_result.host_primitive_type);
  key_data.sample_count = sample_count;
  key_data.depth_format = uint32_t(depth_format);
  key_data.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(color_formats[i]);
  }
  key_data.normalized_color_mask = rendering_key.normalized_color_mask;
  key_data.alpha_to_mask_enable = rendering_key.alpha_to_mask_enable;
  std::memcpy(key_data.blendcontrol, rendering_key.blendcontrol,
              sizeof(key_data.blendcontrol));
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));

  auto it = tessellation_pipeline_cache_.find(key);
  if (it != tessellation_pipeline_cache_.end()) {
    return &it->second;
  }

  xenos::TessellationMode tessellation_mode =
      primitive_processing_result.tessellation_mode;

  if (!generated_stages_) {
    XELOGE("Tessellation pipeline: generated stage cache is not initialized");
    return nullptr;
  }

  auto* vertex_stage = generated_stages_->GetTessellationVertexStage(
      domain_translation, tessellation_mode);
  if (!vertex_stage || !vertex_stage->library ||
      !vertex_stage->stage_in_library) {
    return nullptr;
  }

  auto* hull_stage = generated_stages_->GetTessellationHullStage(
      primitive_processing_result, tessellation_mode);
  if (!hull_stage || !hull_stage->library) {
    return nullptr;
  }

  auto* domain_stage =
      generated_stages_->GetTessellationDomainStage(domain_translation);
  if (!domain_stage || !domain_stage->library) {
    return nullptr;
  }

  IRRuntimeTessellatorOutputPrimitive output_primitive =
      IRRuntimeTessellatorOutputUndefined;
  switch (hull_stage->reflection.hs_tessellator_output_primitive) {
    case IRRuntimeTessellatorOutputPoint:
      output_primitive = IRRuntimeTessellatorOutputPoint;
      break;
    case IRRuntimeTessellatorOutputLine:
      output_primitive = IRRuntimeTessellatorOutputLine;
      break;
    case IRRuntimeTessellatorOutputTriangleCW:
      output_primitive = IRRuntimeTessellatorOutputTriangleCW;
      break;
    case IRRuntimeTessellatorOutputTriangleCCW:
      output_primitive = IRRuntimeTessellatorOutputTriangleCCW;
      break;
    default:
      XELOGE("Tessellation pipeline: unsupported tessellator output {}",
             hull_stage->reflection.hs_tessellator_output_primitive);
      return nullptr;
  }

  IRRuntimePrimitiveType geometry_primitive = IRRuntimePrimitiveTypeTriangle;
  const char* geometry_function = kIRTrianglePassthroughGeometryShader;
  switch (output_primitive) {
    case IRRuntimeTessellatorOutputPoint:
      geometry_primitive = IRRuntimePrimitiveTypePoint;
      geometry_function = kIRPointPassthroughGeometryShader;
      break;
    case IRRuntimeTessellatorOutputLine:
      geometry_primitive = IRRuntimePrimitiveTypeLine;
      geometry_function = kIRLinePassthroughGeometryShader;
      break;
    case IRRuntimeTessellatorOutputTriangleCW:
    case IRRuntimeTessellatorOutputTriangleCCW:
      geometry_primitive = IRRuntimePrimitiveTypeTriangle;
      geometry_function = kIRTrianglePassthroughGeometryShader;
      break;
    default:
      break;
  }

  if (!IRRuntimeValidateTessellationPipeline(
          output_primitive, geometry_primitive,
          hull_stage->reflection.hs_output_control_point_size,
          domain_stage->reflection.ds_input_control_point_size,
          hull_stage->reflection.hs_patch_constants_size,
          domain_stage->reflection.ds_patch_constants_size,
          hull_stage->reflection.hs_output_control_point_count,
          domain_stage->reflection.ds_input_control_point_count)) {
    XELOGE("Tessellation pipeline: validation failed for HS/DS pairing");
    return nullptr;
  }

  MTL::MeshRenderPipelineDescriptor* desc =
      MTL::MeshRenderPipelineDescriptor::alloc()->init();
  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(depth_format);
  desc->setStencilAttachmentPixelFormat(stencil_format);
  desc->setRasterSampleCount(sample_count);
  desc->setAlphaToCoverageEnabled(false);

  ApplyBlendStateToDescriptor(desc->colorAttachments(),
                              key_data.normalized_color_mask,
                              key_data.blendcontrol);
  IRGeometryTessellationEmulationPipelineDescriptor ir_desc = {};
  ir_desc.stageInLibrary = vertex_stage->stage_in_library;
  ir_desc.vertexLibrary = vertex_stage->library;
  ir_desc.vertexFunctionName = vertex_stage->function_name.c_str();
  ir_desc.hullLibrary = hull_stage->library;
  ir_desc.hullFunctionName = hull_stage->function_name.c_str();
  ir_desc.domainLibrary = domain_stage->library;
  ir_desc.domainFunctionName = domain_stage->function_name.c_str();
  ir_desc.geometryLibrary = nullptr;
  ir_desc.geometryFunctionName = geometry_function;
  ir_desc.fragmentLibrary = pixel_library;
  ir_desc.fragmentFunctionName = pixel_function;
  ir_desc.basePipelineDescriptor = desc;
  ir_desc.pipelineConfig.outputPrimitiveType = output_primitive;
  ir_desc.pipelineConfig.vsOutputSizeInBytes =
      vertex_stage->vertex_output_size_in_bytes;
  ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup =
      domain_stage->reflection.ds_max_input_prims_per_mesh_threadgroup;
  ir_desc.pipelineConfig.hsMaxPatchesPerObjectThreadgroup =
      hull_stage->reflection.hs_max_patches_per_object_threadgroup;
  ir_desc.pipelineConfig.hsInputControlPointCount =
      hull_stage->reflection.hs_input_control_point_count;
  ir_desc.pipelineConfig.hsMaxObjectThreadsPerThreadgroup =
      hull_stage->reflection.hs_max_object_threads_per_patch;
  ir_desc.pipelineConfig.hsMaxTessellationFactor =
      hull_stage->reflection.hs_max_tessellation_factor;
  ir_desc.pipelineConfig.gsInstanceCount = 1;

  if (!ir_desc.pipelineConfig.vsOutputSizeInBytes ||
      !ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup ||
      !ir_desc.pipelineConfig.hsMaxPatchesPerObjectThreadgroup ||
      !ir_desc.pipelineConfig.hsInputControlPointCount ||
      !ir_desc.pipelineConfig.hsMaxObjectThreadsPerThreadgroup) {
    XELOGE(
        "Tessellation pipeline: invalid reflection values (vs_output={}, "
        "gs_max_input={}, hs_patches={}, hs_cp_count={}, hs_threads={})",
        ir_desc.pipelineConfig.vsOutputSizeInBytes,
        ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
        ir_desc.pipelineConfig.hsMaxPatchesPerObjectThreadgroup,
        ir_desc.pipelineConfig.hsInputControlPointCount,
        ir_desc.pipelineConfig.hsMaxObjectThreadsPerThreadgroup);
    desc->release();
    return nullptr;
  }

  NS::Error* error = nullptr;
  auto pipeline_start = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline =
      IRRuntimeNewGeometryTessellationEmulationPipeline(device_, &ir_desc,
                                                        &error);
  auto pipeline_end = std::chrono::steady_clock::now();
  RecordRenderPipelineCreation(pipeline != nullptr,
                               ElapsedMs(pipeline_start, pipeline_end));
  desc->release();
  if (!pipeline) {
    XELOGE(
        "Failed to create tessellation pipeline state: {}",
        error ? error->localizedDescription()->utf8String() : "unknown error");
    return nullptr;
  }

  TessellationPipelineState state;
  state.pipeline = pipeline;
  state.config = ir_desc.pipelineConfig;
  state.primitive = geometry_primitive;
  state.control_point_count = ir_desc.pipelineConfig.hsInputControlPointCount;

  auto [inserted_it, inserted] =
      tessellation_pipeline_cache_.emplace(key, std::move(state));
  MetalPipelineDescription stored_description =
      BuildTessellationPipelineDescription(
          domain_translation, pixel_translation, primitive_processing_result,
          tessellation_mode, attachment_formats, rendering_key);
  QueueStoredShader(static_cast<MetalShader&>(domain_translation->shader()));
  if (pixel_translation) {
    QueueStoredShader(static_cast<MetalShader&>(pixel_translation->shader()));
  }
  QueueStoredPipeline(
      stored_description,
      XXH3_64bits(&stored_description, sizeof(stored_description)));
  return &inserted_it->second;
}

MetalPipelineCache::TessellationPipelineState*
MetalPipelineCache::GetOrCreateNativeMslTessellationPipelineState(
    MetalShader::MetalTranslation* domain_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  if (!domain_translation || !domain_translation->is_native_msl() ||
      !domain_translation->metal_library()) {
    XELOGE(
        "Native tessellation pipeline requires a native MSL domain shader");
    return nullptr;
  }
  if (pixel_translation && (!pixel_translation->is_native_msl() ||
                            !pixel_translation->metal_function())) {
    XELOGE("Native tessellation pipeline requires a native MSL pixel shader");
    return nullptr;
  }

  MetalPipelineDescription description = BuildPipelineDescription(
      PipelineKind::kNativeTessellationMesh, domain_translation,
      pixel_translation, attachment_formats, rendering_key);
  description.auxiliary0 =
      uint32_t(primitive_processing_result.host_vertex_shader_type);
  description.auxiliary1 =
      uint32_t(primitive_processing_result.tessellation_mode);
  description.auxiliary2 =
      uint32_t(primitive_processing_result.host_primitive_type);
  uint64_t key = XXH3_64bits(&description, sizeof(description));
  auto it = tessellation_pipeline_cache_.find(key);
  if (it != tessellation_pipeline_cache_.end()) {
    return &it->second;
  }

  const uint32_t control_point_count =
      NativeMslTessellationControlPointCount(primitive_processing_result);
  if (!control_point_count) {
    XELOGE("Native tessellation pipeline: unsupported host vertex shader type {}",
           uint32_t(primitive_processing_result.host_vertex_shader_type));
    return nullptr;
  }

  bool use_fallback_pixel_shader = (pixel_translation == nullptr);
  MTL::Function* pixel_function =
      use_fallback_pixel_shader ? nullptr : pixel_translation->metal_function();
  if (use_fallback_pixel_shader) {
    if (!EnsureNativeDepthOnlyPixelShader()) {
      XELOGE(
          "Native tessellation pipeline: failed to create native depth-only "
          "PS");
      return nullptr;
    }
    NS::String* fallback_name = NS::String::string(
        native_depth_only_pixel_function_name_.c_str(), NS::UTF8StringEncoding);
    pixel_function = native_depth_only_pixel_library_->newFunction(
        fallback_name);
    if (!pixel_function) {
      XELOGE(
          "Native tessellation pipeline: missing native depth-only PS "
          "function");
      return nullptr;
    }
  }

  NS::String* object_name =
      NS::String::string("main_tess_object", NS::UTF8StringEncoding);
  MTL::Function* object_function =
      domain_translation->metal_library()->newFunction(object_name);
  if (!object_function) {
    XELOGE(
        "Native tessellation pipeline: domain shader {:016X} does not export "
        "main_tess_object",
        domain_translation->shader().ucode_data_hash());
    if (use_fallback_pixel_shader && pixel_function) {
      pixel_function->release();
    }
    return nullptr;
  }

  NS::String* mesh_name =
      NS::String::string("main_tess_mesh", NS::UTF8StringEncoding);
  MTL::Function* mesh_function =
      domain_translation->metal_library()->newFunction(mesh_name);
  if (!mesh_function) {
    XELOGE(
        "Native tessellation pipeline: domain shader {:016X} does not export "
        "main_tess_mesh",
        domain_translation->shader().ucode_data_hash());
    object_function->release();
    if (use_fallback_pixel_shader && pixel_function) {
      pixel_function->release();
    }
    return nullptr;
  }

  MTL::MeshRenderPipelineDescriptor* desc =
      MTL::MeshRenderPipelineDescriptor::alloc()->init();
  desc->setObjectFunction(object_function);
  desc->setMeshFunction(mesh_function);
  if (pixel_function) {
    desc->setFragmentFunction(pixel_function);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(
        attachment_formats.color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(attachment_formats.depth_format);
  desc->setStencilAttachmentPixelFormat(attachment_formats.stencil_format);
  desc->setRasterSampleCount(attachment_formats.sample_count);
  desc->setAlphaToCoverageEnabled(false);
  desc->setMaxTotalThreadsPerObjectThreadgroup(1);
  desc->setMaxTotalThreadsPerMeshThreadgroup(1);
  desc->setRequiredThreadsPerObjectThreadgroup(MTL::Size::Make(1, 1, 1));
  desc->setRequiredThreadsPerMeshThreadgroup(MTL::Size::Make(1, 1, 1));
  desc->setMaxTotalThreadgroupsPerMeshGrid(8192);
  desc->setPayloadMemoryLength(64);

  ApplyBlendStateToDescriptor(desc->colorAttachments(),
                              rendering_key.normalized_color_mask,
                              rendering_key.blendcontrol);

  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    if (pipeline_binary_archive_) {
      NS::Array* archives = NS::Array::array(pipeline_binary_archive_);
      desc->setBinaryArchives(archives);
      NS::Error* archive_error = nullptr;
      if (pipeline_binary_archive_->addMeshRenderPipelineFunctions(
              desc, &archive_error)) {
        pipeline_binary_archive_dirty_ = true;
      }
    }
  }

  NS::Error* error = nullptr;
  auto pipeline_start = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline = device_->newRenderPipelineState(
      desc, MTL::PipelineOptionNone, nullptr, &error);
  auto pipeline_end = std::chrono::steady_clock::now();
  RecordRenderPipelineCreation(pipeline != nullptr,
                               ElapsedMs(pipeline_start, pipeline_end));
  desc->release();
  object_function->release();
  mesh_function->release();
  if (use_fallback_pixel_shader && pixel_function) {
    pixel_function->release();
  }

  if (!pipeline) {
    XELOGE("Failed to create native tessellation pipeline state: {}",
           error ? error->localizedDescription()->utf8String()
                 : "unknown error");
    LogMetalErrorDetails("Native tessellation pipeline error", error);
    return nullptr;
  }

  TessellationPipelineState state;
  state.pipeline = pipeline;
  state.native_msl = true;
  state.control_point_count = control_point_count;

  auto [inserted_it, inserted] =
      tessellation_pipeline_cache_.emplace(key, std::move(state));
  QueueStoredShader(static_cast<MetalShader&>(domain_translation->shader()));
  if (pixel_translation) {
    QueueStoredShader(static_cast<MetalShader&>(pixel_translation->shader()));
  }
  QueueStoredPipeline(description, key);
  return &inserted_it->second;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
