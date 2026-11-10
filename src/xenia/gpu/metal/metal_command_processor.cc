/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/msl_bindings.h"

#include <dispatch/dispatch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "third_party/metal-cpp/Foundation/NSProcessInfo.hpp"
#include "third_party/metal-cpp/Foundation/NSURL.hpp"
#include "third_party/metal-cpp/Metal/MTLEvent.hpp"

#include "metal_irconverter_runtime.h"
#include "spirv2air.h"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/metal/metal_compile_activity.h"
#include "xenia/gpu/metal/metal_graphics_system.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/metal/metal_tessellation_shaders.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_to_dxil_compiler.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/ui/metal/metal_presenter.h"

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

DECLARE_bool(precise_interpolation);
DECLARE_bool(clear_memory_page_state);
DECLARE_bool(submit_on_primary_buffer_end);

DEFINE_int32(
    metal_draw_ring_count, 128,
    "Metal per-command-buffer draw ring size (descriptor-table pages). "
    "Higher reduces ring churn but uses more memory.",
    "Metal");
DEFINE_bool(
    metal_use_dxil, true,
    "Compile guest shaders to AIR with the Metal Shader Converter resource "
    "interface (see metal_shader_compiler) instead of SPIRV-Cross MSL.",
    "Metal");
UPDATE_from_bool(metal_use_dxil, 2026, 8, 21, 12, false);
DEFINE_string(
    metal_shader_compiler, "spirv2air",
    "Guest shader compiler with metal_use_dxil.\n"
    " spirv2air: SPIR-V -> AIR directly, falling back to msc for shaders it "
    "can't translate.\n"
    " msc: SPIR-V -> DXIL -> AIR with Apple's Metal Shader Converter.",
    "Metal");
DEFINE_bool(
    metal_finite_multiply, true,
    "Skip the Xenos 0 * x = 0 guard on multiplications whose operands are "
    "proven finite (DXIL path with MSC multiply-add support only).",
    "Metal");
DEFINE_int32(
    metal_pipeline_creation_threads, -1,
    "Number of threads used for SPIRV-Cross shader and render pipeline "
    "compilation in the Metal backend. -1 to calculate automatically (75% of "
    "logical CPU cores), a positive number to specify the number of threads "
    "explicitly (up to the number of logical CPU cores), 0 to disable "
    "multithreaded compilation.",
    "Metal");
DEFINE_bool(
    "Metal");
DEFINE_bool(
    "Metal");
DEFINE_bool(
    metal_log_frame_gpu_waits, false,
    "Log, at every swap, the time the CPU spent waiting for the GPU in the "
    "frame by reason, and the command buffers it created by kind.",
    "Metal");

namespace xe {
namespace gpu {
namespace metal {

namespace {

// Float24 polygon offsets are absolute, not host floating-depth ULP counts.
bool NeedsExactFloatDepthBias(const RegisterFile& regs) {
  auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  // Limit this path to host-depth configurations with validated fragment
  // interpolation. MSAA needs per-sample depth, and tessellation has separate
  // pipeline assembly. Those paths retain their existing hardware bias.
  if (!cvars::metal_use_dxil || cvars::depth_float24_convert_in_pixel_shader ||
      regs.Get<reg::RB_SURFACE_INFO>().msaa_samples !=
          xenos::MsaaSamples::k1X ||
      (xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) &&
       regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
           xenos::VGTOutputPath::kTessellationEnable)) {
    return false;
  }

  if (!draw_util::IsPrimitivePolygonal(regs) ||
      !regs.Get<reg::RB_DEPTHCONTROL>().z_enable ||
      regs.Get<reg::RB_DEPTH_INFO>().depth_format !=
          xenos::DepthRenderTargetFormat::kD24FS8) {
    return false;
  }
  auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  return (mode.poly_offset_front_enable &&
          (regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE) ||
           regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET))) ||
         (mode.poly_offset_back_enable &&
          (regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE) ||
           regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET)));
}

// A draw that only exports memory from the vertex shader: non-rasterizing,
// auto-indexed (a host builtin index buffer may be used for the expansion, but
// no guest indices are read from the shared buffer being exported), with no
// pixel shader, and not tessellated - DXIL tessellation lowers the guest vertex
// shader to object/mesh work, which a barrier naming only
// MTL::RenderStageVertex does not order. Matches PrimitiveProcessor::Process
// before it can read or convert indices.
bool IsPureDxilMemexportDraw(const RegisterFile& regs, bool rasterization_done,
                             bool vertex_memexport, bool has_export_range,
                             bool has_pixel_shader,
                             bool has_guest_index_buffer) {
  const auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  return !rasterization_done && vertex_memexport && has_export_range &&
         !has_pixel_shader && !has_guest_index_buffer &&
         initiator.source_select == xenos::SourceSelect::kAutoIndex &&
         !(xenos::IsMajorModeExplicit(initiator.major_mode,
                                      initiator.prim_type) &&
           regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
               xenos::VGTOutputPath::kTessellationEnable);
}

// Merges a write into one it overlaps or touches, so long chains of narrowed
// exports keep the overlap checks against them short.
void AddPendingWrite(std::vector<draw_util::MemExportRange>& writes,
                     const draw_util::MemExportRange& write) {
  uint64_t begin = uint64_t(write.base_address_dwords) << 2;
  uint64_t end = begin + write.size_bytes;
  for (draw_util::MemExportRange& other : writes) {
    uint64_t other_begin = uint64_t(other.base_address_dwords) << 2;
    uint64_t other_end = other_begin + other.size_bytes;
    if (begin <= other_end && other_begin <= end) {
      begin = std::min(begin, other_begin);
      other.base_address_dwords = uint32_t(begin >> 2);
      other.size_bytes = uint32_t(std::max(end, other_end) - begin);
      return;
    }
  }
  writes.push_back(write);
}

// Select the converter-compatible resource interface rather than SPIRV-Cross
// MSL. Guest compilation may use native AIR or the Mesa/DXIL converter path.
bool UseDxilPath() { return cvars::metal_use_dxil; }

// RB_BLENDCONTROL fields that blend, and their value that doesn't: 1 * source
// + 0 * destination for color and alpha.
constexpr uint32_t kBlendControlMask = 0x1FFF1FFF;
constexpr uint32_t kBlendControlNoBlending = 0x00010001;

// Whether fixed-function blending of a 16_16 or 16_16_16_16 target's snorm
// attachment, holding the EDRAM's fixed point values, gives the console's
// result: with factors ONE and ZERO only, every ADD and subtraction of the
// console's blender is the exact sum or difference, and MIN and MAX only
// compare the values themselves with both factors ONE (the host's MIN and MAX
// ignore the factors).
bool IsFixed16Format(xenos::ColorRenderTargetFormat format) {
  return format == xenos::ColorRenderTargetFormat::k_16_16 ||
         format == xenos::ColorRenderTargetFormat::k_16_16_16_16;
}

bool IsFixed16BlendExact(xenos::BlendFactor src, xenos::BlendFactor dest,
                         xenos::BlendOp op) {
  auto unit = [](xenos::BlendFactor factor) {
    return factor == xenos::BlendFactor::kZero ||
           factor == xenos::BlendFactor::kOne;
  };
  if (op == xenos::BlendOp::kMin || op == xenos::BlendOp::kMax) {
    return src == xenos::BlendFactor::kOne && dest == xenos::BlendFactor::kOne;
  }
  return unit(src) && unit(dest);
}

// The blend control of a color target that the pixel shader blends itself,
// or kBlendControlNoBlending if the target isn't one, isn't written, or
// fixed-function blending gives the exact result. Two kinds:
// - 8_8_8_8_GAMMA stored as 8-bit gamma (gamma_fetch_blend,
//   metal_gamma_fetch_blend). Alpha isn't gamma-encoded, so only blended color
//   needs the shader: not when the draw writes no color, or when color is the
//   source (1 * source +/- 0 * destination) or the destination (0 * source +
//   1 * destination, or 1 * destination - 0 * source), which blending the
//   encoded bytes reproduces.
// - 16_16 and 16_16_16_16 (fixed16_fetch_blend, metal_rt16_fetch_blend),
//   unless IsFixed16BlendExact holds for each control a written channel uses.
//   16_16's green blends under the alpha control on the console but under the
//   colour control on the host, so the two must also match when green is
//   written. 16_16 is marked kColorBlendFixed16TwoChannels.
uint32_t GetShaderBlendControl(const RegisterFile& regs, uint32_t rt,
                               uint32_t normalized_color_mask,
                               bool gamma_fetch_blend,
                               bool fixed16_fetch_blend) {
  uint32_t write_mask = (normalized_color_mask >> (rt * 4)) & 0xF;
  xenos::ColorRenderTargetFormat format =
      regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[rt])
          .color_format;
  auto blendcontrol = regs.Get<reg::RB_BLENDCONTROL>(
      reg::RB_BLENDCONTROL::rt_register_indices[rt]);
  if (fixed16_fetch_blend && IsFixed16Format(format)) {
    bool color_exact = IsFixed16BlendExact(blendcontrol.color_srcblend,
                                           blendcontrol.color_destblend,
                                           blendcontrol.color_comb_fcn);
    bool alpha_exact = IsFixed16BlendExact(blendcontrol.alpha_srcblend,
                                           blendcontrol.alpha_destblend,
                                           blendcontrol.alpha_comb_fcn);
    bool exact;
    uint32_t kind = 0;
    if (format == xenos::ColorRenderTargetFormat::k_16_16) {
      kind = SpirvShaderTranslator::kColorBlendFixed16TwoChannels;
      bool alpha_as_color =
          blendcontrol.alpha_srcblend == blendcontrol.color_srcblend &&
          blendcontrol.alpha_destblend == blendcontrol.color_destblend &&
          blendcontrol.alpha_comb_fcn == blendcontrol.color_comb_fcn;
      exact = (!(write_mask & 0b0001) || color_exact) &&
              (!(write_mask & 0b0010) || (alpha_exact && alpha_as_color));
    } else {
      exact = (!(write_mask & 0b0111) || color_exact) &&
              (!(write_mask & 0b1000) || alpha_exact);
    }
    if (!write_mask || exact) {
      return kBlendControlNoBlending;
    }
    return (blendcontrol.value & kBlendControlMask) | kind;
  }
  if (!gamma_fetch_blend || !(write_mask & 0x7) ||
      format != xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
    return kBlendControlNoBlending;
  }
  xenos::BlendFactor src = blendcontrol.color_srcblend;
  xenos::BlendFactor dest = blendcontrol.color_destblend;
  xenos::BlendOp op = blendcontrol.color_comb_fcn;
  if ((src == xenos::BlendFactor::kOne && dest == xenos::BlendFactor::kZero &&
       (op == xenos::BlendOp::kAdd || op == xenos::BlendOp::kSubtract)) ||
      (src == xenos::BlendFactor::kZero && dest == xenos::BlendFactor::kOne &&
       (op == xenos::BlendOp::kAdd || op == xenos::BlendOp::kRevSubtract))) {
    return kBlendControlNoBlending;
  }
  return blendcontrol.value & kBlendControlMask;
}

// The color targets with a non-empty write mask, a bit each: a pixel shader's
// color_targets_used unless it reads its targets back.
uint32_t GetColorTargetsWritten(uint32_t normalized_color_mask) {
  uint32_t color_targets = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if ((normalized_color_mask >> (i * 4)) & 0xF) {
      color_targets |= UINT32_C(1) << i;
    }
  }
  return color_targets;
}

// function_name_suffix is appended to the entry point name of the converted
// shader, for functions MSC emits under a derived name.
bool CreateMetalFunction(MTL::Device* device,
                         const MetalShaderConversionResult& conversion,
                         MTL::Library*& library_out,
                         MTL::Function*& function_out,
                         const char* function_name_suffix = "") {
  NS::Error* error = nullptr;
  dispatch_data_t metallib_data = dispatch_data_create(
      conversion.metallib.data(), conversion.metallib.size(), nullptr,
      DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* library = device->newLibrary(metallib_data, &error);
  dispatch_release(metallib_data);
  if (!library) {
    return false;
  }
  std::string function_name =
      conversion.entry_point_name + function_name_suffix;
  NS::String* name =
      NS::String::string(function_name.c_str(), NS::UTF8StringEncoding);
  MTL::Function* function = library->newFunction(name);
  if (!function) {
    library->release();
    return false;
  }
  library_out = library;
  function_out = function;
  return true;
}

const char* StageNameOf(MetalShaderStage stage) {
  switch (stage) {
    case MetalShaderStage::kHull:
      return "hull";
    case MetalShaderStage::kDomain:
      return "domain";
    default:
      return "vertex";
  }
}

IRRuntimeTessellationPipelineConfig BuildTessellationPipelineConfig(
    const MetalShaderReflection& vertex, const MetalShaderReflection& hull,
    const MetalShaderReflection& domain) {
  IRRuntimeTessellationPipelineConfig config = {};
  config.outputPrimitiveType =
      IRRuntimeTessellatorOutputPrimitive(hull.hs_tessellator_output_primitive);
  config.vsOutputSizeInBytes = vertex.vertex_output_size_in_bytes;
  config.gsMaxInputPrimitivesPerMeshThreadgroup =
      domain.ds_max_input_prims_per_mesh_threadgroup;
  config.hsMaxPatchesPerObjectThreadgroup =
      hull.hs_max_patches_per_object_threadgroup;
  config.hsInputControlPointCount = hull.hs_input_control_point_count;
  config.hsMaxObjectThreadsPerThreadgroup =
      hull.hs_max_object_threads_per_patch;
  config.hsMaxTessellationFactor = hull.hs_max_tessellation_factor;
  config.gsInstanceCount = 1;
  return config;
}

void GetRenderPassSize(MTL::RenderPassDescriptor* pass_descriptor,
                       uint32_t fallback_width, uint32_t fallback_height,
                       uint32_t& width_out, uint32_t& height_out) {
  uint32_t width = uint32_t(pass_descriptor->renderTargetWidth());
  uint32_t height = uint32_t(pass_descriptor->renderTargetHeight());
  // The encoded pass is constrained by every attached mip level, as well as
  // any explicit render target dimensions. The cache may bind different
  // targets by the time a later draw reuses this encoder.
  auto constrain = [&](MTL::RenderPassAttachmentDescriptor* attachment) {
    if (MTL::Texture* texture = attachment->texture()) {
      uint32_t attachment_width =
          std::max(uint32_t(texture->width() >> attachment->level()), 1u);
      uint32_t attachment_height =
          std::max(uint32_t(texture->height() >> attachment->level()), 1u);
      width = width ? std::min(width, attachment_width) : attachment_width;
      height = height ? std::min(height, attachment_height) : attachment_height;
    }
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    constrain(pass_descriptor->colorAttachments()->object(i));
  }
  constrain(pass_descriptor->depthAttachment());
  constrain(pass_descriptor->stencilAttachment());
  width_out = std::max(width ? width : fallback_width, 1u);
  height_out = std::max(height ? height : fallback_height, 1u);
}

void ClampScissorToBounds(draw_util::Scissor& scissor, uint32_t width,
                          uint32_t height) {
  width = std::max(width, uint32_t(1));
  height = std::max(height, uint32_t(1));

  scissor.offset[0] = std::min(scissor.offset[0], width);
  scissor.offset[1] = std::min(scissor.offset[1], height);

  uint32_t max_scissor_width = width - scissor.offset[0];
  uint32_t max_scissor_height = height - scissor.offset[1];
  scissor.extent[0] = std::min(scissor.extent[0], max_scissor_width);
  scissor.extent[1] = std::min(scissor.extent[1], max_scissor_height);
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

constexpr int64_t kAsyncCompileLogIntervalNs =
    int64_t(std::chrono::nanoseconds(std::chrono::seconds(1)).count());

// Indexed by CommandBufferKind, for the shutdown summary. Submission kinds
// name what ended the previous submission.
const char* const kCommandBufferKindNames[] = {
    "submission_other",
    "submission_copy_draw_sync",
    "submission_zpd_query",
    "submission_uniforms_rollover",
    "submission_primary_end",
    "submission_wait",
    "texture_upload_batch",
    "texture_upload_private",
    "texture_other",
    "rt_resolve",
    "rt_dump",
    "rt_other",
    "submission_pass_limit",
    "submission_after_drain",
};
static_assert(std::size(kCommandBufferKindNames) ==
                  size_t(MetalCommandProcessor::CommandBufferKind::kCount),
              "Command buffer kind names must match the enum");

// Indexed by GpuWaitReason.
const char* const kGpuWaitReasonNames[] = {
    "memexport",      "zpd_query",           "shader_done_fence",
    "trace_playback", "uniforms",            "resolve",
    "texture_upload", "scaled_resolve_copy", "snapshot",
    "dump",           "resolve_before_guest_read",
};
static_assert(std::size(kGpuWaitReasonNames) ==
                  size_t(MetalCommandProcessor::GpuWaitReason::kCount),
              "GPU wait reason names must match the enum");

int64_t GetSteadyTimeNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool ShouldLogRateLimited(std::atomic<int64_t>& last_log_ns,
                          int64_t interval_ns) {
  const int64_t now = GetSteadyTimeNs();
  int64_t previous = last_log_ns.load(std::memory_order_relaxed);
  while (now - previous >= interval_ns) {
    if (last_log_ns.compare_exchange_weak(previous, now,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}
void PopulatePipelineFormatsFromRenderPassDescriptor(
    MTL::RenderPassDescriptor* pass_descriptor, MTL::PixelFormat* color_formats,
    uint32_t color_count, MTL::PixelFormat* depth_format,
    MTL::PixelFormat* stencil_format, uint32_t* sample_count) {
  if (!pass_descriptor) {
    return;
  }

  auto update_sample_count = [&](MTL::Texture* texture) {
    if (!texture || !sample_count) {
      return;
    }
    NS::UInteger sc = texture->sampleCount();
    if (sc > 0) {
      *sample_count =
          std::max<uint32_t>(*sample_count, static_cast<uint32_t>(sc));
    }
  };

  if (color_formats) {
    auto* color_attachments = pass_descriptor->colorAttachments();
    for (uint32_t i = 0; i < color_count; ++i) {
      auto* attachment =
          color_attachments ? color_attachments->object(i) : nullptr;
      if (!attachment) {
        continue;
      }
      MTL::Texture* texture = attachment->texture();
      if (!texture) {
        continue;
      }
      color_formats[i] = texture->pixelFormat();
      update_sample_count(texture);
    }
  }

  if (depth_format) {
    if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
      MTL::Texture* texture = depth_attachment->texture();
      if (texture) {
        *depth_format = texture->pixelFormat();
        update_sample_count(texture);
      }
    }
  }

  if (stencil_format) {
    if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
      MTL::Texture* texture = stencil_attachment->texture();
      if (texture) {
        *stencil_format = texture->pixelFormat();
        update_sample_count(texture);
      }
    }
  }

  if (depth_format && stencil_format) {
    if (*depth_format != MTL::PixelFormatInvalid &&
        *stencil_format == MTL::PixelFormatInvalid) {
      switch (*depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          *stencil_format = *depth_format;
          break;
        default:
          break;
      }
    } else if (*stencil_format != MTL::PixelFormatInvalid &&
               *depth_format == MTL::PixelFormatInvalid) {
      switch (*stencil_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          *depth_format = *stencil_format;
          break;
        default:
          break;
      }
    }
  }
}

void EnsureDepthFormatForDepthWritingFragment(const char* pipeline_name,
                                              bool fragment_writes_depth,
                                              MTL::PixelFormat* depth_format) {
  if (!fragment_writes_depth || !depth_format ||
      *depth_format != MTL::PixelFormatInvalid) {
    return;
  }
  // Metal requires a valid depth attachment format if the fragment shader
  // writes depth even when the current render pass has no depth target bound.
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

MTL::ComputePipelineState* CreateComputePipelineFromEmbeddedLibrary(
    MTL::Device* device, const void* metallib_data, size_t metallib_size,
    const char* debug_name) {
  if (!device || !metallib_data || !metallib_size) {
    return nullptr;
  }

  NS::Error* error = nullptr;
  dispatch_data_t data = dispatch_data_create(
      metallib_data, metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* lib = device->newLibrary(data, &error);
  dispatch_release(data);
  if (!lib) {
    XELOGE("Metal: failed to create {} library: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  // XeSL compute entrypoint name used in the embedded metallibs.
  NS::String* fn_name = NS::String::string("entry_xe", NS::UTF8StringEncoding);
  MTL::Function* fn = lib->newFunction(fn_name);
  if (!fn) {
    XELOGE("Metal: {} missing entry_xe", debug_name);
    lib->release();
    return nullptr;
  }

  MTL::ComputePipelineState* pipeline =
      device->newComputePipelineState(fn, &error);
  fn->release();
  lib->release();

  if (!pipeline) {
    XELOGE("Metal: failed to create {} pipeline: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  return pipeline;
}

constexpr uint32_t kPipelineDiskCacheMagic = 0x43504D58;  // 'XMPC'
constexpr uint32_t kPipelineDiskCacheVersion = 3;
constexpr size_t kPipelineDiskCacheMaxEntrySize = 1 << 20;

XEPACKEDSTRUCT(PipelineDiskCacheHeader, {
  uint32_t magic;
  uint32_t version;
  uint32_t reserved[2];
});

XEPACKEDSTRUCT(PipelineDiskCacheEntryHeader, {
  uint32_t entry_size;
  uint32_t reserved;
});

XEPACKEDSTRUCT(PipelineDiskCacheEntryBase, {
  uint64_t pipeline_key;
  uint64_t vertex_shader_cache_key;
  uint64_t pixel_shader_cache_key;
  uint32_t sample_count;
  uint32_t depth_format;
  uint32_t stencil_format;
  uint32_t color_formats[4];
  uint32_t normalized_color_mask;
  uint32_t blendcontrol[4];
  uint32_t vertex_attribute_count;
  uint32_t vertex_layout_count;
});

static_assert(sizeof(PipelineDiskCacheHeader) == 16,
              "Unexpected pipeline disk cache header size.");
static_assert(sizeof(PipelineDiskCacheEntryHeader) == 8,
              "Unexpected pipeline disk cache entry header size.");

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
  // 8 entries for safety since 3 bits from the guest are passed directly.
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
  // 32 because of 0x1F mask, for safety (all unknown to zero).
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
  // Like the RGB map, but with color modes changed to alpha.
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

}  // namespace

MetalCommandProcessor::MetalCommandProcessor(
    MetalGraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      dxil_binder_(*this, metal_shader_converter_) {}

std::string MetalCommandProcessor::MakeTitleStateSuffix() const {
  if (!render_target_cache_) {
    return {};
  }
  std::ostringstream suffix;
  if (!UseDxilPath()) {
    suffix << " - SPIRV-Cross";
  } else {
    suffix << (cvars::metal_shader_compiler == "msc" ? " - MSC"
                                                     : " - spirv2air");
  }
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    suffix << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
  }
  return suffix.str();
}

MetalCommandProcessor::~MetalCommandProcessor() {
  render_encoder_shared_memory_hazards_.Abandon();
  // End any active render encoder before releasing
  // Note: Only call endEncoding if the encoder is still active
  // (not already ended by a committed command buffer)
  if (current_render_encoder_) {
    // The encoder may already be ended if the command buffer was committed
    // In that case, just release it
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
  }
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  WaitForPendingCompletionHandlers();
  ShutdownAsyncCompilation();
  if (render_pass_descriptor_) {
    render_pass_descriptor_->release();
    render_pass_descriptor_ = nullptr;
  }
  if (render_target_texture_) {
    render_target_texture_->release();
    render_target_texture_ = nullptr;
  }
  if (depth_stencil_texture_) {
    depth_stencil_texture_->release();
    depth_stencil_texture_ = nullptr;
  }

  for (auto& pair : depth_stencil_state_cache_) {
    if (pair.second) {
      pair.second->release();
    }
  }
  depth_stencil_state_cache_.clear();

  // Release IR Converter runtime buffers and resources
  if (null_buffer_) {
    null_buffer_->release();
    null_buffer_ = nullptr;
  }
  if (null_texture_) {
    null_texture_->release();
    null_texture_ = nullptr;
  }
  if (null_sampler_) {
    null_sampler_->release();
    null_sampler_ = nullptr;
  }
  uniforms_buffer_ = nullptr;
  command_buffer_spirv_uniform_buffers_.clear();
  size_t uniforms_pool_size = 0;
  {
    std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
    uniforms_pool_size = spirv_uniforms_pool_.size();
    spirv_uniforms_available_.clear();
    for (MTL::Buffer* pool_uniforms : spirv_uniforms_pool_) {
      if (pool_uniforms) {
        pool_uniforms->release();
      }
    }
    spirv_uniforms_pool_.clear();
    spirv_uniforms_pool_initialized_ = false;
  }
  if (spirv_uniforms_available_semaphore_) {
    // Buffers dropped above were never signalled back, and libdispatch traps
    // on a semaphore released below the count it was created with.
    for (size_t i = 0; i < uniforms_pool_size; ++i) {
      dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
    }
#if !OS_OBJECT_USE_OBJC
    dispatch_release(spirv_uniforms_available_semaphore_);
#endif
    spirv_uniforms_available_semaphore_ = nullptr;
  }
}

void MetalCommandProcessor::InitializeAsyncCompilation() {
  ShutdownAsyncCompilation();

  if (!cvars::async_shader_compilation) {
    return;
  }

  uint32_t logical_processor_count = std::thread::hardware_concurrency();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }

  if (cvars::metal_pipeline_creation_threads == 0) {
    return;
  }

  size_t thread_count = 0;
  if (cvars::metal_pipeline_creation_threads < 0) {
    thread_count = std::max<uint32_t>(logical_processor_count * 3 / 4, 1);
  } else {
    thread_count =
        std::min<uint32_t>(uint32_t(cvars::metal_pipeline_creation_threads),
                           logical_processor_count);
  }
  if (!thread_count) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    async_compile_shutdown_ = false;
    async_compile_base_thread_count_ = thread_count;
    async_compile_thread_limit_ = thread_count;
  }

  async_compile_threads_.reserve(thread_count);
  for (size_t i = 0; i < thread_count; ++i) {
    async_compile_threads_.emplace_back([this, i]() { AsyncCompileThread(i); });
  }
  {
    // A boost asked for before the pool existed.
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    UpdateCompileThreadLimitLocked();
  }

  XELOGI(
      "Metal: async {} shader/pipeline compilation enabled with {} worker "
      "thread(s)",
      UseDxilPath() ? "DXIL" : "SPIRV-Cross", thread_count);
}

void MetalCommandProcessor::ShutdownAsyncCompilation() {
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    async_compile_shutdown_ = true;
  }
  async_compile_cv_.notify_all();
  async_compile_boost_cv_.notify_all();

  for (std::thread& thread : async_compile_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  async_compile_threads_.clear();
  std::vector<std::thread> boost_threads;
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    boost_threads.swap(async_compile_boost_threads_);
  }
  for (std::thread& thread : boost_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  std::priority_queue<ShaderCompileRequest, std::vector<ShaderCompileRequest>,
                      ShaderCompileRequestCompare>
      empty_queue;
  std::swap(async_shader_queue_, empty_queue);
  for (auto& [order, request] : async_pipeline_queue_) {
    if (request.vertex_function) {
      request.vertex_function->release();
    }
    if (request.fragment_function) {
      request.fragment_function->release();
    }
  }
  async_pipeline_queue_.clear();
  async_pipeline_queued_.clear();
  async_pipeline_queue_arrivals_ = 0;
  while (!async_tess_shaders_queue_.empty()) {
    async_tess_shaders_queue_.pop();
  }
  async_shader_pending_.clear();
  async_shader_failed_.clear();
  async_pipeline_pending_.clear();
  async_pipeline_failed_.clear();
  async_tess_shaders_pending_.clear();
  async_tess_shaders_failed_.clear();
  async_compile_busy_ = 0;
  async_compile_shutdown_ = false;
  async_compile_base_thread_count_ = 0;
  async_compile_thread_limit_ = 0;
  // async_compile_hold_boost_ is the app's request, not pool state: it is kept
  // so a pool that starts after the request still honours it.
  async_compile_awaiters_ = 0;
  storage_stats_pending_ = false;
  storage_stats_boosted_ = false;
  // The dropped requests count as finished, so the backlog reads empty.
  compile_activity.finished.store(
      compile_activity.queued.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  compile_activity.storage_loading = false;
}

MTL::Function* MetalCommandProcessor::GetHostShaderFunction(
    const Shader::Translation* translation) {
  if (!translation) {
    return nullptr;
  }
  if (UseDxilPath()) {
    return static_cast<const DxilShader::DxilTranslation*>(translation)
        ->metal_function();
  }
  return static_cast<const MslShader::MslTranslation*>(translation)
      ->metal_function();
}

MetalCommandProcessor::ShaderCompileStatus
MetalCommandProcessor::EnsureTranslationSpirv(
    Shader::Translation* translation, SpirvShaderTranslator& translator) {
  if (translation->is_translated()) {
    return translation->is_valid() ? ShaderCompileStatus::kReady
                                   : ShaderCompileStatus::kFailed;
  }
  if (!translation->TryClaimTranslation()) {
    // Another thread is translating it.
    return ShaderCompileStatus::kPending;
  }
  if (!translator.TranslateAnalyzedShader(*translation)) {
    XELOGE("Metal: failed to translate shader {:016X} mod {:016X} to SPIR-V",
           translation->shader().ucode_data_hash(),
           translation->modification());
    return ShaderCompileStatus::kFailed;
  }
  return ShaderCompileStatus::kReady;
}

bool MetalCommandProcessor::CompileHostShader(
    Shader::Translation* translation, bool is_ios,
    std::vector<uint8_t>* metallib_out) {
  if (!translation) {
    return false;
  }
  if (UseDxilPath()) {
    return static_cast<DxilShader::DxilTranslation*>(translation)
        ->CompileToAir(device_, metal_shader_converter_, metallib_out);
  }
  return static_cast<MslShader::MslTranslation*>(translation)
      ->CompileToMsl(device_, is_ios);
}

MetalCommandProcessor::ShaderCompileStatus
MetalCommandProcessor::TranslateAndCompileHostShader(
    Shader::Translation* translation, SpirvShaderTranslator& translator,
    bool is_ios) {
  if (!translation) {
    return ShaderCompileStatus::kFailed;
  }
  // Shader dumps and bisection want the translation itself.
  const bool cacheable = UseDxilPath() && cvars::dump_shaders.empty() &&
                         shader_library_cache_.is_open();
  // Translated even when the library is on disk: translating sets the
  // shader's texture and sampler bindings that draws bind from.
  ShaderCompileStatus status = EnsureTranslationSpirv(translation, translator);
  if (status != ShaderCompileStatus::kReady) {
    return status;
  }
  MetalShaderLibraryCache::Key key;
  if (cacheable) {
    key = MetalShaderLibraryCache::MakeKey(*translation);
    MetalShaderLibraryCache::Entry entry;
    if (shader_library_cache_.Find(key, entry) &&
        static_cast<DxilShader::DxilTranslation*>(translation)
            ->LoadMetallib(device_, entry.metallib, entry.entry_point_name)) {
      return ShaderCompileStatus::kReady;
    }
  }
  std::vector<uint8_t> metallib;
  if (!CompileHostShader(translation, is_ios,
                         cacheable ? &metallib : nullptr)) {
    return ShaderCompileStatus::kFailed;
  }
  if (cacheable && !metallib.empty()) {
    shader_library_cache_.Store(
        key, metallib,
        static_cast<DxilShader::DxilTranslation*>(translation)
            ->entry_point_name());
  }
  return ShaderCompileStatus::kReady;
}

void MetalCommandProcessor::NoteShaderCompileFailed(
    Shader::Translation* translation) {
  if (!translation) {
    return;
  }
  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  async_shader_failed_.insert(translation);
}

MetalCommandProcessor::ShaderCompileStatus
MetalCommandProcessor::GetShaderCompileStatus(
    Shader::Translation* translation) {
  if (!translation) {
    return ShaderCompileStatus::kFailed;
  }

  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  if (async_shader_failed_.find(translation) != async_shader_failed_.end()) {
    return ShaderCompileStatus::kFailed;
  }
  if (async_shader_pending_.find(translation) != async_shader_pending_.end()) {
    return ShaderCompileStatus::kPending;
  }
  return GetHostShaderFunction(translation) ? ShaderCompileStatus::kReady
                                            : ShaderCompileStatus::kNotQueued;
}

bool MetalCommandProcessor::EnqueueShaderCompilation(
    Shader::Translation* translation, bool is_ios, uint8_t priority) {
  if (!translation || !cvars::async_shader_compilation ||
      async_compile_threads_.empty()) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    ShaderCompileRequest request;
    request.translation = translation;
    request.shader_hash = translation->shader().ucode_data_hash();
    request.modification = translation->modification();
    request.is_ios = is_ios;
    request.priority = priority;
    async_shader_pending_.insert(translation);
    async_shader_queue_.push(request);
    compile_activity.queued.fetch_add(1, std::memory_order_relaxed);
  }
  async_compile_cv_.notify_one();
  return true;
}

bool MetalCommandProcessor::EnqueuePipelineCompilation(
    const PipelineCompileRequest& request) {
  if (!cvars::async_shader_compilation || async_compile_threads_.empty()) {
    return false;
  }
  // A DXIL tessellation pipeline is linked from the MSC stage libraries, not
  // from Metal functions, so it is the one kind with no vertex function.
  if (request.description.kind == PipelineKind::kDxilTessellation
          ? request.tessellation_shaders == nullptr
          : request.vertex_function == nullptr) {
    return false;
  }

  PipelineCompileRequest queued_request = request;
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    if (async_pipeline_cache_.find(request.pipeline_key) !=
        async_pipeline_cache_.end()) {
      return true;
    }
    if (async_pipeline_failed_.find(request.pipeline_key) !=
        async_pipeline_failed_.end()) {
      return false;
    }
    if (async_pipeline_pending_.find(request.pipeline_key) !=
        async_pipeline_pending_.end()) {
      return true;
    }

    if (queued_request.vertex_function) {
      queued_request.vertex_function->retain();
    }
    if (queued_request.fragment_function) {
      queued_request.fragment_function->retain();
    }
    async_pipeline_pending_.insert(request.pipeline_key);
    PipelineQueueOrder order(-int(queued_request.priority),
                             async_pipeline_queue_arrivals_++);
    async_pipeline_queue_.emplace(order, std::move(queued_request));
    async_pipeline_queued_.emplace(request.pipeline_key, order);
    compile_activity.queued.fetch_add(1, std::memory_order_relaxed);
  }

  async_compile_cv_.notify_one();
  return true;
}

Shader::Translation* MetalCommandProcessor::GetOrCreateHostTranslation(
    SpirvShader& shader, uint64_t modification, bool allow_async,
    ShaderCompileStatus* compile_status_out,
    std::shared_ptr<const Shader::Specialization> specialization) {
  constexpr bool kIsIos =
#if XE_PLATFORM_IOS
      true;
#else
      false;
#endif
  Shader::Translation* translation = shader.GetOrCreateTranslation(
      modification, nullptr, std::move(specialization));
  ShaderCompileStatus status = GetShaderCompileStatus(translation);
  if (status == ShaderCompileStatus::kNotQueued) {
    // Vertex shaders go first: the placeholder a pending pixel shader draws
    // through needs one, so the draw stops being dropped sooner.
    if (allow_async &&
        EnqueueShaderCompilation(
            translation, kIsIos,
            shader.type() == xenos::ShaderType::kVertex ? 2 : 1)) {
      status = ShaderCompileStatus::kPending;
    } else {
      status = TranslateAndCompileHostShader(translation,
                                             *spirv_shader_translator_, kIsIos);
      if (status == ShaderCompileStatus::kFailed) {
        NoteShaderCompileFailed(translation);
      }
    }
  }
  if (status == ShaderCompileStatus::kPending && !allow_async) {
    // An earlier draw that could wait for this queued it. This caller can't, so
    // it waits for the compile threads to get to it.
    AwaitAsyncCompilesUntil([this, translation]() {
      return async_shader_pending_.find(translation) ==
             async_shader_pending_.end();
    });
    status = GetShaderCompileStatus(translation);
  }
  *compile_status_out = status;
  return translation;
}

void MetalCommandProcessor::LogShaderCompilePending(
    const Shader::Translation* translation, const char* stage_tag) {
  if (!ShouldLogRateLimited(async_shader_pending_last_log_ns_,
                            kAsyncCompileLogIntervalNs)) {
    return;
  }
  XELOGI(
      "Metal: skipping draw - {} shader compile pending (shader={:016X}, "
      "mod={:016X})",
      stage_tag, translation->shader().ucode_data_hash(),
      translation->modification());
}

void MetalCommandProcessor::LogPipelineCompilePending(
    const Shader::Translation* vertex_translation,
    const Shader::Translation* pixel_translation) {
  if (!ShouldLogRateLimited(async_pipeline_pending_last_log_ns_,
                            kAsyncCompileLogIntervalNs)) {
    return;
  }
  XELOGI(
      "Metal: skipping draw - render pipeline compile pending "
      "(VS {:016X} mod {:016X}, PS {:016X} mod {:016X})",
      vertex_translation->shader().ucode_data_hash(),
      vertex_translation->modification(),
      pixel_translation ? pixel_translation->shader().ucode_data_hash() : 0,
      pixel_translation ? pixel_translation->modification() : 0);
}

void MetalCommandProcessor::LogPlaceholderDraw(
    const Shader::Translation* vertex_translation,
    const Shader::Translation* pixel_translation) {
  if (!ShouldLogRateLimited(async_placeholder_last_log_ns_,
                            kAsyncCompileLogIntervalNs)) {
    return;
  }
  XELOGI(
      "Metal: drawing through the vertex-only placeholder while the pixel "
      "shader builds (VS {:016X} mod {:016X}, PS {:016X} mod {:016X})",
      vertex_translation->shader().ucode_data_hash(),
      vertex_translation->modification(),
      pixel_translation ? pixel_translation->shader().ucode_data_hash() : 0,
      pixel_translation ? pixel_translation->modification() : 0);
}

void MetalCommandProcessor::ApplyColorAttachmentState(
    MTL::RenderPipelineColorAttachmentDescriptorArray* attachments,
    const PipelineCompileRequest& request) {
  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = attachments->object(i);
    MTL::PixelFormat color_format =
        MTL::PixelFormat(request.description.color_formats[i]);
    color_attachment->setPixelFormat(color_format);
    if (color_format == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    uint32_t rt_write_mask =
        (request.description.normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    reg::RB_BLENDCONTROL blendcontrol;
    blendcontrol.value = request.description.blendcontrol[i];

    MTL::BlendFactor src_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_srcblend);
    MTL::BlendFactor dst_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_destblend);
    MTL::BlendOperation op_rgb =
        ToMetalBlendOperation(blendcontrol.color_comb_fcn);
    MTL::BlendFactor src_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_srcblend);
    MTL::BlendFactor dst_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_destblend);
    MTL::BlendOperation op_alpha =
        ToMetalBlendOperation(blendcontrol.alpha_comb_fcn);

    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    // The pixel shader blends it.
    if (request.description.shader_blend_mask & (1u << i)) {
      blending_enabled = false;
    }
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

MTL::Function* MetalCommandProcessor::GetSpecializedFunction(
    MTL::Library* library, const std::string& function_name,
    const uint32_t* states, uint32_t binding_count, std::string* error_out) {
  if (!library || function_name.empty()) {
    if (error_out) {
      *error_out = "missing shader library";
    }
    return nullptr;
  }
  SpecializedFunctionKey key{
      uintptr_t(library), function_name,
      std::vector<uint32_t>(states, states + 4 * binding_count)};
  {
    std::lock_guard<std::mutex> lock(specialized_functions_mutex_);
    auto it = specialized_functions_.find(key);
    if (it != specialized_functions_.end()) {
      return it->second;
    }
  }
  // The shader only declares the constants of the bindings it reads, and Metal
  // ignores values for constants a function doesn't declare.
  MTL::FunctionConstantValues* values =
      MTL::FunctionConstantValues::alloc()->init();
  for (uint32_t i = 0; i < binding_count; ++i) {
    for (const char* prefix : {"MSC_FC_", "spirv2air_fc_"}) {
      values->setConstantValue(
          &states[4 * i], MTL::DataTypeUInt4,
          NS::String::string(fmt::format("{}{}", prefix, i).c_str(),
                             NS::UTF8StringEncoding));
    }
  }
  // Not through the pipeline archive: the archived pipelines record their
  // specialized functions, and their lookups hit with this one (see
  // MetalPipelineArchive).
  MTL::FunctionDescriptor* function_desc =
      MTL::FunctionDescriptor::alloc()->init();
  function_desc->setName(
      NS::String::string(function_name.c_str(), NS::UTF8StringEncoding));
  function_desc->setConstantValues(values);
  NS::Error* error = nullptr;
  MTL::Function* function = library->newFunction(function_desc, &error);
  function_desc->release();
  values->release();
  if (!function) {
    if (error_out) {
      *error_out = error && error->localizedDescription()
                       ? error->localizedDescription()->utf8String()
                       : "fetch state specialization failed";
    }
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(specialized_functions_mutex_);
  auto [it, inserted] =
      specialized_functions_.emplace(std::move(key), function);
  if (!inserted) {
    // Another compile thread specialized it meanwhile.
    function->release();
  }
  return it->second;
}

MTL::Function* MetalCommandProcessor::GetPipelineVertexFunction(
    const PipelineCompileRequest& request, std::string* error_out) {
  if (!request.vertex_fetch_states_specialized) {
    return request.vertex_function;
  }
  uint32_t states[SpirvShaderTranslator::kFetchStateBindingCount * 4] = {};
  std::memcpy(states, request.description.vertex_fetch_endians,
              sizeof(request.description.vertex_fetch_endians));
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateBoolConstantsBinding * 4],
      request.description.vertex_bool_constants,
      sizeof(request.description.vertex_bool_constants));
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateLoopConstantsBinding * 4],
      request.description.vertex_loop_constants,
      sizeof(request.description.vertex_loop_constants));
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateFiniteMultiplyBinding * 4],
      request.description.vertex_finite_multiply_state,
      sizeof(request.description.vertex_finite_multiply_state));
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateTextureScaleBinding * 4],
      request.description.vertex_texture_scale_bits,
      sizeof(request.description.vertex_texture_scale_bits));
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateTextureDecodeBinding * 4],
      request.description.vertex_texture_decode_bits,
      sizeof(request.description.vertex_texture_decode_bits));
  return GetSpecializedFunction(
      request.vertex_library, request.vertex_function_name, states,
      SpirvShaderTranslator::kFetchStateBindingCount, error_out);
}

MTL::Function* MetalCommandProcessor::GetPipelineFragmentFunction(
    const PipelineCompileRequest& request, std::string* error_out) {
  if (!request.fragment_fetch_states_specialized) {
    return request.fragment_function;
  }
  uint32_t states[SpirvShaderTranslator::kFetchStateBindingCount * 4] = {};
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateBoolConstantsBinding * 4],
      request.description.pixel_bool_constants,
      sizeof(request.description.pixel_bool_constants));
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateLoopConstantsBinding * 4],
      request.description.pixel_loop_constants,
      sizeof(request.description.pixel_loop_constants));
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateFiniteMultiplyBinding * 4],
      request.description.pixel_finite_multiply_state,
      sizeof(request.description.pixel_finite_multiply_state));
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateTextureScaleBinding * 4],
      request.description.pixel_texture_scale_bits,
      sizeof(request.description.pixel_texture_scale_bits));
  std::memcpy(
      &states[SpirvShaderTranslator::kFetchStateTextureDecodeBinding * 4],
      request.description.pixel_texture_decode_bits,
      sizeof(request.description.pixel_texture_decode_bits));
  for (uint32_t i = 0; i < 4; ++i) {
    states[SpirvShaderTranslator::kFetchStateColorBlendBinding * 4 + i] =
        (request.description.shader_blend_mask & (1u << i))
            ? (request.description.blendcontrol[i] & kBlendControlMask) |
                  (request.description.color_formats[i] ==
                           uint32_t(MTL::PixelFormatRG16Snorm)
                       ? SpirvShaderTranslator::kColorBlendFixed16TwoChannels
                       : 0)
            : kBlendControlNoBlending;
  }
  return GetSpecializedFunction(
      request.fragment_library, request.fragment_function_name, states,
      SpirvShaderTranslator::kFetchStateBindingCount, error_out);
}

bool MetalCommandProcessor::UsesVertexFetchStateFunctionConstants(
    const Shader::Translation* translation) const {
  return spirv_translator_features_.fetch_state_function_constants &&
         translation &&
         !Shader::IsHostVertexShaderTypeDomain(
             SpirvShaderTranslator::Modification(translation->modification())
                 .vertex.host_vertex_shader_type) &&
         (!translation->shader().vertex_bindings().empty() ||
          UsesPixelFetchStateFunctionConstants(translation));
}

bool MetalCommandProcessor::UsesPixelFetchStateFunctionConstants(
    const Shader::Translation* translation) const {
  if (!spirv_translator_features_.fetch_state_function_constants ||
      !translation) {
    return false;
  }
  const auto& constants = translation->shader().constant_register_map();
  return SpirvShaderTranslator::Modification(translation->modification())
             .pixel.host_render_target_feedback ||
         (spirv_translator_features_.optimize_finite_multiplication &&
          GetFiniteMultiplyRequirements(translation->shader())) ||
         !translation->shader().texture_bindings().empty() ||
         constants.loop_bitmap ||
         std::any_of(std::begin(constants.bool_bitmap),
                     std::end(constants.bool_bitmap),
                     [](uint32_t bits) { return bits != 0; });
}

const MetalCommandProcessor::FiniteMultiplyInfo&
MetalCommandProcessor::GetFiniteMultiplyInfo(const Shader& shader) const {
  const size_t last_index = shader.type() == xenos::ShaderType::kPixel;
  if (finite_multiply_last_shader_[last_index] == &shader) {
    return *finite_multiply_last_info_[last_index];
  }
  auto found = finite_multiply_requirements_.find(&shader);
  if (found == finite_multiply_requirements_.end()) {
    SpirvShaderTranslator::FiniteMultiplyAnalysis analysis;
    SpirvShaderTranslator::AnalyzeFiniteMultiplications(shader, analysis);
    FiniteMultiplyInfo info;
    info.required = analysis.dynamic_inputs;
    std::memcpy(info.direct_constants, analysis.direct_constants,
                sizeof(info.direct_constants));
    found = finite_multiply_requirements_.emplace(&shader, info).first;
  }
  finite_multiply_last_shader_[last_index] = &shader;
  finite_multiply_last_info_[last_index] = &found->second;
  return found->second;
}

uint64_t MetalCommandProcessor::GetFiniteMultiplyRequirements(
    const Shader& shader) const {
  return spirv_translator_features_.optimize_finite_multiplication
             ? GetFiniteMultiplyInfo(shader).required
             : 0;
}

namespace {
bool FiniteMultiplyTextureFormat(xenos::TextureFormat format) {
  // These packed normalized/small-integer formats remain finite through
  // bounded sign, gamma and exponent adjustments. Other formats are unknown.
  switch (format) {
    case xenos::TextureFormat::k_1_REVERSE:
    case xenos::TextureFormat::k_1:
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_1_5_5_5:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_6_5_5:
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_4_4_4_4:
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_16_16:
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_DXT5A:
    case xenos::TextureFormat::k_CTX1:
    case xenos::TextureFormat::k_DXT3A_AS_1_1_1_1:
      return true;
    default:
      return false;
  }
}
}  // namespace

void MetalCommandProcessor::GetFiniteMultiplyState(
    const Shader::Translation* translation, const RegisterFile& regs,
    uint32_t* state) const {
  const auto& info = GetFiniteMultiplyInfo(translation->shader());
  uint64_t required = info.required;
  uint32_t textures =
      uint32_t((required & SpirvShaderTranslator::kFiniteMultiplyTextures) >>
               SpirvShaderTranslator::kFiniteMultiplyTextureShift);
  uint32_t index;
  while (xe::bit_scan_forward(textures, &index)) {
    textures &= ~(UINT32_C(1) << index);
    const uint32_t* fetch =
        &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + index * 6];
    if ((fetch[0] & 3) != uint32_t(xenos::FetchConstantType::kTexture) ||
        !FiniteMultiplyTextureFormat(xenos::TextureFormat(fetch[1] & 63))) {
      state[0] |= UINT32_C(1) << index;
    }
  }
  if (!(required & SpirvShaderTranslator::kFiniteMultiplyFloatConstants)) {
    return;
  }
  const uint32_t* values =
      &regs.values[translation->shader().type() == xenos::ShaderType::kPixel
                       ? XE_GPU_REG_SHADER_CONSTANT_256_X
                       : XE_GPU_REG_SHADER_CONSTANT_000_X];

  for (uint32_t word = 0; word < 4; ++word) {
    uint64_t used = info.direct_constants[word];
    while (xe::bit_scan_forward(used, &index)) {
      used &= ~(UINT64_C(1) << index);
      for (uint32_t component = 0; component < 4; ++component) {
        if ((values[(word * 64 + index) * 4 + component] &
             UINT32_C(0x7F800000)) == UINT32_C(0x7F800000)) {
          state[1] = 1;
          return;
        }
      }
    }
  }
}

void MetalCommandProcessor::SetPipelineFunctionConstants(
    PipelineCompileRequest& request, const Shader::Translation* vertex,
    const Shader::Translation* pixel) {
  request.vertex_fetch_states_specialized =
      UsesVertexFetchStateFunctionConstants(vertex);
  request.fragment_fetch_states_specialized =
      UsesPixelFetchStateFunctionConstants(pixel);
  if (request.vertex_fetch_states_specialized) {
    auto* translation = static_cast<const DxilShader::DxilTranslation*>(vertex);
    request.vertex_library = translation->metal_library();
    request.vertex_function_name = translation->entry_point_name();
  }
  if (request.fragment_fetch_states_specialized) {
    auto* translation = static_cast<const DxilShader::DxilTranslation*>(pixel);
    request.fragment_library = translation->metal_library();
    request.fragment_function_name = translation->entry_point_name();
  }
}

MTL::RenderPipelineState* MetalCommandProcessor::CreatePipelineState(
    const PipelineCompileRequest& request, std::string* error_out) {
  if (error_out) {
    error_out->clear();
  }
  switch (request.description.kind) {
    case PipelineKind::kMslTessellation:
      return CreateMslTessellationPipelineState(request, error_out);
    case PipelineKind::kDxilTessellation:
      return CreateDxilTessellationPipelineState(request, error_out);
    case PipelineKind::kRender:
      break;
  }
  MTL::Function* vertex_function =
      GetPipelineVertexFunction(request, error_out);
  if (!vertex_function) {
    if (error_out) {
      *error_out = "missing vertex shader function";
    }
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vertex_function);
  MTL::Function* fragment_function =
      GetPipelineFragmentFunction(request, error_out);
  if (request.fragment_fetch_states_specialized && !fragment_function) {
    desc->release();
    return nullptr;
  }
  if (fragment_function) {
    desc->setFragmentFunction(fragment_function);
  }

  ApplyColorAttachmentState(desc->colorAttachments(), request);
  desc->setDepthAttachmentPixelFormat(
      MTL::PixelFormat(request.description.depth_format));
  desc->setStencilAttachmentPixelFormat(
      MTL::PixelFormat(request.description.stencil_format));
  desc->setSampleCount(request.description.sample_count);
  // Xenos alpha-to-mask is emitted by the shader as its sample mask, so the
  // native alpha-to-coverage stays off.
  desc->setAlphaToCoverageEnabled(false);

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      pipeline_archive_.NewRenderPipelineState(device_, desc, &error);
  desc->release();

  if (!pipeline && error_out && error) {
    NS::String* description = error->localizedDescription();
    if (description) {
      *error_out = description->utf8String();
    }
  }

  return pipeline;
}

MTL::RenderPipelineState*
MetalCommandProcessor::CreateMslTessellationPipelineState(
    const PipelineCompileRequest& request, std::string* error_out) {
  if (!request.vertex_function) {
    if (error_out) {
      *error_out = "missing domain shader function";
    }
    return nullptr;
  }
  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  // The post-tessellation vertex function IS the domain shader.
  desc->setVertexFunction(request.vertex_function);
  if (request.fragment_function) {
    desc->setFragmentFunction(request.fragment_function);
  } else if (depth_only_pixel_library_ &&
             !depth_only_pixel_function_name_.empty()) {
    auto* fn_name = NS::String::string(depth_only_pixel_function_name_.c_str(),
                                       NS::UTF8StringEncoding);
    MTL::Function* depth_fn = depth_only_pixel_library_->newFunction(fn_name);
    if (depth_fn) {
      desc->setFragmentFunction(depth_fn);
      depth_fn->release();
    }
  }

  desc->setMaxTessellationFactor(64);
  desc->setTessellationFactorStepFunction(
      MTL::TessellationFactorStepFunctionPerPatch);
  switch (request.description.tessellation_mode) {
    case xenos::TessellationMode::kDiscrete:
      desc->setTessellationPartitionMode(MTL::TessellationPartitionModeInteger);
      break;
    case xenos::TessellationMode::kContinuous:
    case xenos::TessellationMode::kAdaptive:
      desc->setTessellationPartitionMode(
          MTL::TessellationPartitionModeFractionalEven);
      break;
  }
  // The domain shader reads control points from shared memory.
  desc->setTessellationControlPointIndexType(
      MTL::TessellationControlPointIndexTypeNone);

  ApplyColorAttachmentState(desc->colorAttachments(), request);
  desc->setDepthAttachmentPixelFormat(
      MTL::PixelFormat(request.description.depth_format));
  desc->setStencilAttachmentPixelFormat(
      MTL::PixelFormat(request.description.stencil_format));
  desc->setSampleCount(request.description.sample_count);
  // Xenos alpha-to-mask is emitted by the shader as its sample mask, so the
  // native alpha-to-coverage stays off.
  desc->setAlphaToCoverageEnabled(false);

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);
  desc->release();
  if (!pipeline && error_out && error) {
    NS::String* description = error->localizedDescription();
    if (description) {
      *error_out = description->utf8String();
    }
  }
  return pipeline;
}

MTL::RenderPipelineState*
MetalCommandProcessor::CreateDxilTessellationPipelineState(
    const PipelineCompileRequest& request, std::string* error_out) {
  const DxilTessellationShaders* shaders = request.tessellation_shaders;
  if (!shaders) {
    if (error_out) {
      *error_out = "missing tessellation stages";
    }
    return nullptr;
  }
  const MetalShaderReflection& hull = shaders->hull.reflection;
  const MetalShaderReflection& domain = shaders->domain.reflection;
  auto output_primitive =
      IRRuntimeTessellatorOutputPrimitive(hull.hs_tessellator_output_primitive);
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
    default:
      break;
  }
  if (!IRRuntimeValidateTessellationPipeline(
          output_primitive, geometry_primitive,
          hull.hs_output_control_point_size, domain.ds_input_control_point_size,
          hull.hs_patch_constants_size, domain.ds_patch_constants_size,
          hull.hs_output_control_point_count,
          domain.ds_input_control_point_count)) {
    if (error_out) {
      *error_out = "hull and domain stages are not compatible";
    }
    return nullptr;
  }

  MTL::MeshRenderPipelineDescriptor* desc =
      MTL::MeshRenderPipelineDescriptor::alloc()->init();
  ApplyColorAttachmentState(desc->colorAttachments(), request);
  desc->setDepthAttachmentPixelFormat(
      MTL::PixelFormat(request.description.depth_format));
  desc->setStencilAttachmentPixelFormat(
      MTL::PixelFormat(request.description.stencil_format));
  desc->setRasterSampleCount(request.description.sample_count);
  // Xenos alpha-to-mask is emitted by the shader as its sample mask, so the
  // native alpha-to-coverage stays off.
  desc->setAlphaToCoverageEnabled(false);

  // Linked the way IRRuntimeNewGeometryTessellationEmulationPipeline does, but
  // with a fragment function rather than a library and a name, as a pixel
  // shader specialized on bool and loop constants needs function constants.
  MTL::Function* fragment_function =
      GetPipelineFragmentFunction(request, error_out);
  if (request.fragment_fetch_states_specialized && !fragment_function) {
    desc->release();
    return nullptr;
  }
  IRRuntimeTessellationPipelineConfig config =
      BuildTessellationPipelineConfig(shaders->vertex.reflection, hull, domain);
  auto new_function = [](MTL::Library* library, const std::string& name,
                         MTL::FunctionConstantValues* values,
                         NS::Error** error) -> MTL::Function* {
    if (!library) {
      return nullptr;
    }
    NS::String* ns_name =
        NS::String::string(name.c_str(), NS::UTF8StringEncoding);
    return values ? library->newFunction(ns_name, values, error)
                  : library->newFunction(ns_name);
  };
  NS::Error* error = nullptr;
  MTL::FunctionConstantValues* vertex_values =
      MTL::FunctionConstantValues::alloc()->init();
  bool tessellation_enabled = true;
  vertex_values->setConstantValue(&tessellation_enabled, MTL::DataTypeBool,
                                  MTLSTR("tessellationEnabled"));
  MTL::FunctionConstantValues* hull_values =
      MTL::FunctionConstantValues::alloc()->init();
  hull_values->setConstantValue(&config.vsOutputSizeInBytes, MTL::DataTypeInt,
                                MTLSTR("vertex_shader_output_size_fc"));
  hull_values->setConstantValue(&config.hsMaxTessellationFactor,
                                MTL::DataTypeFloat,
                                MTLSTR("max_tessellation_factor_fc"));
  // The guest fetches vertices from shared memory, so the host vertex shader
  // takes no attributes, but the pipeline still links a stage-in function.
  MTL::Function* stage_in_function =
      new_function(shaders->stage_in_library, "irconverter_stage_in_shader",
                   nullptr, nullptr);
  // The vertex stage is emulated in the object function.
  MTL::Function* object_function = new_function(
      shaders->vertex.library,
      shaders->vertex.function_name + ".dxil_irconverter_object_shader",
      vertex_values, &error);
  // MSC compiles the hull stage into the control point and patch constant
  // phases, and the tessellator.
  MTL::Function* hull_function = new_function(
      shaders->hull.library, "irconverter_hull_shader", hull_values, &error);
  MTL::Function* tessellator_function = new_function(
      shaders->hull.library, "irconverter_tessellator", hull_values, &error);
  MTL::Function* domain_function =
      new_function(shaders->domain.library, "irconverter_dxil_domain_shader",
                   nullptr, nullptr);
  // The guest has no geometry shader. MSC synthesizes the passthrough that
  // the mesh function emulates the geometry stage with.
  MTL::Function* mesh_function = new_function(
      shaders->domain.library, geometry_function, nullptr, nullptr);
  vertex_values->release();
  hull_values->release();

  MTL::RenderPipelineState* pipeline = nullptr;
  if (stage_in_function && object_function && hull_function &&
      tessellator_function && domain_function && mesh_function) {
    desc->setObjectFunction(object_function);
    desc->setMeshFunction(mesh_function);
    desc->setFragmentFunction(fragment_function);
    const NS::Object* object_linked[] = {stage_in_function, hull_function};
    MTL::LinkedFunctions* object_linked_functions =
        MTL::LinkedFunctions::alloc()->init();
    object_linked_functions->setFunctions(
        NS::Array::array(object_linked, std::size(object_linked)));
    desc->setObjectLinkedFunctions(object_linked_functions);
    object_linked_functions->release();
    const NS::Object* mesh_linked[] = {tessellator_function, domain_function};
    MTL::LinkedFunctions* mesh_linked_functions =
        MTL::LinkedFunctions::alloc()->init();
    mesh_linked_functions->setFunctions(
        NS::Array::array(mesh_linked, std::size(mesh_linked)));
    desc->setMeshLinkedFunctions(mesh_linked_functions);
    mesh_linked_functions->release();
    error = nullptr;
    pipeline = device_->newRenderPipelineState(desc, MTL::PipelineOptionNone,
                                               nullptr, &error);
  } else if (error_out && !error) {
    *error_out = "missing tessellation stage function";
  }
  for (MTL::Function* function :
       {stage_in_function, object_function, hull_function, tessellator_function,
        domain_function, mesh_function}) {
    if (function) {
      function->release();
    }
  }
  desc->release();
  if (!pipeline && error_out && error) {
    NS::String* description = error->localizedDescription();
    if (description) {
      *error_out = description->utf8String();
    }
  }
  return pipeline;
}

void MetalCommandProcessor::AsyncCompileThread(size_t thread_index) {
  xe::threading::set_name(fmt::format("Metal Shaders {}", thread_index));
  std::unique_ptr<SpirvShaderTranslator> worker_translator =
      CreateSpirvShaderTranslator();
  while (true) {
    enum class RequestKind { kPipeline, kShader, kTessellationShaders };
    RequestKind request_kind = RequestKind::kPipeline;
    ShaderCompileRequest shader_request;
    PipelineCompileRequest pipeline_request;
    TessellationShadersCompileRequest tess_shaders_request;
    {
      std::unique_lock<std::mutex> lock(async_compile_mutex_);
      async_compile_boost_cv_.wait(lock, [this, thread_index]() {
        return async_compile_shutdown_ ||
               thread_index < async_compile_thread_limit_;
      });
      async_compile_cv_.wait(lock, [this, thread_index]() {
        return async_compile_shutdown_ ||
               thread_index >= async_compile_thread_limit_ ||
               !async_shader_queue_.empty() ||
               !async_pipeline_queue_.empty() ||
               !async_tess_shaders_queue_.empty();
      });
      if (async_compile_shutdown_) {
        return;
      }
      if (thread_index >= async_compile_thread_limit_) {
        // The boost ended: park.
        continue;
      }
      // Pipelines first: they are the nearly finished work, and everything
      // else exists to unblock one. Except the stored ones a launch rebuilds:
      // the shaders and tessellation stages draws are waiting for go first.
      if (!async_pipeline_queue_.empty() &&
          (async_pipeline_queue_.begin()->second.priority >
               kPipelinePriorityStored ||
           (async_shader_queue_.empty() &&
            async_tess_shaders_queue_.empty()))) {
        request_kind = RequestKind::kPipeline;
        auto queue_it = async_pipeline_queue_.begin();
        pipeline_request = std::move(queue_it->second);
        async_pipeline_queued_.erase(pipeline_request.pipeline_key);
        async_pipeline_queue_.erase(queue_it);
      } else if (!async_shader_queue_.empty()) {
        request_kind = RequestKind::kShader;
        shader_request = async_shader_queue_.top();
        async_shader_queue_.pop();
      } else {
        request_kind = RequestKind::kTessellationShaders;
        tess_shaders_request = async_tess_shaders_queue_.front();
        async_tess_shaders_queue_.pop();
      }
      ++async_compile_busy_;
    }

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    if (request_kind == RequestKind::kTessellationShaders) {
      auto shaders = std::make_unique<DxilTessellationShaders>();
      bool compiled = BuildDxilTessellationShaders(
          *tess_shaders_request.domain_spirv,
          tess_shaders_request.domain_ucode_hash,
          tess_shaders_request.tessellation_mode,
          tess_shaders_request.host_vertex_shader_type, *shaders);
      std::lock_guard<std::mutex> lock(async_compile_mutex_);
      async_tess_shaders_pending_.erase(tess_shaders_request.key);
      if (compiled) {
        // The pending set admits one build per key, so this always inserts.
        // A loser would release its stages through the destructor.
        dxil_tessellation_cache_.emplace(tess_shaders_request.key,
                                         std::move(shaders));
      } else {
        async_tess_shaders_failed_.insert(tess_shaders_request.key);
      }
      FinishAsyncCompileTaskLocked();
    } else if (request_kind == RequestKind::kPipeline) {
      std::string pipeline_error;
      MTL::RenderPipelineState* pipeline =
          CreatePipelineState(pipeline_request, &pipeline_error);
      bool compiled = pipeline != nullptr;

      {
        std::lock_guard<std::mutex> lock(async_compile_mutex_);
        async_pipeline_pending_.erase(pipeline_request.pipeline_key);
        NoteStoredPipelineDoneLocked(pipeline_request.pipeline_key);
        if (compiled) {
          auto insert_result = async_pipeline_cache_.emplace(
              pipeline_request.pipeline_key, pipeline);
          if (!insert_result.second && pipeline) {
            pipeline->release();
          }
          async_pipeline_failed_.erase(pipeline_request.pipeline_key);
        } else {
          async_pipeline_failed_.insert(pipeline_request.pipeline_key);
        }
        FinishAsyncCompileTaskLocked();
      }

      if (pipeline_request.vertex_function) {
        pipeline_request.vertex_function->release();
      }
      if (pipeline_request.fragment_function) {
        pipeline_request.fragment_function->release();
      }

      if (!compiled && ShouldLogRateLimited(async_pipeline_failure_last_log_ns_,
                                            kAsyncCompileLogIntervalNs)) {
        if (!pipeline_error.empty()) {
          XELOGE(
              "Metal: async pipeline compile failed on worker {} "
              "(VS {:016X} mod {:016X}, PS {:016X} mod {:016X}): {}",
              thread_index, pipeline_request.description.vertex_shader_hash,
              pipeline_request.description.vertex_shader_modification,
              pipeline_request.description.pixel_shader_hash,
              pipeline_request.description.pixel_shader_modification,
              pipeline_error);
        } else {
          XELOGE(
              "Metal: async pipeline compile failed on worker {} "
              "(VS {:016X} mod {:016X}, PS {:016X} mod {:016X})",
              thread_index, pipeline_request.description.vertex_shader_hash,
              pipeline_request.description.vertex_shader_modification,
              pipeline_request.description.pixel_shader_hash,
              pipeline_request.description.pixel_shader_modification);
        }
      }
    } else {
      ShaderCompileStatus translation_status =
          shader_request.translation
              ? TranslateAndCompileHostShader(shader_request.translation,
                                              *worker_translator,
                                              shader_request.is_ios)
              : ShaderCompileStatus::kFailed;
      bool compiled = translation_status == ShaderCompileStatus::kReady;
      // Nothing queues a translation twice and the draw thread only translates
      // one that is not queued, so losing the claim here shouldn't happen.
      // Leave it unqueued for a later draw to retry rather than fail it
      // forever, which would black out the shader for the whole run.
      bool contended = translation_status == ShaderCompileStatus::kPending;

      {
        std::lock_guard<std::mutex> lock(async_compile_mutex_);
        if (shader_request.translation) {
          async_shader_pending_.erase(shader_request.translation);
          if (!compiled && !contended) {
            async_shader_failed_.insert(shader_request.translation);
          }
        }
        FinishAsyncCompileTaskLocked();
      }

      if (!compiled && !contended &&
          ShouldLogRateLimited(async_shader_failure_last_log_ns_,
                               kAsyncCompileLogIntervalNs)) {
        XELOGE(
            "Metal: async shader compile failed on worker {} (shader "
            "{:016X}, mod {:016X})",
            thread_index, shader_request.shader_hash,
            shader_request.modification);
      }
    }
    pool->release();
  }
}

MetalCommandProcessor::SpirvArgumentBufferPage::~SpirvArgumentBufferPage() {
  if (buffer) {
    buffer->release();
    buffer = nullptr;
  }
}

void MetalCommandProcessor::TracePlaybackWillWriteMemory(uint32_t base_ptr,
                                                         uint32_t length) {
  if (!shared_memory_ || !shared_memory_->is_zero_copy() || !length) {
    return;
  }
  // A resolve left to the end of the open pass writes there only when the
  // submission ends, so it has to land before the restore too.
  const bool post_pass_write =
      render_target_cache_ &&
      render_target_cache_->PostPassResolvesWrite(base_ptr, length);
  auto* hazards = shared_memory_->hazards();
  if (post_pass_write || !hazards ||
      render_encoder_shared_memory_hazards_.HasAccess(base_ptr, length) ||
      hazards->HasPendingAccess(base_ptr, length)) {
    // Submission alone does not keep the GPU from seeing a later CPU restore.
    // Consume this recorded version before overwriting aliased guest RAM.
    int64_t wait_start_ns = GetSteadyTimeNs();
    AwaitAllQueueOperationsCompletion();
    NoteGpuWait(GpuWaitReason::kTracePlayback, wait_start_ns);
  }
}

void MetalCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                     uint32_t length) {
  if (shared_memory_) {
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
  if (primitive_processor_) {
    primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
  }
}

void MetalCommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();

  auto abandon_trace = [this](const char* reason) {
    XELOGE("Metal: abandoning frame trace: {}", reason);
    trace_writer_.Close();
    trace_state_ = TraceState::kDisabled;
    trace_frame_file_path_.clear();
  };

  // The resolve work left to the end of an open pass changes ownership too,
  // and comes first.
  if (RenderEncoderResolveOnly()) {
    EndRenderEncoder();
  }
  // Ownership may already name destinations whose transfers were deferred by
  // an abandoned draw. Materialize them before the standalone snapshot reads.
  if (render_target_cache_ &&
      !render_target_cache_->FlushPendingDrawPassTransfers()) {
    abandon_trace("the queued ownership transfers could not be completed");
    return;
  }

  // Neither download is bracketed by a submission of its own, so everything in
  // flight has to have landed before they read what the GPU wrote.
  EndCommandBuffer();
  if (submission_current_) {
    AwaitSubmissionCompletion(submission_current_);
  }

  if (render_target_cache_) {
    // DumpRenderTargets uses submission-owned argument pages even when its
    // compute commands are submitted separately. EndCommandBuffer above removed
    // that owner. Keep a fresh submission alive while the standalone dump and
    // readback run synchronously; the next guest draw can reuse it afterwards.
    if (!EnsureCommandBuffer() ||
        !render_target_cache_->InitializeTraceSubmitDownloads()) {
      abandon_trace("the initial EDRAM snapshot could not be captured");
      return;
    }
    render_target_cache_->InitializeTraceCompleteDownloads();
  }
  if (shared_memory_ && shared_memory_->InitializeTraceSubmitDownloads()) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

bool MetalCommandProcessor::DumpEdramSnapshotToFile(
    const std::filesystem::path& path) {
  if (!render_target_cache_) {
    return false;
  }
  // The resolve work left to the end of an open pass changes ownership too,
  // and comes first.
  if (RenderEncoderResolveOnly()) {
    EndRenderEncoder();
  }
  // Ownership already names the destinations of transfers queued for the next
  // draw's pass, which a replay stopped mid-frame never encodes.
  if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
    return false;
  }
  // As in InitializeTrace, the download reads what everything in flight wrote.
  EndCommandBuffer();
  if (submission_current_) {
    AwaitSubmissionCompletion(submission_current_);
  }
  if (!render_target_cache_->InitializeTraceSubmitDownloads()) {
    return false;
  }
  return render_target_cache_->WriteEdramSnapshotToFile(path);
}

void MetalCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Restore the guest EDRAM snapshot captured in the trace into the Metal
  // render-target cache so that subsequent host render targets created from
  // EDRAM (via LoadTiledData) see the same initial contents as other
  // backends like D3D12.
  if (!snapshot) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called with null "
        "snapshot");
    return;
  }
  if (!render_target_cache_) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called before render "
        "target "
        "cache initialization");
    return;
  }
  // Restoring the snapshot also dumps its bootstrap render target. The dump's
  // descriptor allocations require a live submission, just like capture.
  if (!EnsureCommandBuffer()) {
    XELOGE("Metal: unable to begin submission for EDRAM snapshot restore");
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

void MetalCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  // TODO(wmarti): Add cache_clear_requested_ flag like D3D12 for deferred
  // clearing of pipeline caches, texture caches, etc.
}

void MetalCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void MetalCommandProcessor::ClearReadbackBuffers() {
  // TODO(wmarti): Implement readback buffer clearing when resolve readback
  // is added. See D3D12's readback_buffers_.
}

ui::metal::MetalProvider& MetalCommandProcessor::GetMetalProvider() const {
  return *static_cast<ui::metal::MetalProvider*>(graphics_system_->provider());
}

uint64_t MetalCommandProcessor::GetCurrentSubmission() const {
  return submission_current_ ? submission_current_ : 1;
}

uint64_t MetalCommandProcessor::GetCompletedSubmission() const {
  return completed_command_buffers_.load(std::memory_order_acquire);
}

bool MetalCommandProcessor::TryWriteShaderDoneFence(uint32_t address,
                                                    uint32_t value) {
  if (!shared_memory_ || !shared_memory_->is_zero_copy()) {
    return false;
  }
  if (!trace_writer_.is_open()) {
    // The ordered copy would end an open render pass. Nothing in the pass sees
    // the fence before the pass ends except a later draw of it accessing the
    // address, which ends the pass first (IssueDraw), so the copy is encoded
    // when the pass ends, still after the work the fence follows.
    bool deferred = current_render_encoder_ &&
                    address <= SharedMemory::kBufferSize - sizeof(value) &&
                    shared_memory_->RequestRange(address, sizeof(value));
    // With no pass open, the fence is copied before whatever is encoded next,
    // where its own blit encoder would have been, together with any fences
    // that follow it before that. The guest's fences come in runs with nothing
    // encoded between them, as Halo 3 writes them before a frame's first draw.
    if (!deferred && !current_render_encoder_ && (address & 3) == 0 &&
        address <= SharedMemory::kBufferSize - sizeof(value) &&
        EnsureCommandBuffer()) {
      if (!shader_done_fences_outside_pass_) {
        // Ordered after everything encoded so far, as CopyToBufferGpuOrdered
        // orders it: ends the shared compute encoder, and encodes the resolves
        // a pass left to its end. Fences already queued mean nothing was
        // encoded since the first of them.
        EndRenderEncoder();
      }
      if (!current_render_encoder_ &&
          shared_memory_->RequestRange(address, sizeof(value))) {
        deferred = true;
        shader_done_fences_outside_pass_ = true;
      }
    }
    if (deferred) {
      pending_shader_done_fence_ranges_.emplace_back(address >> 2,
                                                     uint32_t(sizeof(value)));
      pending_shader_done_fence_values_.push_back(value);
      shared_memory_->RangeWrittenByGpu(address, sizeof(value));
    }
    if (deferred || shared_memory_->WriteGuestMemoryGpuOrdered(address, &value,
                                                               sizeof(value))) {
      // The guest may stop submitting commands while polling this fence. The
      // pending resolve write makes OnPrimaryBufferEnd commit this submission,
      // and a ring-empty stall commits it in PrepareForWait, so the fence is
      // published without a submission of its own.
      copy_resolve_writes_pending_ = true;
      return true;
    }
  }
  // Trace records read guest RAM immediately. Allocation failure also needs a
  // safe CPU fallback: never report completion while the GPU still uses RAM.
  int64_t wait_start_ns = GetSteadyTimeNs();
  AwaitAllQueueOperationsCompletion();
  NoteGpuWait(GpuWaitReason::kShaderDoneFence, wait_start_ns);
  return false;
}

void MetalCommandProcessor::EncodeDeferredShaderDoneFences() {
  // Cleared first: EndSharedComputeEncoder below would encode them again.
  shader_done_fences_outside_pass_ = false;
  if (pending_shader_done_fence_values_.empty()) {
    return;
  }
  uint32_t size = uint32_t(pending_shader_done_fence_values_.size() *
                           sizeof(pending_shader_done_fence_values_[0]));
  MTL::Buffer* source = nullptr;
  NS::UInteger source_offset = 0;
  MTL::BlitCommandEncoder* encoder = nullptr;
  if (AcquireSpirvArgumentBufferSlice(size, 16, &source, &source_offset)) {
    EndSharedComputeEncoder();
    encoder = current_command_buffer_->blitCommandEncoder();
  }
  if (encoder) {
    std::memcpy(static_cast<uint8_t*>(source->contents()) + source_offset,
                pending_shader_done_fence_values_.data(), size);
    MetalBufferHazards::Encoder hazards;
    if (shared_memory_->hazards()) {
      hazards.Begin(*shared_memory_->hazards(), current_command_buffer_,
                    encoder);
      // Keep shader-done publication ordered after all preceding buffer uses.
      hazards.OrderPreviousAccesses();
    }
    for (size_t i = 0; i < pending_shader_done_fence_ranges_.size(); ++i) {
      const uint32_t address =
          pending_shader_done_fence_ranges_[i].base_address_dwords << 2;
      if (hazards.is_active()) {
        hazards.Access(address, sizeof(uint32_t), true);
      }
      encoder->copyFromBuffer(source, source_offset + i * sizeof(uint32_t),
                              shared_memory_->GetBuffer(), address,
                              sizeof(uint32_t));
    }
    hazards.End();
    encoder->endEncoding();
  } else {
    // EndRenderEncoder has already cleared the active encoder. Submit and
    // finish its work before publishing completion on the CPU. The nested
    // submission end cannot re-enter fence encoding without an active encoder.
    XELOGE("Metal: couldn't encode shader-done fence writes, waiting for GPU");
    assert_null(current_render_encoder_);
    int64_t wait_start_ns = GetSteadyTimeNs();
    AwaitAllQueueOperationsCompletion();
    NoteGpuWait(GpuWaitReason::kShaderDoneFence, wait_start_ns);
    for (size_t i = 0; i < pending_shader_done_fence_ranges_.size(); ++i) {
      std::memcpy(
          memory_->TranslatePhysical(
              pending_shader_done_fence_ranges_[i].base_address_dwords << 2),
          &pending_shader_done_fence_values_[i], sizeof(uint32_t));
    }
  }
  pending_shader_done_fence_ranges_.clear();
  pending_shader_done_fence_values_.clear();
}

void MetalCommandProcessor::NoteMemexportRangesWritten() {
  if (!shared_memory_ || memexport_ranges_.empty()) {
    return;
  }
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t base_bytes = memexport_range.base_address_dwords << 2;
    shared_memory_->RangeWrittenByGpu(base_bytes, memexport_range.size_bytes);
    MarkMemexportPagesWritten(base_bytes, memexport_range.size_bytes,
                              memexport_ranges_awaited_);
  }
  copy_resolve_writes_pending_ = true;
}

bool MetalCommandProcessor::CanKeepRenderPassAfterTileResolve() const {
  // Past this many guest indices in one pass, the tiler's parameter buffer may
  // overflow and split the pass into partial renders anyway (assumed from the
  // measured knee of 1-1.5 M small triangles).
  constexpr uint64_t kTileResolvePassIndexBudget = 1000000;
  return current_render_encoder_ &&
         render_encoder_index_count_ <= kTileResolvePassIndexBudget;
}

void MetalCommandProcessor::NoteTileResolveWrite(uint32_t start,
                                                 uint32_t length) {
  if (!length) {
    return;
  }
  AddPendingWrite(render_encoder_tile_resolve_ranges_,
                  draw_util::MemExportRange(start >> 2, (start & 3) + length));
}

bool MetalCommandProcessor::TileResolveWritesOverlap(uint32_t start,
                                                     uint32_t length) const {
  for (const auto& written : render_encoder_tile_resolve_ranges_) {
    uint64_t written_base = uint64_t(written.base_address_dwords) << 2;
    if (uint64_t(start) < written_base + written.size_bytes &&
        written_base < uint64_t(start) + length) {
      return true;
    }
  }
  return false;
}

bool MetalCommandProcessor::DrawOverlapsPendingWrites(
    const std::vector<draw_util::MemExportRange>& pending_writes,
    const Shader& vertex_shader, const Shader* pixel_shader,
    const IndexBufferInfo* index_buffer_info,
    const draw_util::VertexIndexRange* vertex_indices) const {
  if (pending_writes.empty()) {
    return false;
  }
  auto overlaps = [&](uint32_t base, uint64_t length) {
    if (!length) {
      return false;
    }
    for (const auto& written : pending_writes) {
      uint64_t written_base = uint64_t(written.base_address_dwords) << 2;
      if (uint64_t(base) < written_base + written.size_bytes &&
          written_base < uint64_t(base) + length) {
        return true;
      }
    }
    return false;
  };
  // Both shader stages can perform guest vertex fetches from shared memory.
  auto fetches_overlap = [&](const Shader& shader) {
    if (vertex_indices && shader.vertex_fetches_vertex_indexed()) {
      for (const Shader::VertexFetchStride& stride :
           shader.vertex_fetch_strides()) {
        const auto fetch =
            register_file_->GetVertexFetch(stride.fetch_constant);
        int64_t first =
            std::max(int64_t(vertex_indices->first) * stride.stride_words +
                         shader.vertex_fetch_min_word_offset(),
                     int64_t(0));
        int64_t end =
            std::min(int64_t(vertex_indices->last) * stride.stride_words +
                         shader.vertex_fetch_end_word_offset(),
                     int64_t(fetch.size));
        if (first < end &&
            overlaps(xenos::CpuToGpu(fetch.address << 2) + uint32_t(first << 2),
                     uint64_t(end - first) << 2)) {
          return true;
        }
      }
      return false;
    }
    const auto& bitmap = shader.constant_register_map().vertex_fetch_bitmap;
    for (uint32_t i = 0; i < xe::countof(bitmap); ++i) {
      uint32_t bits = bitmap[i], bit;
      while (xe::bit_scan_forward(bits, &bit)) {
        bits &= ~(uint32_t(1) << bit);
        const auto fetch = register_file_->GetVertexFetch(i * 32 + bit);
        if (overlaps(xenos::CpuToGpu(fetch.address << 2), uint64_t(fetch.size)
                                                              << 2)) {
          return true;
        }
      }
    }
    return false;
  };
  if (fetches_overlap(vertex_shader) ||
      (pixel_shader && fetches_overlap(*pixel_shader))) {
    return true;
  }
  if (index_buffer_info &&
      overlaps(xenos::CpuToGpu(index_buffer_info->guest_base),
               index_buffer_info->length)) {
    return true;
  }
  // Overlapping exports also require ordering, including partial-word RMW.
  for (const auto& written : memexport_ordering_ranges_) {
    if (overlaps(written.base_address_dwords << 2, written.size_bytes)) {
      return true;
    }
  }
  return false;
}

void MetalCommandProcessor::ForceIssueSwap() {
  // Force a swap to push any pending render target to presenter
  // This is used by trace dumps to capture output when there's no explicit swap
  if (saw_swap_) {
    return;
  }
  IssueSwap(0, render_target_width_, render_target_height_);
}

void MetalCommandProcessor::SetSwapDestSwap(uint32_t dest_base, bool swap) {
  if (!dest_base) {
    return;
  }
  if (swap_dest_swaps_by_base_.size() > 256) {
    swap_dest_swaps_by_base_.clear();
  }
  swap_dest_swaps_by_base_[dest_base] = swap;
}

bool MetalCommandProcessor::ConsumeSwapDestSwap(uint32_t dest_base,
                                                bool* swap_out) {
  if (!swap_out || !dest_base) {
    return false;
  }
  auto it = swap_dest_swaps_by_base_.find(dest_base);
  if (it == swap_dest_swaps_by_base_.end()) {
    return false;
  }
  *swap_out = it->second;
  swap_dest_swaps_by_base_.erase(it);
  return true;
}

bool MetalCommandProcessor::SetupContext() {
  saw_swap_ = false;
  last_swap_ptr_ = 0;
  last_swap_width_ = 0;
  last_swap_height_ = 0;
  swap_dest_swaps_by_base_.clear();
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  const ui::metal::MetalProvider& provider = GetMetalProvider();
  device_ = provider.GetDevice();
  command_queue_ = provider.GetCommandQueue();

  if (!device_ || !command_queue_) {
    XELOGE("MetalCommandProcessor: No Metal device or command queue available");
    return false;
  }

  MTL::CaptureManager* capture_manager =
      MTL::CaptureManager::sharedCaptureManager();
  debug_labels_ =
      (capture_manager &&
       (capture_manager->supportsDestination(
            MTL::CaptureDestinationDeveloperTools) ||
        capture_manager->supportsDestination(
            MTL::CaptureDestinationGPUTraceDocument))) ||
      std::getenv("MTL_DEBUG_LAYER");

  wait_shared_event_ = device_->newSharedEvent();
  if (wait_shared_event_) {
    wait_shared_event_->setLabel(
        NS::String::string("XeniaWaitEvent", NS::UTF8StringEncoding));
    wait_shared_event_value_ = 0;
  } else {
    XELOGW(
        "MetalCommandProcessor: SharedEvent unavailable; falling back to "
        "waitUntilCompleted");
  }

  // MSC runs tessellation as object and mesh stages.
  mesh_shader_supported_ = device_->supportsFamily(MTL::GPUFamilyApple7) ||
                           device_->supportsFamily(MTL::GPUFamilyMac2);

  draw_ring_count_ = std::max<int32_t>(1, ::cvars::metal_draw_ring_count);
  if (!UseDxilPath()) {
    // Large per-command-buffer ring sizes have been observed to corrupt
    // SPIRV-Cross uniform/constant data; cap here and rely on multi-buffer
    // pool growth in EnsureSpirvUniformBuffer* for throughput.
    constexpr size_t kMaxSpirvRingPagesPerCommandBuffer = 8;
    if (draw_ring_count_ > kMaxSpirvRingPagesPerCommandBuffer) {
      XELOGW(
          "SPIRV-Cross: clamping per-command-buffer ring pages from {} to {} "
          "for correctness",
          draw_ring_count_, kMaxSpirvRingPagesPerCommandBuffer);
      draw_ring_count_ = kMaxSpirvRingPagesPerCommandBuffer;
    }
  }
  msl_system_constants_version_ = 1;
  msl_constants_versioned_uniform_buffer_ = nullptr;
  msl_system_constants_written_vertex_versions_.assign(draw_ring_count_, 0);
  msl_system_constants_written_pixel_versions_.assign(draw_ring_count_, 0);
  msl_current_float_constant_map_vertex_.fill(0);
  msl_current_float_constant_map_pixel_.fill(0);
  msl_float_constants_dirty_vertex_ = true;
  msl_float_constants_dirty_pixel_ = true;
  msl_bool_loop_constants_dirty_ = true;
  msl_fetch_constants_dirty_ = true;
  msl_bound_vertex_texture_binding_uid_ = 0;
  msl_bound_pixel_texture_binding_uid_ = 0;
  msl_bound_vertex_sampler_binding_uid_ = 0;
  msl_bound_pixel_sampler_binding_uid_ = 0;
  msl_bound_vertex_argument_buffer_offset_ = 0;
  msl_bound_pixel_argument_buffer_offset_ = 0;
  msl_bound_vertex_argument_buffer_offset_valid_ = false;
  msl_bound_pixel_argument_buffer_offset_valid_ = false;
  msl_last_argbuf_vertex_translation_ = nullptr;
  msl_last_argbuf_vertex_encoded_length_ = 0;
  msl_last_argbuf_vertex_layout_uid_ = 0;
  msl_last_argbuf_pixel_translation_ = nullptr;
  msl_last_argbuf_pixel_encoded_length_ = 0;
  msl_last_argbuf_pixel_layout_uid_ = 0;
  msl_bound_uniforms_buffer_ = nullptr;
  msl_bound_uniforms_vs_base_offset_ = 0;
  msl_bound_uniforms_ps_base_offset_ = 0;
  msl_bound_uniforms_offsets_valid_ = false;

  // Initialize shared memory
  shared_memory_ =
      std::make_unique<MetalSharedMemory>(*this, *memory_, trace_writer_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  // Resolve read watch (ResolveReadWatchActive decides whether it is used).
  InitResolveReadWatch();
  resolve_read_callback_ = memory_->RegisterPhysicalMemoryReadCallback(
      ResolveReadCallbackThunk, this);

  // Initialize primitive processor (index/primitive conversion like D3D12).
  primitive_processor_ = std::make_unique<MetalPrimitiveProcessor>(
      *this, *register_file_, *memory_, trace_writer_, *shared_memory_);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize Metal primitive processor");
    return false;
  }

  texture_cache_ = std::make_unique<MetalTextureCache>(this, *register_file_,
                                                       *shared_memory_, 1, 1);
  if (!texture_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal texture cache");
    return false;
  }

  // Initialize render target cache
  render_target_cache_ = std::make_unique<MetalRenderTargetCache>(
      *register_file_, *memory_, &trace_writer_, 1, 1, *this);
  if (!render_target_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal render target cache");
    return false;
  }
  render_target_cache_->SetStripMerge(&strip_merge_);
  strip_merge_.SetBackend(this);

  // Fallback for query segment normalization when no draw pinned a scale.
  zpd_draw_resolution_scale_x_ = texture_cache_->draw_resolution_scale_x();
  zpd_draw_resolution_scale_y_ = texture_cache_->draw_resolution_scale_y();

  zpd_visibility_pool_ = std::make_unique<MetalZPDVisibilityPool>();
  EnsureQueryResources();

  // Initialize shader translation pipeline
  if (!InitializeShaderTranslation()) {
    XELOGE("Failed to initialize shader translation");
    return false;
  }
  // Create render target texture for offscreen rendering
  MTL::TextureDescriptor* color_desc = MTL::TextureDescriptor::alloc()->init();
  color_desc->setTextureType(MTL::TextureType2D);
  color_desc->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
  color_desc->setWidth(render_target_width_);
  color_desc->setHeight(render_target_height_);
  color_desc->setStorageMode(MTL::StorageModePrivate);
  color_desc->setUsage(MTL::TextureUsageRenderTarget |
                       MTL::TextureUsageShaderRead);

  render_target_texture_ = device_->newTexture(color_desc);
  color_desc->release();

  if (!render_target_texture_) {
    XELOGE("Failed to create render target texture");
    return false;
  }
  render_target_texture_->setLabel(
      NS::String::string("XeniaRenderTarget", NS::UTF8StringEncoding));

  // Create depth/stencil texture
  MTL::TextureDescriptor* depth_desc = MTL::TextureDescriptor::alloc()->init();
  depth_desc->setTextureType(MTL::TextureType2D);
  depth_desc->setPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
  depth_desc->setWidth(render_target_width_);
  depth_desc->setHeight(render_target_height_);
#if XE_PLATFORM_IOS
  // This fallback depth/stencil target is transient (clear/dontcare only) and
  // never sampled, so memoryless is the most efficient iOS storage mode.
  depth_desc->setStorageMode(MTL::StorageModeMemoryless);
#else
  depth_desc->setStorageMode(MTL::StorageModePrivate);
#endif
  depth_desc->setUsage(MTL::TextureUsageRenderTarget);

  depth_stencil_texture_ = device_->newTexture(depth_desc);
  depth_desc->release();

  if (!depth_stencil_texture_) {
    XELOGE("Failed to create depth/stencil texture");
    return false;
  }
  depth_stencil_texture_->setLabel(
      NS::String::string("XeniaDepthStencil", NS::UTF8StringEncoding));

  // Create render pass descriptor
  render_pass_descriptor_ = MTL::RenderPassDescriptor::alloc()->init();

  auto color_attachment =
      render_pass_descriptor_->colorAttachments()->object(0);
  color_attachment->setTexture(render_target_texture_);
  color_attachment->setLoadAction(MTL::LoadActionClear);
  color_attachment->setStoreAction(MTL::StoreActionStore);
  color_attachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));

  auto depth_attachment = render_pass_descriptor_->depthAttachment();
  depth_attachment->setTexture(depth_stencil_texture_);
  depth_attachment->setLoadAction(MTL::LoadActionClear);
  depth_attachment->setStoreAction(MTL::StoreActionDontCare);
  depth_attachment->setClearDepth(1.0);

  auto stencil_attachment = render_pass_descriptor_->stencilAttachment();
  stencil_attachment->setTexture(depth_stencil_texture_);
  stencil_attachment->setLoadAction(MTL::LoadActionClear);
  stencil_attachment->setStoreAction(MTL::StoreActionDontCare);
  stencil_attachment->setClearStencil(0);

  // Create a null buffer for unused descriptor entries
  // This prevents shader validation errors when accessing unpopulated
  // descriptors
  null_buffer_ =
      device_->newBuffer(kNullBufferSize, MTL::ResourceStorageModeShared);
  if (!null_buffer_) {
    XELOGE("Failed to create null buffer");
    return false;
  }
  null_buffer_->setLabel(
      NS::String::string("NullBuffer", NS::UTF8StringEncoding));
  std::memset(null_buffer_->contents(), 0, kNullBufferSize);

  // Create a 1x1x1 placeholder 2D array texture for unbound texture slots
  // Xbox 360 textures are typically 2D arrays (for texture atlases, cubemaps)
  // Using 2DArray prevents "Invalid texture type" validation errors
  MTL::TextureDescriptor* null_tex_desc =
      MTL::TextureDescriptor::alloc()->init();
  null_tex_desc->setTextureType(MTL::TextureType2DArray);
  null_tex_desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  null_tex_desc->setWidth(1);
  null_tex_desc->setHeight(1);
  null_tex_desc->setArrayLength(1);  // Single slice in the array
  null_tex_desc->setStorageMode(MTL::StorageModeShared);
  null_tex_desc->setUsage(MTL::TextureUsageShaderRead);

  null_texture_ = device_->newTexture(null_tex_desc);
  null_tex_desc->release();

  if (!null_texture_) {
    XELOGE("Failed to create null texture");
    return false;
  }
  null_texture_->setLabel(
      NS::String::string("NullTexture2DArray", NS::UTF8StringEncoding));

  // Fill the 1x1x1 texture with opaque white (helps debug if sampled)
  uint32_t white_pixel = 0xFFFFFFFF;
  MTL::Region region =
      MTL::Region(0, 0, 0, 1, 1, 1);  // x,y,z origin, w,h,d size
  null_texture_->replaceRegion(region, 0, 0, &white_pixel, 4, 0);  // slice 0

  // Create a default sampler for unbound sampler slots
  // Must set supportsArgumentBuffers=YES for use in argument buffers
  MTL::SamplerDescriptor* null_smp_desc =
      MTL::SamplerDescriptor::alloc()->init();
  null_smp_desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMipFilter(MTL::SamplerMipFilterLinear);
  null_smp_desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setSupportArgumentBuffers(true);

  null_sampler_ = device_->newSamplerState(null_smp_desc);
  null_smp_desc->release();

  if (!null_sampler_) {
    XELOGE("Failed to create null sampler");
    return false;
  }

  // SPIRV-Cross path: use command-buffer-scoped uniforms buffers so CPU writes
  // to the next submission can't race with in-flight GPU reads. The DXIL path
  // sub-allocates its constants per draw instead.
  if (!UseDxilPath()) {
    if (!EnsureSpirvUniformBuffer()) {
      return false;
    }
  }

  // Needed on both guest shader paths: the render target cache's internal
  // compute shaders go through the converter even when the guest shaders don't.
  if (!metal_shader_converter_.Initialize()) {
    XELOGE("Metal: the shader converter is unavailable, nothing can render");
    return false;
  }

  // After the converter, which the DXIL path's workers compile against.
  InitializeAsyncCompilation();

  return true;
}

std::unique_ptr<SpirvShaderTranslator>
MetalCommandProcessor::CreateSpirvShaderTranslator() const {
  return std::make_unique<SpirvShaderTranslator>(
      spirv_translator_features_,
      spirv_translator_native_2x_msaa_,  // native_2x_msaa_with_att
      false,                             // native_2x_msaa_no_att
      false,  // edram_fragment_shader_interlock (host RT path)
      cvars::precise_interpolation, spirv_translator_resolution_scale_x_,
      spirv_translator_resolution_scale_y_);
}

bool MetalCommandProcessor::SupportsHostRenderTargetFeedback() const {
  // spirv2air reads input attachments as color attachments in raster order
  // group 0. Metal Shader Converter's DXIL path has no equivalent, and the
  // fetch-state function constants specialize the shader's blending.
  return device_ && UseDxilPath() && cvars::metal_shader_compiler == "spirv2air" &&
         device_->supportsFamily(MTL::GPUFamilyApple4) &&
         device_->areRasterOrderGroupsSupported();
}

bool MetalCommandProcessor::InitializeShaderTranslation() {
  // RGBA8 targets need shader gamma encoding. RGBA16 targets store linear
  // values and encode them when packing the render target into EDRAM.
  const bool gamma_render_target_as_unorm8 =
      !render_target_cache_->gamma_render_target_as_unorm16();

  XELOGI("Shader translator init: gamma_as_unorm8={}, msaa_2x={}, scale={}x{}",
         gamma_render_target_as_unorm8,
         render_target_cache_->msaa_2x_supported(),
         render_target_cache_->draw_resolution_scale_x(),
         render_target_cache_->draw_resolution_scale_y());

  // Both guest shader paths consume this translator's SPIR-V. Enable all
  // features as a baseline, then disable what Metal doesn't need.
  SpirvShaderTranslator::Features spirv_features(true);
  spirv_features.structured_vertex_control_flow = UseDxilPath();
  spirv_features.fetch_state_function_constants = UseDxilPath();
  // 8_8_8_8_GAMMA as 8-bit gamma, and 16_16 and 16_16_16_16 as the EDRAM's
  // fixed point, blended in the shader.
  spirv_features.host_render_target_feedback =
      render_target_cache_->gamma_fetch_blend() ||
      render_target_cache_->fixed16_fetch_blend();
  spirv_features.optimize_finite_multiplication =
      cvars::metal_finite_multiply &&
      UseDxilPath() && SpirvToDxilCompiler::SupportsMscFmad32();
  spirv_features.fused_float32_fma =
      UseDxilPath() && SpirvToDxilCompiler::SupportsMscFmad32();
  // Not using fragment shader interlock — we use host render targets.
  spirv_features.fragment_shader_sample_interlock = false;
  // A live primitive-specific input prevents adjacent primitives from sharing
  // fragment helper values on affected Apple drivers. Keep regular varying
  // interpolation; the extra barycentrics only anchor helper execution.
  spirv_features.primitive_fragment_helpers =
      UseDxilPath() && device_->supportsFamily(MTL::GPUFamilyApple1) &&
      device_->supportsShaderBarycentricCoordinates();
  spirv_features.fragment_shader_barycentric = false;
  // The host's centroid depends on hidden surface removal within the render
  // pass, so it's computed from the rasterized coverage.
  spirv_features.centroid_from_raster_coverage =
      UseDxilPath() && device_->supportsShaderBarycentricCoordinates();
  // Metal fast-math doesn't guarantee IEEE NaN/Inf preservation.
  spirv_features.signed_zero_inf_nan_preserve_float32 = false;
  // Metal fast-math flushes denorms.
  spirv_features.denorm_flush_to_zero_float32 = true;
  // RTE rounding not guaranteed by Metal.
  spirv_features.rounding_mode_rte_float32 = false;

  // Snapshotted so a compile thread can build its own translator without
  // reading the render target cache from off the draw thread.
  spirv_translator_features_ = spirv_features;
  spirv_translator_native_2x_msaa_ = render_target_cache_->msaa_2x_supported();
  spirv_translator_resolution_scale_x_ =
      render_target_cache_->draw_resolution_scale_x();
  spirv_translator_resolution_scale_y_ =
      render_target_cache_->draw_resolution_scale_y();
  spirv_shader_translator_ = CreateSpirvShaderTranslator();

  XELOGI("SpirvShaderTranslator init ({} path): msaa_2x={}, scale={}x{}",
         UseDxilPath() ? "DXIL" : "SPIRV-Cross MSL",
         render_target_cache_->msaa_2x_supported(),
         render_target_cache_->draw_resolution_scale_x(),
         render_target_cache_->draw_resolution_scale_y());

  if (!UseDxilPath() && !InitializeMslTessellation()) {
    XELOGW(
        "SPIRV-Cross: Tessellation factor pipelines failed to init; "
        "tessellated draws will be skipped");
  }

  return true;
}

void MetalCommandProcessor::PrepareForWait() {
  // Runs on every ring-empty stall, so it must not block on the GPU.
  EndCommandBuffer(CommandBufferKind::kSubmissionWait);

  CommandProcessor::PrepareForWait();
}

uint64_t MetalCommandProcessor::GetHeldGpuWritesSubmission() {
  // Resolve output, with zero-copy. Command processor stores aren't held: a
  // late held store could overwrite a newer guest write (a read-through
  // variant hung BF4 on a spin at 0x1FC1B002).
  // Output a read watch guards makes a CPU access of it wait instead: held
  // behind the rest only (not watchable), and behind all of it again once the
  // watch has given up.
  uint64_t submission =
      !resolve_read_watch_disabled_.load(std::memory_order_relaxed)
          ? resolve_unwatched_last_submission_
          : resolve_last_submission_;
  return submission > GetCompletedSubmission() ? submission : 0;
}

bool MetalCommandProcessor::ResolveReadWatchActive() const {
  return resolve_page_owners_ && shared_memory_ &&
         shared_memory_->is_zero_copy() &&
         !resolve_read_watch_disabled_.load(std::memory_order_relaxed);
}

bool MetalCommandProcessor::EndResolveSubmission() {
  if (current_command_buffer_) {
    EndCommandBuffer(CommandBufferKind::kSubmissionOther);
  }
  return true;
}

bool MetalCommandProcessor::FaultCopyResolveToGuestRam(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  if (ranges.empty()) {
    return false;
  }
  // Under resolve_read_mutex_. The ranges are pages the owner still owns.
  uint64_t submission = GetResolveSubmittedThrough();
  auto owner_it = resolve_owners_.find(
      resolve_page_owners_[ranges.front().first >> 12].load(
          std::memory_order_relaxed));
  if (owner_it != resolve_owners_.end()) {
    submission = std::min(submission, owner_it->second.submission);
  }
    return true;
  }
  // Not the GPU thread's AwaitSubmissionCompletion: this may be any thread,
  // with or without the global lock. Completion handlers take only
  // completion_mutex_, so they never wait for this.
  auto wait_start = std::chrono::steady_clock::now();
  bool completed;
  {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    completed = completion_cond_.wait_for(
        lock, kResolveFaultWaitBound, [this, submission]() {
          return completed_command_buffers_.load(std::memory_order_acquire) >=
                 submission;
        });
  }
  uint64_t wait_ns =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - wait_start)
                   .count());
  if (!completed) {
    uint32_t hits =
        resolve_fault_bound_hits_.fetch_add(1, std::memory_order_relaxed) + 1;
    XELOGW(
        "Metal: a CPU access of resolve output at 0x{:08X} waited {} ms for "
        "submission {} (completed {}) and goes ahead without it",
        ranges.front().first, kResolveFaultWaitBound.count(), submission,
        completed_command_buffers_.load(std::memory_order_relaxed));
    if (hits >= kResolveFaultMaxBoundHits &&
        !resolve_read_watch_disabled_.exchange(true,
                                               std::memory_order_relaxed)) {
      XELOGE(
          "Metal: resolve read watch waits ran out {} times, so guest signals "
          "are held behind resolve output again for the rest of the run",
          hits);
    }
  }
  // Either way the watch is done with: after a bound hit the access goes
  // ahead rather than faulting again into another wait.
  // TODO(xenios-jp): for a write (or kDiscard) the GPU output then lands over
  // what the guest wrote. Needs counts of how often the bound is hit by writes
  // before changing it for them.
  return true;
}

int MetalCommandProcessor::ResolveOwnerStateAt(uint32_t physical_address,
                                               bool note_read) {
  if (!resolve_page_owners_ ||
      physical_address >= SharedMemory::kBufferSize) {
    return 0;
  }
  uint64_t owner_key = resolve_page_owners_[physical_address >> 12].load(
      std::memory_order_relaxed);
  if (!owner_key) {
    return 0;
void MetalCommandProcessor::ResolveReadCallbackThunk(
    void* context, uint32_t physical_address, uint32_t length,
    Memory::PhysicalAccess access) {
  auto command_processor = static_cast<MetalCommandProcessor*>(context);
  // A read that arrives before its resolve's submission asks for the
  // submission (ResolveOwnerStateAt).
  if (access == Memory::PhysicalAccess::kRead) {
  switch (access) {
    case Memory::PhysicalAccess::kRead:
      command_processor->MarkResolvePagesRead(physical_address, length);
      break;
    case Memory::PhysicalAccess::kWrite:
      command_processor->PrepareResolvePagesForWrite(physical_address, length);
      break;
    case Memory::PhysicalAccess::kDiscard:
      // The GPU writes guest RAM itself, so dropping the owners doesn't stop a
      // submitted resolve from landing over what the pages hold next: wait for
      // it as a write would first.
      command_processor->PrepareResolvePagesForWrite(physical_address, length);
      command_processor->DiscardResolvePages(physical_address, length);
      break;
  }
}

void MetalCommandProcessor::AwaitGpuWritesBeforeGuestRead() {
  // Export output that isn't awaited is left unordered here, as in D3D12 and
  // Vulkan.
  if (resolve_last_submission_ <= GetCompletedSubmission()) {
    return;
  }
  int64_t wait_start_ns = GetSteadyTimeNs();
  if (resolve_last_submission_ >= submission_current_ &&
      current_command_buffer_) {
    EndCommandBuffer(CommandBufferKind::kSubmissionWait);
  }
  AwaitSubmissionCompletion(resolve_last_submission_);
  NoteGpuWait(GpuWaitReason::kResolveBeforeGuestRead, wait_start_ns);
}

void MetalCommandProcessor::PollCompletedSubmission() {
  ProcessCompletedSubmissions();
  PumpQueryResolves();
}

void MetalCommandProcessor::WaitForPendingCompletionHandlers() {
  constexpr auto kMaxWait = std::chrono::seconds(5);
  auto wait_start = std::chrono::steady_clock::now();
  while (pending_completion_handlers_.load(std::memory_order_acquire) != 0) {
    if (std::chrono::steady_clock::now() - wait_start >= kMaxWait) {
      XELOGW(
          "MetalCommandProcessor: timed out waiting for {} completion "
          "handler(s) during shutdown",
          pending_completion_handlers_.load(std::memory_order_relaxed));
      // A timeout is diagnostic, not permission to destroy callback owners.
      wait_start = std::chrono::steady_clock::now();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void MetalCommandProcessor::ShutdownContext() {
  strip_merge_.SetBackend(nullptr);
  render_encoder_memexport_ranges_.clear();
  pending_shader_done_fence_ranges_.clear();
  pending_shader_done_fence_values_.clear();
  shader_done_fences_outside_pass_ = false;
  render_encoder_memexport_draws_are_pure_ = true;
  dxil_binder_.ResetUploadCaches();
  // End any active render encoder before shutdown
  if (current_render_encoder_) {
    if (render_target_cache_) {
      render_target_cache_->FinalizeRenderPassStoreActions(
          current_render_encoder_, current_render_pass_descriptor_);
    }
    current_render_encoder_->endEncoding();
    // Don't release yet - wait until command buffer completes
  }

  // Submit and wait for any pending command buffer
  if (current_command_buffer_) {
    EndSharedComputeEncoder();
    uint64_t wait_value = 0;
    if (wait_shared_event_) {
      wait_value = ++wait_shared_event_value_;
      current_command_buffer_->encodeSignalEvent(wait_shared_event_,
                                                 wait_value);
    }
    if (texture_cache_) {
      texture_cache_->CommitLoadsAhead();
    }
    ScheduleSpirvUniformBufferRelease(current_command_buffer_);
    ScheduleSpirvArgumentBufferRelease(current_command_buffer_);
    current_command_buffer_->commit();
    if (wait_shared_event_) {
      // Bounded so a GPU that has stopped (in the background) can't hold up
      // quitting. The command buffer keeps what it uses alive.
      if (!wait_shared_event_->waitUntilSignaledValue(wait_value, 2000)) {
        XELOGW("Metal: the last command buffer didn't finish in 2 s at shutdown");
      }
    } else {
      current_command_buffer_->waitUntilCompleted();
    }
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    current_draw_index_ = 0;
    copy_resolve_writes_pending_ = false;
  }

  // Finish or cancel any standalone upload batch before joining callbacks.
  if (texture_cache_) {
    texture_cache_->FinishPendingUploads();
  }

  // Even if we have no active command buffer at this point, there may be
  // previously committed command buffers still in flight. Submit and wait for
  // a dummy command buffer to ensure all GPU work on this queue has completed
  // before tearing down resources on thread exit.
  if (command_queue_) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::CommandBuffer* sync_cmd = command_queue_->commandBuffer();
    if (sync_cmd) {
      uint64_t wait_value = 0;
      if (wait_shared_event_) {
        wait_value = ++wait_shared_event_value_;
        sync_cmd->encodeSignalEvent(wait_shared_event_, wait_value);
      }
      sync_cmd->commit();
      if (wait_shared_event_) {
        wait_shared_event_->waitUntilSignaledValue(
            wait_value, std::numeric_limits<uint64_t>::max());
      } else {
        sync_cmd->waitUntilCompleted();
      }
    }
    pool->release();
  }

  WaitForPendingCompletionHandlers();

  // Nothing is in flight now, so no fault can still be waiting for a
  // submission.
  ResetResolveReadWatch();

  if (uint64_t failed =
          failed_command_buffers_.load(std::memory_order_relaxed)) {
    XELOGW("MetalCommandProcessor: {} command buffer(s) didn't complete",
           failed);
  }

  // Deterministic for a given workload, so a single-frame trace replay can be
  // read from it - the 60-frame counter window never closes in one.
  std::string kind_breakdown;
  for (size_t i = 0; i < kCommandBufferKindCount; ++i) {
    if (!command_buffer_kind_counts_[i]) {
      continue;
    }
    kind_breakdown += fmt::format(" {}={}", kCommandBufferKindNames[i],
                                  command_buffer_kind_counts_[i]);
  }
  XELOGI("Metal: {} command buffer(s) executed,{}",
         gpu_time_command_buffers_.load(std::memory_order_relaxed),
         kind_breakdown.empty() ? " none created" : kind_breakdown);

  render_encoder_shared_memory_hazards_.Abandon();
  // Now safe to release encoder and command buffer
  if (current_render_encoder_) {
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
  }
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  DrainCommandBufferAutoreleasePool();

  {
    std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
    command_buffer_spirv_argbuf_pages_.clear();
    pending_spirv_argbuf_releases_.clear();
    spirv_argbuf_pool_.clear();
  }

  ShutdownQueryResources();
  zpd_visibility_pool_.reset();

  if (texture_cache_) {
    texture_cache_->Shutdown();
    texture_cache_.reset();
  }

  if (primitive_processor_) {
    primitive_processor_->Shutdown();
    primitive_processor_.reset();
  }
  frame_open_ = false;
  ResetMemexportPages();

  ShutdownAsyncCompilation();
  storage_writer_.ShutdownShaderStorage();
  // Not saved here: a save takes seconds, and quitting the app may not wait for
  // it. What was added since the last save is compiled again next time.
  pipeline_archive_.CancelSave();
  JoinPipelineArchiveSave();
  pipeline_archive_.Close(false);
  shader_library_cache_.Close();
  if (exact_float_depth_bias_library_) {
    exact_float_depth_bias_library_->release();
    exact_float_depth_bias_library_ = nullptr;
  }

  ShutdownMslTessellation();
  for (auto& [key, pso] : async_pipeline_cache_) {
    if (pso) {
      pso->release();
    }
  }
  async_pipeline_cache_.clear();
  {
    std::lock_guard<std::mutex> lock(specialized_functions_mutex_);
    for (auto& [key, function] : specialized_functions_) {
      function->release();
    }
    specialized_functions_.clear();
  }
  dxil_tessellation_cache_.clear();
  if (tessellator_tables_buffer_) {
    tessellator_tables_buffer_->release();
    tessellator_tables_buffer_ = nullptr;
  }
  finite_multiply_requirements_.clear();
  std::fill(std::begin(finite_multiply_last_shader_),
            std::end(finite_multiply_last_shader_), nullptr);
  for (RecentShaderLoad& recent : recent_shader_loads_) {
    recent = RecentShaderLoad();
  }
  ClearAddressShaderLoads();
  guest_shader_cache_.clear();
  memexport_format_profiles_.Clear();
  spirv_shader_translator_.reset();

  uniforms_buffer_ = nullptr;
  command_buffer_spirv_uniform_buffers_.clear();
  size_t uniforms_pool_size = 0;
  {
    std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
    uniforms_pool_size = spirv_uniforms_pool_.size();
    spirv_uniforms_available_.clear();
    for (MTL::Buffer* pool_uniforms : spirv_uniforms_pool_) {
      if (pool_uniforms) {
        pool_uniforms->release();
      }
    }
    spirv_uniforms_pool_.clear();
    spirv_uniforms_pool_initialized_ = false;
  }
  if (spirv_uniforms_available_semaphore_) {
    // Buffers dropped above were never signalled back, and libdispatch traps
    // on a semaphore released below the count it was created with.
    for (size_t i = 0; i < uniforms_pool_size; ++i) {
      dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
    }
#if !OS_OBJECT_USE_OBJC
    dispatch_release(spirv_uniforms_available_semaphore_);
#endif
    spirv_uniforms_available_semaphore_ = nullptr;
  }

  shared_memory_.reset();
  if (wait_shared_event_) {
    wait_shared_event_->release();
    wait_shared_event_ = nullptr;
  }

  ClearMslShaderSourceCacheDirectory();

  CommandProcessor::ShutdownContext();
}

void MetalCommandProcessor::TranslateShadersForStorage(
    const std::set<std::pair<uint64_t, uint64_t>>& translations_needed) {
  // Queue every modification the stored pipelines reference. Falls back to this
  // thread when there is no pool.
  size_t queued = 0;
  for (const auto& [ucode_hash, modification] : translations_needed) {
    auto it = guest_shader_cache_.find(ucode_hash);
    if (it == guest_shader_cache_.end()) {
      continue;
    }
    // Translation needs the ucode analyzed, which the storage load did only for
    // the shaders it read this time.
    if (!it->second->is_ucode_analyzed()) {
      it->second->AnalyzeUcode(ucode_disasm_buffer_);
    }
    // A domain modification's SPIR-V is a tessellation evaluation module, which
    // the DXIL path cannot build as a standalone vertex function.
    // GetDxilTessellationShaders links it with the host stages instead.
    if (UseDxilPath() && it->second->type() == xenos::ShaderType::kVertex &&
        Shader::IsHostVertexShaderTypeDomain(
            SpirvShaderTranslator::Modification(modification)
                .vertex.host_vertex_shader_type)) {
      continue;
    }
    ShaderCompileStatus status = ShaderCompileStatus::kReady;
    GetOrCreateHostTranslation(*it->second, modification, /*allow_async=*/true,
                               &status);
    if (status == ShaderCompileStatus::kPending) {
      ++queued;
    }
  }
  if (queued) {
    // A pipeline can't be queued without its compiled functions, so the shader
    // half is finished here even when the caller asked for non-blocking init.
    AwaitAsyncCompiles();
  }
}

void MetalCommandProcessor::CreateStoredPipelines(
    const std::vector<PipelineStoredDescription>& stored_descriptions) {
  replaying_stored_pipelines_ = true;

  // Ask for every tessellation stage set first, so the one drain below covers
  // them all rather than one per description.
  bool tessellation_queued = false;
  for (const PipelineStoredDescription& stored : stored_descriptions) {
    const PipelineDescription& description = stored.description;
    if (!UseDxilPath() ||
        PipelineKind(description.kind) != PipelineKind::kDxilTessellation) {
      continue;
    }
    auto it = guest_shader_cache_.find(description.vertex_shader_hash);
    if (it == guest_shader_cache_.end()) {
      continue;
    }
    ShaderCompileStatus status = ShaderCompileStatus::kReady;
    GetDxilTessellationShaders(*static_cast<DxilShader*>(it->second.get()),
                               description.vertex_shader_modification,
                               description.tessellation_mode,
                               description.host_vertex_shader_type,
                               /*allow_async=*/true, &status);
    tessellation_queued |= status == ShaderCompileStatus::kPending;
  }
  if (tessellation_queued) {
    AwaitAsyncCompiles();
  }

  ReplayStoredPipelines(stored_descriptions,
                        [this](const PipelineStoredDescription& stored,
                               Shader*& vertex_shader_out) {
                          return QueueStoredPipeline(stored, vertex_shader_out);
                        });
  replaying_stored_pipelines_ = false;
}

const char* MetalCommandProcessor::QueueStoredPipeline(
    const PipelineStoredDescription& stored, Shader*& vertex_shader_out) {
  const PipelineDescription& description = stored.description;
  if (description.finite_multiply_enabled !=
      spirv_translator_features_.optimize_finite_multiplication) {
    return "belonging to another finite-multiply configuration";
  }
  // A tessellation pipeline belongs to the path that recorded it, and its
  // shaders are the wrong subclass for the other one.
  if (IsTessellationPipelineKind(PipelineKind(description.kind)) &&
      (PipelineKind(description.kind) == PipelineKind::kDxilTessellation) !=
          UseDxilPath()) {
    return "belonging to the other path";
  }
  auto vertex_it = guest_shader_cache_.find(description.vertex_shader_hash);
  if (vertex_it == guest_shader_cache_.end()) {
    return "VS not found";
  }
  ShaderCompileStatus status = ShaderCompileStatus::kReady;
  Shader::Translation* pixel_translation = nullptr;
  if (description.pixel_shader_hash) {
    auto pixel_it = guest_shader_cache_.find(description.pixel_shader_hash);
    if (pixel_it == guest_shader_cache_.end()) {
      return "PS not found";
    }
    pixel_translation = GetOrCreateHostTranslation(
        *pixel_it->second, description.pixel_shader_modification,
        /*allow_async=*/false, &status);
    if (status != ShaderCompileStatus::kReady) {
      return "PS not buildable";
    }
  }

  // The description is the key, so a rebuilt pipeline lands exactly where the
  // draw that stored it will look for it.
  PipelineCompileRequest request = {};
  request.description = description;
  request.pipeline_key = description.GetHash();
  request.priority = pixel_translation ? kPipelinePriorityStored
                                       : kPipelinePriorityStoredDepthOnly;
  Shader::Translation* vertex_translation = nullptr;
  if (PipelineKind(description.kind) == PipelineKind::kDxilTessellation) {
    // The domain modification never becomes a standalone vertex function, so
    // it is asked for through the linked stages rather than translated here.
    const DxilTessellationShaders* shaders = GetDxilTessellationShaders(
        *static_cast<DxilShader*>(vertex_it->second.get()),
        description.vertex_shader_modification, description.tessellation_mode,
        description.host_vertex_shader_type, /*allow_async=*/true, &status);
    if (!shaders) {
      return "tessellation stages failed";
    }
    request.tessellation_shaders = shaders;
    if (pixel_translation) {
      auto* dxil_pixel =
          static_cast<DxilShader::DxilTranslation*>(pixel_translation);
      request.fragment_library = dxil_pixel->metal_library();
      request.fragment_function_name = dxil_pixel->entry_point_name();
    }
  } else {
    vertex_translation = GetOrCreateHostTranslation(
        *vertex_it->second, description.vertex_shader_modification,
        /*allow_async=*/false, &status);
    if (status != ShaderCompileStatus::kReady) {
      return "VS not buildable";
    }
    request.vertex_function = GetHostShaderFunction(vertex_translation);
  }

  request.fragment_function = GetHostShaderFunction(pixel_translation);
  SetPipelineFunctionConstants(request, vertex_translation, pixel_translation);
  if (description.vertex_fetch_states_specialized !=
          request.vertex_fetch_states_specialized ||
      description.fragment_fetch_states_specialized !=
          request.fragment_fetch_states_specialized) {
    return "belonging to another specialization configuration";
  }

  PipelineCompileStatus pipeline_status = PipelineCompileStatus::kFailed;
  AcquirePipelineState(request, /*allow_async=*/true, &pipeline_status);
  if (pipeline_status == PipelineCompileStatus::kFailed) {
    return "pipeline failed";
  }
  {
    // Still compiling: the rebuild record waits for it.
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    if (storage_stats_pending_ &&
        async_pipeline_pending_.count(request.pipeline_key)) {
      storage_stats_keys_.insert(request.pipeline_key);
    }
  }
  vertex_shader_out = vertex_it->second.get();
  return nullptr;
}

void MetalCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking,
                                            nullptr);
  storage_writer_.ShutdownShaderStorage();

  if (!device_) {
    XELOGW("Metal shader storage init skipped (no device)");
    if (completion_callback) {
      completion_callback();
    }
    return;
  }

  compile_activity.storage_queued_end = 0;
  compile_activity.storage_queued_begin =
      compile_activity.queued.load(std::memory_order_relaxed);
  compile_activity.storage_finished_begin =
      compile_activity.finished.load(std::memory_order_relaxed);
  compile_activity.storage_empty = false;
  compile_activity.storage_total = 0;
  compile_activity.storage_seed_us_per_stored = 0;
  compile_activity.storage_loading = true;
  ++compile_activity.storage_generation;

  // The SPIRV-Cross path's MSL text is a translation cache, not a record of
  // what to rebuild.
  SetMslShaderSourceCacheDirectory(GetShaderStorageLocalRoot(cache_root) /
                                   "metal" / fmt::format("{:08X}", title_id) /
                                   "msl_source");

  // One file for both guest shader paths: they share a SpirvShaderTranslator
  // config, so a description means the same thing to either, and only the
  // tessellation kinds are path-specific.
  ShaderStorageWriter<PipelineStoredDescription>::PipelineStorageConfig config;
  config.file_suffix = ".metal.xpso";
  config.api_magic = kPipelineStorageAPIMagicMetal;
  // Sum so a bump of either version invalidates - both only ever move up.
  config.version = PipelineDescription::kVersion +
                   SpirvShaderTranslator::Modification::kVersion;

  // What the stored shaders and pipelines were built into last time, so they
  // load instead of being built again (see MetalShaderLibraryCache and
  // MetalPipelineArchive).
  JoinPipelineArchiveSave();
  {
    const std::filesystem::path title_local_root =
        GetShaderStorageLocalRoot(cache_root) / "metal" /
        fmt::format("{:08X}", title_id);
    // Both are discarded when the build or the translation settings change:
    // the libraries, and the pipelines' functions, are made by this build.
    const std::string fingerprint = MetalShaderLibraryCache::MakeFingerprint(
        fmt::format("{} device={} msaa2x={} scale={}x{}",
                    UseDxilPath() ? "dxil" : "msl",
                    device_->name() ? device_->name()->utf8String() : "",
                    spirv_translator_native_2x_msaa_,
                    spirv_translator_resolution_scale_x_,
                    spirv_translator_resolution_scale_y_));
    if (UseDxilPath()) {
      shader_library_cache_.Open(title_local_root / "shader_libraries.bin",
                                 fingerprint);
    } else {
      shader_library_cache_.Close();
    }
    pipeline_archive_.Open(device_, title_local_root / "pipelines",
                           fingerprint);
    archive_saved_ticks_ = xe::Clock::QueryHostTickCount();
  }

  std::vector<PipelineStoredDescription> stored_descriptions;
  if (!storage_writer_.InitializeShaderStorage(
          cache_root, title_id, config,
          [this](xenos::ShaderType type, const uint32_t* ucode_dwords,
                 uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            return LoadShader(type, ucode_dwords, ucode_dword_count,
                              ucode_data_hash);
          },
          [this](const std::set<std::pair<uint64_t, uint64_t>>
                     & translations_needed) {
            TranslateShadersForStorage(translations_needed);
          },
          stored_descriptions)) {
    XELOGW("Metal: persistent shader storage is disabled");
    compile_activity.storage_loading = false;
    if (completion_callback) {
      completion_callback();
    }
    return;
  }

  // How long the last complete rebuild took, to forecast this one before the
  // compile threads have measured anything. A record from another pipeline
  // version says little about this one.
  const std::filesystem::path stats_path =
      GetShaderStorageLocalRoot(cache_root) / "metal" /
      fmt::format("{:08X}", title_id) / "rebuild_stats.txt";
  uint64_t seed_us_per_stored = 0;
  {
    std::ifstream stats_file(stats_path);
    std::string tag;
    uint64_t stats_version = 0, stats_stored = 0, stats_us = 0;
    if (stats_file >> tag >> stats_version >> stats_stored >> stats_us &&
        tag == "xrebuild1" && stats_version == config.version &&
        stats_stored) {
      seed_us_per_stored = stats_us / stats_stored;
    }
  }
  {
    std::error_code ec;
    std::filesystem::create_directories(stats_path.parent_path(), ec);
  }
  compile_activity.storage_seed_us_per_stored = seed_us_per_stored;
  compile_activity.storage_total = stored_descriptions.size();

  uint64_t start_ticks = xe::Clock::QueryHostTickCount();
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    storage_stats_pending_ = !stored_descriptions.empty();
    storage_stats_boosted_ =
        async_compile_hold_boost_ || async_compile_awaiters_;
    storage_stats_keys_.clear();
    storage_stats_queued_all_ = false;
    storage_stats_start_ticks_ = start_ticks;
    storage_stats_stored_ = stored_descriptions.size();
    storage_stats_version_ = config.version;
    storage_stats_path_ = stats_path;
  }
  CreateStoredPipelines(stored_descriptions);
  compile_activity.storage_empty = stored_descriptions.empty();
  uint64_t storage_queued_end =
      compile_activity.queued.load(std::memory_order_relaxed);
  compile_activity.storage_queued_end = storage_queued_end;
  // Nothing went to the compile threads (no async compilation, or every
  // stored entry was rejected): the load is over, and a queued_end of 0 must
  // not read as "not known yet".
  if (storage_queued_end == compile_activity.storage_queued_begin) {
    compile_activity.storage_loading = false;
  }
  {
    // Everything may already be done, with no later finish to notice it.
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    storage_stats_queued_all_ = true;
    if (storage_stats_pending_ && storage_stats_keys_.empty()) {
      RecordStorageRebuildStatsLocked();
    }
  }
  if (blocking) {
    // The caller is waiting on this, so finish rather than leaving the compile
    // threads to race the guest's first draws.
    AwaitAsyncCompiles();
    compile_activity.storage_loading = false;
  }
  XELOGI("Metal pipeline storage: {} path, stored pipelines {} in {} ms",
         UseDxilPath() ? "DXIL" : "SPIRV-Cross",
         blocking ? "rebuilt" : "rebuilt or queued",
         (xe::Clock::QueryHostTickCount() - start_ticks) * 1000 /
             xe::Clock::QueryHostTickFrequency());

  if (completion_callback) {
    completion_callback();
  }
}

void MetalCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  EndZPDFrame();
  ProcessCompletedSubmissions();
  LogFrameGpuWaits();

  // Completion handlers land a frame or two behind, which a window this wide
  // absorbs.
  static constexpr uint32_t kGpuTimeWindowFrames = 60;
  if (++gpu_time_window_frames_ >= kGpuTimeWindowFrames) {
    uint64_t now_ns = completed_gpu_time_ns_.load(std::memory_order_relaxed);
    COUNT_profile_set("gpu/gpu_busy_us_per_frame",
                      int64_t((now_ns - gpu_time_window_start_ns_) /
                              (uint64_t(1000) * gpu_time_window_frames_)));
    gpu_time_window_start_ns_ = now_ns;

    uint64_t command_buffers_now =
        gpu_time_command_buffers_.load(std::memory_order_relaxed);
    COUNT_profile_set(
        "gpu/command_buffers_per_frame",
        int64_t((command_buffers_now - gpu_time_command_buffers_window_start_) /
                gpu_time_window_frames_));
    gpu_time_command_buffers_window_start_ = command_buffers_now;

    // Unrolled: COUNT_profile_set caches its token in a function-local static,
    // so a loop would file every kind under the first name it saw.
    auto kind_per_frame = [&](CommandBufferKind kind) {
      size_t i = size_t(kind);
      uint64_t now = command_buffer_kind_counts_[i];
      int64_t per_frame = int64_t((now - command_buffer_kind_window_start_[i]) /
                                  gpu_time_window_frames_);
      command_buffer_kind_window_start_[i] = now;
      return per_frame;
    };
    COUNT_profile_set("gpu/cb_submission_other_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionOther));
    COUNT_profile_set(
        "gpu/cb_submission_copy_draw_sync_per_frame",
        kind_per_frame(CommandBufferKind::kSubmissionCopyToDrawSync));
    COUNT_profile_set("gpu/cb_submission_zpd_query_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionZpdQuery));
    COUNT_profile_set(
        "gpu/cb_submission_uniforms_rollover_per_frame",
        kind_per_frame(CommandBufferKind::kSubmissionUniformsRollover));
    COUNT_profile_set(
        "gpu/cb_submission_primary_end_per_frame",
        kind_per_frame(CommandBufferKind::kSubmissionPrimaryBufferEnd));
    COUNT_profile_set("gpu/cb_submission_wait_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionWait));
    COUNT_profile_set("gpu/cb_submission_pass_limit_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionPassLimit));
    COUNT_profile_set("gpu/cb_submission_after_drain_per_frame",
                      kind_per_frame(CommandBufferKind::kSubmissionAfterDrain));
    COUNT_profile_set("gpu/cb_texture_upload_batch_per_frame",
                      kind_per_frame(CommandBufferKind::kTextureUploadBatch));
    COUNT_profile_set("gpu/cb_texture_upload_private_per_frame",
                      kind_per_frame(CommandBufferKind::kTextureUploadPrivate));
    COUNT_profile_set("gpu/cb_texture_other_per_frame",
                      kind_per_frame(CommandBufferKind::kTextureOther));
    COUNT_profile_set("gpu/cb_rt_resolve_per_frame",
                      kind_per_frame(CommandBufferKind::kRenderTargetResolve));
    COUNT_profile_set("gpu/cb_rt_dump_per_frame",
                      kind_per_frame(CommandBufferKind::kRenderTargetDump));
    COUNT_profile_set("gpu/cb_rt_other_per_frame",
                      kind_per_frame(CommandBufferKind::kRenderTargetOther));

    COUNT_profile_set(
        "gpu/render_passes_per_frame",
        int64_t((render_passes_total_ - render_passes_window_start_) /
                gpu_time_window_frames_));
    render_passes_window_start_ = render_passes_total_;

    gpu_time_window_frames_ = 0;
  }

  saw_swap_ = true;
  last_swap_ptr_ = frontbuffer_ptr;
  last_swap_width_ = frontbuffer_width;
  last_swap_height_ = frontbuffer_height;

  // Submit the frame the way every other submission ends, including the
  // argument-buffer reuse cache reset (the cached slices belong to this
  // submission's pages, which may be recycled once it retires) and the
  // autorelease pool drain.
  EndCommandBuffer();
  NoteResolveFrame();

  if (primitive_processor_ && frame_open_) {
    primitive_processor_->EndFrame();
    closed_frames_.push_back({frame_current_, submission_current_});
    ++frame_current_;
    frame_open_ = false;
  }
  if (shared_memory_ && ::cvars::clear_memory_page_state) {
    shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
  }

  // Push the rendered frame to the presenter's guest output mailbox
  // This is required for trace dumps to capture the output. Use the
  // MetalRenderTargetCache color target (like D3D12) rather than the
  // legacy standalone render_target_texture_.
  auto* presenter =
      static_cast<ui::metal::MetalPresenter*>(graphics_system_->presenter());
  if (presenter && render_target_cache_) {
    uint32_t output_width =
        frontbuffer_width ? frontbuffer_width : render_target_width_;
    uint32_t output_height =
        frontbuffer_height ? frontbuffer_height : render_target_height_;

    MTL::Texture* source_texture = nullptr;
    bool use_pwl_gamma_ramp = false;
    if (texture_cache_) {
      uint32_t swap_width = 0;
      uint32_t swap_height = 0;
      xenos::TextureFormat swap_format = xenos::TextureFormat::k_8_8_8_8;
      source_texture = texture_cache_->RequestSwapTexture(
          swap_width, swap_height, swap_format);
      if (source_texture) {
        output_width = swap_width;
        output_height = swap_height;
        use_pwl_gamma_ramp =
            swap_format == xenos::TextureFormat::k_2_10_10_10 ||
            swap_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        static MTL::PixelFormat last_format = MTL::PixelFormatInvalid;
        static uint32_t last_samples = 0;
        static uint32_t last_width = 0;
        static uint32_t last_height = 0;
        static int last_swap_format = -1;
        MTL::PixelFormat src_format = source_texture->pixelFormat();
        uint32_t src_samples = source_texture->sampleCount();
        uint32_t src_width = uint32_t(source_texture->width());
        uint32_t src_height = uint32_t(source_texture->height());
        int swap_format_int = static_cast<int>(swap_format);
        if (src_format != last_format || src_samples != last_samples ||
            src_width != last_width || src_height != last_height ||
            swap_format_int != last_swap_format) {
          last_format = src_format;
          last_samples = src_samples;
          last_width = src_width;
          last_height = src_height;
          last_swap_format = swap_format_int;
        }
        if (presenter) {
          if (!gamma_ramp_256_entry_table_up_to_date_ ||
              !gamma_ramp_pwl_up_to_date_) {
            constexpr size_t kGammaRampTableBytes =
                sizeof(reg::DC_LUT_30_COLOR) * 256;
            constexpr size_t kGammaRampPwlBytes =
                sizeof(reg::DC_LUT_PWL_DATA) * 128 * 3;
            if (presenter->UpdateGammaRamp(
                    gamma_ramp_256_entry_table(), kGammaRampTableBytes,
                    gamma_ramp_pwl_rgb(), kGammaRampPwlBytes)) {
              gamma_ramp_256_entry_table_up_to_date_ = true;
              gamma_ramp_pwl_up_to_date_ = true;
            } else {
              XELOGW("Metal IssueSwap: gamma ramp upload failed");
            }
          }
        }
      }
    }

    bool swap_dest_swap = false;
    const bool has_swap_dest_swap =
        ConsumeSwapDestSwap(frontbuffer_ptr, &swap_dest_swap);
    if (!has_swap_dest_swap && frontbuffer_ptr) {
      static uint32_t swap_dest_miss_count = 0;
      if (swap_dest_miss_count < 8) {
        ++swap_dest_miss_count;
      }
    }
    bool force_swap_rb = has_swap_dest_swap && swap_dest_swap;

    if (!source_texture) {
      static bool missing_swap_logged = false;
      if (!missing_swap_logged) {
        missing_swap_logged = true;
        XELOGW(
            "MetalCommandProcessor::IssueSwap: swap texture unavailable; "
            "presenting inactive (black) output");
      }
      presenter->RefreshGuestOutput(
          0, 0, 0, 0, [](ui::Presenter::GuestOutputRefreshContext&) -> bool {
            return false;
          });
      return;
    }

    if (source_texture) {
      ui::metal::MetalPresenter* metal_presenter = presenter;
      uint32_t source_width = output_width;
      uint32_t source_height = output_height;
      bool force_swap_rb_copy = force_swap_rb;
      bool use_pwl_gamma_ramp_copy = use_pwl_gamma_ramp;
      uint32_t color_space = swap_color_space_;
      auto aspect = graphics_system_->GetScaledAspectRatio();
      guest_frames_presented.fetch_add(1, std::memory_order_relaxed);
      presenter->RefreshGuestOutput(
          output_width, output_height, aspect.first, aspect.second,
          [source_texture, metal_presenter, source_width, source_height,
           force_swap_rb_copy, use_pwl_gamma_ramp_copy, color_space](
              ui::Presenter::GuestOutputRefreshContext& context) -> bool {
            auto& metal_context =
                static_cast<ui::metal::MetalGuestOutputRefreshContext&>(
                    context);
            context.SetIs8bpc(!use_pwl_gamma_ramp_copy);
            uint64_t submission_id = 0;
            bool copy_success = metal_presenter->CopyTextureToGuestOutput(
                source_texture, metal_context.resource_uav_capable(),
                source_width, source_height, force_swap_rb_copy,
                use_pwl_gamma_ramp_copy, color_space, &submission_id);
            if (copy_success && submission_id) {
              metal_context.SetSubmissionId(submission_id);
            }
            return copy_success;
          });
    }
  }
}

void MetalCommandProcessor::OnPrimaryBufferEnd() {
  // Pump any completed resolves now since the guest is likely about to poll.
  PumpQueryResolves();
  PumpPendingRetire();

  if (!current_command_buffer_) {
    return;
  }

  // Keep command buffers open across primary-buffer boundaries unless a
  // copy->draw visibility boundary is pending.
  if (!copy_resolve_writes_pending_) {
    return;
  }

  if (!cvars::submit_on_primary_buffer_end) {
    return;
  }
  // Ending an open pass here stores and reloads its attachments, and nothing
  // waits at this point: a ring-empty stall and every wait commit at once.
  // The pass is left to end, and the draw beginning the next one commits.
  // DXIL path only.
  if (UseDxilPath() && current_render_encoder_) {
    primary_end_commit_at_pass_end_ = true;
    return;
  }
  EndCommandBuffer(CommandBufferKind::kSubmissionPrimaryBufferEnd);
}

void MetalCommandProcessor::CommitBetweenRenderPasses() {
  if (!UseDxilPath() || !current_command_buffer_ || current_render_encoder_) {
    return;
  }
  if (command_buffer_render_passes_ >= kMaxRenderPassesPerCommandBuffer) {
    EndCommandBuffer(CommandBufferKind::kSubmissionPassLimit);
  } else if (primary_end_commit_at_pass_end_) {
    EndCommandBuffer(CommandBufferKind::kSubmissionPrimaryBufferEnd);
  } else if (commit_after_drain_armed_ && command_buffer_render_passes_ &&
             GetCompletedSubmission() + 1 >= submission_current_) {
    // Committed submissions run in order, so the GPU is idle once the one
    // before the open submission has completed.
    EndCommandBuffer(CommandBufferKind::kSubmissionAfterDrain);
  }
}

bool MetalCommandProcessor::IsAsyncCompileIdleLocked() const {
  return async_compile_busy_ == 0 && async_shader_queue_.empty() &&
         async_pipeline_queue_.empty() && async_tess_shaders_queue_.empty() &&
         async_shader_pending_.empty() && async_pipeline_pending_.empty() &&
         async_tess_shaders_pending_.empty();
}

void MetalCommandProcessor::RecordStorageRebuildStatsLocked() {
  storage_stats_pending_ = false;
  storage_stats_keys_.clear();
  if (storage_stats_boosted_) {
    return;
  }
  uint64_t elapsed_us = (xe::Clock::QueryHostTickCount() -
                         storage_stats_start_ticks_) *
                        1000000 / xe::Clock::QueryHostTickFrequency();
  std::ofstream stats_file(storage_stats_path_, std::ios::trunc);
  if (stats_file) {
    stats_file << "xrebuild1 " << storage_stats_version_ << ' '
               << storage_stats_stored_ << ' ' << elapsed_us << '\n';
  }
}

void MetalCommandProcessor::CountAsyncCompileTaskFinishedLocked() {
  compile_activity.finished.fetch_add(1, std::memory_order_relaxed);
}

void MetalCommandProcessor::NoteStoredPipelineDoneLocked(
    uint64_t pipeline_key) {
  if (storage_stats_pending_ && storage_stats_keys_.erase(pipeline_key) &&
      storage_stats_queued_all_ && storage_stats_keys_.empty()) {
    RecordStorageRebuildStatsLocked();
  }
}

void MetalCommandProcessor::FinishAsyncCompileTaskLocked() {
  CountAsyncCompileTaskFinishedLocked();
  if (async_compile_busy_) {
    --async_compile_busy_;
  }
  // A waiter may be after this task rather than the whole queue.
  if (async_compile_awaiters_ || IsAsyncCompileIdleLocked()) {
    async_compile_idle_cv_.notify_all();
  }
}

void MetalCommandProcessor::UpdateCompileThreadLimitLocked() {
  if (async_compile_shutdown_ || !async_compile_base_thread_count_) {
    return;
  }
  size_t limit = async_compile_base_thread_count_;
  if ((async_compile_hold_boost_ || async_compile_awaiters_) &&
      async_compile_boost_allowed_) {
    // Whatever is waiting for the compile threads leaves the CPU to them.
    limit = std::max<size_t>(limit, std::thread::hardware_concurrency());
  }
  if (limit == async_compile_thread_limit_) {
    return;
  }
  async_compile_thread_limit_ = limit;
  for (size_t i =
           async_compile_base_thread_count_ + async_compile_boost_threads_.size();
       i < limit; ++i) {
    async_compile_boost_threads_.emplace_back(
        [this, i]() { AsyncCompileThread(i); });
  }
  // Wakes the parked threads when the limit rises, and sends the extra ones
  // back to park when it falls.
  async_compile_boost_cv_.notify_all();
  async_compile_cv_.notify_all();
}

void MetalCommandProcessor::PromoteQueuedPipelineLocked(uint64_t key,
                                                        uint8_t priority) {
  auto queued_it = async_pipeline_queued_.find(key);
  if (queued_it == async_pipeline_queued_.end() ||
      -queued_it->second.first >= int(priority)) {
    return;
  }
  auto node = async_pipeline_queue_.extract(queued_it->second);
  node.key() =
      PipelineQueueOrder(-int(priority), async_pipeline_queue_arrivals_++);
  node.mapped().priority = priority;
  queued_it->second = node.key();
  async_pipeline_queue_.insert(std::move(node));
}

bool MetalCommandProcessor::TakeQueuedPipelineLocked(uint64_t key) {
  auto queued_it = async_pipeline_queued_.find(key);
  if (queued_it == async_pipeline_queued_.end()) {
    return false;
  }
  auto queue_it = async_pipeline_queue_.find(queued_it->second);
  PipelineCompileRequest& request = queue_it->second;
  if (request.vertex_function) {
    request.vertex_function->release();
  }
  if (request.fragment_function) {
    request.fragment_function->release();
  }
  async_pipeline_queue_.erase(queue_it);
  async_pipeline_queued_.erase(queued_it);
  async_pipeline_pending_.erase(key);
  NoteStoredPipelineDoneLocked(key);
  CountAsyncCompileTaskFinishedLocked();
  if (IsAsyncCompileIdleLocked()) {
    async_compile_idle_cv_.notify_all();
  }
  return true;
}

void MetalCommandProcessor::SetCompileThreadBoost(bool boost) {
  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  if (async_compile_shutdown_ || async_compile_hold_boost_ == boost) {
    return;
  }
  async_compile_hold_boost_ = boost;
  if (boost && storage_stats_pending_) {
    storage_stats_boosted_ = true;
  }
  // Without a pool yet, it applies when the pool starts.
  UpdateCompileThreadLimitLocked();
  XELOGI("Metal: {} shader/pipeline compile thread(s){}",
         async_compile_thread_limit_,
         boost ? " while the game waits for shaders" : "");
}

void MetalCommandProcessor::SetCompileThreadBoostAllowed(bool allowed) {
  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  if (async_compile_boost_allowed_ == allowed) {
    return;
  }
  async_compile_boost_allowed_ = allowed;
  UpdateCompileThreadLimitLocked();
  XELOGI("Metal: compile thread boost {}", allowed ? "allowed" : "off (hot device)");
}

template <typename Done>
void MetalCommandProcessor::AwaitAsyncCompilesUntil(const Done& done) {
  if (async_compile_threads_.empty()) {
    return;
  }
  SCOPE_profile_cpu_i("gpu", "MetalCommandProcessor::AwaitAsyncCompilesUntil");
  {
    std::unique_lock<std::mutex> lock(async_compile_mutex_);
    if (done()) {
      return;
    }
    ++async_compile_awaiters_;
    if (storage_stats_pending_) {
      // Every core compiling would forecast the next rebuild short.
      storage_stats_boosted_ = true;
    }
    UpdateCompileThreadLimitLocked();
    async_compile_idle_cv_.wait(lock, done);
    --async_compile_awaiters_;
    UpdateCompileThreadLimitLocked();
  }
  if (ShouldLogRateLimited(async_compile_drain_last_log_ns_,
                           kAsyncCompileLogIntervalNs)) {
    XELOGI(
        "Metal: blocked the draw thread on async compilation - a draw needed "
        "the guest's exact shaders");
  }
}

void MetalCommandProcessor::AwaitAsyncCompiles() {
  AwaitAsyncCompilesUntil([this]() { return IsAsyncCompileIdleLocked(); });
}

void MetalCommandProcessor::AwaitPendingPipeline() {
  const uint64_t key = last_pending_pipeline_key_;
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    PromoteQueuedPipelineLocked(key, kPipelinePriorityAwaited);
  }
  AwaitAsyncCompilesUntil([this, key]() {
    return async_pipeline_pending_.find(key) == async_pipeline_pending_.end();
  });
}

MTL::CommandBuffer* MetalCommandProcessor::CreateAccountedCommandBuffer(
    CommandBufferKind kind) {
  SCOPE_profile_cpu_f("gpu");
  if (!command_queue_) {
    return nullptr;
  }
  MTL::CommandBuffer* command_buffer = nullptr;
  {
    // Blocks once the queue's in-flight command buffers are all outstanding,
    // so this reads as GPU backpressure rather than allocation cost.
    SCOPE_profile_cpu_i("gpu", "MetalCommandProcessor::QueueCommandBuffer");
    uint64_t wait_start = xe::Clock::QueryHostTickCount();
    command_buffer = command_queue_->commandBuffer();
    gpu_timing.wait_ticks.fetch_add(
        xe::Clock::QueryHostTickCount() - wait_start,
        std::memory_order_relaxed);
  }
  if (command_buffer) {
    ++command_buffer_kind_counts_[size_t(kind)];
    SCOPE_profile_cpu_i("gpu", "MetalCommandProcessor::AddGpuTimeHandler");
    AddGpuTimeHandler(command_buffer);
  }
  return command_buffer;
}

void MetalCommandProcessor::DiscardAccountedCommandBuffer(
    MTL::CommandBuffer* command_buffer, CommandBufferKind kind) {
  if (command_buffer) {
    --command_buffer_kind_counts_[size_t(kind)];
    if (shared_memory_ && shared_memory_->hazards()) {
      shared_memory_->hazards()->Discard(command_buffer);
    }
    pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
  }
}

void MetalCommandProcessor::AddGpuTimeHandler(
    MTL::CommandBuffer* command_buffer) {
  pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
  command_buffer->addCompletedHandler([this](MTL::CommandBuffer* completed) {
    // Every accounted command buffer gets this handler, so a failure is seen
    // once here, per command buffer, never per draw.
    MTL::CommandBufferStatus status = completed->status();
    if (status != MTL::CommandBufferStatusCompleted) {
      LogCommandBufferFailure(completed, status);
    }
    double gpu_start = completed->GPUStartTime();
    double gpu_end = completed->GPUEndTime();
    if (gpu_end > gpu_start) {
      completed_gpu_time_ns_.fetch_add(uint64_t((gpu_end - gpu_start) * 1e9),
                                       std::memory_order_relaxed);
      // Union: only the part past the latest end already counted. Exact when
      // command buffers complete in the order they started; one that finishes
      // inside a still-running earlier one loses the earlier one's head.
      uint64_t start_ns = uint64_t(gpu_start * 1e9);
      uint64_t end_ns = uint64_t(gpu_end * 1e9);
      uint64_t counted_end_ns =
          gpu_timing.busy_end_ns.load(std::memory_order_relaxed);
      while (end_ns > counted_end_ns &&
             !gpu_timing.busy_end_ns.compare_exchange_weak(
                 counted_end_ns, end_ns, std::memory_order_relaxed)) {
      }
      if (end_ns > counted_end_ns) {
        gpu_timing.busy_ns.fetch_add(
            end_ns - std::max(start_ns, counted_end_ns),
            std::memory_order_relaxed);
      }
    }
    gpu_time_command_buffers_.fetch_add(1, std::memory_order_relaxed);
    pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
  });
}

void MetalCommandProcessor::LogCommandBufferFailure(
    MTL::CommandBuffer* command_buffer, MTL::CommandBufferStatus status) {
  failed_command_buffers_.fetch_add(1, std::memory_order_relaxed);
  // A buffer aborted by the GPU (for example, work submitted from the
  // background) was silent before; its output is lost.
  NS::Error* error = command_buffer->error();
  // Work submitted from the background is refused by design, not a GPU fault,
  // so diagnostics don't count it.
  if (!(status == MTL::CommandBufferStatusError && error &&
        error->code() == MTL::CommandBufferErrorNotPermitted)) {
    command_buffer_failures.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t kind_bit =
      status == MTL::CommandBufferStatusError && error
          ? uint64_t(1) << (uint64_t(error->code()) & 31)
          : uint64_t(1) << (32 + (uint64_t(status) & 31));
  if (logged_command_buffer_failures_.fetch_or(
          kind_bit, std::memory_order_relaxed) &
      kind_bit) {
    return;
  }
  // Metal's completion queue may have no pool of its own.
  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
  NS::String* description = error ? error->localizedDescription() : nullptr;
  XELOGE(
      "MetalCommandProcessor: a command buffer finished with status {}, error "
      "{}: {}. Further failures of this kind are only counted.",
      uint32_t(status), error ? int64_t(error->code()) : int64_t(0),
      description ? description->utf8String() : "no description");
  pool->release();
}

void MetalCommandProcessor::AwaitSubmissionCompletion(uint64_t submission) {
  if (completed_command_buffers_.load(std::memory_order_acquire) >=
      submission) {
    return;
  }
  uint64_t wait_start = xe::Clock::QueryHostTickCount();
  {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    completion_cond_.wait(lock, [this, submission]() {
      return completed_command_buffers_.load(std::memory_order_acquire) >=
             submission;
    });
  }
  gpu_timing.wait_ticks.fetch_add(xe::Clock::QueryHostTickCount() - wait_start,
                                  std::memory_order_relaxed);
}

void MetalCommandProcessor::NoteGpuWait(GpuWaitReason reason,
                                        int64_t wait_start_ns) {
  frame_gpu_wait_ns_[size_t(reason)] += GetSteadyTimeNs() - wait_start_ns;
  ++frame_gpu_wait_counts_[size_t(reason)];
}

void MetalCommandProcessor::CommitAndAwait(MTL::CommandBuffer* command_buffer,
                                            GpuWaitReason reason) {
  EndSharedComputeEncoder(command_buffer);
  command_buffer->commit();
  int64_t wait_start_ns = GetSteadyTimeNs();
  command_buffer->waitUntilCompleted();
  NoteGpuWait(reason, wait_start_ns);
}

void MetalCommandProcessor::LogFrameGpuWaits() {
  int64_t now_ns = GetSteadyTimeNs();
  if (cvars::metal_log_frame_gpu_waits) {
    uint64_t wait_ns = 0;
    std::string waits;
    for (size_t i = 0; i < kGpuWaitReasonCount; ++i) {
      wait_ns += frame_gpu_wait_ns_[i];
      if (frame_gpu_wait_counts_[i]) {
        waits += fmt::format(" {}={:.2f}ms/{}", kGpuWaitReasonNames[i],
                             frame_gpu_wait_ns_[i] * 1e-6,
                             frame_gpu_wait_counts_[i]);
      }
    }
    uint64_t command_buffers = 0;
    std::string kinds;
    for (size_t i = 0; i < kCommandBufferKindCount; ++i) {
      uint64_t count =
          command_buffer_kind_counts_[i] - command_buffer_kind_frame_start_[i];
      command_buffers += count;
      if (count) {
        kinds += fmt::format(" {}={}", kCommandBufferKindNames[i], count);
      }
    }
    XELOGI(
        "Metal: frame {}: {:.2f} ms, {:.2f} ms waiting for the GPU{}; {} "
        "command buffer(s){}",
        frame_current_,
        frame_log_start_ns_ ? (now_ns - frame_log_start_ns_) * 1e-6 : 0.0,
        wait_ns * 1e-6, waits, command_buffers, kinds);
  }
  std::fill(std::begin(frame_gpu_wait_ns_), std::end(frame_gpu_wait_ns_), 0);
  std::fill(std::begin(frame_gpu_wait_counts_),
            std::end(frame_gpu_wait_counts_), 0);
  std::copy(std::begin(command_buffer_kind_counts_),
            std::end(command_buffer_kind_counts_),
            command_buffer_kind_frame_start_);
  frame_log_start_ns_ = now_ns;
}

void MetalCommandProcessor::AwaitMemexportSubmission(uint64_t submission) {
  int64_t wait_start_ns = GetSteadyTimeNs();
  // submission_current_ is committed once there's no command buffer open, and
  // completions arrive in order.
  if (submission >= submission_current_ && current_command_buffer_) {
    EndCommandBuffer(CommandBufferKind::kSubmissionWait);
  }
  AwaitSubmissionCompletion(submission);
  NoteGpuWait(GpuWaitReason::kMemexport, wait_start_ns);
}

void MetalCommandProcessor::AwaitAllQueueOperationsCompletion() {
  // Ending first, so a memexport draw that is still only recorded gets
  // submitted rather than waited past.
  EndCommandBuffer(CommandBufferKind::kSubmissionWait);
  AwaitSubmissionCompletion(submission_current_);
}

uint64_t MetalCommandProcessor::AwaitCompletedGpuTimeNs(
    uint64_t* command_buffers) {
  AwaitAllQueueOperationsCompletion();
  WaitForPendingCompletionHandlers();
  *command_buffers = gpu_time_command_buffers_.load(std::memory_order_acquire);
  return completed_gpu_time_ns_.load(std::memory_order_acquire);
}

// ============================================================================
// ZPD (occlusion query) backend overrides.
// ============================================================================

void MetalCommandProcessor::EnsureQueryResources() {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_visibility_pool_) {
    return;
  }
  if (!zpd_visibility_pool_->EnsureInitialized(device_, kQueryPoolCapacity)) {
    // CanOpenQuery gates on the pool, so OpenQuerySegment returns before
    // reaching the base class's own pool-readiness check that arms this. Left
    // unset, every report would resolve to zero samples - fully occluded -
    // instead of falling back to fake counts.
    zpd_force_fake_fallback_ = true;
  }
}

void MetalCommandProcessor::ShutdownQueryResources() {
  if (!zpd_visibility_pool_) {
    return;
  }
  zpd_resolves_in_flight_.clear();
  zpd_active_query_.Reset();
  zpd_visibility_pool_->Shutdown();
}

bool MetalCommandProcessor::IsQueryPoolReady() const {
  return zpd_visibility_pool_ && zpd_visibility_pool_->is_initialized();
}

bool MetalCommandProcessor::CanOpenQuery() const {
  // Metal visibility queries can only be enabled on a render encoder whose
  // descriptor had visibilityResultBuffer set before the encoder was created.
  return current_command_buffer_ != nullptr &&
         current_render_encoder_ != nullptr &&
         render_encoder_has_zpd_visibility_;
}

CommandProcessor::QueryOpenResult MetalCommandProcessor::OpenQuery(
    bool can_close_submission) {
  if (!IsQueryPoolReady()) {
    return QueryOpenResult::kFailed;
  }
  if (!CanOpenQuery()) {
    return QueryOpenResult::kDeferred;
  }

  bool is_pool_exhausted = !zpd_visibility_pool_->has_free_indices();
  if (is_pool_exhausted) {
    PumpQueryResolves();
    is_pool_exhausted = !zpd_visibility_pool_->has_free_indices();
  }

  bool waited_for_submission = false;

  if (is_pool_exhausted) {
    if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
      return QueryOpenResult::kPoolExhausted;
    }

    uint64_t wait_for = 0;
    if (!zpd_resolves_in_flight_.empty()) {
      wait_for = zpd_resolves_in_flight_.front().submission;
    }

    uint64_t completed_submission = GetCompletedSubmission();
    if (wait_for > completed_submission) {
      if (wait_for >= GetCurrentSubmission()) {
        // The oldest slot is held by the submission being recorded. Commit it
        // so it can retire, and let the next draw retry the segment.
        if (can_close_submission) {
          EndRenderEncoder();
          EndCommandBuffer(CommandBufferKind::kSubmissionZpdQuery);
        }
        return QueryOpenResult::kDeferred;
      }

      if (cvars::occlusion_query_log) {
        XELOGI("ZPD: Stall awaiting submission={} completed_before={}",
               wait_for, completed_submission);
      }
      int64_t wait_start_ns = GetSteadyTimeNs();
      AwaitSubmissionCompletion(wait_for);
      NoteGpuWait(GpuWaitReason::kZpdQuery, wait_start_ns);
      waited_for_submission = true;
      PumpQueryResolves();
      is_pool_exhausted = !zpd_visibility_pool_->has_free_indices();
    }
  }

  if (is_pool_exhausted) {
    return waited_for_submission ? QueryOpenResult::kPoolExhausted
                                 : QueryOpenResult::kDeferred;
  }

  MetalZPDActiveQuery active_query;
  if (!zpd_visibility_pool_->Acquire(
          active_query.index, active_query.generation, active_query.offset)) {
    return QueryOpenResult::kFailed;
  }

  current_render_encoder_->setVisibilityResultMode(
      MTL::VisibilityResultModeCounting, active_query.offset);
  zpd_active_query_ = active_query;
  return QueryOpenResult::kOpened;
}

bool MetalCommandProcessor::CloseQuery(ReportHandle report_handle,
                                       const VIZQueryHandle& viz,
                                       uint64_t& out_submission) {
  if (!current_render_encoder_ || !render_encoder_has_zpd_visibility_ ||
      !zpd_active_query_.is_open()) {
    return false;
  }

  // Disable at this segment's own offset. The offset passed alongside Disabled
  // is still touched by the pass, so a hardcoded 0 would zero pool slot 0 and
  // make whatever segment owns it resolve as fully occluded.
  current_render_encoder_->setVisibilityResultMode(
      MTL::VisibilityResultModeDisabled, zpd_active_query_.offset);

  MetalZPDResolve resolve;
  resolve.submission = GetCurrentSubmission();
  resolve.index = zpd_active_query_.index;
  resolve.generation = zpd_active_query_.generation;
  resolve.scale_area = GetZPDScaleArea();
  resolve.report_handle = report_handle;
  resolve.viz = viz;
  zpd_resolves_in_flight_.push_back(resolve);

  out_submission = resolve.submission;

  zpd_active_query_.Reset();
  return true;
}

void MetalCommandProcessor::PumpQueryResolves() {
  if (!zpd_visibility_pool_) {
    return;
  }

  uint64_t completed = GetCompletedSubmission();
  if (completed == 0) {
    return;
  }

  while (!zpd_resolves_in_flight_.empty()) {
    if (zpd_resolves_in_flight_.front().submission > completed) {
      break;
    }
    MetalZPDResolve resolve = zpd_resolves_in_flight_.front();
    zpd_resolves_in_flight_.pop_front();

    if (!zpd_visibility_pool_->IsGenerationCurrent(resolve.index,
                                                   resolve.generation)) {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD/Metal: Dropping stale query index={} generation={} handle={}",
            resolve.index, resolve.generation, resolve.report_handle);
      }
      continue;
    }

    uint64_t raw_samples;
    if (!zpd_visibility_pool_->Read(resolve.index, raw_samples)) {
      // An unpreserved result isn't known to be zero. Count one visible sample,
      // the same floor an abandoned report gets, so culling doesn't flash
      // occluded.
      raw_samples = 1;
    }
    zpd_visibility_pool_->Release(resolve.index, resolve.generation);
    if (resolve.report_handle != kInvalidReportHandle) {
      // Metal has no in-shader counter path, so only ZPass is ever counted.
      OnZPDQueryResolved(resolve.report_handle,
                         XenosZPDReport::FromNativeQuery(raw_samples),
                         resolve.scale_area);
    }
    if (resolve.viz.generation != kInvalidVIZGeneration) {
      OnVIZQueryResolved(resolve.viz.id, resolve.viz.generation,
                         raw_samples != 0);
    }
  }
}

bool MetalCommandProcessor::AwaitQueryResolve(ReportHandle report_handle,
                                              uint64_t wait_for_submission) {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  PumpQueryResolves();

  const ZPDReport* report = FindZPDReport(report_handle);
  if (!report || !report->pending_segments) {
    return true;
  }
  if (wait_for_submission == 0) {
    return false;
  }

  // The segment may still be in the submission being recorded, which has to be
  // committed before it can ever complete. A closed command buffer needs no
  // flush: everything recorded so far is already on the queue.
  if (wait_for_submission >= GetCurrentSubmission() &&
      current_command_buffer_) {
    // No wait on the compile threads: a counted draw already resolved its own
    // shaders, and Metal records concrete pipeline states, so a compile in
    // flight for some other draw has no bearing on committing this one.
    EndRenderEncoder();
    EndCommandBuffer(CommandBufferKind::kSubmissionZpdQuery);
  }

  if (wait_for_submission > GetCompletedSubmission()) {
    int64_t wait_start_ns = GetSteadyTimeNs();
    AwaitSubmissionCompletion(wait_for_submission);
    NoteGpuWait(GpuWaitReason::kZpdQuery, wait_start_ns);
  }

  PumpQueryResolves();

  report = FindZPDReport(report_handle);
  return !report || !report->pending_segments;
}

Shader* MetalCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                          const uint32_t* host_address,
                                          uint32_t dword_count) {
  const size_t ucode_size = dword_count * sizeof(uint32_t);
  for (auto it = recent_shader_loads_.begin(); it != recent_shader_loads_.end();
       ++it) {
    if (!it->shader) {
      break;
    }
    if (it->ucode.size() == dword_count &&
        !std::memcmp(it->ucode.data(), host_address, ucode_size)) {
      std::rotate(recent_shader_loads_.begin(), it, std::next(it));
      return recent_shader_loads_.front().shader;
    }
  }
  SpirvShader* shader =
      LoadShader(shader_type, host_address, dword_count,
                 XXH3_64bits(host_address, ucode_size));
  std::rotate(recent_shader_loads_.begin(), std::prev(recent_shader_loads_.end()),
              recent_shader_loads_.end());
  RecentShaderLoad& recent = recent_shader_loads_.front();
  recent.shader = shader;
  recent.ucode.assign(host_address, host_address + dword_count);
  return shader;
}

Shader* MetalCommandProcessor::LoadShaderAtAddress(
    xenos::ShaderType shader_type, uint32_t guest_address,
    uint32_t dword_count) {
  const uint32_t* host_address =
      memory_->TranslatePhysical<const uint32_t*>(guest_address);
  const uint32_t stage = uint32_t(shader_type);
  if (stage > 1 || !dword_count || !shared_memory_ ||
      !shared_memory_->is_zero_copy() ||
      guest_address >= SharedMemory::kBufferSize ||
      dword_count > (SharedMemory::kBufferSize - guest_address) >> 2) {
    return LoadShader(shader_type, host_address, dword_count);
  }
  const uint32_t length = dword_count * sizeof(uint32_t);
  AddressShaderLoad* entry;
  auto& last = last_address_shader_loads_[stage];
  if (last.second && last.first == guest_address) {
    entry = last.second;
  } else {
    std::unique_ptr<AddressShaderLoad>& slot =
        address_shader_loads_[guest_address];
    if (!slot) {
      slot = std::make_unique<AddressShaderLoad>();
    }
    entry = slot.get();
    last = {guest_address, entry};
  }
  // A watch clears valid before the write it reports goes through, and the
  // guest writes ucode before submitting the packets that load it.
  if (entry->valid.load(std::memory_order_acquire) &&
      entry->dword_count == dword_count && entry->shader_type == shader_type) {
    return entry->shader;
  }

  if (entry->unwatchable) {
    return LoadShader(shader_type, host_address, dword_count);
  }
  SpirvShader* previous_shader = entry->shader;
  if (entry->watch_installed) {
    // Still watched if only the size or the stage differs.
    auto global_lock = xe::global_critical_region::Acquire();
    if (entry->watch) {
      shared_memory_->UnwatchMemoryRange(entry->watch);
      entry->watch = nullptr;
    }
    entry->valid.store(false, std::memory_order_relaxed);
    entry->watch_installed = false;
  }
  // As for textures: make the range valid (protected) first, then read it,
  // then watch it only if no write came in between.
  bool requested = shared_memory_->RequestRange(guest_address, length);
  Shader* shader = LoadShader(shader_type, host_address, dword_count);
  auto* spirv_shader = static_cast<SpirvShader*>(shader);
  if (previous_shader && spirv_shader == previous_shader &&
      entry->dword_count == dword_count && entry->shader_type == shader_type) {
    if (++entry->same_reloads >= 8) {
      entry->unwatchable = true;
      return shader;
    }
  } else {
    entry->same_reloads = 0;
  }
  entry->shader = spirv_shader;
  entry->dword_count = dword_count;
  entry->shader_type = shader_type;
  if (requested && spirv_shader) {
    auto global_lock = xe::global_critical_region::Acquire();
    if (shared_memory_->IsRangeValid(guest_address, length)) {
      entry->watch = shared_memory_->WatchMemoryRange(
          guest_address, length, AddressShaderLoadWatchCallback, this, entry,
          0);
      entry->watch_installed = entry->watch != nullptr;
      entry->valid.store(entry->watch_installed, std::memory_order_relaxed);
    }
  }
  return shader;
}

void MetalCommandProcessor::AddressShaderLoadWatchCallback(
    const global_unique_lock_type& global_lock, void* context, void* data,
    uint64_t argument, bool invalidated_by_gpu) {
  auto* entry = static_cast<AddressShaderLoad*>(data);
  // The watch is cancelled after this callback.
  entry->watch = nullptr;
  entry->valid.store(false, std::memory_order_release);
}

void MetalCommandProcessor::ClearAddressShaderLoads() {
  if (shared_memory_) {
    auto global_lock = xe::global_critical_region::Acquire();
    for (auto& [address, entry] : address_shader_loads_) {
      if (entry->watch) {
        shared_memory_->UnwatchMemoryRange(entry->watch);
        entry->watch = nullptr;
      }
    }
  }
  address_shader_loads_.clear();
  last_address_shader_loads_ = {};
}

SpirvShader* MetalCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                               const uint32_t* host_address,
                                               uint32_t dword_count,
                                               uint64_t hash) {
  auto it = guest_shader_cache_.find(hash);
  if (it != guest_shader_cache_.end()) {
    return it->second.get();
  }
  std::unique_ptr<SpirvShader> shader;
  if (UseDxilPath()) {
    shader = std::make_unique<DxilShader>(shader_type, hash, host_address,
                                          dword_count);
  } else {
    shader = std::make_unique<MslShader>(shader_type, hash, host_address,
                                         dword_count);
  }
  SpirvShader* result = shader.get();
  guest_shader_cache_[hash] = std::move(shader);
  return result;
}

bool MetalCommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  SCOPE_profile_cpu_f("gpu");
  const RegisterFile& regs = *register_file_;
  uint32_t normalized_color_mask = 0;

  // VIZ surveys aren't measured on Metal yet, so every ID stays visible.
  OnVIZSurveyDraw(false);

  // Check for copy mode
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    if (draw_util::IsVIZSurveyDraw(regs)) {
      return true;
    }
    return IssueCopy();
  }
  // A pass kept open for resolves only ends before anything draws, with the
  // resolve work left to its end.
  if (RenderEncoderResolveOnly()) {
    EndRenderEncoder();
  }

  // Vertex shader analysis — use Shader* base type so both draw paths share
  // this common code.
  Shader* vertex_shader = active_vertex_shader();
  if (!vertex_shader) {
    XELOGW("IssueDraw: No vertex shader");
    return false;
  }
  if (!vertex_shader->is_ucode_analyzed()) {
    vertex_shader->AnalyzeUcode(ucode_disasm_buffer_);
  }
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = active_pixel_shader();
      if (pixel_shader) {
        if (!pixel_shader->is_ucode_analyzed()) {
          pixel_shader->AnalyzeUcode(ucode_disasm_buffer_);
        }
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    if (!memexport_used_vertex) {
      return true;
    }
  }
  bool memexport_used_pixel =
      pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  memexport_ranges_awaited_ =
      IsMemexportAwaited(memexport_used_vertex, memexport_used_pixel);
  const bool pure_memexport_draw = IsPureDxilMemexportDraw(
      regs, is_rasterization_done, memexport_used_vertex,
      !memexport_ranges_.empty(), pixel_shader != nullptr,
      index_buffer_info != nullptr);
  // A pure memexport draw is ordered only against what its vertex indices can
  // reach. Auto-indexed guest vertex indices are VGT_INDX_OFFSET plus 0 to
  // below the index count.
  draw_util::VertexIndexRange pure_vertex_indices;
  const draw_util::VertexIndexRange* vertex_indices = nullptr;
  memexport_ordering_ranges_ = memexport_ranges_;
  if (pure_memexport_draw && index_count) {
    pure_vertex_indices.first = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
    if (uint64_t(pure_vertex_indices.first) + index_count <=
        (uint64_t(1) << 24)) {
      pure_vertex_indices.last = pure_vertex_indices.first + index_count - 1;
      vertex_indices = &pure_vertex_indices;
      memexport_ordering_ranges_.clear();
      draw_util::AddMemExportRanges(regs, *vertex_shader,
                                    memexport_ordering_ranges_, vertex_indices);
    }
  }
  // Tile resolves earlier in the pass may not have run in every tile yet.
  if (current_render_encoder_ &&
      !render_encoder_tile_resolve_ranges_.empty() &&
      DrawOverlapsPendingWrites(render_encoder_tile_resolve_ranges_,
                                *vertex_shader, pixel_shader, index_buffer_info,
                                vertex_indices)) {
    EndRenderEncoder();
  }
  render_encoder_index_count_ += index_count;
  // Fences written while the pass is open are only stored when it ends.
  if (current_render_encoder_ && !pending_shader_done_fence_ranges_.empty() &&
      DrawOverlapsPendingWrites(pending_shader_done_fence_ranges_,
                                *vertex_shader, pixel_shader, index_buffer_info,
                                vertex_indices)) {
    EndRenderEncoder();
  }
  if (UseDxilPath() && current_render_encoder_ &&
      DrawOverlapsPendingWrites(render_encoder_memexport_ranges_,
                                *vertex_shader, pixel_shader, index_buffer_info,
                                vertex_indices)) {
    // Order the export before this draw's accesses. Within a chain of pure
    // vertex memexport draws, which can only overlap through vertex fetches
    // and exports, a fresh vertex-to-vertex buffer memory barrier does; any
    // other draw ends the pass.
    if (pure_memexport_draw && render_encoder_memexport_draws_are_pure_) {
      current_render_encoder_->memoryBarrier(MTL::BarrierScopeBuffers,
                                             MTL::RenderStageVertex,
                                             MTL::RenderStageVertex);
    } else {
      EndRenderEncoder();
    }
  }
  // Primitive/index processing (like D3D12/Vulkan).
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_) {
    XELOGE("IssueDraw: primitive processor is not initialized");
    return false;
  }
  // Converted indices are tagged with the current frame, which must be open.
  if (!EnsureCommandBuffer()) {
    return true;
  }
  if (!primitive_processor_->Process(primitive_processing_result)) {
    XELOGE("IssueDraw: primitive processing failed");
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kMemExportCompute) {
    primitive_processing_result.host_vertex_shader_type =
        Shader::HostVertexShaderType::kVertex;
  }

  if (primitive_processing_result.IsTessellated()) {
    // The DXIL path tessellates through MSC's object/mesh emulation.
    if (UseDxilPath() && !mesh_shader_supported_) {
      static bool tess_mesh_logged = false;
      if (!tess_mesh_logged) {
        tess_mesh_logged = true;
        XELOGW(
            "Metal: skipping tessellated draws, mesh shaders are not supported "
            "on this device");
      }
      return true;
    }
    if (!pixel_shader) {
      static bool tess_no_ps_logged = false;
      if (!tess_no_ps_logged) {
        tess_no_ps_logged = true;
        XELOGW(
            "Metal: Tessellation emulation requested without a pixel shader; "
            "using depth-only PS fallback");
      }
    }
  }

  // Configure render targets via MetalRenderTargetCache, similar to D3D12.
  if (render_target_cache_) {
    auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
    uint32_t ps_writes_color_targets =
        pixel_shader ? pixel_shader->writes_color_targets() : 0;
    normalized_color_mask = pixel_shader ? draw_util::GetNormalizedColorMask(
                                               regs, ps_writes_color_targets)
                                         : 0;
    // Strip merge (strip_merge.h): draws on the strip surface either address
    // the frame (the strip's window, extended to the frame when merging) or
    // the strip's band itself, which the merged first strip replicates into
    // every band.
    strip_merge_replicate_draw_ = false;
    strip_merge_first_band_only_ = false;
    bool strip_merge_keep_carries = false;
    if (strip_merge_.recording()) {
      StripMerge::DrawInfo strip_merge_draw;
      strip_merge_draw.vertex_shader = vertex_shader;
      strip_merge_draw.pixel_shader = pixel_shader;
      strip_merge_draw.banded =
          is_rasterization_done && strip_merge_.IsSurfaceBanded();
      strip_merge_draw.frame_space = strip_merge_.IsFrameSpace();
      strip_merge_draw.memexport = memexport_used;
      strip_merge_draw.pixel_position =
          pixel_shader && regs.Get<reg::SQ_PROGRAM_CNTL>().param_gen;
      strip_merge_draw.tessellated =
          primitive_processing_result.IsTessellated();
      strip_merge_.CheckDraw(strip_merge_draw);
      if (strip_merge_.merging() && !strip_merge_.failed()) {
        // A draw only the first strip does goes into its band alone, as
        // that strip draws it.
        strip_merge_first_band_only_ = strip_merge_.first_strip_only_draw();
        strip_merge_keep_carries =
            strip_merge_first_band_only_ && !strip_merge_draw.banded;
        strip_merge_replicate_draw_ =
            strip_merge_draw.banded &&
            (!strip_merge_draw.frame_space || strip_merge_first_band_only_);
      }
    }
    {
      MetalRenderTargetCache::ClearQuadDraw clear_quad_draw;
      clear_quad_draw.candidate =
          is_rasterization_done && !memexport_used &&
          primitive_type == xenos::PrimitiveType::kRectangleList &&
          !primitive_processing_result.IsTessellated();
      clear_quad_draw.has_pixel_shader = pixel_shader != nullptr;
      clear_quad_draw.pixel_shader_kills =
          pixel_shader && pixel_shader->kills_pixels();
      // A merged draw in frame space covers the merged render targets' rows
      // as it does the frame's. One addressing the band is drawn in every
      // band, or in the first alone if only the first strip draws it
      // (DrawStripMergeBands).
      if (strip_merge_replicate_draw_) {
        clear_quad_draw.band_count = strip_merge_first_band_only_
                                         ? 1
                                         : strip_merge_.layout().strip_count;
        clear_quad_draw.band_rows = strip_merge_.layout().band_rows;
      }
      render_target_cache_->SetClearQuadDraw(clear_quad_draw);
    }
    // The window offset stays in the viewport on Metal.
    render_target_cache_->SetStripMergeKeepCarries(strip_merge_keep_carries);
    if (!render_target_cache_->Update(
            is_rasterization_done, normalized_depth_control,
            normalized_color_mask, *vertex_shader, 0)) {
      XELOGE(
          "MetalCommandProcessor::IssueDraw - RenderTargetCache::Update "
          "failed");
      return false;
    }
  }

  // Pipeline formats can be derived from the cache without opening a render
  // encoder. Pending attachment transfers stay queued for the head of the
  // eventual pass while independent texture and shared-memory uploads are
  // encoded first. First-use clears still keep the encoder eager. Queries are
  // opened around guest draws, not around the preceding ownership transfers.
  if (UseDxilPath() && current_render_encoder_ && render_target_cache_ &&
      render_target_cache_->PendingDrawPassTransfersPrepareable() &&
      !render_target_cache_->IsRenderPassDescriptorCompatible(
          current_render_pass_descriptor_, 1)) {
    EndRenderEncoder();
  }
  CommitBetweenRenderPasses();
  const bool initial_draw_encoder_deferred = CanDeferEmptyDrawEncoder();
  if (initial_draw_encoder_deferred) {
    EnsureCommandBuffer();
  } else {
    BeginCommandBuffer();
  }
  if (!current_command_buffer_ ||
      (!current_render_encoder_ && !initial_draw_encoder_deferred)) {
    static bool spirv_no_command_buffer_logged = false;
    if (!spirv_no_command_buffer_logged) {
      spirv_no_command_buffer_logged = true;
      XELOGE(
          "IssueDraw: failed to begin Metal command buffer/render encoder; "
          "skipping draws until uniforms buffer allocation recovers");
    }
    return true;
  }

  if (UseDxilPath()) {
    bool ok =
        IssueDrawDxil(vertex_shader, pixel_shader, primitive_processing_result,
                      primitive_polygonal, memexport_used, pure_memexport_draw,
                      normalized_color_mask, regs);
    // A pending shader or failed upload may abandon the draw. Preserve the
    // eager path's clear and ownership writes before the next packet changes
    // registers or observes EDRAM. Successful draws already opened the pass.
    if (initial_draw_encoder_deferred && !current_render_encoder_) {
      BeginCommandBuffer();
      ok &= current_render_encoder_ != nullptr;
    }
    return ok;
  }

  if (!EnsureSpirvUniformBufferCapacity()) {
    XELOGE(
        "IssueDraw: failed to prepare SPIRV-Cross uniforms ring; skipping "
        "draw");
    return true;
  }
  return IssueDrawMsl(vertex_shader, pixel_shader, primitive_processing_result,
                      primitive_polygonal, is_rasterization_done,
                      memexport_used, normalized_color_mask, regs);
}

bool MetalCommandProcessor::RequestDrawSharedMemoryRanges(
    const Shader& vertex_shader, const RegisterFile& regs) {
  SCOPE_profile_cpu_f("gpu");
  draw_shared_memory_reads_.clear();
  if (!shared_memory_) {
    return true;
  }
  const Shader::ConstantRegisterMap& constant_map_vertex =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map_vertex.vertex_fetch_bitmap);
       ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(vfetch_index);
      switch (vfetch.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (::cvars::gpu_allow_invalid_fetch_constants) {
            break;
          }
          XELOGW(
              "Metal: Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" "
              "type. Use --gpu_allow_invalid_fetch_constants to bypass.",
              vfetch_index, vfetch.dword_0, vfetch.dword_1);
          return false;
        default:
          XELOGW("Metal: Vertex fetch constant {} ({:08X} {:08X}) is invalid.",
                 vfetch_index, vfetch.dword_0, vfetch.dword_1);
          return false;
      }
      // Mask to physical like the shader - the guest may use a mirror window.
      uint32_t buffer_offset = xenos::CpuToGpu(vfetch.address << 2);
      uint32_t buffer_length = vfetch.size << 2;
      if (buffer_offset > SharedMemory::kBufferSize ||
          SharedMemory::kBufferSize - buffer_offset < buffer_length) {
        XELOGW(
            "Metal: Vertex fetch constant {} out of range (offset=0x{:08X} "
            "size={})",
            vfetch_index, buffer_offset, buffer_length);
        return false;
      }
      if (!shared_memory_->RequestRange(buffer_offset, buffer_length)) {
        XELOGE(
            "Metal: Failed to request vertex buffer at 0x{:08X} (size {}) in "
            "shared memory",
            buffer_offset, buffer_length);
        return false;
      }
      draw_shared_memory_reads_.emplace_back(buffer_offset, buffer_length);
    }
  }

  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t base_bytes = memexport_range.base_address_dwords << 2;
    if (!shared_memory_->RequestRange(base_bytes, memexport_range.size_bytes)) {
      XELOGE(
          "Metal: Failed to request memexport stream at 0x{:08X} (size {}) in "
          "shared memory",
          base_bytes, memexport_range.size_bytes);
      return false;
    }
  }

  return true;
}

void MetalCommandProcessor::OrderDrawSharedMemoryAccesses(
    const Shader* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    MTL::Buffer* index_buffer, uint64_t index_buffer_offset,
    uint64_t index_buffer_size, const RegisterFile& regs) {
  if (!render_encoder_shared_memory_hazards_.is_active()) {
    return;
  }
  for (const std::pair<uint32_t, uint32_t>& range : draw_shared_memory_reads_) {
    render_encoder_shared_memory_hazards_.Access(range.first, range.second,
                                                 false);
  }
  // Vertex fetches in the pixel shader aren't requested with the vertex
  // shader's ones, but they still read the buffer.
  if (pixel_shader) {
    const Shader::ConstantRegisterMap& constant_map_pixel =
        pixel_shader->constant_register_map();
    for (uint32_t i = 0;
         i < xe::countof(constant_map_pixel.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining =
          constant_map_pixel.vertex_fetch_bitmap[i];
      uint32_t j;
      while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining &= ~(uint32_t(1) << j);
        xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(i * 32 + j);
        render_encoder_shared_memory_hazards_.Access(
            xenos::CpuToGpu(vfetch.address << 2), vfetch.size << 2, false);
      }
    }
  }
  if (index_buffer && index_buffer == shared_memory_->GetBuffer() &&
      index_buffer_size) {
    render_encoder_shared_memory_hazards_.Access(
        uint32_t(index_buffer_offset),
        uint32_t(
            std::min(index_buffer_size, uint64_t(SharedMemory::kBufferSize))),
        false);
  }
  // Vertex shaders expanding primitives load the guest indices themselves.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    render_encoder_shared_memory_hazards_.Access(
        primitive_processing_result.guest_index_base,
        uint32_t(std::min(
            uint64_t(primitive_processing_result.guest_draw_vertex_count) *
                (regs.Get<reg::VGT_DRAW_INITIATOR>().index_size ==
                         xenos::IndexFormat::kInt32
                     ? sizeof(uint32_t)
                     : sizeof(uint16_t)),
            uint64_t(SharedMemory::kBufferSize))),
        false);
  }
  // Ordering the exports within the pass is up to the draw's own checks.
  for (const draw_util::MemExportRange& range : memexport_ranges_) {
    render_encoder_shared_memory_hazards_.Access(range.base_address_dwords << 2,
                                                 range.size_bytes, true);
  }
}

void MetalCommandProcessor::ComputeDrawViewportInfo(
    const RegisterFile& regs, const Shader* pixel_shader,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    draw_util::ViewportInfo& viewport_info_out) {
  constexpr uint32_t kViewportBoundsMax = 32767;
  bool convert_z_to_float24 = ::cvars::depth_float24_convert_in_pixel_shader;
  // ZPD segments can't mix scales. The resolved sample count is divided by one
  // scale area per segment, so a change splits the segment.
  // Metal has no in-shader counter path, so a segment never counts Total.
  UpdateQuerySegment(
      (texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1) *
          (texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1),
      false, draw_util::IsVIZSurveyDraw(regs));
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1,
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1,
      texture_cache_ ? texture_cache_->draw_resolution_scale_x_divisor()
                     : divisors::MagicDiv(1),
      texture_cache_ ? texture_cache_->draw_resolution_scale_y_divisor()
                     : divisors::MagicDiv(1),
      true, kViewportBoundsMax, kViewportBoundsMax, false,
      normalized_depth_control, convert_z_to_float24, true,
      pixel_shader && pixel_shader->writes_depth(), false);
  gviargs.SetupRegisterValues(regs, false);
  if (gviargs == previous_viewport_info_args_) {
    viewport_info_out = previous_viewport_info_;
  } else {
    draw_util::GetHostViewportInfo(&gviargs, viewport_info_out);
    previous_viewport_info_args_ = gviargs;
    previous_viewport_info_ = viewport_info_out;
  }
}

uint32_t MetalCommandProcessor::GetDrawScissorBottom() const {
  if (!register_file_) {
    return 0;
  }
  const RegisterFile& regs = *register_file_;
  if (!draw_util::IsRasterizationPotentiallyDone(
          regs, draw_util::IsPrimitivePolygonal(regs))) {
    return 0;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  return (scissor.offset[1] + scissor.extent[1]) * draw_resolution_scale_y;
}

void MetalCommandProcessor::ApplyViewportAndScissor(
    const RegisterFile& regs, const draw_util::ViewportInfo& viewport_info) {
  SCOPE_profile_cpu_f("gpu");
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  // Clamp scissor to actual render target bounds (Metal requires this).
  ClampScissorToBounds(scissor, render_encoder_width_, render_encoder_height_);
  // Draws that don't rasterize (memexport-only) still run the vertex shader,
  // but must not touch the retained attachments or contribute samples.
  if (!draw_util::IsRasterizationPotentiallyDone(
          regs, draw_util::IsPrimitivePolygonal(regs))) {
    scissor = {};
  }

  MTL::Viewport mtl_viewport;
  mtl_viewport.originX = static_cast<double>(viewport_info.xy_offset[0]);
  mtl_viewport.originY = static_cast<double>(viewport_info.xy_offset[1]);
  mtl_viewport.width = static_cast<double>(viewport_info.xy_extent[0]);
  mtl_viewport.height = static_cast<double>(viewport_info.xy_extent[1]);
  mtl_viewport.znear = viewport_info.z_min;
  mtl_viewport.zfar = viewport_info.z_max;
  if (!msl_viewport_valid_ ||
      std::memcmp(&msl_viewport_, &mtl_viewport, sizeof(mtl_viewport)) != 0) {
    current_render_encoder_->setViewport(mtl_viewport);
    msl_viewport_ = mtl_viewport;
    msl_viewport_valid_ = true;
  }

  MTL::ScissorRect mtl_scissor;
  mtl_scissor.x = scissor.offset[0];
  mtl_scissor.y = scissor.offset[1];
  mtl_scissor.width = scissor.extent[0];
  mtl_scissor.height = scissor.extent[1];
  if (!msl_scissor_valid_ || msl_scissor_.x != mtl_scissor.x ||
      msl_scissor_.y != mtl_scissor.y ||
      msl_scissor_.width != mtl_scissor.width ||
      msl_scissor_.height != mtl_scissor.height) {
    current_render_encoder_->setScissorRect(mtl_scissor);
    msl_scissor_ = mtl_scissor;
    msl_scissor_valid_ = true;
  }
}

bool MetalCommandProcessor::CanDeferEmptyDrawEncoder() {
  if (!UseDxilPath() || current_render_encoder_ || !render_target_cache_) {
    return false;
  }
  // This only derives a descriptor. Clear consumption remains in the final
  // BeginCommandBuffer, and any pending clear keeps the original ordering.
  auto* desc = render_target_cache_->GetRenderPassDescriptor(1, true);
  if (!desc) {
    return false;
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* a = desc->colorAttachments()->object(i);
    if (a->texture() && a->loadAction() == MTL::LoadActionClear) {
      return false;
    }
  }
  if (desc->depthAttachment()->texture() &&
      desc->depthAttachment()->loadAction() == MTL::LoadActionClear) {
    return false;
  }
  if (desc->stencilAttachment()->texture() &&
      desc->stencilAttachment()->loadAction() == MTL::LoadActionClear) {
    return false;
  }
  return true;
}

bool MetalCommandProcessor::PrepareDrawTextures(uint32_t used_texture_mask,
                                                const RegisterFile& regs) {
  SCOPE_profile_cpu_f("gpu");
  if (texture_cache_ && used_texture_mask &&
      texture_cache_->AnyUsedTextureRequestWorkPending(used_texture_mask)) {
    texture_cache_->RequestTextures(used_texture_mask);
  }
  return true;
}

void MetalCommandProcessor::UpdateGuestConstantCaches(
    const Shader* vertex_shader, const Shader* pixel_shader,
    const RegisterFile& regs) {
  SCOPE_profile_cpu_f("gpu");
  // SpirvShaderTranslator uses packed float constants like Vulkan, so a change
  // in which constants a shader uses changes the packing.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < 4; ++i) {
    if (msl_current_float_constant_map_vertex_[i] !=
        float_constant_map_vertex.float_bitmap[i]) {
      msl_current_float_constant_map_vertex_[i] =
          float_constant_map_vertex.float_bitmap[i];
      msl_float_constants_dirty_vertex_ = true;
    }
  }
  if (pixel_shader) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      if (msl_current_float_constant_map_pixel_[i] !=
          float_constant_map_pixel.float_bitmap[i]) {
        msl_current_float_constant_map_pixel_[i] =
            float_constant_map_pixel.float_bitmap[i];
        msl_float_constants_dirty_pixel_ = true;
      }
    }
  } else {
    for (uint32_t i = 0; i < 4; ++i) {
      if (msl_current_float_constant_map_pixel_[i] != 0) {
        msl_current_float_constant_map_pixel_[i] = 0;
        msl_float_constants_dirty_pixel_ = true;
      }
    }
  }

  auto rebuild_packed_float_constants =
      [&](std::array<uint8_t, kCbvSizeBytes>& dst, const Shader* shader,
          uint32_t regs_base) {
        // The IR path binds only the shader's declared packed constants. The
        // MSL path uploads the full block, including unused zero constants.
        uint32_t bound_bytes = kCbvSizeBytes;
        if (UseDxilPath()) {
          const uint32_t float_count =
              shader ? shader->constant_register_map().float_count : 0;
          bound_bytes = std::min(float_count * 16u, uint32_t(kCbvSizeBytes));
        }
        uint8_t* out = dst.data();
        uint8_t* const end = dst.data() + bound_bytes;
        if (shader && shader->constant_register_map().float_count) {
          const Shader::ConstantRegisterMap& map =
              shader->constant_register_map();
          for (uint32_t i = 0; i < 4; ++i) {
            uint64_t bits = map.float_bitmap[i];
            uint32_t constant_index;
            while (out != end && xe::bit_scan_forward(bits, &constant_index)) {
              // A run of used constants is contiguous in the registers and in
              // the packed buffer alike, so it is copied at once.
              uint32_t run_length;
              if (!xe::bit_scan_forward(~(bits >> constant_index),
                                        &run_length)) {
                run_length = 64;
              }
              run_length = std::min(
                  run_length, uint32_t(size_t(end - out) /
                                       (4 * sizeof(uint32_t))));
              bits = constant_index + run_length < 64
                         ? bits & (~uint64_t(0) << (constant_index + run_length))
                         : 0;
              std::memcpy(
                  out,
                  &regs.values[regs_base + (i << 8) + (constant_index << 2)],
                  run_length * 4 * sizeof(uint32_t));
              out += run_length * 4 * sizeof(uint32_t);
            }
          }
        }
        // Every gathered value replaces its previous contents. Only padding
        // visible to this shader needs clearing after the gather.
        std::memset(out, 0, size_t(end - out));
      };
  if (msl_float_constants_dirty_vertex_) {
    rebuild_packed_float_constants(msl_cached_float_constants_vertex_,
                                   vertex_shader,
                                   XE_GPU_REG_SHADER_CONSTANT_000_X);
    ++dxil_guest_constant_versions_[0];
    msl_float_constants_dirty_vertex_ = false;
  }
  if (msl_float_constants_dirty_pixel_) {
    rebuild_packed_float_constants(msl_cached_float_constants_pixel_,
                                   pixel_shader,
                                   XE_GPU_REG_SHADER_CONSTANT_256_X);
    ++dxil_guest_constant_versions_[1];
    msl_float_constants_dirty_pixel_ = false;
  }

  if (msl_bool_loop_constants_dirty_) {
    std::memcpy(msl_cached_bool_loop_constants_.data(),
                &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    ++dxil_guest_constant_versions_[2];
    msl_bool_loop_constants_dirty_ = false;
  }
  if (msl_fetch_constants_dirty_) {
    // The version names the contents, so writes that left them as they were
    // keep the uploaded copy.
    const uint32_t* fetch_constants =
        &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
    if (std::memcmp(msl_cached_fetch_constants_.data(), fetch_constants,
                    kFetchConstantsSize)) {
      std::memcpy(msl_cached_fetch_constants_.data(), fetch_constants,
                  kFetchConstantsSize);
      ++dxil_guest_constant_versions_[3];
    }
    msl_fetch_constants_dirty_ = false;
  }
}

bool MetalCommandProcessor::ResolveDrawIndexBuffer(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    Shader::HostVertexShaderType host_vertex_shader_type, bool tessellated,
    DrawIndexBuffer& index_buffer_out) {
  index_buffer_out = DrawIndexBuffer();
  // A tessellated draw feeds patches to the tessellator, so its host primitive
  // type is not one of the rasterizer topologies below.
  switch (tessellated ? xenos::PrimitiveType::kTriangleList
                      : primitive_processing_result.host_primitive_type) {
    case xenos::PrimitiveType::kPointList:
      index_buffer_out.primitive_type = MTL::PrimitiveTypePoint;
      break;
    case xenos::PrimitiveType::kLineList:
      index_buffer_out.primitive_type = MTL::PrimitiveTypeLine;
      break;
    case xenos::PrimitiveType::kLineStrip:
      index_buffer_out.primitive_type = MTL::PrimitiveTypeLineStrip;
      break;
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kRectangleList:
      index_buffer_out.primitive_type = MTL::PrimitiveTypeTriangle;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      index_buffer_out.primitive_type = MTL::PrimitiveTypeTriangleStrip;
      break;
    default:
      XELOGE("Metal: Unsupported host primitive type {}",
             uint32_t(primitive_processing_result.host_primitive_type));
      return false;
  }

  // PrimitiveProcessor::Process already requested the guest index range, before
  // the draw's render encoder was resumed. Requesting it again here could
  // encode an upload into the command buffer after the draw's state was bound.
  auto guest_index_range_in_bounds = [&](uint64_t index_base,
                                         uint32_t index_count,
                                         MTL::IndexType index_type) -> bool {
    uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                ? sizeof(uint16_t)
                                : sizeof(uint32_t);
    uint64_t index_length = uint64_t(index_count) * index_stride;
    return shared_memory_ && index_base <= SharedMemory::kBufferSize &&
           SharedMemory::kBufferSize - index_base >= index_length;
  };

  bool use_expansion_triangle_list_fallback = false;
  index_buffer_out.index_count =
      primitive_processing_result.host_draw_vertex_count;
  if ((host_vertex_shader_type ==
           Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
       host_vertex_shader_type ==
           Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) &&
      (primitive_processing_result.index_buffer_type ==
           PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto ||
       primitive_processing_result.index_buffer_type ==
           PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA)) {
    // Expansion strips normally rely on primitive restart separators. Keep a
    // Metal-local triangle-list fallback to avoid dependence on strip restart
    // behavior.
    uint32_t strip_index_count = index_buffer_out.index_count;
    uint32_t expanded_primitive_count =
        strip_index_count ? (strip_index_count + 1u) / 5u : 0u;
    index_buffer_out.index_count = expanded_primitive_count * 6u;
    index_buffer_out.primitive_type = MTL::PrimitiveTypeTriangle;
    use_expansion_triangle_list_fallback = true;
    static bool logged_expansion_triangle_list_fallback = false;
    if (!logged_expansion_triangle_list_fallback) {
      logged_expansion_triangle_list_fallback = true;
      XELOGW(
          "Metal: Using triangle-list fallback for VS primitive expansion "
          "draws to avoid strip-restart dependency");
    }
  }

  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    return true;
  }

  index_buffer_out.indexed = true;
  index_buffer_out.index_type =
      (primitive_processing_result.host_index_format ==
       xenos::IndexFormat::kInt16)
          ? MTL::IndexTypeUInt16
          : MTL::IndexTypeUInt32;
  switch (primitive_processing_result.index_buffer_type) {
    case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
      index_buffer_out.buffer =
          shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
      index_buffer_out.offset = primitive_processing_result.guest_index_base;
      if (!guest_index_range_in_bounds(index_buffer_out.offset,
                                       index_buffer_out.index_count,
                                       index_buffer_out.index_type)) {
        XELOGE("Metal: Failed to validate guest index buffer range");
        return false;
      }
      break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
      if (primitive_processor_) {
        index_buffer_out.buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle,
            index_buffer_out.offset);
      }
      break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
      if (primitive_processor_) {
        if (use_expansion_triangle_list_fallback) {
          index_buffer_out.buffer =
              primitive_processor_->GetExpansionTriangleListIndexBuffer();
          index_buffer_out.offset = 0;
          index_buffer_out.index_type = MTL::IndexTypeUInt32;
        } else {
          index_buffer_out.buffer =
              primitive_processor_->GetBuiltinIndexBuffer();
          index_buffer_out.offset =
              primitive_processing_result.host_index_buffer_handle;
        }
      }
      break;
    default:
      XELOGE("Metal: Unsupported index buffer type {}",
             uint32_t(primitive_processing_result.index_buffer_type));
      return false;
  }
  if (!index_buffer_out.buffer) {
    XELOGE("Metal: Index buffer is null");
    return false;
  }
  return true;
}

// ==========================================================================
// SPIRV-Cross (MSL) draw path
// ==========================================================================
bool MetalCommandProcessor::IssueDrawMsl(
    Shader* vertex_shader, Shader* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool primitive_polygonal, bool is_rasterization_done, bool memexport_used,
    uint32_t normalized_color_mask, const RegisterFile& regs) {
  SCOPE_profile_cpu_f("gpu");
  assert_not_null(vertex_shader);
  // Cast to MslShader for the SPIRV-Cross path.
  auto* msl_vertex_shader = static_cast<MslShader*>(vertex_shader);
  auto* msl_pixel_shader = static_cast<MslShader*>(pixel_shader);

  // Tessellation draws use Metal's native tessellation: tessellation factors
  // are computed on the CPU and the domain shader (TES) runs as the
  // post-tessellation vertex function.
  const bool is_tessellated = primitive_processing_result.IsTessellated();

  // Determine the host vertex shader type for geometry expansion.
  // The primitive processor has already set kPointListAsTriangleStrip or
  // kRectangleListAsTriangleStrip when VS expansion is needed (enabled by
  // setting point_sprites_supported_without_vs_expansion = false in the
  // primitive processor init when spirvcross is active).
  Shader::HostVertexShaderType host_vertex_shader_type =
      primitive_processing_result.host_vertex_shader_type;

  // Compute interpolator mask for shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask = 0;
  if (msl_pixel_shader) {
    interpolator_mask = msl_vertex_shader->writes_interpolators() &
                        msl_pixel_shader->GetInterpolatorInputMask(
                            regs.Get<reg::SQ_PROGRAM_CNTL>(),
                            regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos);
  }

  auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  // Compute SPIRV shader modifications.
  SpirvShaderTranslator::Modification vertex_shader_modification =
      GetCurrentSpirvVertexShaderModification(
          *msl_vertex_shader, host_vertex_shader_type, interpolator_mask,
          ps_param_gen_pos != UINT32_MAX);
  SpirvShaderTranslator::Modification pixel_shader_modification =
      msl_pixel_shader
          ? GetCurrentSpirvPixelShaderModification(
                *msl_pixel_shader, interpolator_mask, ps_param_gen_pos,
                normalized_depth_control, normalized_color_mask)
          : SpirvShaderTranslator::Modification(0);

  // Sanity: if a pixel shader writes color targets and any RT is enabled,
  // color_targets_used must be non-zero or fragments will produce no output.
  if (msl_pixel_shader && msl_pixel_shader->writes_color_targets() &&
      normalized_color_mask) {
    assert_not_zero(pixel_shader_modification.pixel.color_targets_used);
  }

  // Memory exports are guest-visible writes, which a skipped draw or the
  // placeholder (no pixel shader) loses, so a memexport draw waits for its own
  // shaders and pipeline, as in IssueDrawDxil. Counted draws also require the
  // guest's exact kills and alpha test.
  const bool exact_shaders_required = ExactShadersRequired(memexport_used);

  // Get or create shader translations. Both are asked for before either is
  // waited on, so their compiles overlap instead of taking a frame each.
  ShaderCompileStatus vertex_status = ShaderCompileStatus::kReady;
  ShaderCompileStatus pixel_status = ShaderCompileStatus::kReady;
  auto* vertex_translation =
      static_cast<MslShader::MslTranslation*>(GetOrCreateHostTranslation(
          *msl_vertex_shader, vertex_shader_modification.value,
          !exact_shaders_required && cvars::async_shader_skip_draws,
          &vertex_status));
  MslShader::MslTranslation* pixel_translation = nullptr;
  if (msl_pixel_shader) {
    pixel_translation =
        static_cast<MslShader::MslTranslation*>(GetOrCreateHostTranslation(
            *msl_pixel_shader, pixel_shader_modification.value,
            !exact_shaders_required &&
                (!is_tessellated || cvars::async_shader_skip_draws),
            &pixel_status));
  }
  if (vertex_status == ShaderCompileStatus::kFailed ||
      pixel_status == ShaderCompileStatus::kFailed) {
    return false;
  }
  // Nothing can stand in for the vertex shader, so its draws wait.
  if (vertex_status != ShaderCompileStatus::kReady) {
    LogShaderCompilePending(vertex_translation, "vertex");
    return true;
  }

  // Create or retrieve pipeline state.
  MTL::RenderPipelineState* pipeline = nullptr;
  bool bound_placeholder = false;
  PipelineCompileStatus pipeline_compile_status = PipelineCompileStatus::kReady;
  if (is_tessellated) {
    if (pixel_status != ShaderCompileStatus::kReady) {
      LogShaderCompilePending(pixel_translation, "pixel");
      return true;
    }
    pipeline = GetOrCreateMslTessPipelineState(
        vertex_translation, pixel_translation, host_vertex_shader_type, regs,
        &pipeline_compile_status);
  } else if (pixel_status == ShaderCompileStatus::kReady) {
    pipeline = GetOrCreatePipelineState(vertex_translation, pixel_translation,
                                        regs, &pipeline_compile_status);
  }
  if (!pipeline && exact_shaders_required &&
      pipeline_compile_status == PipelineCompileStatus::kPending) {
    AwaitPendingPipeline();
    pipeline =
        is_tessellated
            ? GetOrCreateMslTessPipelineState(
                  vertex_translation, pixel_translation,
                  host_vertex_shader_type, regs, &pipeline_compile_status)
            : GetOrCreatePipelineState(vertex_translation, pixel_translation,
                                       regs, &pipeline_compile_status);
  }
  if (!pipeline && !is_tessellated && !exact_shaders_required &&
      (pixel_status != ShaderCompileStatus::kReady ||
       pipeline_compile_status == PipelineCompileStatus::kPending)) {
    pipeline = GetOrCreatePlaceholderPipelineState(vertex_translation, regs);
    bound_placeholder = pipeline != nullptr;
  }
  if (!pipeline) {
    if (pipeline_compile_status == PipelineCompileStatus::kPending ||
        pixel_status != ShaderCompileStatus::kReady) {
      LogPipelineCompilePending(vertex_translation, pixel_translation);
      return true;
    }
    return false;
  }
  // The placeholder has no fragment function, so the pixel stage's resources
  // must not be bound to it.
  MslShader* bind_pixel_shader = bound_placeholder ? nullptr : msl_pixel_shader;
  MslShader::MslTranslation* bind_pixel_translation =
      bound_placeholder ? nullptr : pixel_translation;
  if (bound_placeholder) {
    LogPlaceholderDraw(vertex_translation, pixel_translation);
  }

  // Request textures used by the shaders. A placeholder samples none of the
  // pixel shader's, and their binding list may still be being filled.
  uint32_t used_texture_mask =
      msl_vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (bind_pixel_shader) {
    used_texture_mask |=
        bind_pixel_shader->GetUsedTextureMaskAfterTranslation();
  }

  if (!PrepareDrawTextures(used_texture_mask, regs)) {
    return true;
  }
  if (texture_cache_) {
    texture_cache_->Load3DAs2DViews(*msl_vertex_shader, bind_pixel_shader);
  }

  if (!RequestDrawSharedMemoryRanges(*msl_vertex_shader, regs)) {
    return false;
  }
  // A texture load may have ended the render encoder to be ordered after the
  // earlier draws in the command buffer.
  BeginCommandBuffer();
  if (!current_render_encoder_) {
    XELOGE("SPIRV-Cross: failed to resume render encoder after uploads");
    return false;
  }
  if (render_encoder_shared_memory_hazards_.is_active()) {
    // Tessellation may read control points without a host index buffer.
    render_encoder_shared_memory_hazards_.Access(0, SharedMemory::kBufferSize,
                                                 memexport_used);
  }

  draw_util::ViewportInfo viewport_info;
  ComputeDrawViewportInfo(regs, msl_pixel_shader, normalized_depth_control,
                          viewport_info);
  ApplyViewportAndScissor(regs, viewport_info);

  // Apply fixed-function state.
  if (msl_bound_pipeline_state_ != pipeline) {
    current_render_encoder_->setRenderPipelineState(pipeline);
    msl_bound_pipeline_state_ = pipeline;
  }
  ApplyRasterizerState(primitive_polygonal);
  ApplyDepthStencilState(primitive_polygonal, normalized_depth_control);

  // Update SPIRV system constants.
  UpdateSpirvSystemConstantValues(
      primitive_processing_result, primitive_polygonal,
      primitive_processing_result.line_loop_closing_index,
      primitive_processing_result.host_shader_index_endian, viewport_info,
      used_texture_mask, normalized_depth_control, normalized_color_mask);

  // Blend constants (fixed-function, same as MSC path).
  float blend_constants[] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  if (!ff_blend_factor_valid_ ||
      std::memcmp(ff_blend_factor_, blend_constants, sizeof(float) * 4) != 0) {
    std::memcpy(ff_blend_factor_, blend_constants, sizeof(float) * 4);
    ff_blend_factor_valid_ = true;
    current_render_encoder_->setBlendColor(
        blend_constants[0], blend_constants[1], blend_constants[2],
        blend_constants[3]);
  }

  // =====================================================================
  // Resource binding — direct Metal encoder calls, no IRDescriptorTable.
  // =====================================================================
  constexpr uint32_t kCBVSize = MslBufferIndex::kCbvSizeBytes;
  uint32_t ring_index = current_draw_index_ % uint32_t(draw_ring_count_);
  constexpr size_t kStageVertex = 0;
  constexpr size_t kStagePixel = 1;
  size_t table_index_vertex = size_t(ring_index) * kStageCount + kStageVertex;
  size_t table_index_pixel = size_t(ring_index) * kStageCount + kStagePixel;

  uint8_t* uniforms_base = static_cast<uint8_t*>(uniforms_buffer_->contents());
  uint8_t* uniforms_vertex =
      uniforms_base + table_index_vertex * kUniformsBytesPerTable;
  uint8_t* uniforms_pixel =
      uniforms_base + table_index_pixel * kUniformsBytesPerTable;
  if (msl_constants_versioned_uniform_buffer_ != uniforms_buffer_) {
    msl_constants_versioned_uniform_buffer_ = uniforms_buffer_;
    std::fill(msl_system_constants_written_vertex_versions_.begin(),
              msl_system_constants_written_vertex_versions_.end(), uint64_t(0));
    std::fill(msl_system_constants_written_pixel_versions_.begin(),
              msl_system_constants_written_pixel_versions_.end(), uint64_t(0));
  }
  auto ensure_uniform_versions_size = [&](std::vector<uint64_t>& versions) {
    if (versions.size() != draw_ring_count_) {
      versions.assign(draw_ring_count_, 0);
    }
  };
  ensure_uniform_versions_size(msl_system_constants_written_vertex_versions_);
  ensure_uniform_versions_size(msl_system_constants_written_pixel_versions_);
  const size_t ring_index_size = size_t(ring_index);
  auto copy_uniform_block_if_stale =
      [&](uint8_t* dst, const void* src, size_t size,
          std::vector<uint64_t>& written_versions, uint64_t source_version) {
        if (ring_index_size >= written_versions.size()) {
          return;
        }
        if (written_versions[ring_index_size] != source_version) {
          std::memcpy(dst, src, size);
          written_versions[ring_index_size] = source_version;
        }
      };

  // b0 (msl_buffer 1): System constants.
  copy_uniform_block_if_stale(uniforms_vertex, &spirv_system_constants_,
                              sizeof(SpirvShaderTranslator::SystemConstants),
                              msl_system_constants_written_vertex_versions_,
                              msl_system_constants_version_);
  copy_uniform_block_if_stale(uniforms_pixel, &spirv_system_constants_,
                              sizeof(SpirvShaderTranslator::SystemConstants),
                              msl_system_constants_written_pixel_versions_,
                              msl_system_constants_version_);

  UpdateGuestConstantCaches(msl_vertex_shader, msl_pixel_shader, regs);

  // b1 (msl_buffer 2/3): Float constants.
  const size_t kFloatConstantOffset = 1 * kCBVSize;
  std::memcpy(uniforms_vertex + kFloatConstantOffset,
              msl_cached_float_constants_vertex_.data(), kCBVSize);
  std::memcpy(uniforms_pixel + kFloatConstantOffset,
              msl_cached_float_constants_pixel_.data(), kCBVSize);

  // b2 (msl_buffer 4): Bool/loop constants.
  const size_t kBoolLoopConstantOffset = 2 * kCBVSize;
  std::memcpy(uniforms_vertex + kBoolLoopConstantOffset,
              msl_cached_bool_loop_constants_.data(), kBoolLoopConstantsSize);
  std::memcpy(uniforms_pixel + kBoolLoopConstantOffset,
              msl_cached_bool_loop_constants_.data(), kBoolLoopConstantsSize);

  // b3 (msl_buffer 5): Fetch constants.
  const size_t kFetchConstantOffset = 3 * kCBVSize;
  std::memcpy(uniforms_vertex + kFetchConstantOffset,
              msl_cached_fetch_constants_.data(), kFetchConstantsSize);
  std::memcpy(uniforms_pixel + kFetchConstantOffset,
              msl_cached_fetch_constants_.data(), kFetchConstantsSize);

  // Keep binding behavior conservative while using per-encoder dedupe caches.
  const bool msl_bind_dedupe = true;

  // Bind shared memory buffer at msl_buffer 0.
  MTL::Buffer* shared_mem_buffer =
      shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
  MTL::ResourceUsage shared_memory_usage = MTL::ResourceUsageRead;
  if (memexport_used) {
    shared_memory_usage |= MTL::ResourceUsageWrite;
  }
  if (!msl_bind_dedupe ||
      msl_bound_shared_memory_buffer_ != shared_mem_buffer) {
    current_render_encoder_->setVertexBuffer(shared_mem_buffer, 0,
                                             MslBufferIndex::kSharedMemory);
    current_render_encoder_->setFragmentBuffer(shared_mem_buffer, 0,
                                               MslBufferIndex::kSharedMemory);
    msl_bound_shared_memory_buffer_ = shared_mem_buffer;
  }
  if (shared_mem_buffer) {
    UseRenderEncoderResource(shared_mem_buffer, shared_memory_usage);
  }

  // Bind a null buffer at the EDRAM slot (msl_buffer 30) as a safety measure.
  // FSI/EDRAM is disabled on this path (fragment_shader_sample_interlock =
  // false), so no shader should reference it, but binding a dummy prevents
  // GPU faults if any code path unexpectedly accesses buffer(30).
  if (!msl_bind_dedupe || msl_bound_null_buffer_ != null_buffer_) {
    current_render_encoder_->setFragmentBuffer(null_buffer_, 0, 30);
    msl_bound_null_buffer_ = null_buffer_;
  }

  // Bind uniforms buffer at the appropriate indices.
  NS::UInteger vs_base_offset = table_index_vertex * kUniformsBytesPerTable;
  NS::UInteger ps_base_offset = table_index_pixel * kUniformsBytesPerTable;
  if (!msl_bind_dedupe || !msl_bound_uniforms_offsets_valid_ ||
      msl_bound_uniforms_buffer_ != uniforms_buffer_ ||
      msl_bound_uniforms_vs_base_offset_ != vs_base_offset ||
      msl_bound_uniforms_ps_base_offset_ != ps_base_offset) {
    // System constants (msl_buffer 1).
    current_render_encoder_->setVertexBuffer(uniforms_buffer_,
                                             vs_base_offset + 0 * kCBVSize,
                                             MslBufferIndex::kSystemConstants);
    current_render_encoder_->setFragmentBuffer(
        uniforms_buffer_, ps_base_offset + 0 * kCBVSize,
        MslBufferIndex::kSystemConstants);

    // Float constants vertex (msl_buffer 2).
    current_render_encoder_->setVertexBuffer(
        uniforms_buffer_, vs_base_offset + 1 * kCBVSize,
        MslBufferIndex::kFloatConstantsVertex);
    // Float constants pixel (msl_buffer 3).
    current_render_encoder_->setFragmentBuffer(
        uniforms_buffer_, ps_base_offset + 1 * kCBVSize,
        MslBufferIndex::kFloatConstantsPixel);

    // Bool/loop constants (msl_buffer 4).
    current_render_encoder_->setVertexBuffer(
        uniforms_buffer_, vs_base_offset + 2 * kCBVSize,
        MslBufferIndex::kBoolLoopConstants);
    current_render_encoder_->setFragmentBuffer(
        uniforms_buffer_, ps_base_offset + 2 * kCBVSize,
        MslBufferIndex::kBoolLoopConstants);

    // Fetch constants (msl_buffer 5).
    current_render_encoder_->setVertexBuffer(uniforms_buffer_,
                                             vs_base_offset + 3 * kCBVSize,
                                             MslBufferIndex::kFetchConstants);
    current_render_encoder_->setFragmentBuffer(uniforms_buffer_,
                                               ps_base_offset + 3 * kCBVSize,
                                               MslBufferIndex::kFetchConstants);

    msl_bound_uniforms_buffer_ = uniforms_buffer_;
    msl_bound_uniforms_vs_base_offset_ = vs_base_offset;
    msl_bound_uniforms_ps_base_offset_ = ps_base_offset;
    msl_bound_uniforms_offsets_valid_ = true;
  }

  UseRenderEncoderResource(uniforms_buffer_, MTL::ResourceUsageRead);

  const bool vertex_uses_argbuf =
      vertex_translation && vertex_translation->uses_argument_buffers();
  const bool pixel_uses_argbuf =
      bind_pixel_translation && bind_pixel_translation->uses_argument_buffers();
  auto get_msl_binding_layout_uid =
      [](const MslShader::MslTranslation* translation) -> uint64_t {
    if (!translation) {
      return 0;
    }
    const auto& texture_binding_indices =
        translation->texture_binding_indices_for_msl_slots();
    const auto& sampler_binding_indices =
        translation->sampler_binding_indices_for_msl_slots();
    uint64_t uid = XXH3_64bits(
        texture_binding_indices.data(),
        texture_binding_indices.size() * sizeof(texture_binding_indices[0]));
    uid = XXH3_64bits_withSeed(
        sampler_binding_indices.data(),
        sampler_binding_indices.size() * sizeof(sampler_binding_indices[0]),
        uid);
    return uid;
  };

  auto bind_msl_argument_buffer = [&](MslShader* shader,
                                      MslShader::MslTranslation* translation,
                                      bool is_pixel_stage) -> bool {
    if (!shader || !translation || !translation->uses_argument_buffers() ||
        !texture_cache_) {
      return true;
    }

    MTL::ArgumentEncoder* arg_encoder = translation->argument_encoder();
    uint32_t encoded_length = translation->argument_encoder_encoded_length();
    if (!arg_encoder || encoded_length == 0) {
      return true;
    }

    const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
    const auto& texture_binding_indices =
        translation->texture_binding_indices_for_msl_slots();
    uint32_t texture_count = std::min(uint32_t(texture_binding_indices.size()),
                                      MslTextureIndex::kMaxPerStage);
    std::array<const MTL::Texture*, MslTextureIndex::kMaxPerStage> textures =
        {};

    MetalTextureCache* metal_texture_cache = texture_cache_.get();
    for (uint32_t slot = 0; slot < texture_count; ++slot) {
      MTL::Texture* texture = nullptr;
      int32_t texture_binding_index = texture_binding_indices[slot];
      if (texture_binding_index >= 0 &&
          size_t(texture_binding_index) < texture_bindings.size()) {
        const auto& binding = texture_bindings[size_t(texture_binding_index)];
        texture = texture_cache_->GetTextureForBinding(
            binding.fetch_constant, binding.dimension, binding.is_signed);
        if (!texture) {
          switch (binding.dimension) {
            case xenos::FetchOpDimension::k3DOrStacked:
              texture = metal_texture_cache->GetNullTexture3D();
              break;
            case xenos::FetchOpDimension::kCube:
              texture = metal_texture_cache->GetNullTextureCube();
              break;
            default:
              texture = metal_texture_cache->GetNullTexture2D();
              break;
          }
        }
      } else {
        texture = metal_texture_cache->GetNullTexture2D();
      }
      textures[slot] = texture;
      // UseRenderEncoderResource must always be called for hazard tracking,
      // even when we skip re-encoding. The dedup map handles per-encoder
      // deduplication.
      if (texture) {
        UseRenderEncoderResource(texture, MTL::ResourceUsageRead);
      }
    }

    const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
    const auto& sampler_binding_indices =
        translation->sampler_binding_indices_for_msl_slots();
    uint32_t sampler_count = std::min(uint32_t(sampler_binding_indices.size()),
                                      MslSamplerIndex::kMaxPerStage);
    std::array<const MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
        samplers = {};
    for (uint32_t smp_index = 0; smp_index < sampler_count; ++smp_index) {
      MTL::SamplerState* sampler_state = null_sampler_;
      uint32_t sampler_binding_index = sampler_binding_indices[smp_index];
      if (sampler_binding_index < sampler_bindings.size()) {
        auto parameters = texture_cache_->GetSamplerParameters(
            sampler_bindings[sampler_binding_index]);
        sampler_state = texture_cache_->GetOrCreateSampler(parameters);
        if (!sampler_state) {
          sampler_state = null_sampler_;
        }
      }
      samplers[smp_index] = sampler_state;
    }

    // Check if textures and samplers match the cached content from the last
    // encoding. If so, skip the expensive acquire+encode and reuse the previous
    // argument buffer slice.
    auto& cached_textures = is_pixel_stage ? msl_last_argbuf_pixel_textures_
                                           : msl_last_argbuf_vertex_textures_;
    auto& cached_texture_count = is_pixel_stage
                                     ? msl_last_argbuf_pixel_texture_count_
                                     : msl_last_argbuf_vertex_texture_count_;
    auto& cached_samplers = is_pixel_stage ? msl_last_argbuf_pixel_samplers_
                                           : msl_last_argbuf_vertex_samplers_;
    auto& cached_sampler_count = is_pixel_stage
                                     ? msl_last_argbuf_pixel_sampler_count_
                                     : msl_last_argbuf_vertex_sampler_count_;
    auto& cached_argbuf_buffer = is_pixel_stage
                                     ? msl_last_argbuf_pixel_buffer_
                                     : msl_last_argbuf_vertex_buffer_;
    auto& cached_argbuf_offset = is_pixel_stage
                                     ? msl_last_argbuf_pixel_offset_
                                     : msl_last_argbuf_vertex_offset_;
    auto& cached_translation = is_pixel_stage
                                   ? msl_last_argbuf_pixel_translation_
                                   : msl_last_argbuf_vertex_translation_;
    auto& cached_encoded_length = is_pixel_stage
                                      ? msl_last_argbuf_pixel_encoded_length_
                                      : msl_last_argbuf_vertex_encoded_length_;
    auto& cached_layout_uid = is_pixel_stage
                                  ? msl_last_argbuf_pixel_layout_uid_
                                  : msl_last_argbuf_vertex_layout_uid_;
    const uint64_t layout_uid = get_msl_binding_layout_uid(translation);

    bool content_changed = cached_translation != translation ||
                           cached_encoded_length != encoded_length ||
                           layout_uid != cached_layout_uid ||
                           texture_count != cached_texture_count ||
                           sampler_count != cached_sampler_count ||
                           !cached_argbuf_buffer;
    if (!content_changed && texture_count > 0) {
      content_changed = std::memcmp(textures.data(), cached_textures.data(),
                                    texture_count * sizeof(textures[0])) != 0;
    }
    if (!content_changed && sampler_count > 0) {
      content_changed = std::memcmp(samplers.data(), cached_samplers.data(),
                                    sampler_count * sizeof(samplers[0])) != 0;
    }

    MTL::Buffer* argbuf_buffer;
    NS::UInteger argbuf_offset;

    if (content_changed) {
      // Content changed — acquire a new slice and re-encode.
      argbuf_buffer = nullptr;
      argbuf_offset = 0;
      if (!AcquireSpirvArgumentBufferSlice(
              encoded_length, translation->argument_encoder_alignment(),
              &argbuf_buffer, &argbuf_offset)) {
        XELOGE(
            "SPIRV-Cross: Failed to allocate argument buffer slice ({} bytes)",
            encoded_length);
        return false;
      }

      arg_encoder->setArgumentBuffer(argbuf_buffer, argbuf_offset);
      if (texture_count) {
        arg_encoder->setTextures(
            textures.data(),
            NS::Range::Make(MslTextureIndex::kBase, texture_count));
      }
      if (sampler_count) {
        arg_encoder->setSamplerStates(
            samplers.data(),
            NS::Range::Make(MslArgumentBufferId::kSamplerBase, sampler_count));
      }

      // Update the cache.
      std::memcpy(cached_textures.data(), textures.data(),
                  texture_count * sizeof(textures[0]));
      if (texture_count < cached_texture_count) {
        std::memset(&cached_textures[texture_count], 0,
                    (cached_texture_count - texture_count) *
                        sizeof(cached_textures[0]));
      }
      cached_texture_count = texture_count;
      std::memcpy(cached_samplers.data(), samplers.data(),
                  sampler_count * sizeof(samplers[0]));
      if (sampler_count < cached_sampler_count) {
        std::memset(&cached_samplers[sampler_count], 0,
                    (cached_sampler_count - sampler_count) *
                        sizeof(cached_samplers[0]));
      }
      cached_sampler_count = sampler_count;
      cached_argbuf_buffer = argbuf_buffer;
      cached_argbuf_offset = argbuf_offset;
      cached_translation = translation;
      cached_encoded_length = encoded_length;
      cached_layout_uid = layout_uid;
    } else {
      // Content unchanged — reuse the previous argument buffer slice.
      argbuf_buffer = cached_argbuf_buffer;
      argbuf_offset = cached_argbuf_offset;
    }

    if (is_pixel_stage) {
      if (argbuf_buffer != msl_bound_pixel_argument_buffer_) {
        current_render_encoder_->setFragmentBuffer(
            argbuf_buffer, 0, MslBufferIndex::kArgumentBufferTexturesSamplers);
        msl_bound_pixel_argument_buffer_ = argbuf_buffer;
        msl_bound_pixel_argument_buffer_offset_valid_ = false;
      }
      if (!msl_bound_pixel_argument_buffer_offset_valid_ ||
          msl_bound_pixel_argument_buffer_offset_ != argbuf_offset) {
        current_render_encoder_->setFragmentBufferOffset(
            argbuf_offset, MslBufferIndex::kArgumentBufferTexturesSamplers);
        msl_bound_pixel_argument_buffer_offset_ = argbuf_offset;
        msl_bound_pixel_argument_buffer_offset_valid_ = true;
      }
    } else {
      if (argbuf_buffer != msl_bound_vertex_argument_buffer_) {
        current_render_encoder_->setVertexBuffer(
            argbuf_buffer, 0, MslBufferIndex::kArgumentBufferTexturesSamplers);
        msl_bound_vertex_argument_buffer_ = argbuf_buffer;
        msl_bound_vertex_argument_buffer_offset_valid_ = false;
      }
      if (!msl_bound_vertex_argument_buffer_offset_valid_ ||
          msl_bound_vertex_argument_buffer_offset_ != argbuf_offset) {
        current_render_encoder_->setVertexBufferOffset(
            argbuf_offset, MslBufferIndex::kArgumentBufferTexturesSamplers);
        msl_bound_vertex_argument_buffer_offset_ = argbuf_offset;
        msl_bound_vertex_argument_buffer_offset_valid_ = true;
      }
    }

    UseRenderEncoderResource(argbuf_buffer, MTL::ResourceUsageRead);
    return true;
  };
  if (vertex_uses_argbuf &&
      !bind_msl_argument_buffer(msl_vertex_shader, vertex_translation, false)) {
    return false;
  }
  if (pixel_uses_argbuf &&
      !bind_msl_argument_buffer(bind_pixel_shader, bind_pixel_translation,
                                true)) {
    return false;
  }

  // Bind textures and samplers directly.
  auto bind_msl_textures = [&](MslShader* shader,
                               MslShader::MslTranslation* translation,
                               bool is_pixel_stage) {
    auto bind_texture_slot = [&](uint32_t slot, MTL::Texture* texture) {
      if (is_pixel_stage) {
        current_render_encoder_->setFragmentTexture(texture, slot);
      } else {
        current_render_encoder_->setVertexTexture(texture, slot);
      }
    };
    uint32_t* previous_bound_count = is_pixel_stage
                                         ? &msl_bound_pixel_texture_count_
                                         : &msl_bound_vertex_texture_count_;
    uint64_t* cached_binding_uid = is_pixel_stage
                                       ? &msl_bound_pixel_texture_binding_uid_
                                       : &msl_bound_vertex_texture_binding_uid_;
    auto* bound_textures = is_pixel_stage ? &msl_bound_pixel_textures_
                                          : &msl_bound_vertex_textures_;
    const uint64_t layout_uid = get_msl_binding_layout_uid(translation);
    const uint64_t binding_uid =
        (shader && translation && texture_cache_)
            ? (uint64_t(reinterpret_cast<uintptr_t>(pipeline)) *
                   UINT64_C(11400714819323198485) ^
               layout_uid)
            : 0;
    const bool force_rebind = *cached_binding_uid != binding_uid;
    auto clear_slots_from = [&](uint32_t start, uint32_t end_exclusive) {
      for (uint32_t slot = start; slot < end_exclusive; ++slot) {
        if (force_rebind || (*bound_textures)[slot] != nullptr) {
          bind_texture_slot(slot, nullptr);
          (*bound_textures)[slot] = nullptr;
        }
      }
    };

    if (!shader || !translation || !texture_cache_) {
      clear_slots_from(0, *previous_bound_count);
      *previous_bound_count = 0;
      *cached_binding_uid = binding_uid;
      return;
    }

    const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
    const auto& texture_binding_indices =
        translation->texture_binding_indices_for_msl_slots();
    uint32_t bound_count = std::min(uint32_t(texture_binding_indices.size()),
                                    MslTextureIndex::kMaxPerStage);
    if (*previous_bound_count > bound_count) {
      clear_slots_from(bound_count, *previous_bound_count);
    }

    MetalTextureCache* metal_texture_cache = texture_cache_.get();
    for (uint32_t slot = 0; slot < bound_count; ++slot) {
      uint32_t tex_index = MslTextureIndex::kBase + slot;
      MTL::Texture* texture = nullptr;
      int32_t texture_binding_index = texture_binding_indices[slot];
      if (texture_binding_index >= 0 &&
          size_t(texture_binding_index) < texture_bindings.size()) {
        const auto& binding = texture_bindings[size_t(texture_binding_index)];
        texture = texture_cache_->GetTextureForBinding(
            binding.fetch_constant, binding.dimension, binding.is_signed);
        if (!texture) {
          switch (binding.dimension) {
            case xenos::FetchOpDimension::k3DOrStacked:
              texture = metal_texture_cache->GetNullTexture3D();
              break;
            case xenos::FetchOpDimension::kCube:
              texture = metal_texture_cache->GetNullTextureCube();
              break;
            default:
              texture = metal_texture_cache->GetNullTexture2D();
              break;
          }
        }
      } else {
        texture = metal_texture_cache->GetNullTexture2D();
      }
      if (force_rebind || (*bound_textures)[tex_index] != texture) {
        bind_texture_slot(tex_index, texture);
        (*bound_textures)[tex_index] = texture;
      }
      if (texture) {
        UseRenderEncoderResource(texture, MTL::ResourceUsageRead);
      }
    }
    *previous_bound_count = bound_count;
    *cached_binding_uid = binding_uid;
  };

  auto bind_msl_samplers = [&](MslShader* shader,
                               MslShader::MslTranslation* translation,
                               bool is_pixel_stage) {
    auto bind_sampler_slot = [&](uint32_t slot, MTL::SamplerState* sampler) {
      if (is_pixel_stage) {
        current_render_encoder_->setFragmentSamplerState(sampler, slot);
      } else {
        current_render_encoder_->setVertexSamplerState(sampler, slot);
      }
    };

    uint32_t* previous_bound_count = is_pixel_stage
                                         ? &msl_bound_pixel_sampler_count_
                                         : &msl_bound_vertex_sampler_count_;
    uint64_t* cached_binding_uid = is_pixel_stage
                                       ? &msl_bound_pixel_sampler_binding_uid_
                                       : &msl_bound_vertex_sampler_binding_uid_;
    auto* bound_samplers = is_pixel_stage ? &msl_bound_pixel_samplers_
                                          : &msl_bound_vertex_samplers_;
    const uint64_t layout_uid = get_msl_binding_layout_uid(translation);
    const uint64_t binding_uid =
        (shader && translation && texture_cache_)
            ? (uint64_t(reinterpret_cast<uintptr_t>(pipeline)) *
                   UINT64_C(11400714819323198485) ^
               layout_uid)
            : 0;
    const bool force_rebind = *cached_binding_uid != binding_uid;

    if (!shader || !translation || !texture_cache_) {
      for (uint32_t slot = 0; slot < *previous_bound_count; ++slot) {
        if (force_rebind || (*bound_samplers)[slot] != null_sampler_) {
          bind_sampler_slot(slot, null_sampler_);
          (*bound_samplers)[slot] = null_sampler_;
        }
      }
      *previous_bound_count = 0;
      *cached_binding_uid = binding_uid;
      return;
    }
    // Samplers are remapped to compact Metal indices 0..M-1 in
    // MslShader::AddResourceBindings. Use the reflected SPIR-V remap order
    // captured in the translation, not the raw translator array order, because
    // SPIRV-Cross may drop/reorder separate samplers.
    const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
    const auto& sampler_binding_indices =
        translation->sampler_binding_indices_for_msl_slots();
    uint32_t bound_count = std::min(uint32_t(sampler_binding_indices.size()),
                                    MslSamplerIndex::kMaxPerStage);
    if (*previous_bound_count > bound_count) {
      for (uint32_t slot = bound_count; slot < *previous_bound_count; ++slot) {
        if (force_rebind || (*bound_samplers)[slot] != null_sampler_) {
          bind_sampler_slot(slot, null_sampler_);
          (*bound_samplers)[slot] = null_sampler_;
        }
      }
    }

    for (uint32_t smp_index = 0; smp_index < bound_count; ++smp_index) {
      MTL::SamplerState* sampler_state = null_sampler_;
      uint32_t sampler_binding_index = sampler_binding_indices[smp_index];
      if (sampler_binding_index < sampler_bindings.size()) {
        auto parameters = texture_cache_->GetSamplerParameters(
            sampler_bindings[sampler_binding_index]);
        sampler_state = texture_cache_->GetOrCreateSampler(parameters);
        if (!sampler_state) {
          sampler_state = null_sampler_;
        }
      }
      if (force_rebind || (*bound_samplers)[smp_index] != sampler_state) {
        bind_sampler_slot(smp_index, sampler_state);
        (*bound_samplers)[smp_index] = sampler_state;
      }
    }
    *previous_bound_count = bound_count;
    *cached_binding_uid = binding_uid;
  };

  if (!vertex_uses_argbuf) {
    bind_msl_textures(msl_vertex_shader, vertex_translation, false);
    bind_msl_samplers(msl_vertex_shader, vertex_translation, false);
  }
  if (!pixel_uses_argbuf) {
    bind_msl_textures(bind_pixel_shader, bind_pixel_translation, true);
    bind_msl_samplers(bind_pixel_shader, bind_pixel_translation, true);
  }

  // Resume a ZPD segment waiting on a render encoder so this draw is counted.
  OpenQuerySegment(false);

  // =====================================================================
  // Draw dispatch — native Metal encoder calls (no IRRuntime).
  // =====================================================================
  if (is_tessellated) {
    // ---------------------------------------------------------------
    // Tessellated draw: fill tessellation factor buffer, then drawPatches.
    // ---------------------------------------------------------------
    // Xenos tess levels are 0-based; add 1 to match other backends.
    float max_tess = std::max(
        1.0f, regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f);

    // Determine control points per patch and patch count from the draw.
    // The primitive processor passes patch count in host_draw_vertex_count
    // for tessellated draws (vertex count = cp_per_patch * patch_count).
    uint32_t cp_per_patch = 1;
    bool is_quad_domain = false;
    switch (host_vertex_shader_type) {
      case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
        cp_per_patch = 3;
        break;
      case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
        cp_per_patch = 4;
        is_quad_domain = true;
        break;
      case Shader::HostVertexShaderType::kLineDomainCPIndexed:
      case Shader::HostVertexShaderType::kLineDomainPatchIndexed:
        cp_per_patch = 2;
        break;
      default:
        break;
    }
    uint32_t vertex_count = primitive_processing_result.host_draw_vertex_count;
    uint32_t patch_count = cp_per_patch > 0 ? vertex_count / cp_per_patch : 0;
    if (patch_count == 0) {
      return true;  // Nothing to draw.
    }

    // Ensure tessellation factor buffer is large enough.
    if (!EnsureTessFactorBuffer(patch_count)) {
      XELOGE(
          "SPIRV-Cross: Failed to allocate tess factor buffer for {} "
          "patches",
          patch_count);
      return false;
    }

    // IEEE 754 float32 → float16 conversion for tessellation factors.
    // Uses round-to-nearest-even and handles subnormals for accuracy
    // near tessellation factor boundaries.
    auto f32_to_f16 = [](float v) -> uint16_t {
      uint32_t b;
      std::memcpy(&b, &v, 4);
      uint16_t s = (b >> 16) & 0x8000u;
      int e = int((b >> 23) & 0xFFu) - 127 + 15;
      uint32_t m = b & 0x7FFFFFu;
      if (e <= 0) {
        // Subnormal or zero in half precision.
        if (e < -10) {
          return s;  // Too small, flush to signed zero.
        }
        // Subnormal half: shift mantissa (with implicit leading 1) right.
        m = (m | 0x800000u) >> (1 - e);
        // Round to nearest even.
        if ((m & 0x1FFFu) > 0x1000u ||
            ((m & 0x1FFFu) == 0x1000u && (m & 0x2000u))) {
          m += 0x2000u;
        }
        return uint16_t(s | (m >> 13));
      }
      if (e >= 31) {
        // Overflow → infinity (or NaN passthrough).
        if (e == 31 && m != 0) {
          // NaN: preserve at least one mantissa bit.
          return uint16_t(s | 0x7C00u | std::max(m >> 13, uint32_t(1)));
        }
        return uint16_t(s | 0x7C00u);
      }
      // Round to nearest even: check the 13 bits being truncated.
      if ((m & 0x1FFFu) > 0x1000u ||
          ((m & 0x1FFFu) == 0x1000u && (m & 0x2000u))) {
        m += 0x2000u;
        if (m & 0x800000u) {
          m = 0;
          e++;
          if (e >= 31) {
            return uint16_t(s | 0x7C00u);
          }
        }
      }
      return uint16_t(s | (e << 10) | (m >> 13));
    };

    // Determine tessellation mode from the register file.
    auto tess_mode = regs.Get<reg::VGT_HOS_CNTL>().tess_mode;

    uint8_t* factor_data =
        static_cast<uint8_t*>(tess_factor_buffer_->contents());

    if (tess_mode == xenos::TessellationMode::kAdaptive && shared_memory_) {
      // ------------------------------------------------------------------
      // Adaptive tessellation: per-edge factors from shared memory.
      // The guest "index buffer" is repurposed as a factor buffer
      // containing big-endian float32 edge factors.
      // ------------------------------------------------------------------
      xenos::Endian index_endian =
          primitive_processing_result.host_shader_index_endian;
      uint32_t factor_base = primitive_processing_result.guest_index_base;
      const uint8_t* xbox_ram = shared_memory_->GetXboxRamBase();
      // Minimum factor from VGT_HOS_MIN_TESS_LEVEL + 1.0 (Xbox 360
      // convention). For fractional_even partitioning, must be >= 2.0.
      float factor_min = std::max(
          2.0f, regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f);
      float factor_max = max_tess;

      if (is_quad_domain) {
        // Quad: 4 edge factors per patch.
        struct QuadFactors {
          uint16_t edge[4];
          uint16_t inside[2];
        };
        static_assert(sizeof(QuadFactors) == 12);
        auto* factors = reinterpret_cast<QuadFactors*>(factor_data);
        for (uint32_t i = 0; i < patch_count; ++i) {
          // Read 4 edge factors from guest memory.
          float ef[4];
          for (uint32_t j = 0; j < 4; ++j) {
            uint32_t addr = factor_base + (i * 4 + j) * sizeof(float);
            float raw;
            std::memcpy(&raw, xbox_ram + addr, sizeof(float));
            // Endian-swap per the index endian mode.
            raw = xenos::GpuSwap(raw, index_endian);
            // Add 1.0 per Xbox 360 convention, clamp.
            ef[j] = std::clamp(raw + 1.0f, factor_min, factor_max);
          }
          // Map Xbox 360 edge order to Metal:
          //   edge[i] = input[(i+3) & 3]
          //   (from adaptive_quad.hs.glsl)
          factors[i].edge[0] = f32_to_f16(ef[3]);
          factors[i].edge[1] = f32_to_f16(ef[0]);
          factors[i].edge[2] = f32_to_f16(ef[1]);
          factors[i].edge[3] = f32_to_f16(ef[2]);
          // Inside factors: minimum of opposing edges.
          // inside[0] along U = min(mapped_edge[1], mapped_edge[3])
          // inside[1] along V = min(mapped_edge[0], mapped_edge[2])
          factors[i].inside[0] = f32_to_f16(std::min(ef[0], ef[2]));
          factors[i].inside[1] = f32_to_f16(std::min(ef[3], ef[1]));
        }
      } else {
        // Triangle: 3 edge factors per patch.
        struct TriFactors {
          uint16_t edge[3];
          uint16_t inside;
        };
        static_assert(sizeof(TriFactors) == 8);
        auto* factors = reinterpret_cast<TriFactors*>(factor_data);
        for (uint32_t i = 0; i < patch_count; ++i) {
          // Read 3 edge factors from guest memory.
          float ef[3];
          for (uint32_t j = 0; j < 3; ++j) {
            uint32_t addr = factor_base + (i * 3 + j) * sizeof(float);
            float raw;
            std::memcpy(&raw, xbox_ram + addr, sizeof(float));
            raw = xenos::GpuSwap(raw, index_endian);
            ef[j] = std::clamp(raw + 1.0f, factor_min, factor_max);
          }
          // Map Xbox 360 edge order to Metal:
          //   Metal edge[0] = U0 (v1->v2) = ef[1]
          //   Metal edge[1] = V0 (v2->v0) = ef[2]
          //   Metal edge[2] = W0 (v0->v1) = ef[0]
          //   (from adaptive_triangle.hs.glsl)
          factors[i].edge[0] = f32_to_f16(ef[1]);
          factors[i].edge[1] = f32_to_f16(ef[2]);
          factors[i].edge[2] = f32_to_f16(ef[0]);
          // Inside factor = minimum of all edge factors.
          factors[i].inside =
              f32_to_f16(std::min(std::min(ef[0], ef[1]), ef[2]));
        }
      }
    } else {
      // ------------------------------------------------------------------
      // Uniform tessellation (discrete / continuous modes).
      // All patches get the same factor from VGT_HOS_MAX_TESS_LEVEL.
      // ------------------------------------------------------------------
      uint16_t ef = f32_to_f16(max_tess);
      if (is_quad_domain) {
        struct QuadFactors {
          uint16_t edge[4];
          uint16_t inside[2];
        };
        static_assert(sizeof(QuadFactors) == 12);
        auto* factors = reinterpret_cast<QuadFactors*>(factor_data);
        for (uint32_t i = 0; i < patch_count; ++i) {
          factors[i].edge[0] = ef;
          factors[i].edge[1] = ef;
          factors[i].edge[2] = ef;
          factors[i].edge[3] = ef;
          factors[i].inside[0] = ef;
          factors[i].inside[1] = ef;
        }
      } else {
        struct TriFactors {
          uint16_t edge[3];
          uint16_t inside;
        };
        static_assert(sizeof(TriFactors) == 8);
        auto* factors = reinterpret_cast<TriFactors*>(factor_data);
        for (uint32_t i = 0; i < patch_count; ++i) {
          factors[i].edge[0] = ef;
          factors[i].edge[1] = ef;
          factors[i].edge[2] = ef;
          factors[i].inside = ef;
        }
      }
    }

    // Draw with tessellation.
    assert_not_null(tess_factor_buffer_);
    UseRenderEncoderResource(tess_factor_buffer_, MTL::ResourceUsageRead);
    current_render_encoder_->setTessellationFactorBuffer(tess_factor_buffer_, 0,
                                                         0);
    // drawPatches signature:
    //   numberOfPatchControlPoints, patchStart, patchCount,
    //   patchIndexBuffer, patchIndexBufferOffset,
    //   instanceCount, baseInstance
    // patchIndexBuffer = nullptr since patches are not indexed (the domain
    // shader reads control points from shared memory via vertex ID).
    current_render_encoder_->drawPatches(
        NS::UInteger(cp_per_patch), NS::UInteger(0), NS::UInteger(patch_count),
        nullptr,           // patchIndexBuffer (non-indexed patches)
        0,                 // patchIndexBufferOffset
        NS::UInteger(1),   // instanceCount
        NS::UInteger(0));  // baseInstance
  } else {
    DrawIndexBuffer draw_index_buffer;
    if (!ResolveDrawIndexBuffer(primitive_processing_result,
                                host_vertex_shader_type,
                                /*tessellated=*/false, draw_index_buffer)) {
      return false;
    }
    if (draw_index_buffer.indexed) {
      UseRenderEncoderResource(draw_index_buffer.buffer,
                               MTL::ResourceUsageRead);
    }
    DrawStripMergeBands([&]() {
      if (draw_index_buffer.indexed) {
        current_render_encoder_->drawIndexedPrimitives(
            draw_index_buffer.primitive_type,
            NS::UInteger(draw_index_buffer.index_count),
            draw_index_buffer.index_type, draw_index_buffer.buffer,
            NS::UInteger(draw_index_buffer.offset));
      } else {
        current_render_encoder_->drawPrimitives(
            draw_index_buffer.primitive_type, NS::UInteger(0),
            NS::UInteger(draw_index_buffer.index_count));
      }
    });
  }

  if (render_target_cache_) {
    render_target_cache_->NoteDrawWrites();
  }
  if (memexport_used) {
    NoteMemexportRangesWritten();
  }

  ++current_draw_index_;
  return true;
}

// ==========================================================================
// SPIR-V -> DXIL -> AIR draw path
// ==========================================================================

bool MetalCommandProcessor::EnsureTessellatorTablesBuffer() {
  if (tessellator_tables_buffer_) {
    return true;
  }
  uint64_t size = IRRuntimeTessellatorTablesSize();
  tessellator_tables_buffer_ =
      device_->newBuffer(size, MTL::ResourceStorageModeShared);
  if (!tessellator_tables_buffer_) {
    XELOGE("DXIL: failed to allocate the tessellator tables buffer ({} bytes)",
           size);
    return false;
  }
  tessellator_tables_buffer_->setLabel(
      NS::String::string("XeniaTessellatorTables", NS::UTF8StringEncoding));
  IRRuntimeLoadTessellatorTables(tessellator_tables_buffer_);
  return true;
}

bool MetalCommandProcessor::BuildDxilTessellationShaders(
    const std::vector<uint8_t>& domain_spirv, uint64_t domain_ucode_hash,
    xenos::TessellationMode tessellation_mode,
    Shader::HostVertexShaderType host_vertex_shader_type,
    DxilTessellationShaders& shaders_out) {
  MetalTessellationHostShaders host_shaders;
  if (!GetMetalTessellationHostShaders(tessellation_mode,
                                       host_vertex_shader_type, host_shaders)) {
    XELOGE("DXIL: no host tessellation shaders for mode {} domain type {}",
           uint32_t(tessellation_mode), uint32_t(host_vertex_shader_type));
    return false;
  }
  if (domain_spirv.empty() || (domain_spirv.size() % sizeof(uint32_t)) != 0) {
    XELOGE("DXIL: domain shader {:016X} has no usable SPIR-V",
           domain_ucode_hash);
    return false;
  }

  // Linked so the hull and domain signatures agree on control point counts and
  // patch constants, which MSC checks before building the pipeline.
  std::vector<SpirvToDxilCompiler::LinkedStage> stages = {
      {host_shaders.vertex_spirv, host_shaders.vertex_word_count,
       SpirvToDxilCompiler::Stage::kVertex},
      {host_shaders.hull_spirv, host_shaders.hull_word_count,
       SpirvToDxilCompiler::Stage::kTessellationControl},
      {reinterpret_cast<const uint32_t*>(domain_spirv.data()),
       domain_spirv.size() / sizeof(uint32_t),
       SpirvToDxilCompiler::Stage::kTessellationEvaluation},
  };
  using HVS = Shader::HostVertexShaderType;
  bool is_quad = host_vertex_shader_type == HVS::kQuadDomainCPIndexed ||
                 host_vertex_shader_type == HVS::kQuadDomainPatchIndexed;
  bool is_control_point_indexed =
      host_vertex_shader_type == HVS::kQuadDomainCPIndexed ||
      host_vertex_shader_type == HVS::kTriangleDomainCPIndexed;
  // Unsized hull input arrays use the maximum patch size in SPIR-V. Mesa
  // needs the actual pipeline size to avoid grouping unrelated guest patches.
  // Adaptive patches read a factor per corner; non-adaptive patch-indexed
  // shaders receive one patch index rather than a vertex for each corner.
  uint32_t input_control_point_count =
      tessellation_mode == xenos::TessellationMode::kAdaptive ||
              is_control_point_indexed
          ? (is_quad ? 4u : 3u)
          : 1u;
  std::vector<std::vector<uint8_t>> dxil = SpirvToDxilCompiler::TranslateLinked(
      stages, /*lower_to_bindless=*/true,
      /*preserve_msc_fmad32=*/false, input_control_point_count);
  if (dxil.size() != 3 || dxil[0].empty() || dxil[1].empty() ||
      dxil[2].empty()) {
    XELOGE("DXIL: linked tessellation translation failed (domain {:016X})",
           domain_ucode_hash);
    return false;
  }

  const MetalShaderStage kStages[] = {MetalShaderStage::kVertex,
                                      MetalShaderStage::kHull,
                                      MetalShaderStage::kDomain};
  DxilTessellationStage* stage_out[] = {&shaders_out.vertex, &shaders_out.hull,
                                        &shaders_out.domain};
  auto release_stages = [&]() {
    for (DxilTessellationStage* stage : stage_out) {
      if (stage->function) {
        stage->function->release();
        stage->function = nullptr;
      }
      if (stage->library) {
        stage->library->release();
        stage->library = nullptr;
      }
    }
    if (shaders_out.stage_in_library) {
      shaders_out.stage_in_library->release();
      shaders_out.stage_in_library = nullptr;
    }
  };
  for (size_t i = 0; i < 3; ++i) {
    const char* stage_name = StageNameOf(kStages[i]);
    MetalShaderConversionResult conversion = metal_shader_converter_.Convert(
        kStages[i], dxil[i], /*tessellation_emulation=*/true);
    if (!conversion.success) {
      XELOGE("DXIL: DXIL to AIR failed for the {} stage of domain {:016X}: {}",
             stage_name, domain_ucode_hash, conversion.error_message);
      release_stages();
      return false;
    }
    // With tessellation emulation, the vertex stage becomes an object function
    // under a derived name, which IRRuntimeNewGeometryTessellationEmulation-
    // Pipeline also derives from the entry point name.
    if (!CreateMetalFunction(device_, conversion, stage_out[i]->library,
                             stage_out[i]->function,
                             kStages[i] == MetalShaderStage::kVertex
                                 ? ".dxil_irconverter_object_shader"
                                 : "")) {
      XELOGE("DXIL: could not create the {} function of domain {:016X}",
             stage_name, domain_ucode_hash);
      release_stages();
      return false;
    }
    if (kStages[i] == MetalShaderStage::kVertex) {
      NS::Error* error = nullptr;
      dispatch_data_t stage_in_data =
          dispatch_data_create(conversion.stage_in_metallib.data(),
                               conversion.stage_in_metallib.size(), nullptr,
                               DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      shaders_out.stage_in_library = device_->newLibrary(stage_in_data, &error);
      dispatch_release(stage_in_data);
      if (!shaders_out.stage_in_library) {
        XELOGE("DXIL: could not create the stage-in library of domain {:016X}",
               domain_ucode_hash);
        release_stages();
        return false;
      }
    }
    stage_out[i]->function_name = std::move(conversion.entry_point_name);
    stage_out[i]->reflection = conversion.reflection;
  }

  XELOGI(
      "DxilShader: compiled tessellation for domain {:016X} (mode {}, {} "
      "control points)",
      domain_ucode_hash, uint32_t(tessellation_mode),
      shaders_out.hull.reflection.hs_input_control_point_count);
  return true;
}

MetalCommandProcessor::DxilTessellationShaders*
MetalCommandProcessor::GetDxilTessellationShaders(
    DxilShader& domain_shader, uint64_t domain_modification,
    xenos::TessellationMode tessellation_mode,
    Shader::HostVertexShaderType host_vertex_shader_type, bool allow_async,
    ShaderCompileStatus* compile_status_out) {
  *compile_status_out = ShaderCompileStatus::kFailed;
  struct {
    uint64_t ucode_hash;
    uint64_t modification;
    uint32_t tessellation_mode;
    uint32_t host_vertex_shader_type;
  } key_data = {domain_shader.ucode_data_hash(), domain_modification,
                uint32_t(tessellation_mode), uint32_t(host_vertex_shader_type)};
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    auto it = dxil_tessellation_cache_.find(key);
    if (it != dxil_tessellation_cache_.end()) {
      *compile_status_out = ShaderCompileStatus::kReady;
      return it->second.get();
    }
    if (async_tess_shaders_failed_.find(key) !=
        async_tess_shaders_failed_.end()) {
      return nullptr;
    }
    if (async_tess_shaders_pending_.find(key) !=
        async_tess_shaders_pending_.end()) {
      *compile_status_out = ShaderCompileStatus::kPending;
      return nullptr;
    }
  }

  // Only this path uses domain modifications, so the claim is uncontended and
  // the SPIR-V is produced here. What is queued is the three stages behind it.
  auto* domain_translation = static_cast<DxilShader::DxilTranslation*>(
      domain_shader.GetOrCreateTranslation(domain_modification));
  ShaderCompileStatus spirv_status =
      EnsureTranslationSpirv(domain_translation, *spirv_shader_translator_);
  if (spirv_status != ShaderCompileStatus::kReady) {
    if (spirv_status == ShaderCompileStatus::kFailed) {
      std::lock_guard<std::mutex> lock(async_compile_mutex_);
      async_tess_shaders_failed_.insert(key);
    }
    *compile_status_out = spirv_status;
    return nullptr;
  }

  TessellationShadersCompileRequest request;
  request.key = key;
  request.domain_spirv = &domain_translation->translated_binary();
  request.domain_ucode_hash = domain_shader.ucode_data_hash();
  request.tessellation_mode = tessellation_mode;
  request.host_vertex_shader_type = host_vertex_shader_type;
  if (allow_async && cvars::async_shader_compilation &&
      !async_compile_threads_.empty()) {
    {
      std::lock_guard<std::mutex> lock(async_compile_mutex_);
      async_tess_shaders_pending_.insert(key);
      async_tess_shaders_queue_.push(request);
      compile_activity.queued.fetch_add(1, std::memory_order_relaxed);
    }
    async_compile_cv_.notify_one();
    *compile_status_out = ShaderCompileStatus::kPending;
    return nullptr;
  }

  auto shaders = std::make_unique<DxilTessellationShaders>();
  if (!BuildDxilTessellationShaders(
          *request.domain_spirv, request.domain_ucode_hash, tessellation_mode,
          host_vertex_shader_type, *shaders)) {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    async_tess_shaders_failed_.insert(key);
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  auto [it, inserted] =
      dxil_tessellation_cache_.emplace(key, std::move(shaders));
  *compile_status_out = ShaderCompileStatus::kReady;
  return it->second.get();
}

MTL::RenderPipelineState*
MetalCommandProcessor::GetOrCreateDxilTessellationPipelineState(
    const DxilTessellationShaders& shaders,
    const Shader::Translation* domain_translation,
    xenos::TessellationMode tessellation_mode,
    Shader::HostVertexShaderType host_vertex_shader_type,
    DxilShader::DxilTranslation* pixel_translation, const RegisterFile& regs,
    PipelineCompileStatus* compile_status_out) {
  PipelineCompileRequest request = {};
  request.fragment_function = GetHostShaderFunction(pixel_translation);
  PopulatePipelineCompileRequest(regs, domain_translation, pixel_translation,
                                 request);
  // The linked stage set is identified by the domain shader plus these two, so
  // the description covers it without naming the stages themselves.
  request.description.kind = PipelineKind::kDxilTessellation;
  request.description.tessellation_mode = tessellation_mode;
  request.description.host_vertex_shader_type = host_vertex_shader_type;
  request.pipeline_key = request.description.GetHash();
  request.tessellation_shaders = &shaders;
  if (pixel_translation) {
    request.fragment_library = pixel_translation->metal_library();
    request.fragment_function_name = pixel_translation->entry_point_name();
  }
  return AcquirePipelineState(request, /*allow_async=*/true,
                              compile_status_out);
}

bool MetalCommandProcessor::IssueDrawDxil(
    Shader* vertex_shader, Shader* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool primitive_polygonal, bool memexport_used, bool pure_memexport_draw,
    uint32_t normalized_color_mask, const RegisterFile& regs) {
  SCOPE_profile_cpu_f("gpu");
  assert_not_null(vertex_shader);
  if (!metal_shader_converter_.is_available()) {
    static bool converter_unavailable_logged = false;
    if (!converter_unavailable_logged) {
      converter_unavailable_logged = true;
      XELOGE(
          "DXIL: the Metal Shader Converter is unavailable, no draws can be "
          "issued");
    }
    return false;
  }
  auto* dxil_vertex_shader = static_cast<DxilShader*>(vertex_shader);
  auto* dxil_pixel_shader = static_cast<DxilShader*>(pixel_shader);

  Shader::HostVertexShaderType host_vertex_shader_type =
      primitive_processing_result.host_vertex_shader_type;

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask = 0;
  if (dxil_pixel_shader) {
    interpolator_mask = dxil_vertex_shader->writes_interpolators() &
                        dxil_pixel_shader->GetInterpolatorInputMask(
                            regs.Get<reg::SQ_PROGRAM_CNTL>(),
                            regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos);
  }

  auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  SpirvShaderTranslator::Modification vertex_shader_modification =
      GetCurrentSpirvVertexShaderModification(
          *dxil_vertex_shader, host_vertex_shader_type, interpolator_mask,
          ps_param_gen_pos != UINT32_MAX);
  std::shared_ptr<const Shader::Specialization> vertex_specialization;
  const bool is_tessellated = primitive_processing_result.IsTessellated();
  // Specialize eligible streams only on non-tessellated export-only draws.
  // Tessellation uses a separate compilation path without profile retry.
  // Excluding pixel shaders also guarantees the synchronous pipeline fallback
  // has no pending pixel translation to consume.
  if (!dxil_pixel_shader && !is_tessellated) {
    const uint32_t slot = memexport_format_profile::SelectSlot(
        memexport_format_profiles_, dxil_vertex_shader, regs,
        vertex_specialization);
    vertex_shader_modification.vertex.memexport_format_specialized = slot != 0;
    vertex_shader_modification.vertex.memexport_format_slot = slot;
  }
  SpirvShaderTranslator::Modification pixel_shader_modification =
      dxil_pixel_shader
          ? GetCurrentSpirvPixelShaderModification(
                *dxil_pixel_shader, interpolator_mask, ps_param_gen_pos,
                normalized_depth_control, normalized_color_mask)
          : SpirvShaderTranslator::Modification(0);

  // Memory exports are guest-visible writes. A pending generic pipeline,
  // including after a profile guard miss, must never skip the export draw.
  // Counted draws also require the guest's exact kills and alpha test.
  const bool exact_shaders_required = ExactShadersRequired(memexport_used);

  ShaderCompileStatus compile_status = ShaderCompileStatus::kReady;
  ShaderCompileStatus pixel_status = ShaderCompileStatus::kReady;
  DxilShader::DxilTranslation* pixel_translation = nullptr;
  if (dxil_pixel_shader) {
    pixel_translation =
        static_cast<DxilShader::DxilTranslation*>(GetOrCreateHostTranslation(
            *dxil_pixel_shader, pixel_shader_modification.value,
            !exact_shaders_required &&
                (!is_tessellated || cvars::async_shader_skip_draws),
            &pixel_status));
    // Only spirv2air reads render targets. If it can't translate the shader,
    // blend the targets with fixed-function blending (gamma in gamma space)
    // rather than dropping the draw.
    if (pixel_status == ShaderCompileStatus::kFailed &&
        pixel_shader_modification.pixel.host_render_target_feedback) {
      pixel_shader_modification.pixel.host_render_target_feedback = 0;
      pixel_shader_modification.pixel.color_targets_used =
          GetColorTargetsWritten(normalized_color_mask);
      pixel_translation =
          static_cast<DxilShader::DxilTranslation*>(GetOrCreateHostTranslation(
              *dxil_pixel_shader, pixel_shader_modification.value,
              !exact_shaders_required &&
                  (!is_tessellated || cvars::async_shader_skip_draws),
              &pixel_status));
    }
    if (pixel_status == ShaderCompileStatus::kFailed) {
      return false;
    }
  }

  // The guest vertex shader becomes the domain shader of a tessellated draw,
  // linked with the host vertex and hull shaders. Those three stages are linked
  // together and built here rather than on the compile threads.
  MTL::RenderPipelineState* pipeline = nullptr;
  DxilTessellationShaders* tessellation_shaders = nullptr;
  bool bound_placeholder = false;
  if (is_tessellated) {
    if (!EnsureTessellatorTablesBuffer()) {
      return false;
    }
    // GetDxilTessellationShaders owns this translation. Kept here for the logs.
    Shader::Translation* domain_translation =
        dxil_vertex_shader->GetOrCreateTranslation(
            vertex_shader_modification.value);
    // A tessellated draw links all three stages into its pipeline, so the
    // vertex-only placeholder can't stand in for its pixel shader.
    if (pixel_status != ShaderCompileStatus::kReady) {
      LogShaderCompilePending(pixel_translation, "pixel");
      return true;
    }
    tessellation_shaders = GetDxilTessellationShaders(
        *dxil_vertex_shader, vertex_shader_modification.value,
        primitive_processing_result.tessellation_mode, host_vertex_shader_type,
        !exact_shaders_required && cvars::async_shader_skip_draws,
        &compile_status);
    if (!tessellation_shaders && exact_shaders_required &&
        compile_status == ShaderCompileStatus::kPending) {
      AwaitAsyncCompiles();
      tessellation_shaders = GetDxilTessellationShaders(
          *dxil_vertex_shader, vertex_shader_modification.value,
          primitive_processing_result.tessellation_mode,
          host_vertex_shader_type, /*allow_async=*/false, &compile_status);
    }
    if (!tessellation_shaders) {
      if (compile_status == ShaderCompileStatus::kPending) {
        LogShaderCompilePending(domain_translation, "tessellation");
        return true;
      }
      return false;
    }
    PipelineCompileStatus tess_pipeline_status = PipelineCompileStatus::kReady;
    pipeline = GetOrCreateDxilTessellationPipelineState(
        *tessellation_shaders, domain_translation,
        primitive_processing_result.tessellation_mode, host_vertex_shader_type,
        pixel_translation, regs, &tess_pipeline_status);
    if (!pipeline && exact_shaders_required &&
        tess_pipeline_status == PipelineCompileStatus::kPending) {
      AwaitPendingPipeline();
      pipeline = GetOrCreateDxilTessellationPipelineState(
          *tessellation_shaders, domain_translation,
          primitive_processing_result.tessellation_mode,
          host_vertex_shader_type, pixel_translation, regs,
          &tess_pipeline_status);
    }
    if (!pipeline) {
      if (tess_pipeline_status == PipelineCompileStatus::kPending) {
        LogPipelineCompilePending(domain_translation, pixel_translation);
        return true;
      }
      return false;
    }
  } else {
    auto* vertex_translation =
        static_cast<DxilShader::DxilTranslation*>(GetOrCreateHostTranslation(
            *dxil_vertex_shader, vertex_shader_modification.value,
            !exact_shaders_required && cvars::async_shader_skip_draws,
            &compile_status, vertex_specialization));
    // Nothing can stand in for the vertex shader, so its draws wait.
    if (compile_status == ShaderCompileStatus::kPending) {
      LogShaderCompilePending(vertex_translation, "vertex");
      return true;
    }
    // A memory export format profile is speculative. If the compiler rejects
    // the specialized translation, or its pipeline fails to link, use the
    // generic translation, which always exists, rather than failing this draw
    // on every frame for the rest of the session or handing it to a
    // placeholder that cannot export.
    auto use_generic_vertex_translation = [&]() {
      vertex_shader_modification.vertex.memexport_format_specialized = 0;
      vertex_shader_modification.vertex.memexport_format_slot = 0;
      vertex_translation =
          static_cast<DxilShader::DxilTranslation*>(GetOrCreateHostTranslation(
              *dxil_vertex_shader, vertex_shader_modification.value,
              /*allow_async=*/false, &compile_status));
      return compile_status == ShaderCompileStatus::kReady;
    };
    if (compile_status != ShaderCompileStatus::kReady &&
        (!vertex_shader_modification.vertex.memexport_format_specialized ||
         !use_generic_vertex_translation())) {
      return false;
    }
    PipelineCompileStatus pipeline_compile_status =
        PipelineCompileStatus::kReady;
    if (pixel_status == ShaderCompileStatus::kReady) {
      pipeline = GetOrCreatePipelineState(vertex_translation, pixel_translation,
                                          regs, &pipeline_compile_status);
      if (!pipeline && exact_shaders_required &&
          pipeline_compile_status == PipelineCompileStatus::kPending) {
        AwaitPendingPipeline();
        pipeline =
            GetOrCreatePipelineState(vertex_translation, pixel_translation,
                                     regs, &pipeline_compile_status);
      }
    }
    if (!pipeline && pixel_status == ShaderCompileStatus::kReady &&
        vertex_shader_modification.vertex.memexport_format_specialized) {
      if (!use_generic_vertex_translation()) {
        return false;
      }
      pipeline = GetOrCreatePipelineState(vertex_translation, pixel_translation,
                                          regs, &pipeline_compile_status);
      if (!pipeline &&
          pipeline_compile_status == PipelineCompileStatus::kPending) {
        AwaitPendingPipeline();
        pipeline =
            GetOrCreatePipelineState(vertex_translation, pixel_translation,
                                     regs, &pipeline_compile_status);
      }
    }
    if (!pipeline && !exact_shaders_required &&
        (pixel_status != ShaderCompileStatus::kReady ||
         pipeline_compile_status == PipelineCompileStatus::kPending)) {
      pipeline = GetOrCreatePlaceholderPipelineState(vertex_translation, regs);
      bound_placeholder = pipeline != nullptr;
      if (bound_placeholder) {
        LogPlaceholderDraw(vertex_translation, pixel_translation);
      }
    }
    if (!pipeline) {
      if (pipeline_compile_status == PipelineCompileStatus::kPending ||
          pixel_status != ShaderCompileStatus::kReady) {
        LogPipelineCompilePending(vertex_translation, pixel_translation);
        return true;
      }
      return false;
    }
  }
  // The placeholder has no fragment function, so the pixel stage's resources
  // must not be bound to it.
  DxilShader* bind_pixel_shader =
      bound_placeholder ? nullptr : dxil_pixel_shader;

  // A placeholder samples none of the pixel shader's textures, and their
  // binding list may still be being filled.
  uint32_t used_texture_mask =
      dxil_vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (bind_pixel_shader) {
    used_texture_mask |=
        bind_pixel_shader->GetUsedTextureMaskAfterTranslation();
  }
  if (!PrepareDrawTextures(used_texture_mask, regs)) {
    return true;
  }
  if (texture_cache_) {
    texture_cache_->Load3DAs2DViews(*dxil_vertex_shader, bind_pixel_shader);
  }

  if (!RequestDrawSharedMemoryRanges(*dxil_vertex_shader, regs)) {
    return false;
  }
  // A texture load may have ended the render encoder to be ordered after the
  // earlier draws in the command buffer. A pass begun here starts with this
  // draw, drawn with its own shaders unless a placeholder stands in.
  if (render_target_cache_) {
    render_target_cache_->SetClearQuadPassBegin(!bound_placeholder);
  }
  BeginCommandBuffer();
  if (render_target_cache_) {
    render_target_cache_->SetClearQuadPassBegin(false);
  }
  if (!current_render_encoder_) {
    XELOGE("DXIL: failed to resume render encoder after uploads");
    return false;
  }

  draw_util::ViewportInfo viewport_info;
  ComputeDrawViewportInfo(regs, dxil_pixel_shader, normalized_depth_control,
                          viewport_info);
  ApplyViewportAndScissor(regs, viewport_info);

  if (msl_bound_pipeline_state_ != pipeline) {
    current_render_encoder_->setRenderPipelineState(pipeline);
    msl_bound_pipeline_state_ = pipeline;
  }
  ApplyRasterizerState(primitive_polygonal);
  ApplyDepthStencilState(primitive_polygonal, normalized_depth_control);

  UpdateSpirvSystemConstantValues(
      primitive_processing_result, primitive_polygonal,
      primitive_processing_result.line_loop_closing_index,
      primitive_processing_result.host_shader_index_endian, viewport_info,
      used_texture_mask, normalized_depth_control, normalized_color_mask);

  float blend_constants[] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  if (!ff_blend_factor_valid_ ||
      std::memcmp(ff_blend_factor_, blend_constants, sizeof(float) * 4) != 0) {
    std::memcpy(ff_blend_factor_, blend_constants, sizeof(float) * 4);
    ff_blend_factor_valid_ = true;
    current_render_encoder_->setBlendColor(
        blend_constants[0], blend_constants[1], blend_constants[2],
        blend_constants[3]);
  }

  UpdateGuestConstantCaches(dxil_vertex_shader, dxil_pixel_shader, regs);
  MetalDxilBinder::Constants constants;
  constants.system = {&spirv_system_constants_,
                      uint32_t(sizeof(spirv_system_constants_)),
                      msl_system_constants_version_};
  // The uniform block is declared as float_count vec4s (256 with
  // float_dynamic_addressing), so nothing past that is addressable.
  auto declared_float_bytes = [](const Shader* shader) -> uint32_t {
    if (!shader) {
      return 0;
    }
    return std::min(uint32_t(shader->constant_register_map().float_count) * 16u,
                    uint32_t(kCbvSizeBytes));
  };
  constants.float_vertex = {msl_cached_float_constants_vertex_.data(),
                            declared_float_bytes(dxil_vertex_shader),
                            dxil_guest_constant_versions_[0]};
  // The placeholder binds no pixel stage, so it declares no float constants.
  constants.float_pixel = {msl_cached_float_constants_pixel_.data(),
                           declared_float_bytes(bind_pixel_shader),
                           dxil_guest_constant_versions_[1]};
  constants.bool_loop = {msl_cached_bool_loop_constants_.data(),
                         uint32_t(kBoolLoopConstantsSize),
                         dxil_guest_constant_versions_[2]};
  constants.fetch = {msl_cached_fetch_constants_.data(),
                     uint32_t(kFetchConstantsSize),
                     dxil_guest_constant_versions_[3]};
  if (!dxil_binder_.Bind(current_render_encoder_, dxil_vertex_shader,
                         bind_pixel_shader, constants, memexport_used,
                         is_tessellated)) {
    return false;
  }

  DrawIndexBuffer draw_index_buffer;
  if (!ResolveDrawIndexBuffer(primitive_processing_result,
                              host_vertex_shader_type, is_tessellated,
                              draw_index_buffer)) {
    return false;
  }

  OrderDrawSharedMemoryAccesses(
      bind_pixel_shader, primitive_processing_result,
      draw_index_buffer.indexed ? draw_index_buffer.buffer : nullptr,
      draw_index_buffer.offset,
      uint64_t(draw_index_buffer.index_count) *
          (draw_index_buffer.index_type == MTL::IndexTypeUInt16
               ? sizeof(uint16_t)
               : sizeof(uint32_t)),
      regs);

  // Resume a ZPD segment waiting on a render encoder so this draw is counted.
  OpenQuerySegment(false);

  if (is_tessellated) {
    // The companion draw helpers consume these tables but don't bind them.
    current_render_encoder_->setObjectBuffer(
        tessellator_tables_buffer_, 0, kIRRuntimeTessellatorTablesBindPoint);
    current_render_encoder_->setMeshBuffer(
        tessellator_tables_buffer_, 0, kIRRuntimeTessellatorTablesBindPoint);
    UseRenderEncoderResource(tessellator_tables_buffer_,
                             MTL::ResourceUsageRead);
    IRRuntimeTessellationPipelineConfig config =
        BuildTessellationPipelineConfig(
            tessellation_shaders->vertex.reflection,
            tessellation_shaders->hull.reflection,
            tessellation_shaders->domain.reflection);
    // Every patch list lowers to the same runtime primitive type; the control
    // point count comes from the hull reflection in the config.
    if (draw_index_buffer.indexed) {
      UseRenderEncoderResource(draw_index_buffer.buffer,
                               MTL::ResourceUsageRead);
      uint32_t index_stride =
          draw_index_buffer.index_type == MTL::IndexTypeUInt16
              ? sizeof(uint16_t)
              : sizeof(uint32_t);
      IRRuntimeDrawIndexedPatchesTessellationEmulation(
          current_render_encoder_, IRRuntimePrimitiveTypeTriangle,
          draw_index_buffer.index_type, draw_index_buffer.buffer, config, 1,
          draw_index_buffer.index_count, 0, 0,
          uint32_t(draw_index_buffer.offset / index_stride));
    } else {
      IRRuntimeDrawPatchesTessellationEmulation(
          current_render_encoder_, IRRuntimePrimitiveTypeTriangle, config, 1,
          draw_index_buffer.index_count, 0, 0);
    }
  } else {
    // The converter's draw helpers also push the draw arguments the compiled
    // vertex function reads for SV_VertexID and SV_InstanceID.
    if (draw_index_buffer.indexed) {
      UseRenderEncoderResource(draw_index_buffer.buffer,
                               MTL::ResourceUsageRead);
    }
    DrawStripMergeBands([&]() {
      if (draw_index_buffer.indexed) {
        IRRuntimeDrawIndexedPrimitives(
            current_render_encoder_, draw_index_buffer.primitive_type,
            draw_index_buffer.index_count, draw_index_buffer.index_type,
            draw_index_buffer.buffer, draw_index_buffer.offset);
      } else {
        IRRuntimeDrawPrimitives(current_render_encoder_,
                                draw_index_buffer.primitive_type, uint64_t(0),
                                uint64_t(draw_index_buffer.index_count));
      }
    });
  }

  if (memexport_used) {
    for (const draw_util::MemExportRange& range : memexport_ordering_ranges_) {
      AddPendingWrite(render_encoder_memexport_ranges_, range);
    }
    NoteMemexportRangesWritten();
  }
  render_encoder_memexport_draws_are_pure_ &= pure_memexport_draw;

  if (render_target_cache_) {
    render_target_cache_->NoteDrawWrites();
  }
  ++current_draw_index_;
  return true;
}

const char* MetalCommandProcessor::StripMergeBegin(
    const StripMerge::Layout& layout) {
  if (!render_target_cache_ ||
      render_target_cache_->GetPath() !=
          RenderTargetCache::Path::kHostRenderTargets) {
    return "the render targets aren't host ones";
  }
  // Transfers address a band up to strip_count - 1 bands away with a 12-bit
  // offset, biased or not (MakeTransferAddressConstant). Strip-merged render
  // targets are 32bpp.
  if (uint64_t(layout.strip_count - 1) * layout.band_tile_rows() *
          layout.pitch_tiles_at_32bpp >=
      2 * xenos::kEdramTileCount) {
    return "the last band is farther from the first than transfers address";
  }
  // The merged render targets start in a pass of their own, with nothing
  // queued for the render targets before.
  EndRenderEncoder();
  if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
    return "the queued render target transfers failed";
  }
  return render_target_cache_->BeginStripMerge(layout.strip_count,
                                               layout.band_tile_rows());
}

void MetalCommandProcessor::StripMergeEnd(uint32_t band, bool carry) {
  strip_merge_replicate_draw_ = false;
  strip_merge_first_band_only_ = false;
  if (!render_target_cache_) {
    return;
  }
  // The transfers out of the merged render targets read what the pass and
  // the transfers queued for it wrote.
  EndRenderEncoder();
  render_target_cache_->FlushPendingDrawPassTransfers();
  render_target_cache_->EndStripMerge(band, carry);
}

void MetalCommandProcessor::DrawStripMergeBands(
    const std::function<void()>& draw) {
  if (!strip_merge_replicate_draw_) {
    draw();
    return;
  }
  // A draw addressing the strip's band (window offset 0, a window scissor
  // other than the strip's), in each band of the merged render targets: the
  // viewport moved down by the band's top and the scissor clamped to the band.
  // Drawing resolution scaling is off while merging.
  const StripMerge::Layout& layout = strip_merge_.layout();
  MTL::Viewport viewport = msl_viewport_;
  MTL::ScissorRect scissor = msl_scissor_;
  uint32_t band_count = strip_merge_first_band_only_ ? 1 : layout.strip_count;
  for (uint32_t band = 0; band < band_count; ++band) {
    uint64_t band_top = uint64_t(band) * layout.band_rows;
    uint64_t band_bottom =
        std::min(band_top + layout.band_rows, uint64_t(render_encoder_height_));
    uint64_t top = std::max(uint64_t(scissor.y) + band_top, band_top);
    uint64_t bottom = std::min(
        uint64_t(scissor.y) + uint64_t(scissor.height) + band_top, band_bottom);
    if (bottom <= top) {
      continue;
    }
    MTL::Viewport band_viewport = viewport;
    band_viewport.originY += double(band_top);
    MTL::ScissorRect band_scissor = scissor;
    band_scissor.y = NS::UInteger(top);
    band_scissor.height = NS::UInteger(bottom - top);
    current_render_encoder_->setViewport(band_viewport);
    current_render_encoder_->setScissorRect(band_scissor);
    draw();
  }
  current_render_encoder_->setViewport(viewport);
  current_render_encoder_->setScissorRect(scissor);
}

bool MetalCommandProcessor::IssueCopy() {
  SCOPE_profile_cpu_f("gpu");
  // Finish any in-flight rendering so render target contents are visible to
  // resolve logic. The render target cache ends the pass itself, once it
  // knows which of its attachments the resolve reads and clears, unless it
  // reads them from the open pass with a tile dispatch.
  if (!render_target_cache_) {
    EndRenderEncoder();
  }
  }
  MTL::CommandBuffer* copy_command_buffer = EnsureCommandBuffer();
  if (!copy_command_buffer) {
    XELOGE("MetalCommandProcessor::IssueCopy: failed to get command buffer");
    EndRenderEncoder();
    return false;
  }

  if (!render_target_cache_) {
    XELOGW("MetalCommandProcessor::IssueCopy - No render target cache");
    return true;
  }

  uint32_t written_address = 0;
  uint32_t written_length = 0;

  if (render_target_cache_) {
    render_target_cache_->SetStripMergeKeepCarries(false);
  }
  // Strip merge (strip_merge.h): a strip-own resolve of the merged first strip
  // resolves every strip's rows. Its source is the strip-merged render target
  // as one rectangle (GetStripMergedResolveRectangle), so the tile resolve
  // can read every band from the open merged pass.
  uint32_t strip_merge_resolve_clear_bottom = 0;
  uint32_t strip_merge_resolve_bottom =
      strip_merge_.recording()
          ? strip_merge_.NoteResolve(trace_writer_,
                                     strip_merge_resolve_clear_bottom)
          : 0;
  render_target_cache_->SetStripMergeResolveBottom(
      strip_merge_resolve_bottom, strip_merge_resolve_clear_bottom);
  bool resolved = render_target_cache_->Resolve(
      *memory_, written_address, written_length, copy_command_buffer);
  render_target_cache_->SetStripMergeResolveBottom(0);
  // The pass ends at the resolve, unless a tile resolve kept it open,
  // recording what it wrote.
  if (!render_target_cache_->last_resolve_kept_pass()) {
    EndRenderEncoder();
  }
  if (!resolved) {
    XELOGE("MetalCommandProcessor::IssueCopy - Resolve failed");
    return false;
  }

  if (!written_length) {
    // Keep the submission open for no-op copies and let primary-buffer end,
    // swap, or explicit sync points choose the commit boundary.
    return true;
  }

  // The resolve overwrote any export output here, so no fence need await it.
  ClearMemexportPages(written_address, written_length);
  // With zero-copy the resolve writes guest RAM itself, which the guest may
  // read once a fence or interrupt tells it the GPU got past the resolve.
  if (shared_memory_->is_zero_copy()) {
    resolve_last_submission_ = submission_current_;
    // With the read watch, a CPU access of the output waits for this
    // submission instead, so guest signals needn't
    // (GetHeldGpuWritesSubmission).
    if (ResolveReadWatchActive() &&
        IsResolveDestinationResident(written_address, written_length)) {
      NoteResolveForReadback(written_address, written_length, false);
      // lazy submission.
      if (AdaptiveResolveSubmitsEarly(
              MakeReadbackResolveKey(written_address, written_length))) {
        EndResolveSubmission();
      }
    } else {
      resolve_unwatched_last_submission_ = submission_current_;
    }
  }

  // Later work in the submission that reads the output (draws, texture loads
  // encoded into the same command buffer) is ordered after the resolve on the
  // GPU, so the submission stays open. The pending output only makes the end
  // of the primary buffer commit it.
  copy_resolve_writes_pending_ = true;
  return true;
}

void MetalCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void MetalCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

MetalCommandProcessor::RegisterRangeClass
MetalCommandProcessor::ClassifyRegisterRange(uint32_t start_index,
                                             uint32_t num_registers) {
  const uint32_t end = start_index + num_registers;
  auto within = [&](uint32_t first, uint32_t last) {
    return start_index >= first && end <= last + 1;
  };
  auto overlaps = [&](uint32_t first, uint32_t last) {
    return start_index <= last && first < end;
  };
  if (end < start_index || end > RegisterFile::kRegisterCount) {
    return RegisterRangeClass::kPerRegister;
  }
  if (within(XE_GPU_REG_SHADER_CONSTANT_000_X,
             XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 - 1)) {
    return RegisterRangeClass::kFloatConstants;
  }
  if (within(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
             XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5)) {
    return RegisterRangeClass::kFetchConstants;
  }
  if (within(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
             XE_GPU_REG_SHADER_CONSTANT_LOOP_31)) {
    return RegisterRangeClass::kBoolLoopConstants;
  }
  // HandleSpecialRegisterWrite runs for the scratch, coherency and gamma ramp
  // registers, and a range mixing a shader constant class with anything else
  // needs the per-register dirtying.
  if (overlaps(XE_GPU_REG_SCRATCH_REG0, XE_GPU_REG_SCRATCH_REG7) ||
      overlaps(XE_GPU_REG_COHER_STATUS_HOST, XE_GPU_REG_COHER_STATUS_HOST) ||
      overlaps(XE_GPU_REG_DC_LUT_RW_INDEX, XE_GPU_REG_DC_LUT_30_COLOR) ||
      overlaps(XE_GPU_REG_SHADER_CONSTANT_000_X,
               XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) ||
      overlaps(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
               XE_GPU_REG_SHADER_CONSTANT_LOOP_31)) {
    return RegisterRangeClass::kPerRegister;
  }
  return RegisterRangeClass::kOrdinary;
}

void MetalCommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  const RegisterRangeClass range_class =
      ClassifyRegisterRange(start_index, num_registers);
  if (range_class == RegisterRangeClass::kPerRegister) {
    CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
    return;
  }
  WriteRegisterRangeFromMem(range_class, start_index, base, num_registers);
}

void MetalCommandProcessor::WriteRegisterRangeFromRing(xe::RingBuffer* ring,
                                                       uint32_t base,
                                                       uint32_t num_registers) {
  const RegisterRangeClass range_class =
      ClassifyRegisterRange(base, num_registers);
  if (range_class == RegisterRangeClass::kPerRegister) {
    CommandProcessor::WriteRegisterRangeFromRing(ring, base, num_registers);
    return;
  }
  // A range wrapping around the end of the ring is read in two parts.
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));
  const uint32_t first_registers =
      uint32_t(range.first_length / sizeof(uint32_t));
  WriteRegisterRangeFromMem(
      range_class, base,
      reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
      first_registers);
  if (range.second) {
    WriteRegisterRangeFromMem(
        range_class, base + first_registers,
        reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
        num_registers - first_registers);
  }
  ring->EndRead(range);
}

void MetalCommandProcessor::WriteRegisterRangeFromMem(
    RegisterRangeClass range_class, uint32_t start_index, uint32_t* base,
    uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);
  switch (range_class) {
    case RegisterRangeClass::kFloatConstants: {
      // A write to a constant the current shader uses dirties its stage
      // whether or not the value changed, matching the per-register path.
      const uint32_t first_constant =
          (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      const uint32_t last_constant = (start_index + num_registers - 1 -
                                      XE_GPU_REG_SHADER_CONSTANT_000_X) >>
                                     2;
      auto touches_live = [&](const uint64_t* constant_map,
                              uint32_t stage_first_constant) {
        if (last_constant < stage_first_constant ||
            first_constant >= stage_first_constant + 256) {
          return false;
        }
        const uint32_t first_index =
            std::max(first_constant, stage_first_constant) -
            stage_first_constant;
        const uint32_t last_index =
            std::min(last_constant, stage_first_constant + 255) -
            stage_first_constant;
        for (uint32_t word = first_index >> 6; word <= last_index >> 6;
             ++word) {
          uint64_t mask = ~uint64_t(0);
          if (word == first_index >> 6) {
            mask &= ~uint64_t(0) << (first_index & 63);
          }
          if (word == last_index >> 6) {
            mask &= ~uint64_t(0) >> (63 - (last_index & 63));
          }
          if (constant_map[word] & mask) {
            return true;
          }
        }
        return false;
      };
      msl_float_constants_dirty_vertex_ =
          msl_float_constants_dirty_vertex_ ||
          touches_live(msl_current_float_constant_map_vertex_.data(), 0);
      msl_float_constants_dirty_pixel_ =
          msl_float_constants_dirty_pixel_ ||
          touches_live(msl_current_float_constant_map_pixel_.data(), 256);
    } break;
    case RegisterRangeClass::kFetchConstants: {
      msl_fetch_constants_dirty_ = true;
      if (texture_cache_) {
        const uint32_t dword_start =
            start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
        const uint32_t first_fetch = dword_start / 6;
        const uint32_t last_fetch = (dword_start + num_registers - 1) / 6;
        texture_cache_->TextureFetchConstantsWrittenIfChanged(first_fetch,
                                                              last_fetch);
      }
    } break;
    case RegisterRangeClass::kBoolLoopConstants:
      msl_bool_loop_constants_dirty_ = true;
      break;
    default:
      break;
  }
}

void MetalCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    const uint32_t float_constant_index =
        (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    const uint32_t stage_constant_index = float_constant_index & 0xFF;
    const uint32_t map_index = stage_constant_index >> 6;
    const uint64_t map_bit = uint64_t(1) << (stage_constant_index & 63);
    if (float_constant_index >= 256) {
      if (msl_current_float_constant_map_pixel_[map_index] & map_bit) {
        msl_float_constants_dirty_pixel_ = true;
      }
    } else {
      if (msl_current_float_constant_map_vertex_[map_index] & map_bit) {
        msl_float_constants_dirty_vertex_ = true;
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    msl_bool_loop_constants_dirty_ = true;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    msl_fetch_constants_dirty_ = true;
    if (texture_cache_) {
      const uint32_t fetch_index =
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6;
      texture_cache_->TextureFetchConstantsWrittenIfChanged(fetch_index,
                                                            fetch_index);
    }
  }
}

MTL::CommandBuffer* MetalCommandProcessor::EnsureCommandBuffer() {
  SCOPE_profile_cpu_f("gpu");
  ProcessCompletedSubmissions();
  if (current_command_buffer_) {
    return current_command_buffer_;
  }
  if (!command_queue_) {
    XELOGE("EnsureCommandBuffer: no command queue");
    return nullptr;
  }

  EnsureCommandBufferAutoreleasePool();

  // Note: commandBuffer() returns an autoreleased object, we must retain it.
  {
    SCOPE_profile_cpu_i("gpu", "MetalCommandProcessor::QueueCommandBuffer");
    uint64_t wait_start = xe::Clock::QueryHostTickCount();
    current_command_buffer_ = command_queue_->commandBuffer();
    gpu_timing.wait_ticks.fetch_add(
        xe::Clock::QueryHostTickCount() - wait_start,
        std::memory_order_relaxed);
  }
  if (!current_command_buffer_) {
    XELOGE("EnsureCommandBuffer: failed to create command buffer");
    DrainCommandBufferAutoreleasePool();
    return nullptr;
  }
  current_command_buffer_->retain();
  if (debug_labels_) {
    current_command_buffer_->setLabel(
        NS::String::string("XeniaCommandBuffer", NS::UTF8StringEncoding));
  }
  command_buffer_render_passes_ = 0;

  if (!UseDxilPath() && !EnsureSpirvUniformBuffer()) {
    static auto last_ensure_uniforms_fail_log =
        std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_ensure_uniforms_fail_log >= std::chrono::seconds(1)) {
      last_ensure_uniforms_fail_log = now;
      XELOGE(
          "EnsureCommandBuffer: failed to prepare SPIRV-Cross uniforms "
          "buffer");
    }
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    DrainCommandBufferAutoreleasePool();
    return nullptr;
  }

  ++submission_current_;
  ++command_buffer_kind_counts_[size_t(next_submission_kind_)];
  next_submission_kind_ = CommandBufferKind::kSubmissionOther;
  AddGpuTimeHandler(current_command_buffer_);
  pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
  current_command_buffer_->addCompletedHandler(
      [this](MTL::CommandBuffer* command_buffer) {
        {
          std::lock_guard<std::mutex> lock(completion_mutex_);
          completed_command_buffers_.fetch_add(1, std::memory_order_release);
          completion_cond_.notify_all();
        }
        // Publish callback completion after its final owner access and unlock.
        pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
      });

  if (primitive_processor_) {
    primitive_processor_->BeginSubmission();
  }
  if (texture_cache_) {
    texture_cache_->BeginSubmission(submission_current_);
  }
  if (primitive_processor_ && !frame_open_) {
    primitive_processor_->BeginFrame();
    if (render_target_cache_) {
      render_target_cache_->BeginFrame();
      render_target_cache_->AdvanceRenderTargetUseFrame();
    }
    if (texture_cache_) {
      texture_cache_->BeginFrame();
    }
    frame_open_ = true;
  }

  return current_command_buffer_;
}

void MetalCommandProcessor::ProcessCompletedSubmissions() {
  const uint64_t completed =
      completed_command_buffers_.load(std::memory_order_relaxed);
  while (!closed_frames_.empty() &&
         closed_frames_.front().last_submission <= completed) {
    frame_completed_ = closed_frames_.front().frame;
    closed_frames_.pop_front();
  }
  if (completed <= submission_completed_processed_) {
    return;
  }
  submission_completed_processed_ = completed;
  if (primitive_processor_) {
    primitive_processor_->CompletedSubmissionUpdated();
  }
  if (texture_cache_) {
    texture_cache_->CompletedSubmissionUpdated(completed);
  }
}

void MetalCommandProcessor::EnsureCommandBufferAutoreleasePool() {
  if (command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_ = NS::AutoreleasePool::alloc()->init();
}

void MetalCommandProcessor::DrainCommandBufferAutoreleasePool() {
  if (!command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_->release();
  command_buffer_autorelease_pool_ = nullptr;
}

void MetalCommandProcessor::ResetMslRenderEncoderStateCache() {
  msl_bound_vertex_texture_count_ = 0;
  msl_bound_pixel_texture_count_ = 0;
  msl_bound_vertex_sampler_count_ = 0;
  msl_bound_pixel_sampler_count_ = 0;
  msl_bound_vertex_texture_binding_uid_ = 0;
  msl_bound_pixel_texture_binding_uid_ = 0;
  msl_bound_vertex_sampler_binding_uid_ = 0;
  msl_bound_pixel_sampler_binding_uid_ = 0;
  msl_bound_vertex_textures_.fill(nullptr);
  msl_bound_pixel_textures_.fill(nullptr);
  msl_bound_vertex_samplers_.fill(nullptr);
  msl_bound_pixel_samplers_.fill(nullptr);
  msl_bound_shared_memory_buffer_ = nullptr;
  msl_bound_vertex_argument_buffer_ = nullptr;
  msl_bound_pixel_argument_buffer_ = nullptr;
  msl_bound_vertex_argument_buffer_offset_ = 0;
  msl_bound_pixel_argument_buffer_offset_ = 0;
  msl_bound_vertex_argument_buffer_offset_valid_ = false;
  msl_bound_pixel_argument_buffer_offset_valid_ = false;
  msl_bound_null_buffer_ = nullptr;
  msl_bound_uniforms_buffer_ = nullptr;
  msl_bound_uniforms_vs_base_offset_ = 0;
  msl_bound_uniforms_ps_base_offset_ = 0;
  msl_bound_uniforms_offsets_valid_ = false;
  msl_bound_pipeline_state_ = nullptr;
  msl_viewport_valid_ = false;
  msl_scissor_valid_ = false;
  msl_rasterizer_state_valid_ = false;
  msl_depth_stencil_state_ = nullptr;
  msl_stencil_reference_valid_ = false;
  msl_stencil_reference_ = 0;
  render_encoder_buffers_ = {};
  ff_blend_factor_valid_ = false;
  ResetRenderEncoderResourceUsage();
}

void MetalCommandProcessor::InvalidateRenderEncoderStateAfterDrawPassTransfers(
    MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations) {
  if (!mutations) {
    return;
  }
  using RTC = MetalRenderTargetCache;
  if (mutations & RTC::kDrawPassTransferEncoderMutationPipeline) {
    msl_bound_pipeline_state_ = nullptr;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationDepthStencil) {
    msl_depth_stencil_state_ = nullptr;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationStencilReference) {
    msl_stencil_reference_valid_ = false;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationViewport) {
    msl_viewport_valid_ = false;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationScissor) {
    msl_scissor_valid_ = false;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationRasterizer) {
    msl_rasterizer_state_valid_ = false;
  }
  // Transfer draws through the DXIL path bind their own descriptor heap and
  // argument buffer at the Metal Shader Converter's bind points, which the mask
  // doesn't name.
  render_encoder_buffers_ = {};
  // For the MSL path, shared memory sits at buffer slot 0 and the constant
  // buffers from slot 1, both stages, and its textures start at fragment
  // texture 0.
  if (mutations & (RTC::kDrawPassTransferEncoderMutationVertexSlot0 |
                   RTC::kDrawPassTransferEncoderMutationFragmentSlot0)) {
    msl_bound_shared_memory_buffer_ = nullptr;
  }
  if (mutations & (RTC::kDrawPassTransferEncoderMutationVertexSlot1 |
                   RTC::kDrawPassTransferEncoderMutationFragmentSlot1)) {
    msl_bound_uniforms_offsets_valid_ = false;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationFragmentTextures) {
    msl_bound_pixel_textures_.fill(nullptr);
    msl_bound_pixel_texture_count_ = 0;
    msl_bound_pixel_texture_binding_uid_ = 0;
  }
}

void MetalCommandProcessor::ResetMslCrossEncoderReuseCaches() {
  msl_last_argbuf_vertex_textures_.fill(nullptr);
  msl_last_argbuf_vertex_texture_count_ = 0;
  msl_last_argbuf_vertex_samplers_.fill(nullptr);
  msl_last_argbuf_vertex_sampler_count_ = 0;
  msl_last_argbuf_vertex_buffer_ = nullptr;
  msl_last_argbuf_vertex_offset_ = 0;
  msl_last_argbuf_vertex_translation_ = nullptr;
  msl_last_argbuf_vertex_encoded_length_ = 0;
  msl_last_argbuf_vertex_layout_uid_ = 0;
  msl_last_argbuf_pixel_textures_.fill(nullptr);
  msl_last_argbuf_pixel_texture_count_ = 0;
  msl_last_argbuf_pixel_samplers_.fill(nullptr);
  msl_last_argbuf_pixel_sampler_count_ = 0;
  msl_last_argbuf_pixel_buffer_ = nullptr;
  msl_last_argbuf_pixel_offset_ = 0;
  msl_last_argbuf_pixel_translation_ = nullptr;
  msl_last_argbuf_pixel_encoded_length_ = 0;
  msl_last_argbuf_pixel_layout_uid_ = 0;
}

MTL::ComputeCommandEncoder* MetalCommandProcessor::BeginSharedComputeEncoder(
    MTL::CommandBuffer* command_buffer,
    MetalBufferHazards::Encoder** hazards_out) {
  if (!command_buffer || command_buffer != current_command_buffer_) {
    return nullptr;
  }
  if (!shared_compute_encoder_) {
    EndRenderEncoder();
    shared_compute_encoder_ = command_buffer->computeCommandEncoder();
    if (!shared_compute_encoder_) {
      return nullptr;
    }
    shared_compute_encoder_->retain();
    shared_compute_command_buffer_ = command_buffer;
    if (shared_memory_ && shared_memory_->hazards()) {
      shared_compute_hazards_.Begin(*shared_memory_->hazards(), command_buffer,
                                    shared_compute_encoder_);
    }
  }
  *hazards_out = &shared_compute_hazards_;
  return shared_compute_encoder_;
}

void MetalCommandProcessor::EndSharedComputeEncoder() {
  if (shared_compute_encoder_) {
    shared_compute_hazards_.End();
    shared_compute_encoder_->endEncoding();
    shared_compute_encoder_->release();
    shared_compute_encoder_ = nullptr;
    shared_compute_command_buffer_ = nullptr;
  }
  // Fences written with no pass open go before the next encoder. A pass
  // begins only after this, so no pass is open while they're pending.
  if (shader_done_fences_outside_pass_) {
    assert_null(current_render_encoder_);
    EncodeDeferredShaderDoneFences();
  }
}

bool MetalCommandProcessor::ExactShadersRequired(bool memexport_used) const {
  return memexport_used ||
         (GetZPDMode() != ZPDMode::kFake && !zpd_force_fake_fallback_ &&
          zpd_current_report_.handle != kInvalidReportHandle);
}

bool MetalCommandProcessor::EncodeHeldDrawPassTransfers() {
  if (!render_target_cache_) {
    return true;
  }
  const bool in_pass = render_target_cache_->ReleaseHeldDrawPassTransfers();
  if (!render_target_cache_->HasPendingDrawPassTransfers()) {
    return true;
  }
  if (!in_pass || !current_render_encoder_) {
    return render_target_cache_->FlushPendingDrawPassTransfers();
  }
  // As at the head of the pass, after the clear quads that ran ahead of them.
  CloseQuerySegment();
  MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations =
      MetalRenderTargetCache::kDrawPassTransferEncoderMutationNone;
  bool transfers_encoded = render_target_cache_->EncodePendingDrawPassTransfers(
      current_render_encoder_, current_render_pass_descriptor_, &mutations);
  InvalidateRenderEncoderStateAfterDrawPassTransfers(mutations);
  if (!transfers_encoded) {
    EndRenderEncoder();
    return render_target_cache_->FlushPendingDrawPassTransfers();
  }
  return true;
}

void MetalCommandProcessor::EndRenderEncoder() {
  SCOPE_profile_cpu_f("gpu");
  // Transfers held behind clear quads in this pass belong to it.
  if (current_render_encoder_ && render_target_cache_ &&
      render_target_cache_->DrawPassTransfersHeldInPass()) {
    EncodeHeldDrawPassTransfers();
    if (!current_render_encoder_) {
      return;
    }
  }
  render_encoder_resolve_only_ = false;
  EndSharedComputeEncoder();
  render_encoder_memexport_ranges_.clear();
  render_encoder_tile_resolve_ranges_.clear();
  render_encoder_index_count_ = 0;
  render_encoder_tile_member_class_ = 0;
  render_encoder_memexport_draws_are_pure_ = true;
  if (!current_render_encoder_) {
    if (current_render_pass_descriptor_) {
      current_render_pass_descriptor_->release();
      current_render_pass_descriptor_ = nullptr;
    }
    render_encoder_has_zpd_visibility_ = false;
    // Left only by a pass that ended without EndRenderEncoder.
    FlushPostPassResolves();
    return;
  }
  // Visibility results are scoped to the render encoder and an offset can't be
  // selected again after it ends, so the segment closes here. The logical
  // report stays open and resumes on the next encoder.
  if (GetZPDMode() != ZPDMode::kFake) {
    CloseQuerySegment();
  }
  render_encoder_shared_memory_hazards_.End();
  if (render_target_cache_) {
    render_target_cache_->FinalizeRenderPassStoreActions(
        current_render_encoder_, current_render_pass_descriptor_);
  }
  current_render_encoder_->endEncoding();
  if (render_encoder_has_zpd_visibility_) {
    zpd_visibility_pool_->EndRenderPass(current_command_buffer_);
  }
  current_render_encoder_->release();
  current_render_encoder_ = nullptr;
  ++command_buffer_render_passes_;
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  render_encoder_has_zpd_visibility_ = false;
  ResetMslRenderEncoderStateCache();
  // The resolve work left to the pass end comes before anything else after
  // the pass, including the shader-done fences of commands that followed it.
  FlushPostPassResolves();
  EncodeDeferredShaderDoneFences();
}

void MetalCommandProcessor::FlushPostPassResolves() {
  if (!render_target_cache_ || !render_target_cache_->HasPostPassResolves()) {
    return;
  }
  render_target_cache_->FlushPostPassResolves(current_command_buffer_);
  // Whoever ended the pass expects no encoder open.
  EndSharedComputeEncoder();
}

void MetalCommandProcessor::ResetRenderEncoderResourceUsage() {
  render_encoder_resource_usage_.clear();
  render_encoder_textures_.clear();
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Texture* texture,
                                                     MTL::ResourceUsage usage) {
  if (current_render_encoder_ && texture &&
      !render_encoder_resource_usage_.Contains(texture)) {
    render_encoder_textures_.push_back(texture);
  }
  UseRenderEncoderResource(static_cast<MTL::Resource*>(texture), usage);
}

bool MetalCommandProcessor::RenderEncoderUsesAnyTexture(
    std::span<MTL::Texture* const> textures) const {
  auto root_texture = [](MTL::Texture* texture) {
    while (MTL::Texture* parent = texture->parentTexture()) {
      texture = parent;
    }
    return texture;
  };
  for (MTL::Texture* used : render_encoder_textures_) {
    MTL::Texture* used_root = root_texture(used);
    for (MTL::Texture* texture : textures) {
      if (texture && root_texture(texture) == used_root) {
        return true;
      }
    }
  }
  return false;
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Resource* resource,
                                                     MTL::ResourceUsage usage) {
  if (!current_render_encoder_ || !resource) {
    return;
  }
  // No useHeap: tracking on a tracked heap is heap-granular, so declaring the
  // heap would make this encoder depend on every write to anything in it.
  uint32_t usage_bits = static_cast<uint32_t>(usage);
  bool inserted;
  uint32_t& declared_usage_bits =
      render_encoder_resource_usage_.FindOrInsert(resource, inserted);
  if (!inserted && (declared_usage_bits & usage_bits) == usage_bits) {
    return;
  }
  declared_usage_bits |= usage_bits;
  current_render_encoder_->useResource(resource, usage);
}

void MetalCommandProcessor::SetRenderEncoderBuffer(RenderEncoderStage stage,
                                                   MTL::Buffer* buffer,
                                                   NS::UInteger offset,
                                                   NS::UInteger index) {
  bool offset_only = false;
  if (index < kRenderEncoderTrackedBufferCount) {
    RenderEncoderBuffer& bound = render_encoder_buffers_[size_t(stage)][index];
    if (bound.valid && bound.buffer == buffer) {
      if (bound.offset == offset) {
        return;
      }
      offset_only = true;
    }
    bound.buffer = buffer;
    bound.offset = offset;
    bound.valid = true;
  }
  switch (stage) {
    case RenderEncoderStage::kVertex:
      if (offset_only) {
        current_render_encoder_->setVertexBufferOffset(offset, index);
      } else {
        current_render_encoder_->setVertexBuffer(buffer, offset, index);
      }
      break;
    case RenderEncoderStage::kFragment:
      if (offset_only) {
        current_render_encoder_->setFragmentBufferOffset(offset, index);
      } else {
        current_render_encoder_->setFragmentBuffer(buffer, offset, index);
      }
      break;
    case RenderEncoderStage::kObject:
      if (offset_only) {
        current_render_encoder_->setObjectBufferOffset(offset, index);
      } else {
        current_render_encoder_->setObjectBuffer(buffer, offset, index);
      }
      break;
    case RenderEncoderStage::kMesh:
      if (offset_only) {
        current_render_encoder_->setMeshBufferOffset(offset, index);
      } else {
        current_render_encoder_->setMeshBuffer(buffer, offset, index);
      }
      break;
    default:
      assert_unhandled_case(stage);
  }
}

void MetalCommandProcessor::BeginCommandBuffer() {
  SCOPE_profile_cpu_f("gpu");
  if (!EnsureCommandBuffer()) {
    return;
  }

  // The visibility result buffer has to be on the descriptor before the encoder
  // is created, so a segment waiting to open needs the pool allocated now.
  const bool zpd_segment_pending =
      GetZPDMode() != ZPDMode::kFake &&
      zpd_current_report_.handle != kInvalidReportHandle &&
      active_segment_.segment_pending_begin;
  if (zpd_segment_pending) {
    EnsureQueryResources();
  }

  if (!current_render_encoder_ && !render_encoder_resource_usage_.empty()) {
    ResetRenderEncoderResourceUsage();
  }

  // An encoder created before the pool existed can never host a query, so
  // restart it rather than leave the segment pending indefinitely.
  if (current_render_encoder_ && zpd_segment_pending && IsQueryPoolReady() &&
      !render_encoder_has_zpd_visibility_) {
    EndRenderEncoder();
  }

  // A draw reaching below the open pass's render area needs a pass covering
  // it.
  if (current_render_encoder_ && render_encoder_area_restricted_ &&
      GetDrawScissorBottom() > render_encoder_height_) {
    EndRenderEncoder();
  }

  // Obtain the render pass descriptor. Prefer the one provided by
  // MetalRenderTargetCache (host render-target path), falling back to the
  // legacy descriptor if needed.
  MTL::RenderPassDescriptor* pass_descriptor =
      current_render_encoder_ ? current_render_pass_descriptor_
                              : render_pass_descriptor_;
  if (render_target_cache_) {
    // Check attachment identity before asking the cache to rebuild a dirty
    // descriptor. Rebuilding releases the cache-owned descriptor, and an
    // allocator may reuse the same address even though the active encoder has
    // already captured its old attachments.
    if (current_render_encoder_ &&
        !render_target_cache_->IsRenderPassDescriptorCompatible(
            current_render_pass_descriptor_, 1)) {
      EndRenderEncoder();
      pass_descriptor = render_pass_descriptor_;
    }
    if (!current_render_encoder_) {
      if (MTL::RenderPassDescriptor* cache_desc =
              render_target_cache_->GetRenderPassDescriptor(1, true)) {
        pass_descriptor = cache_desc;
      }
    }
  }
  if (!pass_descriptor) {
    XELOGE("BeginCommandBuffer: No render pass descriptor available");
    return;
  }

  // Ownership transfers queued for this pass have to be encoded into it ahead
  // of the guest's draws. Resolve what it cannot take here, while no encoder
  // exists yet and the descriptor can still be rebuilt.
  if (render_target_cache_ &&
      render_target_cache_->HasPendingDrawPassTransfers() &&
      !render_target_cache_->PreflightPendingDrawPassTransfers(
          pass_descriptor)) {
    if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
      XELOGE("BeginCommandBuffer: failed to perform render target transfers");
      return;
    }
    // The flush may release the old descriptor, including on a failed rebuild.
    pass_descriptor = render_target_cache_->GetRenderPassDescriptor(
        1, !current_render_encoder_);
    if (!pass_descriptor) {
      XELOGE("BeginCommandBuffer: no render pass descriptor after transfers");
      return;
    }
  }

  // Only passes that host a query need the buffer. A segment pending later
  // restarts the encoder above to pick it up.
  if (zpd_segment_pending && IsQueryPoolReady()) {
    pass_descriptor->setVisibilityResultBuffer(
        zpd_visibility_pool_->visibility_buffer());
    // Slots from earlier passes may still await CPU readback. The accumulate
    // result type keeps them across passes; otherwise the pool copies each
    // pass's slots to readback storage when EndRenderEncoder ends the pass.
    if (__builtin_available(macOS 26.0, iOS 26.0, *)) {
      pass_descriptor->setVisibilityResultType(
          zpd_visibility_pool_->uses_accumulation()
              ? MTL::VisibilityResultTypeAccumulate
              : MTL::VisibilityResultTypeReset);
    }
  } else {
    pass_descriptor->setVisibilityResultBuffer(nullptr);
  }

  // Detect Reverse-Z usage and update clear depth.
  // A resolve clear deferred to this pass brings its own clear depth.
  if (register_file_ &&
      !(render_target_cache_ &&
        render_target_cache_->HasDeferredDepthClear(pass_descriptor))) {
    auto depth_control = register_file_->Get<reg::RB_DEPTHCONTROL>();
    bool reverse_z =
        depth_control.z_enable &&
        (depth_control.zfunc == xenos::CompareFunction::kGreater ||
         depth_control.zfunc == xenos::CompareFunction::kGreaterEqual);
    if (auto* da = pass_descriptor->depthAttachment()) {
      if (reverse_z) {
        da->setClearDepth(0.0);
      } else {
        da->setClearDepth(1.0);
      }
    }
  }

  // If the render pass configuration has changed since the current render
  // encoder was created (e.g. dummy RT0 -> real RTs, depth/stencil binding),
  // restart the render encoder with the updated descriptor.
  if (current_render_encoder_ &&
      current_render_pass_descriptor_ != pass_descriptor) {
    EndRenderEncoder();
  }

  if (!current_render_encoder_) {
    if (render_target_cache_) {
      render_target_cache_->SetRenderPassArea(pass_descriptor,
                                              GetDrawScissorBottom());
    }
    // A clear quad beginning the pass may overwrite all it would load.
    const uint32_t clear_quad_dontcare_mask =
        render_target_cache_
            ? render_target_cache_->SetClearQuadLoadActions(pass_descriptor)
            : 0;
    EndSharedComputeEncoder();
    // If some path cleared the encoder without going through EndRenderEncoder,
    // avoid leaking cached binding state into the new encoder.
    render_encoder_shared_memory_hazards_.Abandon();
    ResetMslRenderEncoderStateCache();
    // Note: renderCommandEncoder() returns an autoreleased object, we must
    // retain it.
    current_render_encoder_ =
        current_command_buffer_->renderCommandEncoder(pass_descriptor);
    if (!current_render_encoder_) {
      if (clear_quad_dontcare_mask) {
        render_target_cache_->RestoreClearQuadLoadActions(
            clear_quad_dontcare_mask);
      }
      XELOGE("Failed to create render command encoder");
      return;
    }
    current_render_encoder_->retain();
    if (shared_memory_ && shared_memory_->hazards()) {
      render_encoder_shared_memory_hazards_.Begin(*shared_memory_->hazards(),
                                                  current_command_buffer_,
                                                  current_render_encoder_);
    }
    // A non-null encoder will execute the descriptor's first-use clears when
    // the pass ends. Keep them pending across encoder-creation failures.
    if (render_target_cache_) {
      render_target_cache_->ConsumeRenderPassDescriptorClears(pass_descriptor);
    }
    ++render_passes_total_;
    if (debug_labels_) {
      current_render_encoder_->setLabel(
          NS::String::string("XeniaRenderEncoder", NS::UTF8StringEncoding));
    }
    render_encoder_has_zpd_visibility_ =
        IsQueryPoolReady() && (pass_descriptor->visibilityResultBuffer() ==
                               zpd_visibility_pool_->visibility_buffer());
    ff_blend_factor_valid_ = false;
    current_render_pass_descriptor_ = pass_descriptor;
    current_render_pass_descriptor_->retain();
    {
      RenderPassPipelineFormats& formats =
          current_render_pass_pipeline_formats_;
      std::fill(std::begin(formats.color), std::end(formats.color),
                MTL::PixelFormatInvalid);
      formats.depth = MTL::PixelFormatInvalid;
      formats.stencil = MTL::PixelFormatInvalid;
      formats.sample_count = 1;
      PopulatePipelineFormatsFromRenderPassDescriptor(
          pass_descriptor, formats.color, 4, &formats.depth, &formats.stencil,
          &formats.sample_count);
    }

    // Start the encoder off covering the whole active render pass rather than
    // a hard-coded 1280x720. Every draw applies the guest's own viewport and
    // scissor over this before it runs.
    uint32_t rt_width = 1;
    uint32_t rt_height = 1;
    GetRenderPassSize(pass_descriptor, render_target_width_,
                      render_target_height_, rt_width, rt_height);
    render_encoder_width_ = rt_width;
    render_encoder_height_ = rt_height;
    render_encoder_area_restricted_ =
        pass_descriptor->renderTargetHeight() != 0;
    // The descriptor is reused for later passes, which have to load.
    if (clear_quad_dontcare_mask) {
      render_target_cache_->RestoreClearQuadLoadActions(
          clear_quad_dontcare_mask);
    }
    MTL::Viewport viewport = {
        0.0, 0.0, static_cast<double>(rt_width), static_cast<double>(rt_height),
        0.0, 1.0};
    current_render_encoder_->setViewport(viewport);
    // Must not exceed the render pass dimensions.
    MTL::ScissorRect scissor = {0, 0, rt_width, rt_height};
    current_render_encoder_->setScissorRect(scissor);
  }

  if (render_target_cache_ &&
      render_target_cache_->HasPendingDrawPassTransfers() &&
      !render_target_cache_->HoldDrawPassTransfersForQuad()) {
    // An occlusion query already counting on this encoder would count the
    // transfer draws too. Closing the segment makes the next draw reopen it on
    // a fresh pool slot, the only legal way to resume counting mid-encoder.
    CloseQuerySegment();
    MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations =
        MetalRenderTargetCache::kDrawPassTransferEncoderMutationNone;
    bool transfers_encoded =
        render_target_cache_->EncodePendingDrawPassTransfers(
            current_render_encoder_, pass_descriptor, &mutations);
    InvalidateRenderEncoderStateAfterDrawPassTransfers(mutations);
    if (!transfers_encoded) {
      // Preflight passed, so this is a resource failure part way in. Drop the
      // pass and run the queue standalone: every attachment loaded DontCare is
      // one the transfers rewrite in full, so the flush restores it.
      static bool draw_pass_transfer_encode_failed_logged = false;
      if (!draw_pass_transfer_encode_failed_logged) {
        draw_pass_transfer_encode_failed_logged = true;
        XELOGE(
            "BeginCommandBuffer: failed to encode the queued render target "
            "ownership transfers into the draw pass");
      }
      EndRenderEncoder();
      if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
        XELOGE(
            "BeginCommandBuffer: failed to perform the queued render target "
            "ownership transfers");
      }
      return;
    }
  }
}

bool MetalCommandProcessor::EnsureSpirvUniformBuffer() {
  if (uniforms_buffer_) {
    return true;
  }
  if (!device_) {
    XELOGE("EnsureSpirvUniformBuffer: Metal device is null");
    return false;
  }

  // Keep this aligned with the SPIRV-Cross descriptor table layout used by
  // IssueDrawMsl (6 x 4KB CBVs + texture/sampler descriptor blocks).
  constexpr size_t kUniformsBytesPerTable = 24576;
  constexpr size_t kStageCount = 2;

  if (!draw_ring_count_) {
    XELOGW("SPIRV-Cross: draw ring count was zero, forcing to 1");
    draw_ring_count_ = 1;
  }

  // Keep a slightly larger initial pool on iOS to reduce early-frame pressure.
#if XE_PLATFORM_IOS
  constexpr size_t kUniformsBuffersInFlightInitial = 6;
  // iOS commonly needs extra headroom to avoid command-buffer churn when
  // submissions retire later than the CPU draw cadence.
  constexpr size_t kUniformsBuffersInFlightMax = 24;
#else
  constexpr size_t kUniformsBuffersInFlightInitial = 4;
  constexpr size_t kUniformsBuffersInFlightMax = 12;
#endif

  if (!spirv_uniforms_pool_initialized_) {
    size_t requested_ring_count = std::max<size_t>(1, draw_ring_count_);
    while (requested_ring_count >= 1) {
      const size_t descriptor_table_count = kStageCount * requested_ring_count;
      const size_t uniforms_buffer_size =
          kUniformsBytesPerTable * descriptor_table_count;

      std::vector<MTL::Buffer*> new_pool;
      new_pool.reserve(kUniformsBuffersInFlightInitial);
      bool allocation_failed = false;
      for (size_t i = 0; i < kUniformsBuffersInFlightInitial; ++i) {
        MTL::Buffer* buffer = device_->newBuffer(
            uniforms_buffer_size, MTL::ResourceStorageModeShared);
        if (!buffer) {
          allocation_failed = true;
          break;
        }
        buffer->setLabel(
            NS::String::string("MslUniformsBuffer", NS::UTF8StringEncoding));
        std::memset(buffer->contents(), 0, uniforms_buffer_size);
        new_pool.push_back(buffer);
      }

      if (!allocation_failed &&
          new_pool.size() == kUniformsBuffersInFlightInitial) {
        {
          std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
          for (MTL::Buffer* old_buffer : spirv_uniforms_pool_) {
            if (old_buffer) {
              old_buffer->release();
            }
          }
          spirv_uniforms_pool_ = std::move(new_pool);
          spirv_uniforms_available_.clear();
          spirv_uniforms_available_.insert(spirv_uniforms_available_.end(),
                                           spirv_uniforms_pool_.begin(),
                                           spirv_uniforms_pool_.end());
        }

        if (spirv_uniforms_available_semaphore_) {
#if !OS_OBJECT_USE_OBJC
          dispatch_release(spirv_uniforms_available_semaphore_);
#endif
          spirv_uniforms_available_semaphore_ = nullptr;
        }
        spirv_uniforms_available_semaphore_ = dispatch_semaphore_create(
            static_cast<long>(kUniformsBuffersInFlightInitial));
        if (!spirv_uniforms_available_semaphore_) {
          XELOGE(
              "SPIRV-Cross: failed to create uniforms availability semaphore");
          return false;
        }

        if (requested_ring_count != draw_ring_count_) {
          XELOGW(
              "SPIRV-Cross: reduced uniforms ring from {} to {} pages after "
              "allocation pressure",
              draw_ring_count_, requested_ring_count);
          draw_ring_count_ = requested_ring_count;
        }
        spirv_uniforms_pool_initialized_ = true;
        break;
      }

      for (MTL::Buffer* buffer : new_pool) {
        if (buffer) {
          buffer->release();
        }
      }
      if (requested_ring_count == 1) {
        break;
      }
      const size_t fallback_ring_count =
          std::max<size_t>(1, requested_ring_count / 2);
      XELOGW(
          "SPIRV-Cross: failed to allocate uniforms pool with {} ring pages, "
          "retrying with {}",
          requested_ring_count, fallback_ring_count);
      requested_ring_count = fallback_ring_count;
    }

    if (!spirv_uniforms_pool_initialized_) {
      XELOGE(
          "Failed to create uniforms buffer pool for SPIRV-Cross path (ring "
          "pages={}, bytes per table={})",
          draw_ring_count_, kUniformsBytesPerTable);
      return false;
    }
  }

  if (!spirv_uniforms_available_semaphore_) {
    XELOGE("SPIRV-Cross: uniforms pool semaphore is not initialized");
    return false;
  }

  const auto try_grow_uniforms_pool = [&]() -> bool {
    size_t pool_size = 0;
    {
      std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
      pool_size = spirv_uniforms_pool_.size();
      if (pool_size >= kUniformsBuffersInFlightMax) {
        return false;
      }
    }
    const size_t descriptor_table_count =
        kStageCount * std::max<size_t>(size_t(1), draw_ring_count_);
    const size_t uniforms_buffer_size =
        kUniformsBytesPerTable * descriptor_table_count;
    MTL::Buffer* buffer = device_->newBuffer(uniforms_buffer_size,
                                             MTL::ResourceStorageModeShared);
    if (!buffer) {
      return false;
    }
    buffer->setLabel(
        NS::String::string("MslUniformsBufferGrow", NS::UTF8StringEncoding));
    std::memset(buffer->contents(), 0, uniforms_buffer_size);
    size_t new_pool_size = 0;
    {
      std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
      spirv_uniforms_pool_.push_back(buffer);
      spirv_uniforms_available_.push_back(buffer);
      new_pool_size = spirv_uniforms_pool_.size();
    }
    dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
    XELOGW("SPIRV-Cross: grew uniforms pool to {} buffers under load",
           new_pool_size);
    return true;
  };

  if (dispatch_semaphore_wait(spirv_uniforms_available_semaphore_,
                              DISPATCH_TIME_NOW) != 0) {
    bool acquired_after_grow = false;
    if (try_grow_uniforms_pool()) {
      acquired_after_grow =
          dispatch_semaphore_wait(spirv_uniforms_available_semaphore_,
                                  DISPATCH_TIME_NOW) == 0;
    }
    if (!acquired_after_grow) {
      // Last resort: block until one in-flight command buffer retires.
      // D3D12-style behavior is to avoid this in common paths by growing first.
      static auto last_wait_log = std::chrono::steady_clock::time_point{};
      static uint32_t suppressed_wait_logs = 0;
      const auto now = std::chrono::steady_clock::now();
      if (now - last_wait_log >= std::chrono::seconds(10)) {
        last_wait_log = now;
        size_t pool_size = 0;
        size_t available_size = 0;
        {
          std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
          pool_size = spirv_uniforms_pool_.size();
          available_size = spirv_uniforms_available_.size();
        }
        XELOGW(
            "SPIRV-Cross: uniforms pool busy; waiting for an in-flight command "
            "buffer to retire (in-use={}, total={}, available={}, ring "
            "pages={}, suppressed_wait_logs={})",
            pool_size - available_size, pool_size, available_size,
            draw_ring_count_, suppressed_wait_logs);
        suppressed_wait_logs = 0;
      } else {
        ++suppressed_wait_logs;
      }
      int64_t wait_start_ns = GetSteadyTimeNs();
      dispatch_semaphore_wait(spirv_uniforms_available_semaphore_,
                              DISPATCH_TIME_FOREVER);
      NoteGpuWait(GpuWaitReason::kUniforms, wait_start_ns);
    }
  }

  {
    std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
    if (spirv_uniforms_available_.empty()) {
      XELOGE(
          "SPIRV-Cross: uniforms semaphore signaled but no reusable buffer is "
          "available");
      return false;
    }
    uniforms_buffer_ = spirv_uniforms_available_.back();
    spirv_uniforms_available_.pop_back();
  }
  if (uniforms_buffer_) {
    command_buffer_spirv_uniform_buffers_.push_back(uniforms_buffer_);
  }

  return uniforms_buffer_ != nullptr;
}

bool MetalCommandProcessor::EnsureSpirvUniformBufferCapacity() {
  if (!draw_ring_count_) {
    draw_ring_count_ = 1;
  }
  if (!uniforms_buffer_) {
    return EnsureSpirvUniformBuffer();
  }
  if (current_draw_index_ == 0) {
    return true;
  }
  const uint32_t ring_index =
      current_draw_index_ % uint32_t(std::max<size_t>(1, draw_ring_count_));
  if (ring_index != 0) {
    return true;
  }

  // Try to rotate to another uniforms buffer in the current command buffer to
  // avoid forcing a split at every ring wrap.
  uniforms_buffer_ = nullptr;
  const auto try_acquire_uniforms_buffer = [&]() -> bool {
    if (!spirv_uniforms_available_semaphore_ ||
        dispatch_semaphore_wait(spirv_uniforms_available_semaphore_,
                                DISPATCH_TIME_NOW) != 0) {
      return false;
    }
    std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
    if (!spirv_uniforms_available_.empty()) {
      uniforms_buffer_ = spirv_uniforms_available_.back();
      spirv_uniforms_available_.pop_back();
      command_buffer_spirv_uniform_buffers_.push_back(uniforms_buffer_);
      return true;
    }
    // Keep semaphore state consistent if availability changed concurrently.
    dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
    return false;
  };

#if XE_PLATFORM_IOS
  constexpr size_t kUniformsBuffersInFlightMax = 24;
#else
  constexpr size_t kUniformsBuffersInFlightMax = 12;
#endif
  const auto try_grow_uniforms_pool = [&]() -> bool {
    if (!device_) {
      return false;
    }
    size_t pool_size = 0;
    {
      std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
      pool_size = spirv_uniforms_pool_.size();
      if (pool_size >= kUniformsBuffersInFlightMax) {
        return false;
      }
    }
    const size_t descriptor_table_count =
        kStageCount * std::max<size_t>(size_t(1), draw_ring_count_);
    const size_t uniforms_buffer_size =
        kUniformsBytesPerTable * descriptor_table_count;
    MTL::Buffer* buffer = device_->newBuffer(uniforms_buffer_size,
                                             MTL::ResourceStorageModeShared);
    if (!buffer) {
      return false;
    }
    buffer->setLabel(
        NS::String::string("MslUniformsBufferGrow", NS::UTF8StringEncoding));
    std::memset(buffer->contents(), 0, uniforms_buffer_size);
    size_t new_pool_size = 0;
    {
      std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
      spirv_uniforms_pool_.push_back(buffer);
      spirv_uniforms_available_.push_back(buffer);
      new_pool_size = spirv_uniforms_pool_.size();
    }
    dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
    XELOGW(
        "SPIRV-Cross: grew uniforms pool to {} buffers at ring-wrap pressure",
        new_pool_size);
    return true;
  };

  if (try_acquire_uniforms_buffer()) {
    return true;
  }
  if (try_grow_uniforms_pool() && try_acquire_uniforms_buffer()) {
    return true;
  }

  static bool rollover_logged = false;
  if (!rollover_logged) {
    rollover_logged = true;
    XELOGW(
        "SPIRV-Cross: uniforms ring exhausted; rotating Metal command buffer");
  }

  EndCommandBuffer(CommandBufferKind::kSubmissionUniformsRollover);
  BeginCommandBuffer();
  if (!current_command_buffer_ || !current_render_encoder_ ||
      !uniforms_buffer_) {
    XELOGE(
        "SPIRV-Cross: failed to restart command buffer after uniforms ring "
        "rollover");
    return false;
  }
  return true;
}

void MetalCommandProcessor::ScheduleSpirvUniformBufferRelease(
    MTL::CommandBuffer* command_buffer) {
  if (!command_buffer) {
    return;
  }

  std::vector<MTL::Buffer*> submitted_uniforms;
  if (!command_buffer_spirv_uniform_buffers_.empty()) {
    submitted_uniforms.swap(command_buffer_spirv_uniform_buffers_);
  } else if (uniforms_buffer_) {
    submitted_uniforms.reserve(1);
    submitted_uniforms.push_back(uniforms_buffer_);
  }
  uniforms_buffer_ = nullptr;

  if (submitted_uniforms.empty()) {
    return;
  }

  pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
  command_buffer->addCompletedHandler(
      [this, submitted_uniforms =
                 std::move(submitted_uniforms)](MTL::CommandBuffer*) mutable {
        size_t returned_count = 0;
        {
          std::lock_guard<std::mutex> lock(spirv_uniforms_mutex_);
          for (MTL::Buffer* uniforms : submitted_uniforms) {
            if (!uniforms) {
              continue;
            }
            spirv_uniforms_available_.push_back(uniforms);
            ++returned_count;
          }
        }
        if (spirv_uniforms_available_semaphore_) {
          for (size_t i = 0; i < returned_count; ++i) {
            dispatch_semaphore_signal(spirv_uniforms_available_semaphore_);
          }
        }
        pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
      });
}

bool MetalCommandProcessor::AcquireSpirvArgumentBufferSlice(
    uint32_t bytes, uint32_t alignment, MTL::Buffer** buffer_out,
    NS::UInteger* offset_out) {
  if (!buffer_out || !offset_out) {
    return false;
  }
  *buffer_out = nullptr;
  *offset_out = 0;
  if (!device_ || !current_command_buffer_ || bytes == 0) {
    return false;
  }

  const size_t align = std::max<size_t>(1, size_t(alignment));
  auto align_up = [](size_t value, size_t alignment) -> size_t {
    return ((value + alignment - 1) / alignment) * alignment;
  };

  if (!command_buffer_spirv_argbuf_pages_.empty()) {
    auto& page = command_buffer_spirv_argbuf_pages_.back();
    const size_t aligned_offset = align_up(page->offset, align);
    if (aligned_offset + bytes <= page->bytes) {
      page->offset = aligned_offset + bytes;
      *buffer_out = page->buffer;
      *offset_out = NS::UInteger(aligned_offset);
      return *buffer_out != nullptr;
    }
  }

  constexpr size_t kDefaultSpirvArgumentBufferPageBytes = 1024 * 1024;
  const size_t required_page_bytes = align_up(bytes, align);
  const size_t page_bytes =
      std::max(kDefaultSpirvArgumentBufferPageBytes, required_page_bytes);

  std::shared_ptr<SpirvArgumentBufferPage> page;
  {
    std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
    for (auto it = spirv_argbuf_pool_.begin(); it != spirv_argbuf_pool_.end();
         ++it) {
      if ((*it) && (*it)->bytes >= page_bytes) {
        page = *it;
        spirv_argbuf_pool_.erase(it);
        break;
      }
    }
  }
  if (!page) {
    page = std::make_shared<SpirvArgumentBufferPage>();
    page->bytes = page_bytes;
    page->buffer =
        device_->newBuffer(page_bytes, MTL::ResourceStorageModeShared);
    if (!page->buffer) {
      return false;
    }
  }
  page->offset = 0;
  command_buffer_spirv_argbuf_pages_.push_back(page);

  const size_t aligned_offset = align_up(page->offset, align);
  if (aligned_offset + bytes > page->bytes) {
    return false;
  }
  page->offset = aligned_offset + bytes;
  *buffer_out = page->buffer;
  *offset_out = NS::UInteger(aligned_offset);
  return *buffer_out != nullptr;
}

void MetalCommandProcessor::ScheduleSpirvArgumentBufferRelease(
    MTL::CommandBuffer* command_buffer) {
  // Cached addresses become invalid when these pages enter the recycling pool.
  dxil_binder_.ResetUploadCaches();
  if (!command_buffer || command_buffer_spirv_argbuf_pages_.empty()) {
    return;
  }

  std::vector<std::shared_ptr<SpirvArgumentBufferPage>> pages;
  pages.swap(command_buffer_spirv_argbuf_pages_);

  bool add_handler = false;
  {
    std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
    auto& pending = pending_spirv_argbuf_releases_[command_buffer];
    add_handler = pending.empty();
    pending.reserve(pending.size() + pages.size());
    for (auto& page : pages) {
      pending.push_back(std::move(page));
    }
  }

  if (add_handler) {
    pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
    command_buffer->addCompletedHandler(
        [this](MTL::CommandBuffer* completed_cmd) {
          {
            std::lock_guard<std::mutex> lock(spirv_argbuf_mutex_);
            auto it = pending_spirv_argbuf_releases_.find(completed_cmd);
            if (it != pending_spirv_argbuf_releases_.end()) {
              for (auto& page : it->second) {
                if (page) {
                  page->offset = 0;
                  spirv_argbuf_pool_.push_back(std::move(page));
                }
              }
              pending_spirv_argbuf_releases_.erase(it);
            }
          }
          pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
        });
  }
}

void MetalCommandProcessor::JoinPipelineArchiveSave() {
  if (archive_save_thread_.joinable()) {
    archive_save_thread_.join();
  }
  archive_save_running_ = false;
}

void MetalCommandProcessor::MaybeSavePipelineArchive() {
  // Once the stored pipelines are rebuilt and nothing is compiling, so the
  // compile threads don't wait for the save to add theirs.
  if (archive_save_running_.load(std::memory_order_acquire) ||
      !pipeline_archive_.unsaved_additions() ||
      compile_activity.storage_loading.load(std::memory_order_relaxed)) {
    return;
  }
  if (compile_activity.queued.load(std::memory_order_relaxed) !=
      compile_activity.finished.load(std::memory_order_relaxed)) {
    return;
  }
  const uint64_t now = xe::Clock::QueryHostTickCount();
  if (now - archive_saved_ticks_ <
      kPipelineArchiveSaveSeconds * xe::Clock::QueryHostTickFrequency()) {
    return;
  }
  archive_saved_ticks_ = now;
  if (archive_save_thread_.joinable()) {
    archive_save_thread_.join();
  }
  archive_save_running_ = true;
  archive_save_thread_ = std::thread([this]() {
    pipeline_archive_.Save();
    archive_save_running_.store(false, std::memory_order_release);
  });
}

void MetalCommandProcessor::EndCommandBuffer(CommandBufferKind next_kind) {
  if (shader_storage_flush_needed_ || pipeline_storage_flush_needed_) {
    storage_writer_.RequestFlush(shader_storage_flush_needed_,
                                 pipeline_storage_flush_needed_);
    shader_storage_flush_needed_ = false;
    pipeline_storage_flush_needed_ = false;
  }
  MaybeSavePipelineArchive();
  if (current_command_buffer_) {
    next_submission_kind_ = next_kind;
  }
  primary_end_commit_at_pass_end_ = false;
  if (next_kind == CommandBufferKind::kSubmissionWait) {
    commit_after_drain_armed_ = true;
  } else if (current_command_buffer_) {
    commit_after_drain_armed_ = false;
  }
  EndRenderEncoder();

  if (current_command_buffer_) {
    // Only a command buffer fills these. Without one (the worker runs here on
    // every empty ring) they are already clear.
    ResetMslCrossEncoderReuseCaches();
    // Texture loads that need nothing from this command buffer run before it.
    if (texture_cache_) {
      texture_cache_->CommitLoadsAhead();
    }
    ScheduleSpirvUniformBufferRelease(current_command_buffer_);
    ScheduleSpirvArgumentBufferRelease(current_command_buffer_);
    current_command_buffer_->commit();
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    current_draw_index_ = 0;
    if (shared_memory_ && shared_memory_->hazards()) {
      shared_memory_->hazards()->Retire();
    }
    // Resolves recorded into it can now be waited for from a fault, so the
    // watches reads racing them used up are armed again.
    resolve_submitted_through_.store(submission_current_,
                                     std::memory_order_release);
    OnResolveSubmissionEnded();
  }
  copy_resolve_writes_pending_ = false;
  DrainCommandBufferAutoreleasePool();
}

void MetalCommandProcessor::ApplyDepthStencilState(
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
  SCOPE_profile_cpu_f("gpu");
  if (!current_render_encoder_ || !device_) {
    return;
  }

  const RegisterFile& regs = *register_file_;
  auto stencil_ref_mask_front = regs.Get<reg::RB_STENCILREFMASK>();
  auto stencil_ref_mask_back =
      regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto depth_control = normalized_depth_control;

  bool has_depth_attachment = false;
  bool has_stencil_attachment = false;
  if (current_render_pass_descriptor_) {
    if (auto* depth_attachment =
            current_render_pass_descriptor_->depthAttachment()) {
      has_depth_attachment = depth_attachment->texture() != nullptr;
    }
    if (auto* stencil_attachment =
            current_render_pass_descriptor_->stencilAttachment()) {
      has_stencil_attachment = stencil_attachment->texture() != nullptr;
    }
  }

  // Without a depth attachment, the depth test passes and nothing is written
  // either way, but Metal rejects a state that enables either.
  if (!has_depth_attachment) {
    depth_control.z_enable = 0;
    depth_control.z_write_enable = 0;
  }

  if (!has_stencil_attachment && depth_control.stencil_enable) {
    static bool no_stencil_logged = false;
    if (!no_stencil_logged) {
      no_stencil_logged = true;
      XELOGW(
          "Metal: stencil enabled but no stencil attachment bound; disabling "
          "stencil for this pass");
    }
    depth_control.stencil_enable = 0;
    depth_control.backface_enable = 0;
    depth_control.stencilfunc = xenos::CompareFunction::kAlways;
    depth_control.stencilfail = xenos::StencilOp::kKeep;
    depth_control.stencilzpass = xenos::StencilOp::kKeep;
    depth_control.stencilzfail = xenos::StencilOp::kKeep;
    depth_control.stencilfunc_bf = xenos::CompareFunction::kAlways;
    depth_control.stencilfail_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzpass_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzfail_bf = xenos::StencilOp::kKeep;
    stencil_ref_mask_front.value = 0;
    stencil_ref_mask_back.value = 0;
  }

  DepthStencilStateKey key;
  key.depth_control = depth_control.value;
  key.stencil_ref_mask_front = stencil_ref_mask_front.value;
  key.stencil_ref_mask_back = stencil_ref_mask_back.value;
  key.polygonal_and_backface = (primitive_polygonal ? 1u : 0u) |
                               (depth_control.backface_enable ? 2u : 0u);

  MTL::DepthStencilState* state = nullptr;
  auto it = depth_stencil_state_cache_.find(key);
  if (it != depth_stencil_state_cache_.end()) {
    state = it->second;
  } else {
    MTL::DepthStencilDescriptor* ds_desc =
        MTL::DepthStencilDescriptor::alloc()->init();
    if (depth_control.z_enable) {
      ds_desc->setDepthCompareFunction(
          ToMetalCompareFunction(depth_control.zfunc));
      ds_desc->setDepthWriteEnabled(depth_control.z_write_enable != 0);
    } else {
      ds_desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
      ds_desc->setDepthWriteEnabled(false);
    }

    if (depth_control.stencil_enable) {
      auto* front = MTL::StencilDescriptor::alloc()->init();
      front->setStencilCompareFunction(
          ToMetalCompareFunction(depth_control.stencilfunc));
      front->setStencilFailureOperation(
          ToMetalStencilOperation(depth_control.stencilfail));
      front->setDepthFailureOperation(
          ToMetalStencilOperation(depth_control.stencilzfail));
      front->setDepthStencilPassOperation(
          ToMetalStencilOperation(depth_control.stencilzpass));
      front->setReadMask(stencil_ref_mask_front.stencilmask);
      front->setWriteMask(stencil_ref_mask_front.stencilwritemask);

      ds_desc->setFrontFaceStencil(front);

      if (primitive_polygonal && depth_control.backface_enable) {
        auto* back = MTL::StencilDescriptor::alloc()->init();
        back->setStencilCompareFunction(
            ToMetalCompareFunction(depth_control.stencilfunc_bf));
        back->setStencilFailureOperation(
            ToMetalStencilOperation(depth_control.stencilfail_bf));
        back->setDepthFailureOperation(
            ToMetalStencilOperation(depth_control.stencilzfail_bf));
        back->setDepthStencilPassOperation(
            ToMetalStencilOperation(depth_control.stencilzpass_bf));
        back->setReadMask(stencil_ref_mask_back.stencilmask);
        back->setWriteMask(stencil_ref_mask_back.stencilwritemask);
        ds_desc->setBackFaceStencil(back);
        back->release();
      } else {
        ds_desc->setBackFaceStencil(front);
      }

      front->release();
    }

    state = device_->newDepthStencilState(ds_desc);
    ds_desc->release();

    if (!state) {
      XELOGE("Failed to create Metal depth/stencil state");
      return;
    }
    depth_stencil_state_cache_.emplace(key, state);
  }

  if (state != msl_depth_stencil_state_) {
    current_render_encoder_->setDepthStencilState(state);
    msl_depth_stencil_state_ = state;
  }

  if (depth_control.stencil_enable) {
    uint32_t ref_front = stencil_ref_mask_front.stencilref;
    uint32_t ref_back = primitive_polygonal && depth_control.backface_enable
                            ? stencil_ref_mask_back.stencilref
                            : ref_front;
    current_render_encoder_->setStencilReferenceValues(ref_front, ref_back);
  }
}

void MetalCommandProcessor::ApplyRasterizerState(bool primitive_polygonal) {
  SCOPE_profile_cpu_f("gpu");
  if (!current_render_encoder_ || !render_target_cache_) {
    return;
  }

  const RegisterFile& regs = *register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();

  MTL::CullMode cull_mode = MTL::CullModeNone;
  if (primitive_polygonal) {
    bool cull_front = pa_su_sc_mode_cntl.cull_front;
    bool cull_back = pa_su_sc_mode_cntl.cull_back;
    if (cull_front && !cull_back) {
      cull_mode = MTL::CullModeFront;
    } else if (cull_back && !cull_front) {
      cull_mode = MTL::CullModeBack;
    }
  }
  MTL::Winding winding = pa_su_sc_mode_cntl.face ? MTL::WindingClockwise
                                                 : MTL::WindingCounterClockwise;

  MTL::TriangleFillMode fill_mode = MTL::TriangleFillModeFill;
  if (primitive_polygonal &&
      pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
    xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
    if (!pa_su_sc_mode_cntl.cull_front) {
      polygon_type =
          std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
    }
    if (!pa_su_sc_mode_cntl.cull_back) {
      polygon_type =
          std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
    }
    if (polygon_type != xenos::PolygonType::kTriangles) {
      fill_mode = MTL::TriangleFillModeLines;
    }
  }

  float polygon_offset_scale = 0.0f;
  float polygon_offset = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(
      regs, primitive_polygonal, polygon_offset_scale, polygon_offset);
  float depth_bias_constant =
      static_cast<float>(draw_util::GetD3D10IntegerPolygonOffset(
          regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset));
  float depth_bias_slope =
      polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit *
      float(std::max(render_target_cache_->draw_resolution_scale_x(),
                     render_target_cache_->draw_resolution_scale_y()));
  if (NeedsExactFloatDepthBias(regs)) {
    depth_bias_constant = depth_bias_slope = 0.0f;
  }

  // With force_depth_clamp, use the host viewport clamp instead of near and far
  // Z plane clipping. X/Y/W clipping is unchanged.
  MTL::DepthClipMode depth_clip_mode =
      (pa_cl_clip_cntl.clip_disable || cvars::force_depth_clamp)
          ? MTL::DepthClipModeClamp
          : MTL::DepthClipModeClip;

  // Encoder state persists until changed, so only what differs from the last
  // draw in this encoder is set.
  bool all = !msl_rasterizer_state_valid_;
  if (all || cull_mode != msl_cull_mode_) {
    current_render_encoder_->setCullMode(cull_mode);
  }
  if (all || winding != msl_winding_) {
    current_render_encoder_->setFrontFacingWinding(winding);
  }
  if (all || fill_mode != msl_fill_mode_) {
    current_render_encoder_->setTriangleFillMode(fill_mode);
  }
  if (all || depth_bias_constant != msl_depth_bias_constant_ ||
      depth_bias_slope != msl_depth_bias_slope_) {
    current_render_encoder_->setDepthBias(depth_bias_constant, depth_bias_slope,
                                          0.0f);
  }
  if (all || depth_clip_mode != msl_depth_clip_mode_) {
    current_render_encoder_->setDepthClipMode(depth_clip_mode);
  }
  msl_rasterizer_state_valid_ = true;
  msl_cull_mode_ = cull_mode;
  msl_winding_ = winding;
  msl_fill_mode_ = fill_mode;
  msl_depth_bias_constant_ = depth_bias_constant;
  msl_depth_bias_slope_ = depth_bias_slope;
  msl_depth_clip_mode_ = depth_clip_mode;
}

MTL::RenderPassDescriptor*
MetalCommandProcessor::GetCurrentRenderPassDescriptor() {
  return render_pass_descriptor_;
}

// ==========================================================================
// SPIRV-Cross tessellation support.
// ==========================================================================

// Tessellation factor compute kernels are defined in msl_tess_factor_kernels.h.
#include "xenia/gpu/metal/msl_tess_factor_kernels.h"

bool MetalCommandProcessor::InitializeMslTessellation() {
  if (!device_) {
    return false;
  }

  auto compile_kernel =
      [&](const char* source,
          const char* function_name) -> MTL::ComputePipelineState* {
    NS::Error* error = nullptr;
    auto* src = NS::String::string(source, NS::UTF8StringEncoding);
    auto* opts = MTL::CompileOptions::alloc()->init();
    opts->setFastMathEnabled(true);
    MTL::Library* lib = device_->newLibrary(src, opts, &error);
    opts->release();
    if (!lib) {
      if (error) {
        XELOGE("Tessellation kernel compile error: {}",
               error->localizedDescription()->utf8String());
      }
      return nullptr;
    }
    auto* fn_name = NS::String::string(function_name, NS::UTF8StringEncoding);
    MTL::Function* fn = lib->newFunction(fn_name);
    lib->release();
    if (!fn) {
      XELOGE("Tessellation kernel: function '{}' not found", function_name);
      return nullptr;
    }
    MTL::ComputePipelineState* pso =
        device_->newComputePipelineState(fn, &error);
    fn->release();
    if (!pso && error) {
      XELOGE("Tessellation kernel PSO error: {}",
             error->localizedDescription()->utf8String());
    }
    return pso;
  };

  // Uniform factor kernels (discrete / continuous modes).
  tess_factor_pipeline_tri_ =
      compile_kernel(kMslTessFactorUniformTriangle, "tess_factor_triangle");
  tess_factor_pipeline_quad_ =
      compile_kernel(kMslTessFactorUniformQuad, "tess_factor_quad");

  if (!tess_factor_pipeline_tri_ || !tess_factor_pipeline_quad_) {
    XELOGW(
        "SPIRV-Cross: Failed to create uniform tessellation factor "
        "pipelines");
    return false;
  }

  // Adaptive factor kernels (per-edge factors from shared memory).
  tess_factor_pipeline_adaptive_tri_ = compile_kernel(
      kMslTessFactorAdaptiveTriangle, "tess_factor_adaptive_triangle");
  tess_factor_pipeline_adaptive_quad_ =
      compile_kernel(kMslTessFactorAdaptiveQuad, "tess_factor_adaptive_quad");

  if (!tess_factor_pipeline_adaptive_tri_ ||
      !tess_factor_pipeline_adaptive_quad_) {
    XELOGW(
        "SPIRV-Cross: Failed to create adaptive tessellation factor "
        "pipelines (adaptive tessellation will fall back to uniform)");
    // Non-fatal — adaptive tessellation will degrade to uniform factors.
  }

  XELOGI("SPIRV-Cross: Tessellation factor pipelines initialized");
  return true;
}

void MetalCommandProcessor::ShutdownMslTessellation() {
  if (tess_factor_buffer_) {
    tess_factor_buffer_->release();
    tess_factor_buffer_ = nullptr;
    tess_factor_buffer_patch_capacity_ = 0;
  }
  if (tess_factor_pipeline_tri_) {
    tess_factor_pipeline_tri_->release();
    tess_factor_pipeline_tri_ = nullptr;
  }
  if (tess_factor_pipeline_quad_) {
    tess_factor_pipeline_quad_->release();
    tess_factor_pipeline_quad_ = nullptr;
  }
  if (tess_factor_pipeline_adaptive_tri_) {
    tess_factor_pipeline_adaptive_tri_->release();
    tess_factor_pipeline_adaptive_tri_ = nullptr;
  }
  if (tess_factor_pipeline_adaptive_quad_) {
    tess_factor_pipeline_adaptive_quad_->release();
    tess_factor_pipeline_adaptive_quad_ = nullptr;
  }
}

bool MetalCommandProcessor::EnsureTessFactorBuffer(uint32_t patch_count) {
  // MTLQuadTessellationFactorsHalf is the larger of the two (12 bytes vs 8).
  constexpr size_t kMaxFactorSize = 12;
  size_t needed = size_t(patch_count) * kMaxFactorSize;
  if (tess_factor_buffer_ && tess_factor_buffer_->length() >= needed) {
    // Buffer already large enough; don't reduce the tracked capacity.
    return true;
  }
  if (tess_factor_buffer_) {
    tess_factor_buffer_->release();
  }
  // Round up and over-allocate for future growth.
  size_t alloc_size = std::max(needed, size_t(4096));
  alloc_size = (alloc_size + 4095) & ~size_t(4095);
  // Use Shared storage so the CPU can fill tessellation factors directly
  // (avoids needing a separate compute encoder for uniform factors).
  tess_factor_buffer_ =
      device_->newBuffer(alloc_size, MTL::ResourceStorageModeShared);
  if (!tess_factor_buffer_) {
    XELOGE("Failed to allocate tessellation factor buffer ({} bytes)",
           alloc_size);
    return false;
  }
  tess_factor_buffer_->setLabel(
      NS::String::string("Xenia Tess Factor Buffer", NS::UTF8StringEncoding));
  tess_factor_buffer_patch_capacity_ = uint32_t(alloc_size / kMaxFactorSize);
  return true;
}

MTL::RenderPipelineState*
MetalCommandProcessor::GetOrCreateMslTessPipelineState(
    MslShader::MslTranslation* domain_translation,
    MslShader::MslTranslation* pixel_translation,
    Shader::HostVertexShaderType host_vertex_shader_type,
    const RegisterFile& regs, PipelineCompileStatus* compile_status_out) {
  if (compile_status_out) {
    *compile_status_out = PipelineCompileStatus::kFailed;
  }
  MTL::Function* domain_function = GetHostShaderFunction(domain_translation);
  if (!domain_function) {
    XELOGE("SPIRV-Cross tess: no domain shader function");
    return nullptr;
  }

  PipelineCompileRequest request = {};
  PopulatePipelineCompileRequest(regs, domain_translation, pixel_translation,
                                 request);
  request.description.kind = PipelineKind::kMslTessellation;
  request.description.tessellation_mode =
      regs.Get<reg::VGT_HOS_CNTL>().tess_mode;
  request.description.host_vertex_shader_type = host_vertex_shader_type;
  request.vertex_function = domain_function;
  request.fragment_function = GetHostShaderFunction(pixel_translation);
  // The depth-only fragment fallback writes depth, so a pass without a depth
  // target still needs a pipeline depth format.
  bool fragment_writes_depth =
      (pixel_translation && pixel_translation->shader().writes_depth()) ||
      (!pixel_translation && depth_only_pixel_library_ &&
       !depth_only_pixel_function_name_.empty());
  MTL::PixelFormat depth_format =
      MTL::PixelFormat(request.description.depth_format);
  EnsureDepthFormatForDepthWritingFragment(
      "SPIRV-Cross tess pipeline", fragment_writes_depth, &depth_format);
  request.description.depth_format = uint32_t(depth_format);
  request.pipeline_key = request.description.GetHash();
  return AcquirePipelineState(request, /*allow_async=*/true,
                              compile_status_out);
}

SpirvShaderTranslator::Modification
MetalCommandProcessor::GetCurrentSpirvVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  const auto& regs = *register_file_;

  SpirvShaderTranslator::Modification modification(
      spirv_shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    modification.vertex.tessellation_mode =
        regs.Get<reg::VGT_HOS_CNTL>().tess_mode;
  }

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);

  if (host_vertex_shader_type ==
      Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                     xenos::PrimitiveType::kPointList);
  }

  return modification;
}

SpirvShaderTranslator::Modification
MetalCommandProcessor::GetCurrentSpirvPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) const {
  const auto& regs = *register_file_;

  SpirvShaderTranslator::Modification modification(
      spirv_shader_translator_->GetDefaultPixelShaderModification(
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

  using DepthStencilMode =
      SpirvShaderTranslator::Modification::DepthStencilMode;
  if (NeedsExactFloatDepthBias(regs) && !shader.writes_depth()) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kPolygonOffset;
  } else if (::cvars::depth_float24_convert_in_pixel_shader &&
             normalized_depth_control.z_enable &&
             regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                 xenos::DepthRenderTargetFormat::kD24FS8) {
    modification.pixel.depth_stencil_mode =
        ::cvars::depth_float24_round ? DepthStencilMode::kFloat24Rounding
                                     : DepthStencilMode::kFloat24Truncating;
  } else if (shader.implicit_early_z_write_allowed() &&
             (!shader.writes_color_target(0) ||
              !draw_util::DoesCoverageDependOnAlpha(
                  regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }

  // Initialize MIN/MAX blend pre-multiply factors to kOne (no pre-multiply).
  modification.pixel.rt0_blend_rgb_factor_for_premult =
      xenos::BlendFactor::kOne;
  modification.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;

  bool rt0_minmax_premult_rgb_expected = false;
  bool rt0_minmax_premult_a_expected = false;
  // A gamma or 16-bit fixed point target is encoded by the shader, and if it
  // blends, blended there with the factors applied before MIN and MAX.
  xenos::ColorRenderTargetFormat rt0_format =
      regs.Get<reg::RB_COLOR_INFO>().color_format;
  if (shader.writes_color_target(0) &&
      !(render_target_cache_->gamma_fetch_blend() &&
        rt0_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) &&
      !(render_target_cache_->fixed16_fetch_blend() &&
        (rt0_format == xenos::ColorRenderTargetFormat::k_16_16 ||
         rt0_format == xenos::ColorRenderTargetFormat::k_16_16_16_16))) {
    auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[0]);
    rt0_minmax_premult_rgb_expected =
        (blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.color_destblend == xenos::BlendFactor::kOne;
    rt0_minmax_premult_a_expected =
        (blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.alpha_destblend == xenos::BlendFactor::kOne;
    if (rt0_minmax_premult_rgb_expected) {
      modification.pixel.rt0_blend_rgb_factor_for_premult =
          xenos::BlendFactor::kSrcAlpha;
    }
    if (rt0_minmax_premult_a_expected) {
      modification.pixel.rt0_blend_a_factor_for_premult =
          xenos::BlendFactor::kSrcAlpha;
    }
  }
  if (rt0_minmax_premult_rgb_expected || rt0_minmax_premult_a_expected) {
    XELOGD(
        "SPIRV-Cross PS mod diagnostic: shader={:016X} expected_premult(rgb={},"
        "a={}) selected(rgb={},a={})",
        shader.ucode_data_hash(), rt0_minmax_premult_rgb_expected ? 1 : 0,
        rt0_minmax_premult_a_expected ? 1 : 0,
        uint32_t(modification.pixel.rt0_blend_rgb_factor_for_premult),
        uint32_t(modification.pixel.rt0_blend_a_factor_for_premult));
  }

  // Extract 1 bit per RT from the 4-bits-per-RT normalized_color_mask.
  // Without this, color_targets_used defaults to 0 and the SPIR-V translator
  // declares NO fragment color outputs, producing black/transparent rendering.
  modification.pixel.color_targets_used =
      GetColorTargetsWritten(normalized_color_mask);

  // Only shaders writing a bound 16_16 or 16_16_16_16 target carry the
  // console's fixed point encode, so the others compile as without it.
  // Only draws blending into a gamma or 16-bit fixed point target read their
  // targets back: a pixel shader that reads a target can't have its fragments
  // removed as hidden. The shader reads back only the targets it blends, which
  // color_targets_used then holds. Metal rejects a pipeline reading an
  // attachment the pass doesn't have, so those must be bound. A target that
  // fixed-function blending writes must not be declared for reading either:
  // with RT0 read and RT1 blended at 4x MSAA, some samples of RT1 lost their
  // blend or were blended over zero, varying run to run.
  if (spirv_translator_features_.host_render_target_feedback) {
    uint32_t bound_color_targets =
        (render_target_cache_->GetLastUpdateBoundRenderTargets() >> 1) &
        ((UINT32_C(1) << xenos::kMaxColorRenderTargets) - 1);
    uint32_t rts = shader.writes_color_targets() &
                   modification.pixel.color_targets_used & bound_color_targets;
    uint32_t shader_blended_rts = 0;
    uint32_t rt;
    while (xe::bit_scan_forward(rts, &rt)) {
      rts &= ~(UINT32_C(1) << rt);
      if (render_target_cache_->fixed16_fetch_blend() &&
          IsFixed16Format(regs.Get<reg::RB_COLOR_INFO>(
                                  reg::RB_COLOR_INFO::rt_register_indices[rt])
                              .color_format)) {
        modification.pixel.host_render_target_fixed16 = 1;
      }
      if (GetShaderBlendControl(regs, rt, normalized_color_mask,
                                render_target_cache_->gamma_fetch_blend(),
                                render_target_cache_->fixed16_fetch_blend()) !=
          kBlendControlNoBlending) {
        shader_blended_rts |= UINT32_C(1) << rt;
      }
    }
    if (shader_blended_rts) {
      modification.pixel.host_render_target_feedback = 1;
      modification.pixel.color_targets_used = shader_blended_rts;
    }
  }

  return modification;
}

uint64_t MetalCommandProcessor::PopulatePipelineCompileRequest(
    const RegisterFile& regs, const Shader::Translation* vertex_translation,
    const Shader::Translation* pixel_translation,
    PipelineCompileRequest& request) {
  // Match the active render pass attachments exactly to avoid
  // setRenderPipelineState validation failures when the pass descriptor differs
  // from the cache snapshot (for instance, after attachment reconfiguration).
  // Without a pass, determine attachment formats from render target cache.
  uint32_t sample_count = 1;
  MTL::PixelFormat color_formats[4] = {
      MTL::PixelFormatInvalid, MTL::PixelFormatInvalid, MTL::PixelFormatInvalid,
      MTL::PixelFormatInvalid};
  MTL::PixelFormat depth_format = MTL::PixelFormatInvalid;
  MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
  if (current_render_pass_descriptor_) {
    const RenderPassPipelineFormats& formats =
        current_render_pass_pipeline_formats_;
    std::copy(std::begin(formats.color), std::end(formats.color),
              color_formats);
    depth_format = formats.depth;
    stencil_format = formats.stencil;
    sample_count = formats.sample_count;
  } else if (!render_target_cache_) {
    PopulatePipelineFormatsFromRenderPassDescriptor(
        render_pass_descriptor_, color_formats, 4, &depth_format,
        &stencil_format, &sample_count);
  } else {
    for (uint32_t i = 0; i < 4; ++i) {
      if (MTL::Texture* rt = render_target_cache_->GetColorTargetForDraw(i)) {
        color_formats[i] = rt->pixelFormat();
        if (rt->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(rt->sampleCount()));
        }
      }
    }
    if (color_formats[0] == MTL::PixelFormatInvalid) {
      if (MTL::Texture* dummy =
              render_target_cache_->GetDummyColorTargetForDraw()) {
        color_formats[0] = dummy->pixelFormat();
        if (dummy->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(dummy->sampleCount()));
        }
      }
    }
    if (MTL::Texture* depth_tex =
            render_target_cache_->GetDepthTargetForDraw()) {
      depth_format = depth_tex->pixelFormat();
      stencil_format =
          MetalRenderTargetCache::GetStencilAttachmentPixelFormat(depth_format);
      if (depth_tex->sampleCount() > 0) {
        sample_count = std::max<uint32_t>(
            sample_count, static_cast<uint32_t>(depth_tex->sampleCount()));
      }
    }
  }
  bool pixel_shader_writes_depth =
      pixel_translation && pixel_translation->shader().writes_depth();
  EnsureDepthFormatForDepthWritingFragment(
      "Metal pipeline", pixel_shader_writes_depth, &depth_format);

  // Record the guest ucode alongside the pipeline that uses it, so the next run
  // has something to rebuild from.
  if (storage_writer_.is_active()) {
    for (const Shader::Translation* translation :
         {vertex_translation, pixel_translation}) {
      if (!translation) {
        continue;
      }
      Shader& shader = translation->shader();
      if (shader.try_set_ucode_storage_index(storage_writer_.storage_index())) {
        storage_writer_.QueueShaderWrite(&shader);
        shader_storage_flush_needed_ = true;
      }
    }
  }

  PipelineDescription& description = request.description;
  if (vertex_translation) {
    description.vertex_shader_hash =
        vertex_translation->shader().ucode_data_hash();
    description.vertex_shader_modification = vertex_translation->modification();
  }
  if (pixel_translation) {
    description.pixel_shader_hash =
        pixel_translation->shader().ucode_data_hash();
    description.pixel_shader_modification = pixel_translation->modification();
  }
  description.sample_count = sample_count;
  description.depth_format = uint32_t(depth_format);
  description.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    description.color_formats[i] = uint32_t(color_formats[i]);
  }
  uint32_t pixel_shader_writes_color_targets =
      pixel_translation ? pixel_translation->shader().writes_color_targets()
                        : 0;
  description.normalized_color_mask =
      pixel_shader_writes_color_targets
          ? draw_util::GetNormalizedColorMask(regs,
                                              pixel_shader_writes_color_targets)
          : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    // ApplyColorAttachmentState only reads the blend register for a bound RT
    // with a non-zero write mask; zeroing it elsewhere lets those draws share
    // one pipeline.
    uint32_t rt_write_mask =
        (description.normalized_color_mask >> (i * 4)) & 0xF;
    description.blendcontrol[i] =
        (rt_write_mask && color_formats[i] != MTL::PixelFormatInvalid)
            ? regs.Get<reg::RB_BLENDCONTROL>(
                      reg::RB_BLENDCONTROL::rt_register_indices[i])
                  .value
            : 0u;
    // Gamma and 16-bit fixed point targets the pixel shader reads back and
    // blends itself.
    if (pixel_translation && rt_write_mask &&
        color_formats[i] != MTL::PixelFormatInvalid) {
      SpirvShaderTranslator::Modification pixel_modification(
          pixel_translation->modification());
      if (pixel_modification.pixel.host_render_target_feedback &&
          (pixel_modification.pixel.color_targets_used & (1u << i))) {
        description.shader_blend_mask |= 1u << i;
      }
    }
  }
  SetPipelineFunctionConstants(request, vertex_translation, pixel_translation);
  description.vertex_fetch_states_specialized =
      request.vertex_fetch_states_specialized;
  description.fragment_fetch_states_specialized =
      request.fragment_fetch_states_specialized;
  description.finite_multiply_enabled =
      spirv_translator_features_.optimize_finite_multiplication;
  auto get_bool_loop_constants = [&regs](const Shader::Translation* translation,
                                         uint32_t* bool_constants,
                                         uint32_t* loop_constants) {
    const auto& constants = translation->shader().constant_register_map();
    for (uint32_t i = 0; i < 256 / 32; ++i) {
      bool_constants[i] =
          regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i] &
          constants.bool_bitmap[i];
    }
    uint32_t loops = constants.loop_bitmap;
    uint32_t i;
    while (xe::bit_scan_forward(loops, &i)) {
      loops &= ~(UINT32_C(1) << i);
      loop_constants[i] = regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i];
    }
  };
  if (request.vertex_fetch_states_specialized) {
    const uint32_t* fetch = &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
    for (const Shader::VertexBinding& binding :
         vertex_translation->shader().vertex_bindings()) {
      uint32_t i = binding.fetch_constant;
      description.vertex_fetch_endians[i >> 4] |= (fetch[2 * i + 1] & 3)
                                                  << (2 * (i & 15));
    }
    get_bool_loop_constants(vertex_translation,
                            description.vertex_bool_constants,
                            description.vertex_loop_constants);
    for (const Shader::TextureBinding& binding :
         vertex_translation->shader().texture_bindings()) {
      description.vertex_texture_scale_bits[binding.fetch_constant] =
          texture_cache_->GetFetchIntegerScaleBits(binding.fetch_constant);
      description.vertex_texture_decode_bits[binding.fetch_constant] =
          texture_cache_->GetFetchDecodeBits(binding.fetch_constant);
    }
    if (spirv_translator_features_.optimize_finite_multiplication) {
      GetFiniteMultiplyState(vertex_translation, regs,
                             description.vertex_finite_multiply_state);
    }
  }
  if (request.fragment_fetch_states_specialized) {
    get_bool_loop_constants(pixel_translation, description.pixel_bool_constants,
                            description.pixel_loop_constants);
    for (const Shader::TextureBinding& binding :
         pixel_translation->shader().texture_bindings()) {
      description.pixel_texture_scale_bits[binding.fetch_constant] =
          texture_cache_->GetFetchIntegerScaleBits(binding.fetch_constant);
      description.pixel_texture_decode_bits[binding.fetch_constant] =
          texture_cache_->GetFetchDecodeBits(binding.fetch_constant);
    }
    if (spirv_translator_features_.optimize_finite_multiplication) {
      GetFiniteMultiplyState(pixel_translation, regs,
                             description.pixel_finite_multiply_state);
    }
  }
  request.priority = pixel_translation ? kPipelinePriorityDraw
                                       : kPipelinePriorityDrawDepthOnly;
  request.pipeline_key = description.GetHash();
  return request.pipeline_key;
}

MTL::RenderPipelineState* MetalCommandProcessor::GetOrCreatePipelineState(
    const Shader::Translation* vertex_translation,
    const Shader::Translation* pixel_translation, const RegisterFile& regs,
    PipelineCompileStatus* compile_status_out) {
  if (compile_status_out) {
    *compile_status_out = PipelineCompileStatus::kFailed;
  }
  MTL::Function* vertex_function = GetHostShaderFunction(vertex_translation);
  if (!vertex_function) {
    XELOGE("Metal: no valid vertex shader function");
    return nullptr;
  }

  PipelineCompileRequest request = {};
  PopulatePipelineCompileRequest(regs, vertex_translation, pixel_translation,
                                 request);
  request.vertex_function = vertex_function;
  request.fragment_function = GetHostShaderFunction(pixel_translation);
  MTL::Function* exact_depth_function = nullptr;
  if (UseDxilPath() && !pixel_translation && NeedsExactFloatDepthBias(regs)) {
    if (!exact_float_depth_bias_library_) {
      auto translator = CreateSpirvShaderTranslator();
      auto spirv = translator->CreateDepthOnlyFragmentShader(
          SpirvShaderTranslator::Modification::DepthStencilMode::
              kPolygonOffset);
      spirv2air::Options options;
      options.halfPackingRounding = spirv2air::HalfPackingRounding::TowardZero;
      options.graphics.functionConstantSet =
          SpirvShaderTranslator::kDescriptorSetFetchState;
      auto converted =
          spirv2air::translate(reinterpret_cast<const uint32_t*>(spirv.data()),
                               spirv.size() / sizeof(uint32_t), options);
      if (!converted.ok()) {
        XELOGE("Depth-only bias conversion: {}", converted.error);
        return nullptr;
      }
      NS::Error* error = nullptr;
      auto data = dispatch_data_create(converted.metallib.data(),
                                       converted.metallib.size(), nullptr,
                                       DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      exact_float_depth_bias_library_ = device_->newLibrary(data, &error);
      dispatch_release(data);
      if (!exact_float_depth_bias_library_) {
        XELOGE("Depth-only bias library failed");
        return nullptr;
      }
    }
    exact_depth_function = exact_float_depth_bias_library_->newFunction(
        NS::String::string("main", NS::UTF8StringEncoding));
    if (!exact_depth_function) {
      return nullptr;
    }
    request.fragment_function = exact_depth_function;
    SpirvShaderTranslator::Modification depth_modification;
    depth_modification.pixel.depth_stencil_mode =
        SpirvShaderTranslator::Modification::DepthStencilMode::kPolygonOffset;
    request.description.pixel_shader_modification = depth_modification.value;
    request.pipeline_key = request.description.GetHash();
  }
  // A pipeline with no pixel shader is quick to link and is what a placeholder
  // draw waits on, so it is built here rather than queued.
  auto* result = AcquirePipelineState(request, pixel_translation != nullptr,
                                      compile_status_out);
  if (exact_depth_function) {
    exact_depth_function->release();
  }
  return result;
}

MTL::RenderPipelineState* MetalCommandProcessor::AcquirePipelineState(
    const PipelineCompileRequest& request, bool allow_async,
    PipelineCompileStatus* compile_status_out) {
  SCOPE_profile_cpu_f("gpu");
  if (compile_status_out) {
    *compile_status_out = PipelineCompileStatus::kFailed;
  }
  const uint64_t key = request.pipeline_key;

  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    auto it = async_pipeline_cache_.find(key);
    if (it == async_pipeline_cache_.end() && !replaying_stored_pipelines_ &&
        async_pipeline_pending_.find(key) == async_pipeline_pending_.end() &&
        async_pipeline_failed_.find(key) == async_pipeline_failed_.end() &&
        storage_writer_.is_active() &&
        // This synthetic fragment has no guest microcode to reconstruct at
        // cache warm-up. Keep its PSO in memory and rebuild it on demand.
        (request.description.pixel_shader_hash ||
         !request.description.pixel_shader_modification) &&
        // A memexport format profile slot is a bounded, session-scoped pool
        // index. Persisting it would let the next launch replay this
        // modification with no guard that the slot still denotes the same
        // format words, and translations are never evicted once created. Guest
        // microcode is still stored, so these pipelines rebuild normally.
        !SpirvShaderTranslator::Modification(
             request.description.vertex_shader_modification)
             .vertex.memexport_format_specialized) {
      // Recorded before creation, so one that fails on this driver is still
      // described for another. Replay would re-record what it just read.
      PipelineStoredDescription stored;
      stored.description = request.description;
      stored.description_hash = key;
      storage_writer_.QueuePipelineWrite(stored);
      pipeline_storage_flush_needed_ = true;
    }
    if (it != async_pipeline_cache_.end()) {
      if (compile_status_out) {
        *compile_status_out = PipelineCompileStatus::kReady;
      }
      return it->second;
    }
    // Most likely queued by the storage load, behind the rest of the rebuild.
    // A caller that builds its pipelines here takes it back, as it would have
    // without the storage. One that queues moves it up to its own priority.
    if (async_pipeline_pending_.find(key) != async_pipeline_pending_.end() &&
        (allow_async || !TakeQueuedPipelineLocked(key))) {
      PromoteQueuedPipelineLocked(key, request.priority);
      last_pending_pipeline_key_ = key;
      if (compile_status_out) {
        *compile_status_out = PipelineCompileStatus::kPending;
      }
      return nullptr;
    }
    if (async_pipeline_failed_.find(key) != async_pipeline_failed_.end()) {
      return nullptr;
    }
  }

  if (allow_async) {
    if (EnqueuePipelineCompilation(request)) {
      std::lock_guard<std::mutex> lock(async_compile_mutex_);
      auto it = async_pipeline_cache_.find(key);
      if (it != async_pipeline_cache_.end()) {
        if (compile_status_out) {
          *compile_status_out = PipelineCompileStatus::kReady;
        }
        return it->second;
      }
      if (async_pipeline_failed_.find(key) != async_pipeline_failed_.end()) {
        return nullptr;
      }
      last_pending_pipeline_key_ = key;
      if (compile_status_out) {
        *compile_status_out = PipelineCompileStatus::kPending;
      }
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    if (async_pipeline_failed_.find(key) != async_pipeline_failed_.end()) {
      return nullptr;
    }
  }

  std::string error_message;
  MTL::RenderPipelineState* pipeline =
      CreatePipelineState(request, &error_message);
  if (!pipeline) {
    if (!error_message.empty()) {
      XELOGE("Metal: failed to create pipeline: {}", error_message);
    } else {
      XELOGE("Metal: failed to create pipeline (unknown error)");
    }
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    async_pipeline_failed_.insert(key);
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    auto [it, inserted] = async_pipeline_cache_.emplace(key, pipeline);
    if (!inserted) {
      pipeline->release();
      pipeline = it->second;
    }
    async_pipeline_failed_.erase(key);
  }
  if (compile_status_out) {
    *compile_status_out = PipelineCompileStatus::kReady;
  }
  return pipeline;
}

MTL::RenderPipelineState*
MetalCommandProcessor::GetOrCreatePlaceholderPipelineState(
    const Shader::Translation* vertex_translation, const RegisterFile& regs) {
  // A null pixel translation yields no fragment function and a zero color mask,
  // which is both the placeholder and what a depth-only draw builds.
  return GetOrCreatePipelineState(vertex_translation, nullptr, regs);
}

void MetalCommandProcessor::UpdateSpirvSystemConstantValues(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  SCOPE_profile_cpu_f("gpu");
  const SpirvShaderTranslator::SystemConstants previous_system_constants =
      spirv_system_constants_;

  const RegisterFile& regs = *register_file_;
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  auto& consts = spirv_system_constants_;
  std::memset(&consts, 0, sizeof(consts));

  if (NeedsExactFloatDepthBias(regs)) {
    auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    float scale =
        xenos::kPolygonOffsetScaleSubpixelUnit *
        float(std::max(render_target_cache_->draw_resolution_scale_x(),
                       render_target_cache_->draw_resolution_scale_y()));
    if (mode.poly_offset_front_enable) {
      consts.edram_poly_offset_front_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE) * scale;
      consts.edram_poly_offset_front_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET) * 0.5f;
    }
    if (mode.poly_offset_back_enable) {
      consts.edram_poly_offset_back_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE) * scale;
      consts.edram_poly_offset_back_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET) * 0.5f;
    }
  }

  // Build flags (matching Vulkan backend's SpirvShaderTranslator kSysFlag_*).
  uint32_t flags = 0;

  // Coordinate format.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }

  // Primitive type.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineList ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineStrip ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineLoop ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::k2DLineStrip) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }

  // MSAA sample count.
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;

  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }

  // Alpha test — pack the CompareFunction value directly into the flag bits
  // (matching Vulkan backend behavior).
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;

  // Gamma correction for render targets. RGBA16Unorm gamma targets contain
  // linear values; their linear-to-gamma conversion happens on the EDRAM
  // store, so the pixel shader must not pre-encode them.
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
  }
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }

  // Vertex index loading for VS-based primitive expansion (point sprites,
  // rectangle lists).  When the primitive processor builds a host-side
  // index buffer for DMA-based VS expansion the shader must load the
  // original guest vertex index from shared memory.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator::
          kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
  }

  consts.flags = flags;

  // Vertex index.
  consts.vertex_index_endian = index_endian;
  consts.vertex_base_index = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  const bool is_vs_expansion_draw =
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
  consts.vertex_index_count =
      is_vs_expansion_draw ? primitive_processing_result.guest_draw_vertex_count
                           : primitive_processing_result.host_draw_vertex_count;

  // Vertex index load address (for VS-based primitive expansion).
  if (flags &
      (SpirvShaderTranslator::kSysFlag_VertexIndexLoad |
       SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    consts.vertex_index_load_address =
        primitive_processing_result.guest_index_base;
  }

  // NDC scale/offset.
  for (uint32_t i = 0; i < 3; ++i) {
    consts.ndc_scale[i] = viewport_info.ndc_scale[i];
    consts.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point rendering (matching Vulkan backend).
  auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
  auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
  consts.point_vertex_diameter_min =
      float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
  consts.point_vertex_diameter_max =
      float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
  consts.point_constant_diameter[0] =
      float(pa_su_point_size.width) * (2.0f / 16.0f);
  consts.point_constant_diameter[1] =
      float(pa_su_point_size.height) * (2.0f / 16.0f);
  // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
  // to radius conversion — matching the Vulkan backend formula.
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  consts.point_screen_diameter_to_ndc_radius[0] =
      float(draw_resolution_scale_x) /
      float(std::max(viewport_info.xy_extent[0], uint32_t(1)));
  // Metal NDC Y increases upward; point T increases downward.
  consts.point_screen_diameter_to_ndc_radius[1] =
      -float(draw_resolution_scale_y) /
      float(std::max(viewport_info.xy_extent[1], uint32_t(1)));

  // Texture swizzled signs and swizzles — retrieved from the texture cache
  // (matching Vulkan backend behavior).
  if (texture_cache_) {
    for (uint32_t i = 0; i < 32; ++i) {
      if (!(used_texture_mask & (uint32_t(1) << i))) {
        continue;
      }
      // Swizzled signs: 8 bits per texture, 4 textures per uint32.
      uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(i);
      uint32_t signs_shift = 8 * (i & 3);
      consts.texture_swizzled_signs[i >> 2] |= uint32_t(texture_signs)
                                               << signs_shift;

      consts.texture_integer_scale_bits[i] =
          texture_cache_->GetActiveIntegerScaleBits(i);

      // Host swizzles: 12 bits per texture, 2 textures per uint32.
      uint32_t texture_swizzle = texture_cache_->GetActiveTextureHostSwizzle(i);
      uint32_t swizzle_shift = 12 * (i & 1);
      consts.texture_swizzles[i >> 1] |= (texture_swizzle & 0xFFF)
                                         << swizzle_shift;

      // Integer num_format scale, plus bit 24 for normalized rounding.
      consts.texture_integer_scale_bits[i] =
          texture_cache_->GetActiveIntegerScaleBits(i);
    }
  }

  // Textures resolved — which textures are from scaled resolve operations
  // (matching Vulkan backend).
  if (texture_cache_) {
    uint32_t textures_resolved = 0;
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      textures_resolved |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
    consts.textures_resolved = textures_resolved;
  }

  // Alpha test reference.
  consts.alpha_test_reference = rb_alpha_ref;

  // Alpha to mask — if enabled, bits 0:7 are sample offsets, bit 8 = 1.
  // (matching Vulkan backend / MSC path).
  if (rb_colorcontrol.alpha_to_mask_enable) {
    consts.alpha_to_mask = (rb_colorcontrol.value >> 24) | (1 << 8);
  }

  // Color exponent bias (matching Vulkan backend).
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t color_exp_bias = color_infos[i].color_exp_bias;
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    if (render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        ((color_infos[i].color_format ==
              xenos::ColorRenderTargetFormat::k_16_16 &&
          !render_target_cache_->IsFixedRG16TruncatedToMinus1To1()) ||
         (color_infos[i].color_format ==
              xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
          !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1()))) {
      if (render_target_cache_->fixed16_fetch_blend()) {
        // The shader encodes the EDRAM's bits itself: a negative scale marks
        // the target (SpirvShaderTranslator::Features::
        // host_render_target_fixed16).
        color_exp_bias_scale = -color_exp_bias_scale;
      } else {
        // Remap to -1...1 by scaling the output values by 1024/32767, so the
        // SNORM bits are the EDRAM ones apart from -32.
        color_exp_bias_scale *= 1024.0f / 32767.0f;
      }
    }
    consts.color_exp_bias[i] = color_exp_bias_scale;
  }

  // The blend constant of targets blended in the pixel shader. The pipeline
  // specializes their blend controls.
  if (spirv_translator_features_.host_render_target_feedback) {
    bool shader_blends = false;
    for (uint32_t i = 0; i < 4; ++i) {
      shader_blends |=
          GetShaderBlendControl(regs, i, normalized_color_mask,
                                render_target_cache_->gamma_fetch_blend(),
                                render_target_cache_->fixed16_fetch_blend()) !=
          kBlendControlNoBlending;
    }
    if (shader_blends) {
      consts.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
      consts.edram_blend_constant[1] =
          regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
      consts.edram_blend_constant[2] =
          regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
      consts.edram_blend_constant[3] =
          regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    }
  }

  // User clip planes and tessellation constants in the system constants.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  std::memset(spirv_system_constants_.user_clip_planes, 0,
              sizeof(spirv_system_constants_.user_clip_planes));
  if (!pa_cl_clip_cntl.clip_disable && pa_cl_clip_cntl.ucp_ena) {
    float* clip_plane_write_ptr = spirv_system_constants_.user_clip_planes[0];
    uint32_t clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t clip_plane_index;
    while (xe::bit_scan_forward(clip_planes_remaining, &clip_plane_index)) {
      clip_planes_remaining &= ~(UINT32_C(1) << clip_plane_index);
      const float* clip_plane_regs = reinterpret_cast<const float*>(
          &regs.values[XE_GPU_REG_PA_CL_UCP_0_X + clip_plane_index * 4]);
      std::memcpy(clip_plane_write_ptr, clip_plane_regs, 4 * sizeof(float));
      clip_plane_write_ptr += 4;
    }
  }
  spirv_system_constants_.tessellation_factor_range[0] =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  spirv_system_constants_.tessellation_factor_range[1] =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  spirv_system_constants_.tessellation_vertex_index_endian =
      static_cast<uint32_t>(index_endian);
  spirv_system_constants_.tessellation_vertex_index_offset =
      regs[XE_GPU_REG_VGT_INDX_OFFSET];
  spirv_system_constants_.tessellation_vertex_index_min_max[0] =
      regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
  spirv_system_constants_.tessellation_vertex_index_min_max[1] =
      regs[XE_GPU_REG_VGT_MAX_VTX_INDX];

  // The system constants version bump covers clip planes and tessellation,
  // which live in the same struct.
  if (std::memcmp(&previous_system_constants, &spirv_system_constants_,
                  sizeof(spirv_system_constants_)) != 0) {
    ++msl_system_constants_version_;
    if (msl_system_constants_version_ == 0) {
      msl_system_constants_version_ = 1;
    }
  }
}

#define COMMAND_PROCESSOR MetalCommandProcessor
#include "../pm4_command_processor_implement.h"
#undef COMMAND_PROCESSOR

}  // namespace metal
}  // namespace gpu
}  // namespace xe
