/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_pipeline_cache.h"

#include <dispatch/dispatch.h>
#include <pthread/qos.h>
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
#include "xenia/gpu/metal/metal_compile_activity.h"
#include "xenia/gpu/metal/metal_graphics_system.h"
#include "xenia/gpu/metal/metal_render_target_cache.h"
#include "xenia/gpu/metal/metal_shared_memory.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/metal/metal_util.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

DECLARE_bool(precise_interpolation);
DEFINE_bool(
    metal_bake_fetch_state, true,
    "Compile a shader's fetch-state function constants into its library "
    "(kept in the shader library cache) instead of having Metal specialize the "
    "generic function at every launch. Specializing is a compile of its own "
    "whenever the system shader cache is empty, as it is after an app update "
    "or a restart.",
    "Metal");
DEFINE_bool(
    metal_bake_fetch_state_uncached, false,
    "Bake the fetch-state constants (metal_bake_fetch_state) even when the "
    "title has no shader library cache to keep them in. For frame checks of "
    "baked code against Metal's specialization; the variants are rebuilt "
    "every run.",
    "Metal");
DEFINE_bool(
    metal_bake_fetch_state_generic_library, false,
    "Also compile the generic library (the one with function constants) of a "
    "shader whose pipelines all take baked libraries "
    "(metal_bake_fetch_state). It goes unused, so it is skipped by default; "
    "this is the off switch of the skip.",
    "Metal");
DEFINE_bool(
    metal_finite_multiply, true,
    "Skip the Xenos 0 * x = 0 guard on multiplications whose operands are "
    "proven finite.",
    "Metal");
DEFINE_int32(
    metal_pipeline_creation_threads, -1,
    "Number of threads used for shader and render pipeline compilation in the "
    "Metal backend. -1 to calculate automatically (75% of logical CPU cores), "
    "a positive number to specify the number of threads explicitly (up to the "
    "number of logical CPU cores), 0 to disable multithreaded compilation.",
    "Metal");
DEFINE_string(
    metal_compile_thread_qos, "default",
    "Quality of service of the shader and pipeline compile threads: default, "
    "user-initiated (ahead of the game's own threads, which run at default) or "
    "utility (behind them). The compile threads start at default, like any "
    "thread created without one.",
    "Metal");
DEFINE_bool(
    metal_finite_multiply_general_fallback, true,
    "When the pipeline for a draw's finite multiply state isn't built, draw "
    "with the pair's general variant (guarded products throughout) if that is "
    "built, and build the exact one in the background. Pipelines stored under "
    "another finite multiply analysis are rebuilt as the general variant.",
    "Metal");
DEFINE_bool(
    metal_finite_multiply_force_general, false,
    "Test: every pipeline uses the general variant of its finite multiply "
    "state. Must give the same frames and guest memory as off.",
    "Metal");
DEFINE_bool(
    metal_finite_multiply_prewarm_general, false,
    "Test: build the general variant before the first draw of each finite "
    "multiply state, as loading a description stored under another analysis "
    "does, so replays take the fallback. Must give the same frames and guest "
    "memory as off.",
    "Metal");

namespace xe {
namespace gpu {
namespace metal {

namespace {
constexpr int64_t kAsyncCompileLogIntervalNs =
    int64_t(std::chrono::nanoseconds(std::chrono::seconds(1)).count());

// Indexed by CommandBufferKind, for the shutdown summary. Submission kinds
// name what ended the previous submission.

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

// Float24 polygon offsets are absolute, not host floating-depth ULP counts.
bool MetalPipelineCache::NeedsExactFloatDepthBias(const RegisterFile& regs) {
  auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  // Limit this path to host-depth configurations with validated fragment
  // interpolation. MSAA needs per-sample depth, and tessellation has separate
  // pipeline assembly. Those paths retain their existing hardware bias.
  if (cvars::depth_float24_convert_in_pixel_shader ||
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

// Whether fixed-function blending of a 16_16 or 16_16_16_16 target's snorm
// attachment, holding the EDRAM's fixed point values, gives the console's
// result: with factors ONE and ZERO only, every ADD and subtraction of the
// console's blender is the exact sum or difference, and MIN and MAX only
// compare the values themselves with both factors ONE (the host's MIN and MAX
// ignore the factors).
bool MetalPipelineCache::IsFixed16Format(
    xenos::ColorRenderTargetFormat format) {
  return format == xenos::ColorRenderTargetFormat::k_16_16 ||
         format == xenos::ColorRenderTargetFormat::k_16_16_16_16;
}

bool MetalPipelineCache::IsFixed16BlendExact(xenos::BlendFactor src,
                                             xenos::BlendFactor dest,
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
uint32_t MetalPipelineCache::GetShaderBlendControl(
    const RegisterFile& regs, uint32_t rt, uint32_t normalized_color_mask,
    bool gamma_fetch_blend, bool fixed16_fetch_blend) {
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
uint32_t MetalPipelineCache::GetColorTargetsWritten(
    uint32_t normalized_color_mask) {
  uint32_t color_targets = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if ((normalized_color_mask >> (i * 4)) & 0xF) {
      color_targets |= UINT32_C(1) << i;
    }
  }
  return color_targets;
}

void MetalPipelineCache::PopulatePipelineFormatsFromRenderPassDescriptor(
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

MetalPipelineCache::MetalPipelineCache(MetalCommandProcessor& command_processor)
    : command_processor_(command_processor) {}

MetalPipelineCache::~MetalPipelineCache() { Shutdown(); }

void MetalPipelineCache::FlushStorageIfNeeded() {
  if (shader_storage_flush_needed_ || pipeline_storage_flush_needed_) {
    storage_writer_.RequestFlush(shader_storage_flush_needed_,
                                 pipeline_storage_flush_needed_);
    shader_storage_flush_needed_ = false;
    pipeline_storage_flush_needed_ = false;
  }
}

void MetalPipelineCache::Shutdown() {
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
    for (MTL::Library* library : baked_libraries_) {
      library->release();
    }
    baked_libraries_.clear();
    for (auto& fallback : generic_fallback_libraries_) {
      if (fallback.second.first) {
        fallback.second.first->release();
      }
    }
    generic_fallback_libraries_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(shader_analysis_mutex_);
    finite_multiply_requirements_.clear();
    std::fill(std::begin(finite_multiply_last_shader_),
              std::end(finite_multiply_last_shader_), nullptr);
    std::fill(std::begin(finite_multiply_last_info_),
              std::end(finite_multiply_last_info_), nullptr);
    draw_exact_analyses_.clear();
    draw_exact_last_shader_ = nullptr;
    draw_exact_last_analysis_ = nullptr;
  }
  for (RecentShaderLoad& recent : recent_shader_loads_) {
    recent = RecentShaderLoad();
  }
  ClearAddressShaderLoads();
  guest_shader_cache_.clear();
  memexport_format_profiles_.Clear();
  guest_spirv_shader_cache_.reset();
}

void MetalPipelineCache::InitializeAsyncCompilation() {
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

  creation_queue_.Initialize(
      "Metal Shaders",
      [this](const CompileTask& task, WorkerThreadContext* context) {
        return ExecuteCompileTask(task, context);
      },
      [this](const CompileTask& task, CompileResult result) {
        StoreCompileResult(task, result);
      },
      [this]() -> std::unique_ptr<WorkerThreadContext> {
        if (cvars::metal_compile_thread_qos == "user-initiated") {
          pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        } else if (cvars::metal_compile_thread_qos == "utility") {
          pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        }
        auto ctx = std::make_unique<WorkerThreadContext>();
        ctx->translator = guest_spirv_shader_cache_->CreateWorkerTranslator();
        return ctx;
      },
      [this](bool busy) {
        command_processor_.graphics_system()->on_shader_compilation(busy);
      });
  creation_queue_.SetActiveThreadLimit(thread_count);
  creation_queue_.SetThreadCount(thread_count);
  {
    // A boost asked for before the pool existed.
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    UpdateCompileThreadLimitLocked();
  }

  // How many compiles Metal's compiler service runs at once for this app
  // (iOS 26, macOS 13.3). Threads past it wait in Metal, though they still do
  // our own translation first.
  uint64_t metal_compile_tasks = 0;
  if (class_respondsToSelector(
          object_getClass(reinterpret_cast<id>(command_processor_.device_)),
          sel_registerName("maximumConcurrentCompilationTaskCount"))) {
    metal_compile_tasks =
        command_processor_.device_->maximumConcurrentCompilationTaskCount();
  }
  XELOGI(
      "Metal: async shader/pipeline compilation enabled with {} worker "
      "thread(s), qos {}, Metal compiles at once: {}",
      thread_count, cvars::metal_compile_thread_qos, metal_compile_tasks);
}

void MetalPipelineCache::ShutdownAsyncCompilation() {
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    async_compile_shutdown_ = true;
  }
  for (auto& [task, result] : creation_queue_.Shutdown()) {
    if (result.pipeline) {
      result.pipeline->release();
    }
  }

  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  async_shader_pending_.clear();
  async_shader_failed_.clear();
  async_pipeline_pending_.clear();
  async_pipeline_failed_.clear();
  async_compile_shutdown_ = false;
  async_compile_base_thread_count_ = 0;
  async_compile_thread_limit_ = 0;
  // async_compile_hold_boost_ is the app's request, not pool state: it is kept
  // so a pool that starts after the request still honours it.
  async_compile_awaiters_ = 0;
  storage_stats_pending_ = false;
  storage_stats_boosted_ = false;
  storage_stats_keys_.clear();
  storage_shader_requests_ = 0;
  storage_pipeline_requests_ = 0;
  storage_requests_done_ = 0;
  storage_stats_expected_pipelines_ = 0;
  // The dropped requests count as finished, so the backlog reads empty.
  compile_activity.finished.store(
      compile_activity.queued.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  compile_activity.storage_loading = false;
}

MTL::Function* MetalPipelineCache::GetHostShaderFunction(
    const Shader::Translation* translation) {
  if (!translation) {
    return nullptr;
  }
  return static_cast<const MetalShader::MetalTranslation*>(translation)
      ->metal_function();
}

bool MetalPipelineCache::IsHostShaderReady(
    const Shader::Translation* translation) {
  return translation &&
         static_cast<const MetalShader::MetalTranslation*>(translation)
             ->is_ready();
}

MetalPipelineCache::ShaderCompileStatus
MetalPipelineCache::EnsureTranslationSpirv(Shader::Translation* translation,
                                           SpirvShaderTranslator& translator) {
  if (translation->is_translated()) {
    return translation->is_valid() ? ShaderCompileStatus::kReady
                                   : ShaderCompileStatus::kFailed;
  }
  if (!translation->TryClaimTranslation()) {
    // Another thread is translating it.
    return ShaderCompileStatus::kPending;
  }
  return TranslateClaimedSpirv(translation, translator);
}

MetalPipelineCache::ShaderCompileStatus
MetalPipelineCache::TranslateClaimedSpirv(Shader::Translation* translation,
                                          SpirvShaderTranslator& translator) {
  const int64_t translate_start_ns = GetSteadyTimeNs();
  const bool translated = translator.TranslateAnalyzedShader(*translation);
  AddCompileStageTime(kCompileStageTranslate, translate_start_ns);
  if (!translated) {
    XELOGE("Metal: failed to translate shader {:016X} mod {:016X} to SPIR-V",
           translation->shader().ucode_data_hash(),
           translation->modification());
    return ShaderCompileStatus::kFailed;
  }
  return ShaderCompileStatus::kReady;
}

bool MetalPipelineCache::CompileHostShader(Shader::Translation* translation,
                                           std::vector<uint8_t>* metallib_out) {
  if (!translation) {
    return false;
  }
  return static_cast<MetalShader::MetalTranslation*>(translation)
      ->CompileToAir(command_processor_.device_, metallib_out);
}

MetalPipelineCache::ShaderCompileStatus
MetalPipelineCache::TranslateAndCompileHostShader(
    Shader::Translation* translation, SpirvShaderTranslator& translator) {
  if (!translation) {
    return ShaderCompileStatus::kFailed;
  }
  // Shader dumps and bisection want the translation itself.
  const bool cacheable =
      cvars::dump_shaders.empty() && shader_library_cache_.is_open();
  auto& spirv_shader = static_cast<SpirvShader&>(translation->shader());
  // Its pipelines take functions with the constants compiled in, so a generic
  // library would be converted and loaded to go unused.
  const bool bakes_only = BakesFetchState(translation);
  MetalShaderLibraryCache::Key key;
  ShaderCompileStatus status;
  if (cacheable && !translation->is_translated()) {
    // A stored library (or, for a shader that bakes only, none) and the
    // shader's stored bindings stand in for the translation, so a hit doesn't
    // translate. The claim keeps one thread on this translation, as
    // EnsureTranslationSpirv's does.
    if (!translation->TryClaimTranslation()) {
      return ShaderCompileStatus::kPending;
    }
    key = MetalShaderLibraryCache::MakeKey(*translation);
    MetalShaderLibraryCache::Entry entry;
    const int64_t cached_start_ns = GetSteadyTimeNs();
    if (shader_library_cache_.Find(key, entry)) {
      spirv_shader.PublishBindings(entry.texture_bindings,
                                   entry.sampler_bindings);
      auto* metal_translation =
          static_cast<MetalShader::MetalTranslation*>(translation);
      if (entry.metallib.empty()) {
        if (bakes_only) {
          metal_translation->SetBakesOnly(entry.used_buffer_mask);
          AddCompileStageTime(kCompileStageLibraryCached, cached_start_ns);
          return ShaderCompileStatus::kReady;
        }
      } else if (metal_translation->LoadMetallib(
                     command_processor_.device_, entry.metallib,
                     entry.entry_point_name, entry.used_buffer_mask)) {
        AddCompileStageTime(kCompileStageLibraryCached, cached_start_ns);
        return ShaderCompileStatus::kReady;
      }
    }
    status = TranslateClaimedSpirv(translation, translator);
  } else {
    if (cacheable) {
      key = MetalShaderLibraryCache::MakeKey(*translation);
    }
    status = EnsureTranslationSpirv(translation, translator);
  }
  if (status != ShaderCompileStatus::kReady) {
    return status;
  }
  if (bakes_only) {
    auto* metal_translation =
        static_cast<MetalShader::MetalTranslation*>(translation);
    metal_translation->SetBakesOnly(
        metal_translation->GetSpirv2airBufferMask());
    if (cacheable) {
      shader_library_cache_.StoreBindings(
          key, metal_translation->used_buffer_mask(), spirv_shader);
    }
    return ShaderCompileStatus::kReady;
  }
  std::vector<uint8_t> metallib;
  const int64_t air_start_ns = GetSteadyTimeNs();
  const bool compiled =
      CompileHostShader(translation, cacheable ? &metallib : nullptr);
  AddCompileStageTime(kCompileStageAir, air_start_ns);
  if (!compiled) {
    return ShaderCompileStatus::kFailed;
  }
  if (cacheable && !metallib.empty()) {
    auto* metal_translation =
        static_cast<MetalShader::MetalTranslation*>(translation);
    shader_library_cache_.Store(
        key, metallib, metal_translation->entry_point_name(),
        metal_translation->used_buffer_mask(), spirv_shader);
  }
  return ShaderCompileStatus::kReady;
}

void MetalPipelineCache::NoteShaderCompileFailed(
    Shader::Translation* translation) {
  if (!translation) {
    return;
  }
  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  async_shader_failed_.insert(translation);
}

MetalPipelineCache::ShaderCompileStatus
MetalPipelineCache::GetShaderCompileStatus(Shader::Translation* translation) {
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
  return IsHostShaderReady(translation) ? ShaderCompileStatus::kReady
                                        : ShaderCompileStatus::kNotQueued;
}

bool MetalPipelineCache::EnqueueShaderCompilation(
    Shader::Translation* translation, uint8_t priority) {
  if (!translation || !cvars::async_shader_compilation ||
      !creation_queue_.has_threads()) {
    return false;
  }

  ShaderCompileRequest request;
  request.translation = translation;
  request.shader_hash = translation->shader().ucode_data_hash();
  request.modification = translation->modification();
  request.priority = priority;
  request.storage = queueing_storage_requests_;
  const size_t lane =
      priority >= 2 ? kQueuePriorityShaderVertex : kQueuePriorityShaderPixel;
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    async_shader_pending_.insert(translation);
    compile_activity.queued.fetch_add(1, std::memory_order_relaxed);
    if (request.storage) {
      ++storage_shader_requests_;
      PublishStorageRequestsLocked();
    }
    creation_queue_.PushUnordered(CompileTask(std::move(request)), lane);
  }
  return true;
}

bool MetalPipelineCache::EnqueuePipelineCompilation(
    const PipelineCompileRequest& request) {
  if (!cvars::async_shader_compilation || !creation_queue_.has_threads()) {
    return false;
  }
  // A vertex shader that bakes only has no vertex_function until
  // GetPipelineVertexFunction makes it.
  if (request.vertex_function == nullptr &&
      !(request.vertex_translation &&
        request.vertex_translation->bakes_only())) {
    return false;
  }

  QueuedPipelineRequest queued;
  queued.request = request;
  if (queued.request.vertex_function) {
    queued.owned_vertex_function =
        NS::RetainPtr(queued.request.vertex_function);
  }
  if (queued.request.fragment_function) {
    queued.owned_fragment_function =
        NS::RetainPtr(queued.request.fragment_function);
  }
  const size_t lane = PipelinePriorityToQueueLane(request.priority);
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

    async_pipeline_pending_.insert(request.pipeline_key);
    compile_activity.queued.fetch_add(1, std::memory_order_relaxed);
    creation_queue_.PushUnordered(CompileTask(std::move(queued)), lane);
  }
  return true;
}

Shader::Translation* MetalPipelineCache::GetOrCreateHostTranslation(
    SpirvShader& shader, uint64_t modification, bool allow_async,
    ShaderCompileStatus* compile_status_out,
    std::shared_ptr<const Shader::Specialization> specialization) {
  Shader::Translation* translation = shader.GetOrCreateTranslation(
      modification, nullptr, std::move(specialization));
  ShaderCompileStatus status = GetShaderCompileStatus(translation);
  if (status == ShaderCompileStatus::kNotQueued) {
    // Vertex shaders go first: the placeholder a pending pixel shader draws
    // through needs one, so the draw stops being dropped sooner.
    if (allow_async &&
        EnqueueShaderCompilation(
            translation, shader.type() == xenos::ShaderType::kVertex ? 2 : 1)) {
      status = ShaderCompileStatus::kPending;
    } else {
      status = TranslateAndCompileHostShader(
          translation, guest_spirv_shader_cache_->translator());
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

void MetalPipelineCache::LogShaderCompilePending(
    const Shader::Translation* translation, const char* stage_tag) {
  // Every skipped or placeholder draw comes through here.
  command_processor_.NoteZPDDrawInexact();
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

void MetalPipelineCache::LogPipelineCompilePending(
    const Shader::Translation* vertex_translation,
    const Shader::Translation* pixel_translation) {
  // Every skipped or placeholder draw comes through here.
  command_processor_.NoteZPDDrawInexact();
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

void MetalPipelineCache::LogPlaceholderDraw(
    const Shader::Translation* vertex_translation,
    const Shader::Translation* pixel_translation) {
  // Every skipped or placeholder draw comes through here.
  command_processor_.NoteZPDDrawInexact();
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

void MetalPipelineCache::AddCompileStageTime(CompileStage stage,
                                             int64_t start_ns) {
  const uint64_t ns =
      uint64_t(std::max<int64_t>(GetSteadyTimeNs() - start_ns, 0));
  CompileStageTotal& total = compile_stage_totals_[stage];
  total.count.fetch_add(1, std::memory_order_relaxed);
  total.ns.fetch_add(ns, std::memory_order_relaxed);
  uint64_t max_ns = total.max_ns.load(std::memory_order_relaxed);
  while (ns > max_ns && !total.max_ns.compare_exchange_weak(
                            max_ns, ns, std::memory_order_relaxed)) {
  }
}

void MetalPipelineCache::LogCompileStages() {
  constexpr int64_t kIntervalNs = int64_t(5) * 1000 * 1000 * 1000;
  if (!ShouldLogRateLimited(compile_stage_last_log_ns_, kIntervalNs)) {
    return;
  }
  static constexpr const char* kNames[kCompileStageCount] = {
      "translate",
      "cached library",
      "AIR",
      "specialize",
      "bake",
      "baked library",
      "archive pipeline",
      "compiled pipeline",
      "draw wait"};
  std::string line;
  for (size_t i = 0; i < kCompileStageCount; ++i) {
    CompileStageTotal& total = compile_stage_totals_[i];
    const uint64_t count = total.count.load(std::memory_order_relaxed);
    const uint64_t ns = total.ns.load(std::memory_order_relaxed);
    const uint64_t new_count = count - compile_stage_logged_count_[i];
    const uint64_t new_ns = ns - compile_stage_logged_ns_[i];
    compile_stage_logged_count_[i] = count;
    compile_stage_logged_ns_[i] = ns;
    const uint64_t max_ns = total.max_ns.exchange(0, std::memory_order_relaxed);
    if (!new_count) {
      continue;
    }
    line += fmt::format("{}{} {} in {:.1f} ms (max {:.1f})",
                        line.empty() ? "" : ", ", kNames[i], new_count,
                        double(new_ns) / 1e6, double(max_ns) / 1e6);
  }
  if (!line.empty()) {
    XELOGI("Metal compile stages, last 5 s: {}", line);
  }
}

void MetalPipelineCache::ApplyColorAttachmentState(
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

MTL::Function* MetalPipelineCache::GetSpecializedFunction(
    const MetalShader::MetalTranslation* translation, MTL::Library* library,
    const std::string& function_name, const uint32_t* states,
    uint32_t binding_count, std::string* error_out) {
  // A translation that bakes only has no library: its own address keys its
  // functions.
  const bool bakes_only = translation && translation->bakes_only();
  if ((!library && !bakes_only) || function_name.empty()) {
    if (error_out) {
      *error_out = "missing shader library";
    }
    return nullptr;
  }
  SpecializedFunctionKey key{
      library ? uintptr_t(library) : uintptr_t(translation), function_name,
      std::vector<uint32_t>(states, states + 4 * binding_count)};
  {
    std::lock_guard<std::mutex> lock(specialized_functions_mutex_);
    auto it = specialized_functions_.find(key);
    if (it != specialized_functions_.end()) {
      return it->second;
    }
  }
  if (translation &&
      (bakes_only || (cvars::metal_bake_fetch_state &&
                      (shader_library_cache_.is_open() ||
                       cvars::metal_bake_fetch_state_uncached)))) {
    if (MTL::Function* baked =
            GetBakedFunction(translation, states, binding_count, key)) {
      return baked;
    }
  }
  std::string generic_function_name = function_name;
  if (!library) {
    library = GetGenericFallbackLibrary(translation, generic_function_name);
    if (!library) {
      if (error_out) {
        *error_out = "no library for the shader's fetch-state constants";
      }
      return nullptr;
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
  function_desc->setName(NS::String::string(generic_function_name.c_str(),
                                            NS::UTF8StringEncoding));
  function_desc->setConstantValues(values);
  NS::Error* error = nullptr;
  const int64_t specialize_start_ns = GetSteadyTimeNs();
  MTL::Function* function = library->newFunction(function_desc, &error);
  AddCompileStageTime(kCompileStageSpecialize, specialize_start_ns);
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

// Further compile-time levers, parked untried on park/s2a-compile-levers
// (tools/PARKED.md: their gains were only modelled, never timed on a device):
// TODO(xenios-jp): decode 2D and cube texture sizes on the host (f17f39fc8).
// TODO(xenios-jp): take texture gradient scales from an exp2 table (3ae74a406).
// TODO(xenios-jp): compile gamma color targets per pipeline (77c743c53).
// TODO(xenios-jp): compile the vertex position format and the vertex
// expansion index load per pipeline (ef2ac46b2, f5ba07305).
MTL::Function* MetalPipelineCache::GetBakedFunction(
    const MetalShader::MetalTranslation* translation, const uint32_t* states,
    uint32_t binding_count, const SpecializedFunctionKey& key) {
  const MetalShaderLibraryCache::Key variant_key =
      MetalShaderLibraryCache::MakeVariantKey(
          MetalShaderLibraryCache::MakeKey(*translation), states,
          binding_count);
  MetalShaderLibraryCache::Entry entry;
  const int64_t start_ns = GetSteadyTimeNs();
  bool cached = shader_library_cache_.FindVariant(variant_key, entry);
  if (!cached) {
    // A shader made ready from the library cache is translated here, the
    // first time a constant set of it has no stored variant.
    if (!EnsureBakeSpirv(translation) ||
        !translation->CompileWithFetchState(
            states, binding_count, entry.metallib, entry.entry_point_name)) {
      return nullptr;
    }
    AddCompileStageTime(kCompileStageBake, start_ns);
  }
  const int64_t load_start_ns = GetSteadyTimeNs();
  NS::Error* error = nullptr;
  dispatch_data_t data =
      dispatch_data_create(entry.metallib.data(), entry.metallib.size(),
                           nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* library = command_processor_.device_->newLibrary(data, &error);
  dispatch_release(data);
  if (!library) {
    return nullptr;
  }
  MTL::Function* function = library->newFunction(NS::String::string(
      entry.entry_point_name.c_str(), NS::UTF8StringEncoding));
  if (!function) {
    library->release();
    return nullptr;
  }
  if (cached) {
    AddCompileStageTime(kCompileStageBakedCached, load_start_ns);
  } else {
    shader_library_cache_.StoreVariant(variant_key, entry.metallib,
                                       entry.entry_point_name,
                                       translation->used_buffer_mask());
  }
  std::lock_guard<std::mutex> lock(specialized_functions_mutex_);
  auto [it, inserted] = specialized_functions_.emplace(key, function);
  if (!inserted) {
    function->release();
    library->release();
    return it->second;
  }
  baked_libraries_.push_back(library);
  return function;
}

bool MetalPipelineCache::EnsureBakeSpirv(
    const MetalShader::MetalTranslation* translation) {
  if (!translation->is_translated()) {
    // Only a translation TranslateAndCompileHostShader made ready from the
    // library cache gets here untranslated, and that took the claim. Compile
    // requests see the translation through const; it is the shader's own.
    std::lock_guard<std::mutex> lock(bake_translate_mutex_);
    if (!translation->is_translated()) {
      auto translator = guest_spirv_shader_cache_->CreateWorkerTranslator();
      TranslateClaimedSpirv(
          const_cast<MetalShader::MetalTranslation*>(translation), *translator);
    }
  }
  return translation->is_valid() && !translation->translated_binary().empty();
}

MTL::Library* MetalPipelineCache::GetGenericFallbackLibrary(
    const MetalShader::MetalTranslation* translation,
    std::string& entry_point_name_out) {
  std::lock_guard<std::mutex> lock(generic_fallback_mutex_);
  auto it = generic_fallback_libraries_.find(translation);
  if (it == generic_fallback_libraries_.end()) {
    MTL::Library* library = nullptr;
    std::string entry_point_name;
    if (EnsureBakeSpirv(translation)) {
      library = translation->NewGenericLibrary(command_processor_.device_,
                                               entry_point_name);
    }
    XELOGW(
        "Metal: fetch-state constants of shader {:016X} mod {:016X} can't be "
        "compiled in, {}",
        translation->shader().ucode_data_hash(), translation->modification(),
        library ? "Metal specializes its generic library"
                : "and it has no generic library either");
    it = generic_fallback_libraries_
             .emplace(translation,
                      std::make_pair(library, std::move(entry_point_name)))
             .first;
  }
  entry_point_name_out = it->second.second;
  return it->second.first;
}

MTL::Function* MetalPipelineCache::GetPipelineVertexFunction(
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
      request.vertex_translation, request.vertex_library,
      request.vertex_function_name, states,
      SpirvShaderTranslator::kFetchStateBindingCount, error_out);
}

MTL::Function* MetalPipelineCache::GetPipelineFragmentFunction(
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
      request.fragment_translation, request.fragment_library,
      request.fragment_function_name, states,
      SpirvShaderTranslator::kFetchStateBindingCount, error_out);
}

bool MetalPipelineCache::UsesVertexFetchStateFunctionConstants(
    const Shader::Translation* translation) const {
  return spirv_translator_features_.fetch_state_function_constants &&
         translation &&
         !Shader::IsHostVertexShaderTypeDomain(
             SpirvShaderTranslator::Modification(translation->modification())
                 .vertex.host_vertex_shader_type) &&
         (!translation->shader().vertex_bindings().empty() ||
          UsesPixelFetchStateFunctionConstants(translation) ||
          GetDrawExactAnalysis(translation->shader()).flag_count);
}

bool MetalPipelineCache::BakesFetchState(
    const Shader::Translation* translation) const {
  // As GetSpecializedFunction decides it. Shader dumps want the library of the
  // translation itself.
  if (!cvars::metal_bake_fetch_state ||
      cvars::metal_bake_fetch_state_generic_library ||
      !cvars::dump_shaders.empty() ||
      !(shader_library_cache_.is_open() ||
        cvars::metal_bake_fetch_state_uncached)) {
    return false;
  }
  return translation->shader().type() == xenos::ShaderType::kVertex
             ? UsesVertexFetchStateFunctionConstants(translation)
             : UsesPixelFetchStateFunctionConstants(translation);
}

bool MetalPipelineCache::UsesPixelFetchStateFunctionConstants(
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

const MetalPipelineCache::FiniteMultiplyInfo&
MetalPipelineCache::GetFiniteMultiplyInfo(const Shader& shader) const {
  const size_t last_index = shader.type() == xenos::ShaderType::kPixel;
  {
    std::lock_guard<std::mutex> lock(shader_analysis_mutex_);
    if (finite_multiply_last_shader_[last_index] == &shader) {
      return *finite_multiply_last_info_[last_index];
    }
    auto found = finite_multiply_requirements_.find(&shader);
    if (found != finite_multiply_requirements_.end()) {
      finite_multiply_last_shader_[last_index] = &shader;
      finite_multiply_last_info_[last_index] = &found->second;
      return found->second;
    }
  }
  SpirvShaderTranslator::FiniteMultiplyAnalysis analysis;
  SpirvShaderTranslator::AnalyzeFiniteMultiplications(
      shader, analysis, spirv_translator_features_.finite_multiply_one_sided,
      spirv_translator_features_.finite_multiply_control_flow_join);
  FiniteMultiplyInfo info;
  info.required = analysis.dynamic_inputs;
  std::memcpy(info.direct_constants, analysis.direct_constants,
              sizeof(info.direct_constants));
  std::lock_guard<std::mutex> lock(shader_analysis_mutex_);
  auto found = finite_multiply_requirements_.emplace(&shader, info).first;
  finite_multiply_last_shader_[last_index] = &shader;
  finite_multiply_last_info_[last_index] = &found->second;
  return found->second;
}

const SpirvShaderTranslator::DrawExactAnalysis&
MetalPipelineCache::GetDrawExactAnalysis(const Shader& shader) const {
  {
    std::lock_guard<std::mutex> lock(shader_analysis_mutex_);
    if (draw_exact_last_shader_ == &shader) {
      return *draw_exact_last_analysis_;
    }
    auto found = draw_exact_analyses_.find(&shader);
    if (found != draw_exact_analyses_.end()) {
      draw_exact_last_shader_ = &shader;
      draw_exact_last_analysis_ = &found->second;
      return found->second;
    }
  }
  SpirvShaderTranslator::DrawExactAnalysis analysis;
  // The same conditions as in the translator.
  if (spirv_translator_features_.optimize_finite_multiplication &&
      spirv_translator_features_.fetch_state_function_constants &&
      shader.type() == xenos::ShaderType::kVertex &&
      cvars::alu_round_toward_zero &&
      !spirv_translator_features_.rounding_mode_rtz_float32 &&
      cvars::alu_rtz_draw_exact) {
    SpirvShaderTranslator::AnalyzeDrawExactness(shader, analysis);
  }
  std::lock_guard<std::mutex> lock(shader_analysis_mutex_);
  auto found = draw_exact_analyses_.emplace(&shader, std::move(analysis)).first;
  draw_exact_last_shader_ = &shader;
  draw_exact_last_analysis_ = &found->second;
  return found->second;
}

void MetalPipelineCache::UpdateDrawExactness(
    const Shader& vertex_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result) {
  draw_exact_shader_ = &vertex_shader;
  draw_exact_flags_ = 0;
  draw_exact_key_flags_ = 0;
  const auto& analysis = GetDrawExactAnalysis(vertex_shader);
  if (!analysis.flag_count) {
    std::memset(draw_exact_estimates_, 0, sizeof(draw_exact_estimates_));
    return;
  }
  const RegisterFile& regs = *command_processor_.register_file_;
  // r0.x is the guest vertex index plus the base where the vertex shader runs
  // once for a guest vertex. Automatic indices are below the vertex count, and
  // only 16-bit ones from an index buffer are known to be small.
  double vertex_index_bound = -1.0;
  if (analysis.uses_vertex_index &&
      (primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kVertex ||
       primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kMemExportCompute)) {
    auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    double base =
        std::abs(double(int32_t(regs.Get<reg::VGT_INDX_OFFSET>().indx_offset)));
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex) {
      vertex_index_bound =
          base +
          double(std::max(primitive_processing_result.guest_draw_vertex_count,
                          primitive_processing_result.host_draw_vertex_count));
    } else if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA &&
               vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
      vertex_index_bound = base + 65535.0;
    }
  }
  draw_exact_flags_ = SpirvShaderTranslator::EvaluateDrawExactness(
      analysis, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X],
      vertex_index_bound, draw_exact_estimates_);
  // What this draw proves depends on the values of its constants. A pipeline
  // per set of flags would have draws wait for new pipelines whenever those
  // values change, so the pipelines are for the flags all the draws of the
  // shader have had, which only ever lose bits.
  std::lock_guard<std::mutex> lock(draw_exact_stable_flags_mutex_);
  DrawExactStableFlags& stable =
      draw_exact_stable_flags_[vertex_shader.ucode_data_hash()];
  uint32_t stable_flags = stable.flags & draw_exact_flags_;
  if (stable_flags != stable.flags) {
    stable.flags = stable_flags;
    stable.settled_draws = 0;
  } else if (stable.settled_draws < UINT32_MAX) {
    ++stable.settled_draws;
  }
  // Draws in a row that must agree before their flags key pipelines.
  constexpr uint32_t kDrawExactSettledDraws = 32;
  if (stable.settled_draws >= kDrawExactSettledDraws) {
    draw_exact_key_flags_ = stable_flags;
  }
}

uint64_t MetalPipelineCache::GetFiniteMultiplyRequirements(
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

void MetalPipelineCache::GetFiniteMultiplyState(
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

bool MetalPipelineCache::HasFiniteMultiplyRequirements(
    const Shader::Translation* vertex, const Shader::Translation* pixel,
    const PipelineDescription& description) const {
  if (!spirv_translator_features_.optimize_finite_multiplication) {
    return false;
  }
  return (description.vertex_fetch_states_specialized && vertex &&
          GetFiniteMultiplyInfo(vertex->shader()).required) ||
         (description.fragment_fetch_states_specialized && pixel &&
          GetFiniteMultiplyInfo(pixel->shader()).required);
}

bool MetalPipelineCache::SetGeneralFiniteMultiplyState(
    const Shader::Translation* vertex, const Shader::Translation* pixel,
    PipelineDescription& description) const {
  if (!HasFiniteMultiplyRequirements(vertex, pixel, description)) {
    return false;
  }
  auto set = [this](const Shader::Translation* translation, uint32_t* state) {
    uint64_t required = GetFiniteMultiplyInfo(translation->shader()).required;
    state[0] |=
        uint32_t((required & SpirvShaderTranslator::kFiniteMultiplyTextures) >>
                 SpirvShaderTranslator::kFiniteMultiplyTextureShift);
    if (required & SpirvShaderTranslator::kFiniteMultiplyFloatConstants) {
      state[1] |= 1;
    }
  };
  if (description.vertex_fetch_states_specialized && vertex) {
    set(vertex, description.vertex_finite_multiply_state);
    // Nothing proven for the draw either (DrawExactAnalysis flags).
    description.vertex_finite_multiply_state[1] &= 1;
  }
  if (description.fragment_fetch_states_specialized && pixel) {
    set(pixel, description.pixel_finite_multiply_state);
  }
  return true;
}

MTL::RenderPipelineState* MetalPipelineCache::FindBuiltPipelineState(
    uint64_t key) {
  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  auto it = async_pipeline_cache_.find(key);
  return it != async_pipeline_cache_.end() ? it->second : nullptr;
}

void MetalPipelineCache::SetPipelineFunctionConstants(
    PipelineCompileRequest& request, const Shader::Translation* vertex,
    const Shader::Translation* pixel) {
  request.vertex_fetch_states_specialized =
      UsesVertexFetchStateFunctionConstants(vertex);
  request.fragment_fetch_states_specialized =
      UsesPixelFetchStateFunctionConstants(pixel);
  if (request.vertex_fetch_states_specialized) {
    auto* translation =
        static_cast<const MetalShader::MetalTranslation*>(vertex);
    request.vertex_translation = translation;
    request.vertex_library = translation->metal_library();
    request.vertex_function_name = translation->entry_point_name();
  }
  if (request.fragment_fetch_states_specialized) {
    auto* translation =
        static_cast<const MetalShader::MetalTranslation*>(pixel);
    request.fragment_translation = translation;
    request.fragment_library = translation->metal_library();
    request.fragment_function_name = translation->entry_point_name();
  }
}

MTL::RenderPipelineState* MetalPipelineCache::CreatePipelineState(
    const PipelineCompileRequest& request, std::string* error_out) {
  if (error_out) {
    error_out->clear();
  }
  if (request.description.kind != PipelineKind::kRender &&
      request.description.kind != PipelineKind::kPatchTessellation) {
    return nullptr;
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

  if (request.description.kind == PipelineKind::kPatchTessellation) {
    desc->setMaxTessellationFactor(command_processor_.max_tessellation_factor_);
    desc->setTessellationFactorStepFunction(
        request.description.tessellation_mode ==
                xenos::TessellationMode::kAdaptive
            ? MTL::TessellationFactorStepFunctionPerPatch
            : MTL::TessellationFactorStepFunctionConstant);
    desc->setTessellationPartitionMode(
        request.description.tessellation_mode ==
                xenos::TessellationMode::kDiscrete
            ? MTL::TessellationPartitionModeInteger
            : MTL::TessellationPartitionModeFractionalEven);
    desc->setTessellationControlPointIndexType(
        MTL::TessellationControlPointIndexTypeNone);
    // Guest domain shaders are clockwise in the Direct3D and Vulkan tessellator
    // domain (v downwards). Metal's has v upwards, so the same winding is
    // counterclockwise in its terms. The opposite one inverts the facing, and
    // guest face culling removes the surface.
    desc->setTessellationOutputWindingOrder(MTL::WindingCounterClockwise);
  }

  NS::Error* error = nullptr;
  const int64_t pipeline_start_ns = GetSteadyTimeNs();
  bool archive_hit = false;
  MTL::RenderPipelineState* pipeline = pipeline_archive_.NewRenderPipelineState(
      command_processor_.device_, desc, &error, &archive_hit);
  AddCompileStageTime(
      archive_hit ? kCompileStagePipelineHit : kCompileStagePipelineMiss,
      pipeline_start_ns);
  desc->release();

  if (!pipeline && error_out && error) {
    NS::String* description = error->localizedDescription();
    if (description) {
      *error_out = description->utf8String();
    }
  }

  return pipeline;
}

MetalPipelineCache::CompileResult MetalPipelineCache::ExecuteCompileTask(
    const CompileTask& task, WorkerThreadContext* context) {
  ScopedAutoreleasePool autorelease_pool;
  CompileResult result;
  if (const auto* queued_pipeline = std::get_if<QueuedPipelineRequest>(&task)) {
    result.pipeline =
        CreatePipelineState(queued_pipeline->request, &result.error);
  } else {
    const auto& shader_request = std::get<ShaderCompileRequest>(task);
    result.shader_status =
        shader_request.translation && context && context->translator
            ? TranslateAndCompileHostShader(shader_request.translation,
                                            *context->translator)
            : ShaderCompileStatus::kFailed;
  }
  return result;
}

void MetalPipelineCache::StoreCompileResult(const CompileTask& task,
                                            const CompileResult& result) {
  if (const auto* queued_pipeline = std::get_if<QueuedPipelineRequest>(&task)) {
    const PipelineCompileRequest& pipeline_request = queued_pipeline->request;
    const bool compiled = result.pipeline != nullptr;
    {
      std::lock_guard<std::mutex> lock(async_compile_mutex_);
      async_pipeline_pending_.erase(pipeline_request.pipeline_key);
      NoteStoredPipelineDoneLocked(pipeline_request.pipeline_key);
      if (compiled) {
        auto insert_result = async_pipeline_cache_.emplace(
            pipeline_request.pipeline_key, result.pipeline);
        if (!insert_result.second && result.pipeline) {
          result.pipeline->release();
        }
        async_pipeline_failed_.erase(pipeline_request.pipeline_key);
      } else {
        async_pipeline_failed_.insert(pipeline_request.pipeline_key);
      }
      FinishAsyncCompileTaskLocked();
    }
    if (!compiled && ShouldLogRateLimited(async_pipeline_failure_last_log_ns_,
                                          kAsyncCompileLogIntervalNs)) {
      if (!result.error.empty()) {
        XELOGE(
            "Metal: async pipeline compile failed "
            "(VS {:016X} mod {:016X}, PS {:016X} mod {:016X}): {}",
            pipeline_request.description.vertex_shader_hash,
            pipeline_request.description.vertex_shader_modification,
            pipeline_request.description.pixel_shader_hash,
            pipeline_request.description.pixel_shader_modification,
            result.error);
      } else {
        XELOGE(
            "Metal: async pipeline compile failed "
            "(VS {:016X} mod {:016X}, PS {:016X} mod {:016X})",
            pipeline_request.description.vertex_shader_hash,
            pipeline_request.description.vertex_shader_modification,
            pipeline_request.description.pixel_shader_hash,
            pipeline_request.description.pixel_shader_modification);
      }
    }
  } else {
    const auto& shader_request = std::get<ShaderCompileRequest>(task);
    const bool compiled = result.shader_status == ShaderCompileStatus::kReady;
    // Nothing queues a translation twice and the draw thread only translates
    // one that is not queued, so losing the claim here shouldn't happen.
    // Leave it unqueued for a later draw to retry rather than fail it forever,
    // which would black out the shader for the whole run.
    const bool contended =
        result.shader_status == ShaderCompileStatus::kPending;
    {
      std::lock_guard<std::mutex> lock(async_compile_mutex_);
      if (shader_request.translation) {
        async_shader_pending_.erase(shader_request.translation);
        if (!compiled && !contended) {
          async_shader_failed_.insert(shader_request.translation);
        }
      }
      if (shader_request.storage) {
        NoteStorageRequestDoneLocked();
      }
      FinishAsyncCompileTaskLocked();
    }
    if (!compiled && !contended &&
        ShouldLogRateLimited(async_shader_failure_last_log_ns_,
                             kAsyncCompileLogIntervalNs)) {
      XELOGE("Metal: async shader compile failed (shader {:016X}, mod {:016X})",
             shader_request.shader_hash, shader_request.modification);
    }
  }
}

std::unique_ptr<SpirvShaderTranslator> MetalPipelineCache::CreateTranslator()
    const {
  return std::make_unique<SpirvShaderTranslator>(
      spirv_translator_features_,
      spirv_translator_native_2x_msaa_,  // native_2x_msaa_with_att
      false,                             // native_2x_msaa_no_att
      false,  // edram_fragment_shader_interlock (host RT path)
      cvars::precise_interpolation, spirv_translator_resolution_scale_x_,
      spirv_translator_resolution_scale_y_);
}

bool MetalPipelineCache::depth_float24_round() const {
  return ::cvars::depth_float24_round;
}

bool MetalPipelineCache::depth_float24_convert_in_pixel_shader() const {
  return ::cvars::depth_float24_convert_in_pixel_shader;
}

bool MetalPipelineCache::SupportsHostRenderTargetFeedback() const {
  // spirv2air reads input attachments as color attachments in raster order
  // group 0, and fetch-state function constants specialize the blending.
  return command_processor_.device_ &&
         command_processor_.device_->supportsFamily(MTL::GPUFamilyApple4) &&
         command_processor_.device_->areRasterOrderGroupsSupported();
}

bool MetalPipelineCache::InitializeShaderTranslation() {
  // RGBA8 targets need shader gamma encoding. RGBA16 targets store linear
  // values and encode them when packing the render target into EDRAM.
  const bool gamma_render_target_as_unorm8 =
      !command_processor_.render_target_cache_
           ->gamma_render_target_as_unorm16();

  XELOGI("Shader translator init: gamma_as_unorm8={}, msaa_2x={}, scale={}x{}",
         gamma_render_target_as_unorm8,
         command_processor_.render_target_cache_->msaa_2x_supported(),
         command_processor_.render_target_cache_->draw_resolution_scale_x(),
         command_processor_.render_target_cache_->draw_resolution_scale_y());

  // Enable all features as a baseline, then disable what Metal doesn't need.
  SpirvShaderTranslator::Features spirv_features(true);
  spirv_features.structured_vertex_control_flow = true;
  spirv_features.fetch_state_function_constants = true;
  // 8_8_8_8_GAMMA as 8-bit gamma, and 16_16 and 16_16_16_16 as the EDRAM's
  // fixed point, blended in the shader.
  spirv_features.host_render_target_feedback =
      command_processor_.render_target_cache_->gamma_fetch_blend() ||
      command_processor_.render_target_cache_->fixed16_fetch_blend();
  // The AIR float division is correctly rounded (verified for the epsilon over
  // every texture size), so the host division gives the shader's bits.
  spirv_features.texture_coord_epsilon_constants = true;
  spirv_features.predecoded_texture_fetch_lod_gradient = true;
  spirv_features.optimize_finite_multiplication = cvars::metal_finite_multiply;
  spirv_features.finite_multiply_one_sided = true;
  spirv_features.finite_multiply_control_flow_join = true;
  spirv_features.fused_float32_fma = true;
  // Not using fragment shader interlock — we use host render targets.
  spirv_features.fragment_shader_sample_interlock = false;
  // A live primitive-specific input prevents adjacent primitives from sharing
  // fragment helper values on affected Apple drivers. Keep regular varying
  // interpolation; the extra barycentrics only anchor helper execution.
  spirv_features.primitive_fragment_helpers =
      command_processor_.device_->supportsFamily(MTL::GPUFamilyApple1) &&
      command_processor_.device_->supportsShaderBarycentricCoordinates();
  spirv_features.fragment_shader_barycentric = false;
  // The host's centroid depends on hidden surface removal within the render
  // pass, so it's computed from the rasterized coverage.
  spirv_features.centroid_from_raster_coverage =
      command_processor_.device_->supportsShaderBarycentricCoordinates();
  // Metal fast-math doesn't guarantee IEEE NaN/Inf preservation.
  spirv_features.signed_zero_inf_nan_preserve_float32 = false;
  // Metal fast-math flushes denorms.
  spirv_features.denorm_flush_to_zero_float32 = true;
  // RTE rounding not guaranteed by Metal.
  spirv_features.rounding_mode_rte_float32 = false;

  // Snapshotted so a compile thread can build its own translator without
  // reading the render target cache from off the draw thread.
  spirv_translator_features_ = spirv_features;
  spirv_translator_native_2x_msaa_ =
      command_processor_.render_target_cache_->msaa_2x_supported();
  spirv_translator_resolution_scale_x_ =
      command_processor_.render_target_cache_->draw_resolution_scale_x();
  spirv_translator_resolution_scale_y_ =
      command_processor_.render_target_cache_->draw_resolution_scale_y();
  guest_spirv_shader_cache_ = std::make_unique<GuestSpirvShaderCache>(
      static_cast<GuestSpirvShaderCache::Host&>(*this),
      *command_processor_.register_file_,
      *command_processor_.render_target_cache_);
  if (!guest_spirv_shader_cache_->Initialize()) {
    guest_spirv_shader_cache_.reset();
    return false;
  }

  XELOGI("SpirvShaderTranslator init: msaa_2x={}, scale={}x{}",
         command_processor_.render_target_cache_->msaa_2x_supported(),
         command_processor_.render_target_cache_->draw_resolution_scale_x(),
         command_processor_.render_target_cache_->draw_resolution_scale_y());

  return true;
}

void MetalPipelineCache::TranslateShadersForStorage(
    const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
    size_t stored_count) {
  // Known before the shaders are compiled, which can be most of the load, so
  // the progress and the forecast start now. The stored pipelines will go to
  // the compile threads too, if there are any.
  compile_activity.storage_total = stored_count;
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    storage_stats_expected_pipelines_ =
        creation_queue_.has_threads() ? stored_count : 0;
    PublishStorageRequestsLocked();
  }
  // Queue every modification the stored pipelines reference. Falls back to this
  // thread when there is no pool.
  size_t queued = 0;
  queueing_storage_requests_ = true;
  for (const auto& [ucode_hash, modification] : translations_needed) {
    auto it = guest_shader_cache_.find(ucode_hash);
    if (it == guest_shader_cache_.end()) {
      continue;
    }
    // Translation needs the ucode analyzed, which the storage load did only for
    // the shaders it read this time.
    if (!it->second->is_ucode_analyzed()) {
      it->second->AnalyzeUcode(command_processor_.ucode_disasm_buffer_);
    }
    ShaderCompileStatus status = ShaderCompileStatus::kReady;
    GetOrCreateHostTranslation(*it->second, modification, /*allow_async=*/true,
                               &status);
    if (status == ShaderCompileStatus::kPending) {
      ++queued;
    }
  }
  queueing_storage_requests_ = false;
  if (queued) {
    // A pipeline can't be queued without its compiled functions, so the shader
    // half is finished here even when the caller asked for non-blocking init.
    AwaitAsyncCompiles();
  }
}

void MetalPipelineCache::CreateStoredPipelines(
    const std::vector<PipelineStoredDescription>& stored_descriptions) {
  replaying_stored_pipelines_ = true;
  ReplayStoredPipelines(stored_descriptions,
                        [this](const PipelineStoredDescription& stored,
                               Shader*& vertex_shader_out) {
                          return QueueStoredPipeline(stored, vertex_shader_out);
                        });
  replaying_stored_pipelines_ = false;
}

const char* MetalPipelineCache::QueueStoredPipeline(
    const PipelineStoredDescription& stored, Shader*& vertex_shader_out) {
  const PipelineDescription& description = stored.description;
  if (description.finite_multiply_enabled !=
      spirv_translator_features_.optimize_finite_multiplication) {
    return "belonging to another finite-multiply configuration";
  }
  if (uint32_t draw_exact_flags =
          description.vertex_finite_multiply_state[1] >> 1) {
    // What the draws of the vertex shader had proven when this was stored:
    // settled already, and still only used where a draw proves as much.
    std::lock_guard<std::mutex> lock(draw_exact_stable_flags_mutex_);
    DrawExactStableFlags& stable =
        draw_exact_stable_flags_[description.vertex_shader_hash];
    stable.flags &= draw_exact_flags;
    stable.settled_draws = UINT32_MAX;
  }
  // Reject stored descriptions from removed tessellation paths (1 = MSL, 2 =
  // MSC mesh).
  if (PipelineKind(description.kind) != PipelineKind::kRender &&
      PipelineKind(description.kind) != PipelineKind::kPatchTessellation) {
    return "belonging to a removed tessellation path";
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
  Shader::Translation* vertex_translation = GetOrCreateHostTranslation(
      *vertex_it->second, description.vertex_shader_modification,
      /*allow_async=*/false, &status);
  if (status != ShaderCompileStatus::kReady) {
    return "VS not buildable";
  }
  request.vertex_function = GetHostShaderFunction(vertex_translation);

  request.fragment_function = GetHostShaderFunction(pixel_translation);
  SetPipelineFunctionConstants(request, vertex_translation, pixel_translation);
  if (description.vertex_fetch_states_specialized !=
          request.vertex_fetch_states_specialized ||
      description.fragment_fetch_states_specialized !=
          request.fragment_fetch_states_specialized) {
    return "belonging to another specialization configuration";
  }

  if (cvars::metal_finite_multiply_general_fallback &&
      HasFiniteMultiplyRequirements(vertex_translation, pixel_translation,
                                    request.description) &&
      bool(request.description.finite_multiply_one_sided) !=
          spirv_translator_features_.finite_multiply_one_sided) {
    // Stored under another finite multiply analysis: the state it was stored
    // with may prove more than this build's draws will. Build the general
    // variant in its place, which every draw of the pair can use until its own
    // is built behind it.
    SetGeneralFiniteMultiplyState(vertex_translation, pixel_translation,
                                  request.description);
    request.description.finite_multiply_one_sided =
        spirv_translator_features_.finite_multiply_one_sided;
    request.pipeline_key = request.description.GetHash();
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
        async_pipeline_pending_.count(request.pipeline_key) &&
        storage_stats_keys_.insert(request.pipeline_key).second) {
      ++storage_pipeline_requests_;
      PublishStorageRequestsLocked();
    }
  }
  vertex_shader_out = vertex_it->second.get();
  return nullptr;
}

void MetalPipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  storage_writer_.ShutdownShaderStorage();

  if (!command_processor_.device_) {
    XELOGW("Metal shader storage init skipped (no device)");
    if (completion_callback) {
      completion_callback();
    }
    return;
  }

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
    // The libraries are discarded when the translation code, its compilers or
    // the translation settings change (MakeFingerprint), and kept across builds
    // that leave those alone. Rebuilding them is our own translation work only;
    // the archive below keeps the GPU binaries.
    const std::string fingerprint = MetalShaderLibraryCache::MakeFingerprint(
        fmt::format("spirv2air device={} msaa2x={} scale={}x{}",
                    command_processor_.device_->name()
                        ? command_processor_.device_->name()->utf8String()
                        : "",
                    spirv_translator_native_2x_msaa_,
                    spirv_translator_resolution_scale_x_,
                    spirv_translator_resolution_scale_y_));
    shader_library_cache_.Open(title_local_root / "shader_libraries.bin",
                               fingerprint);
    // The archive outlives builds and settings: Metal finds a pipeline in it
    // only for the same descriptor and functions, the functions by the hash of
    // their bitcode (spirv2air writes it), so a changed translation misses and
    // compiles, which Apple documents as safe. Lookups also need the same GPU
    // and OS build, which Open adds. Discarding it on every app update made
    // the first session after each one rebuild every pipeline.
    pipeline_archive_.Open(
        command_processor_.device_, title_local_root / "pipelines",
        fmt::format("archive1 spirv2air device={}",
                    command_processor_.device_->name()
                        ? command_processor_.device_->name()->utf8String()
                        : ""));
    archive_saved_ticks_ = xe::Clock::QueryHostTickCount();
  }

  // How long the last complete rebuild took, to forecast this one before the
  // compile threads have measured anything. A record from another pipeline
  // version says little about this one. xrebuild2 times the whole load, the
  // stored shaders included; xrebuild1 timed only the pipelines after them.
  const std::filesystem::path stats_path =
      GetShaderStorageLocalRoot(cache_root) / "metal" /
      fmt::format("{:08X}", title_id) / "rebuild_stats.txt";
  uint64_t seed_us_per_stored = 0;
  {
    std::ifstream stats_file(stats_path);
    std::string tag;
    uint64_t stats_version = 0, stats_stored = 0, stats_us = 0;
    if (stats_file >> tag >> stats_version >> stats_stored >> stats_us &&
        tag == "xrebuild2" && stats_version == config.version && stats_stored) {
      seed_us_per_stored = stats_us / stats_stored;
    }
  }
  {
    std::error_code ec;
    std::filesystem::create_directories(stats_path.parent_path(), ec);
  }

  const uint64_t load_start_ticks = xe::Clock::QueryHostTickCount();
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    // Requests of an earlier load that are still queued are no longer its.
    storage_stats_pending_ = false;
    storage_stats_boosted_ = async_compile_hold_boost_;
    storage_stats_keys_.clear();
    storage_stats_queued_all_ = false;
    storage_stats_start_ticks_ = load_start_ticks;
    storage_stats_stored_ = 0;
    storage_stats_version_ = config.version;
    storage_stats_path_ = stats_path;
    storage_shader_requests_ = 0;
    storage_pipeline_requests_ = 0;
    storage_requests_done_ = 0;
    storage_stats_expected_pipelines_ = 0;
    compile_activity.storage_requests = 0;
    compile_activity.storage_requests_done = 0;
    compile_activity.storage_empty = false;
    compile_activity.storage_total = 0;
    compile_activity.storage_seed_us_per_stored = seed_us_per_stored;
    compile_activity.storage_loading = true;
    ++compile_activity.storage_generation;
  }

  std::vector<PipelineStoredDescription> stored_descriptions;
  if (!storage_writer_.InitializeShaderStorage(
          cache_root, title_id, config,
          [this](xenos::ShaderType type, const uint32_t* ucode_dwords,
                 uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            return LoadShader(type, ucode_dwords, ucode_dword_count,
                              ucode_data_hash);
          },
          // The descriptions are read by the time the shaders are asked for.
          [this,
           &stored_descriptions](const std::set<std::pair<uint64_t, uint64_t>>
                                     & translations_needed) {
            TranslateShadersForStorage(translations_needed,
                                       stored_descriptions.size());
          },
          stored_descriptions)) {
    XELOGW("Metal: persistent shader storage is disabled");
    compile_activity.storage_loading = false;
    if (completion_callback) {
      completion_callback();
    }
    return;
  }

  compile_activity.storage_total = stored_descriptions.size();
  uint64_t start_ticks = xe::Clock::QueryHostTickCount();
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    storage_stats_pending_ = !stored_descriptions.empty();
    storage_stats_boosted_ = storage_stats_boosted_ ||
                             async_compile_hold_boost_ ||
                             async_compile_awaiters_;
    storage_stats_stored_ = stored_descriptions.size();
  }
  CreateStoredPipelines(stored_descriptions);
  compile_activity.storage_empty = stored_descriptions.empty();
  {
    // Every stored pipeline the compile threads will build is queued now. They
    // may all be done already (or none went to them: no async compilation, or
    // every stored entry was rejected), with no later finish to notice it.
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    storage_stats_queued_all_ = true;
    PublishStorageRequestsLocked();
    if (storage_stats_keys_.empty()) {
      FinishStorageRebuildLocked();
    }
  }
  if (blocking) {
    // The caller is waiting on this, so finish rather than leaving the compile
    // threads to race the guest's first draws.
    AwaitAsyncCompiles();
    compile_activity.storage_loading = false;
  }
  XELOGI("Metal pipeline storage: stored pipelines {} in {} ms",
         blocking ? "rebuilt" : "rebuilt or queued",
         (xe::Clock::QueryHostTickCount() - start_ticks) * 1000 /
             xe::Clock::QueryHostTickFrequency());

  if (completion_callback) {
    completion_callback();
  }
}

bool MetalPipelineCache::IsAsyncCompileIdleLocked() const {
  return async_shader_pending_.empty() && async_pipeline_pending_.empty();
}

void MetalPipelineCache::RecordStorageRebuildStatsLocked() {
  storage_stats_pending_ = false;
  storage_stats_keys_.clear();
  if (storage_stats_boosted_) {
    return;
  }
  uint64_t elapsed_us =
      (xe::Clock::QueryHostTickCount() - storage_stats_start_ticks_) * 1000000 /
      xe::Clock::QueryHostTickFrequency();
  std::ofstream stats_file(storage_stats_path_, std::ios::trunc);
  if (stats_file) {
    stats_file << "xrebuild2 " << storage_stats_version_ << ' '
               << storage_stats_stored_ << ' ' << elapsed_us << '\n';
  }
}

void MetalPipelineCache::CountAsyncCompileTaskFinishedLocked() {
  compile_activity.finished.fetch_add(1, std::memory_order_relaxed);
}

void MetalPipelineCache::NoteStoredPipelineDoneLocked(uint64_t pipeline_key) {
  if (!storage_stats_pending_ || !storage_stats_keys_.erase(pipeline_key)) {
    return;
  }
  NoteStorageRequestDoneLocked();
  if (storage_stats_queued_all_ && storage_stats_keys_.empty()) {
    FinishStorageRebuildLocked();
  }
}

void MetalPipelineCache::PublishStorageRequestsLocked() {
  uint64_t pipelines = storage_stats_queued_all_
                           ? storage_pipeline_requests_
                           : std::max(storage_pipeline_requests_,
                                      storage_stats_expected_pipelines_);
  compile_activity.storage_requests.store(storage_shader_requests_ + pipelines,
                                          std::memory_order_relaxed);
}

void MetalPipelineCache::NoteStorageRequestDoneLocked() {
  // Every request counted done was counted queued first, under this lock. One
  // left over from an earlier load, which can't count, is all this can catch.
  if (storage_requests_done_ >=
      storage_shader_requests_ + storage_pipeline_requests_) {
    return;
  }
  ++storage_requests_done_;
  compile_activity.storage_requests_done.store(storage_requests_done_,
                                               std::memory_order_relaxed);
}

void MetalPipelineCache::FinishStorageRebuildLocked() {
  if (storage_stats_pending_) {
    RecordStorageRebuildStatsLocked();
  }
  compile_activity.storage_loading.store(false, std::memory_order_relaxed);
}

void MetalPipelineCache::FinishAsyncCompileTaskLocked() {
  CountAsyncCompileTaskFinishedLocked();
  // A waiter may be after this task rather than the whole queue.
  if (async_compile_awaiters_ || IsAsyncCompileIdleLocked()) {
    async_compile_idle_cv_.notify_all();
  }
}

void MetalPipelineCache::UpdateCompileThreadLimitLocked() {
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
  creation_queue_.SetActiveThreadLimit(limit);
}

void MetalPipelineCache::PromoteQueuedPipelineLocked(uint64_t key,
                                                     uint8_t priority) {
  creation_queue_.Promote(
      [key](const CompileTask& task) {
        const auto* queued = std::get_if<QueuedPipelineRequest>(&task);
        return queued && queued->request.pipeline_key == key;
      },
      PipelinePriorityToQueueLane(priority));
}

bool MetalPipelineCache::TakeQueuedPipelineLocked(uint64_t key) {
  if (!creation_queue_.TakeQueued([key](const CompileTask& task) {
        const auto* queued = std::get_if<QueuedPipelineRequest>(&task);
        return queued && queued->request.pipeline_key == key;
      })) {
    return false;
  }
  async_pipeline_pending_.erase(key);
  NoteStoredPipelineDoneLocked(key);
  CountAsyncCompileTaskFinishedLocked();
  if (IsAsyncCompileIdleLocked()) {
    async_compile_idle_cv_.notify_all();
  }
  return true;
}

void MetalPipelineCache::SetCompileThreadBoost(bool boost) {
  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  if (async_compile_shutdown_ || async_compile_hold_boost_ == boost) {
    return;
  }
  async_compile_hold_boost_ = boost;
  // Also while the stored shaders compile, before storage_stats_pending_.
  if (boost && (storage_stats_pending_ || compile_activity.storage_loading.load(
                                              std::memory_order_relaxed))) {
    storage_stats_boosted_ = true;
  }
  // Without a pool yet, it applies when the pool starts.
  UpdateCompileThreadLimitLocked();
  XELOGI("Metal: {} shader/pipeline compile thread(s){}",
         async_compile_thread_limit_,
         boost ? " while the game waits for shaders" : "");
}

void MetalPipelineCache::SetCompileThreadBoostAllowed(bool allowed) {
  std::lock_guard<std::mutex> lock(async_compile_mutex_);
  if (async_compile_boost_allowed_ == allowed) {
    return;
  }
  async_compile_boost_allowed_ = allowed;
  UpdateCompileThreadLimitLocked();
  XELOGI("Metal: compile thread boost {}",
         allowed ? "allowed" : "off (hot device)");
}

template <typename Done>
void MetalPipelineCache::AwaitAsyncCompilesUntil(const Done& done) {
  if (!creation_queue_.has_threads()) {
    return;
  }
  SCOPE_profile_cpu_i("gpu", "MetalPipelineCache::AwaitAsyncCompilesUntil");
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
    const int64_t wait_start_ns = GetSteadyTimeNs();
    async_compile_idle_cv_.wait(lock, done);
    AddCompileStageTime(kCompileStageDrawWait, wait_start_ns);
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

void MetalPipelineCache::AwaitAsyncCompiles() {
  AwaitAsyncCompilesUntil([this]() { return IsAsyncCompileIdleLocked(); });
}

void MetalPipelineCache::AwaitPendingPipeline() {
  const uint64_t key = last_pending_pipeline_key_;
  {
    std::lock_guard<std::mutex> lock(async_compile_mutex_);
    PromoteQueuedPipelineLocked(key, kPipelinePriorityAwaited);
  }
  AwaitAsyncCompilesUntil([this, key]() {
    return async_pipeline_pending_.find(key) == async_pipeline_pending_.end();
  });
}

Shader* MetalPipelineCache::LoadShader(xenos::ShaderType shader_type,
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
  SpirvShader* shader = LoadShader(shader_type, host_address, dword_count,
                                   XXH3_64bits(host_address, ucode_size));
  std::rotate(recent_shader_loads_.begin(),
              std::prev(recent_shader_loads_.end()),
              recent_shader_loads_.end());
  RecentShaderLoad& recent = recent_shader_loads_.front();
  recent.shader = shader;
  recent.ucode.assign(host_address, host_address + dword_count);
  return shader;
}

Shader* MetalPipelineCache::LoadShaderAtAddress(xenos::ShaderType shader_type,
                                                uint32_t guest_address,
                                                uint32_t dword_count) {
  command_processor_.AwaitGpuWritesTo(guest_address,
                                      dword_count * sizeof(uint32_t));
  const uint32_t* host_address =
      command_processor_.memory_->TranslatePhysical<const uint32_t*>(
          guest_address);
  const uint32_t stage = uint32_t(shader_type);
  if (stage > 1 || !dword_count || !command_processor_.shared_memory_ ||
      !command_processor_.shared_memory_->is_zero_copy() ||
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
      command_processor_.shared_memory_->UnwatchMemoryRange(entry->watch);
      entry->watch = nullptr;
    }
    entry->valid.store(false, std::memory_order_relaxed);
    entry->watch_installed = false;
  }
  // As for textures: make the range valid (protected) first, then read it,
  // then watch it only if no write came in between.
  bool requested =
      command_processor_.shared_memory_->RequestRange(guest_address, length);
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
    if (command_processor_.shared_memory_->IsRangeValid(guest_address,
                                                        length)) {
      entry->watch = command_processor_.shared_memory_->WatchMemoryRange(
          guest_address, length, AddressShaderLoadWatchCallback, this, entry,
          0);
      entry->watch_installed = entry->watch != nullptr;
      entry->valid.store(entry->watch_installed, std::memory_order_relaxed);
    }
  }
  return shader;
}

void MetalPipelineCache::AddressShaderLoadWatchCallback(
    const global_unique_lock_type& global_lock, void* context, void* data,
    uint64_t argument, bool invalidated_by_gpu) {
  auto* entry = static_cast<AddressShaderLoad*>(data);
  // The watch is cancelled after this callback.
  entry->watch = nullptr;
  entry->valid.store(false, std::memory_order_release);
}

void MetalPipelineCache::ClearAddressShaderLoads() {
  if (command_processor_.shared_memory_) {
    auto global_lock = xe::global_critical_region::Acquire();
    for (auto& [address, entry] : address_shader_loads_) {
      if (entry->watch) {
        command_processor_.shared_memory_->UnwatchMemoryRange(entry->watch);
        entry->watch = nullptr;
      }
    }
  }
  address_shader_loads_.clear();
  last_address_shader_loads_ = {};
}

SpirvShader* MetalPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                            const uint32_t* host_address,
                                            uint32_t dword_count,
                                            uint64_t hash) {
  auto it = guest_shader_cache_.find(hash);
  if (it != guest_shader_cache_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<MetalShader>(shader_type, hash, host_address,
                                              dword_count);
  SpirvShader* result = shader.get();
  guest_shader_cache_[hash] = std::move(shader);
  return result;
}

void MetalPipelineCache::JoinPipelineArchiveSave() {
  if (archive_save_thread_.joinable()) {
    archive_save_thread_.join();
  }
  archive_save_running_ = false;
}

void MetalPipelineCache::MaybeSavePipelineArchive() {
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

SpirvShaderTranslator::Modification
MetalPipelineCache::GetCurrentSpirvVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  return SpirvShaderTranslator::Modification(
      guest_spirv_shader_cache_->GetVertexShaderModification(
          shader, host_vertex_shader_type, interpolator_mask,
          ps_param_gen_used));
}

SpirvShaderTranslator::Modification
MetalPipelineCache::GetCurrentSpirvPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) const {
  const auto& regs = *command_processor_.register_file_;
  const bool apply_polygon_offset_in_shader =
      NeedsExactFloatDepthBias(regs) && !shader.writes_depth();

  SpirvShaderTranslator::Modification modification(
      guest_spirv_shader_cache_->GetPixelShaderModification(
          shader, interpolator_mask, param_gen_pos, normalized_depth_control,
          normalized_color_mask, apply_polygon_offset_in_shader));

  // A gamma or 16-bit fixed point target is encoded by the shader, and if it
  // blends, blended there with the factors applied before MIN and MAX —
  // suppress the fixed-function MIN/MAX premultiply workaround on RT0.
  xenos::ColorRenderTargetFormat rt0_format =
      regs.Get<reg::RB_COLOR_INFO>().color_format;
  if ((command_processor_.render_target_cache_->gamma_fetch_blend() &&
       rt0_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) ||
      (command_processor_.render_target_cache_->fixed16_fetch_blend() &&
       IsFixed16Format(rt0_format))) {
    modification.pixel.rt0_blend_rgb_factor_for_premult =
        xenos::BlendFactor::kOne;
    modification.pixel.rt0_blend_a_factor_for_premult =
        xenos::BlendFactor::kOne;
  }

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
        (command_processor_.render_target_cache_
             ->GetLastUpdateBoundRenderTargets() >>
         1) &
        ((UINT32_C(1) << xenos::kMaxColorRenderTargets) - 1);
    uint32_t rts = shader.writes_color_targets() &
                   modification.pixel.color_targets_used & bound_color_targets;
    uint32_t shader_blended_rts = 0;
    uint32_t rt;
    while (xe::bit_scan_forward(rts, &rt)) {
      rts &= ~(UINT32_C(1) << rt);
      if (command_processor_.render_target_cache_->fixed16_fetch_blend() &&
          IsFixed16Format(regs.Get<reg::RB_COLOR_INFO>(
                                  reg::RB_COLOR_INFO::rt_register_indices[rt])
                              .color_format)) {
        modification.pixel.host_render_target_fixed16 = 1;
      }
      if (GetShaderBlendControl(
              regs, rt, normalized_color_mask,
              command_processor_.render_target_cache_->gamma_fetch_blend(),
              command_processor_.render_target_cache_->fixed16_fetch_blend()) !=
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

uint64_t MetalPipelineCache::PopulatePipelineCompileRequest(
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
  if (command_processor_.current_render_pass_descriptor_) {
    const MetalCommandProcessor::RenderPassPipelineFormats& formats =
        command_processor_.current_render_pass_pipeline_formats_;
    std::copy(std::begin(formats.color), std::end(formats.color),
              color_formats);
    depth_format = formats.depth;
    stencil_format = formats.stencil;
    sample_count = formats.sample_count;
  } else if (!command_processor_.render_target_cache_) {
    PopulatePipelineFormatsFromRenderPassDescriptor(
        command_processor_.render_pass_descriptor_, color_formats, 4,
        &depth_format, &stencil_format, &sample_count);
  } else {
    for (uint32_t i = 0; i < 4; ++i) {
      if (MTL::Texture* rt =
              command_processor_.render_target_cache_->GetColorTargetForDraw(
                  i)) {
        color_formats[i] = rt->pixelFormat();
        if (rt->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(rt->sampleCount()));
        }
      }
    }
    if (color_formats[0] == MTL::PixelFormatInvalid) {
      if (MTL::Texture* dummy = command_processor_.render_target_cache_
                                    ->GetDummyColorTargetForDraw()) {
        color_formats[0] = dummy->pixelFormat();
        if (dummy->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(dummy->sampleCount()));
        }
      }
    }
    if (MTL::Texture* depth_tex =
            command_processor_.render_target_cache_->GetDepthTargetForDraw()) {
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
    SpirvShaderTranslator::Modification vertex_mod(
        vertex_translation->modification());
    if (Shader::IsHostVertexShaderTypeDomain(
            vertex_mod.vertex.host_vertex_shader_type)) {
      description.kind = PipelineKind::kPatchTessellation;
      description.tessellation_mode = vertex_mod.vertex.tessellation_mode;
      description.host_vertex_shader_type =
          vertex_mod.vertex.host_vertex_shader_type;
    }
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
          command_processor_.texture_cache_->GetFetchIntegerScaleBits(
              binding.fetch_constant);
      description.vertex_texture_decode_bits[binding.fetch_constant] =
          command_processor_.texture_cache_->GetFetchDecodeBits(
              binding.fetch_constant);
    }
    if (spirv_translator_features_.optimize_finite_multiplication) {
      GetFiniteMultiplyState(vertex_translation, regs,
                             description.vertex_finite_multiply_state);
      // Bit 0 is whether a float constant is not finite, the rest are the
      // DrawExactAnalysis flags.
      description.vertex_finite_multiply_state[1] &= 1;
      if (draw_exact_shader_ == &vertex_translation->shader() &&
          !Shader::IsHostVertexShaderTypeDomain(
              SpirvShaderTranslator::Modification(
                  vertex_translation->modification())
                  .vertex.host_vertex_shader_type)) {
        description.vertex_finite_multiply_state[1] |= draw_exact_key_flags_
                                                       << 1;
      }
    }
  }
  if (request.fragment_fetch_states_specialized) {
    get_bool_loop_constants(pixel_translation, description.pixel_bool_constants,
                            description.pixel_loop_constants);
    for (const Shader::TextureBinding& binding :
         pixel_translation->shader().texture_bindings()) {
      description.pixel_texture_scale_bits[binding.fetch_constant] =
          command_processor_.texture_cache_->GetFetchIntegerScaleBits(
              binding.fetch_constant);
      description.pixel_texture_decode_bits[binding.fetch_constant] =
          command_processor_.texture_cache_->GetFetchDecodeBits(
              binding.fetch_constant);
    }
    if (spirv_translator_features_.optimize_finite_multiplication) {
      GetFiniteMultiplyState(pixel_translation, regs,
                             description.pixel_finite_multiply_state);
    }
  }
  if (HasFiniteMultiplyRequirements(vertex_translation, pixel_translation,
                                    description)) {
    description.finite_multiply_one_sided =
        spirv_translator_features_.finite_multiply_one_sided;
    if (cvars::metal_finite_multiply_force_general) {
      SetGeneralFiniteMultiplyState(vertex_translation, pixel_translation,
                                    description);
    }
  }
  request.priority = pixel_translation ? kPipelinePriorityDraw
                                       : kPipelinePriorityDrawDepthOnly;
  request.pipeline_key = description.GetHash();
  return request.pipeline_key;
}

MTL::RenderPipelineState* MetalPipelineCache::GetOrCreatePipelineState(
    const Shader::Translation* vertex_translation,
    const Shader::Translation* pixel_translation, const RegisterFile& regs,
    PipelineCompileStatus* compile_status_out) {
  if (compile_status_out) {
    *compile_status_out = PipelineCompileStatus::kFailed;
  }
  if (!IsHostShaderReady(vertex_translation)) {
    XELOGE("Metal: no valid vertex shader function");
    return nullptr;
  }
  // Null for a vertex shader that bakes only (GetPipelineVertexFunction).
  MTL::Function* vertex_function = GetHostShaderFunction(vertex_translation);

  PipelineCompileRequest request = {};
  PopulatePipelineCompileRequest(regs, vertex_translation, pixel_translation,
                                 request);
  request.vertex_function = vertex_function;
  request.fragment_function = GetHostShaderFunction(pixel_translation);
  MTL::Function* exact_depth_function = nullptr;
  if (!pixel_translation && NeedsExactFloatDepthBias(regs)) {
    if (!exact_float_depth_bias_library_) {
      auto translator = guest_spirv_shader_cache_->CreateWorkerTranslator();
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
      exact_float_depth_bias_library_ =
          command_processor_.device_->newLibrary(data, &error);
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
  if (request.description.vertex_finite_multiply_state[1] & ~UINT32_C(1)) {
    // A pipeline for what the draws of the vertex shader have proven. It's
    // never waited for: built behind the draws, and until it's there the draw
    // goes on as if nothing were proven, which is always right.
    uint64_t pending_key = last_pending_pipeline_key_;
    PipelineCompileRequest proven_request = request;
    proven_request.priority = pixel_translation
                                  ? kPipelinePriorityStored
                                  : kPipelinePriorityStoredDepthOnly;
    PipelineCompileStatus proven_status = PipelineCompileStatus::kFailed;
    auto* proven = AcquirePipelineState(proven_request, true, &proven_status);
    if (proven) {
      if (exact_depth_function) {
        exact_depth_function->release();
      }
      if (compile_status_out) {
        *compile_status_out = proven_status;
      }
      return proven;
    }
    last_pending_pipeline_key_ = pending_key;
    request.description.vertex_finite_multiply_state[1] &= 1;
    request.pipeline_key = request.description.GetHash();
  }
  if (cvars::metal_finite_multiply_general_fallback && pixel_translation &&
      !FindBuiltPipelineState(request.pipeline_key)) {
    // The pipeline for this draw's finite multiply state isn't built. If the
    // general variant of the same pair is (a set bit only takes a proof away,
    // so it's right for every draw), draw with it and build the exact one
    // behind the draws instead of waiting or using the placeholder.
    PipelineCompileRequest general_request = request;
    if (SetGeneralFiniteMultiplyState(vertex_translation, pixel_translation,
                                      general_request.description)) {
      general_request.pipeline_key = general_request.description.GetHash();
      MTL::RenderPipelineState* general =
          general_request.pipeline_key != request.pipeline_key
              ? FindBuiltPipelineState(general_request.pipeline_key)
              : nullptr;
      if (!general && cvars::metal_finite_multiply_prewarm_general &&
          general_request.pipeline_key != request.pipeline_key) {
        general = AcquirePipelineState(general_request, false, nullptr);
      }
      if (general) {
        uint64_t pending_key = last_pending_pipeline_key_;
        PipelineCompileRequest exact_request = request;
        exact_request.priority = kPipelinePriorityStored;
        PipelineCompileStatus exact_status = PipelineCompileStatus::kFailed;
        AcquirePipelineState(exact_request, true, &exact_status);
        last_pending_pipeline_key_ = pending_key;
        ++finite_multiply_general_draws_;
        if (ShouldLogRateLimited(finite_multiply_general_last_log_ns_,
                                 kAsyncCompileLogIntervalNs)) {
          XELOGI(
              "Metal: drawing with the general finite multiply variant while "
              "the draw's own builds (VS {:016X}, PS {:016X}, {} draws so far)",
              vertex_translation->shader().ucode_data_hash(),
              pixel_translation->shader().ucode_data_hash(),
              finite_multiply_general_draws_);
        }
        if (exact_depth_function) {
          exact_depth_function->release();
        }
        if (compile_status_out) {
          *compile_status_out = PipelineCompileStatus::kReady;
        }
        return general;
      }
    }
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

MTL::RenderPipelineState* MetalPipelineCache::AcquirePipelineState(
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
MetalPipelineCache::GetOrCreatePlaceholderPipelineState(
    const Shader::Translation* vertex_translation, const RegisterFile& regs) {
  // A null pixel translation yields no fragment function and a zero color mask,
  // which is both the placeholder and what a depth-only draw builds.
  return GetOrCreatePipelineState(vertex_translation, nullptr, regs);
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
