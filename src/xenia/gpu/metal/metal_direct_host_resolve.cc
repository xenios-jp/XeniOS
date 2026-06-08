/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_render_target_cache.h"

#include <dispatch/dispatch.h>

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_32bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_32bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_32bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_32bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_64bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_64bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_64bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_64bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_128bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_128bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_128bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_128bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_128bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_128bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_16bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_16bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_16bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_16bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_16bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_16bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_32bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_32bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_32bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_32bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_64bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_64bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_64bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_64bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_8bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_8bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_8bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_8bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_8bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_8bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_128bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_128bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_128bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_128bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_128bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_128bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_32bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_32bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_32bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_32bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_64bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_64bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_64bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_64bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_color_uint_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_depth_32bpp_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_depth_32bpp_1xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_depth_32bpp_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_depth_32bpp_2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_depth_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_host_depth_32bpp_4xmsaa_scaled_cs.h"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

class ScopedAutoreleasePool {
 public:
  ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}
  ~ScopedAutoreleasePool() {
    if (pool_) {
      pool_->release();
    }
  }

  ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
  ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

 private:
  NS::AutoreleasePool* pool_;
};

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

  NS::String* fn_name = NS::String::string("entry_xe", NS::UTF8StringEncoding);
  MTL::Function* fn = lib->newFunction(fn_name);
  if (!fn) {
    XELOGE("Metal: {} missing entry_xe", debug_name);
    lib->release();
    return nullptr;
  }

  MTL::ComputePipelineDescriptor* pipeline_desc =
      MTL::ComputePipelineDescriptor::alloc()->init();
  pipeline_desc->setComputeFunction(fn);
  if (debug_name) {
    pipeline_desc->setLabel(
        NS::String::string(debug_name, NS::UTF8StringEncoding));
  }
  MTL::ComputePipelineState* pipeline = device->newComputePipelineState(
      pipeline_desc, MTL::PipelineOptionNone,
      static_cast<MTL::AutoreleasedComputePipelineReflection*>(nullptr),
      &error);
  pipeline_desc->release();
  fn->release();
  lib->release();

  if (!pipeline) {
    XELOGE("Metal: failed to create {} pipeline: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  return pipeline;
}

bool IsResolveDirectHostRTFastCandidate(
    draw_util::ResolveCopyShaderIndex shader) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
      return true;
    default:
      return false;
  }
}

size_t DirectHostResolveFullDestIndex(
    draw_util::ResolveCopyShaderIndex shader) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFull8bpp:
      return 0;
    case draw_util::ResolveCopyShaderIndex::kFull16bpp:
      return 1;
    case draw_util::ResolveCopyShaderIndex::kFull32bpp:
      return 2;
    case draw_util::ResolveCopyShaderIndex::kFull64bpp:
      return 3;
    case draw_util::ResolveCopyShaderIndex::kFull128bpp:
      return 4;
    default:
      return 5;
  }
}

bool IsResolveDirectHostRTFullColorCandidate(
    draw_util::ResolveCopyShaderIndex shader) {
  return DirectHostResolveFullDestIndex(shader) < 5;
}

bool IsResolveDirectHostRTFullColorSourcePackable(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

uint32_t DirectHostResolvePixelsPerThread(
    draw_util::ResolveCopyShaderIndex shader, bool source_is_64bpp,
    xenos::MsaaSamples msaa_samples) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFull8bpp:
      return msaa_samples >= xenos::MsaaSamples::k4X ? 4 : 8;
    case draw_util::ResolveCopyShaderIndex::kFull128bpp:
      return 2;
    case draw_util::ResolveCopyShaderIndex::kFull16bpp:
    case draw_util::ResolveCopyShaderIndex::kFull32bpp:
    case draw_util::ResolveCopyShaderIndex::kFull64bpp:
      return 4;
    default:
      return source_is_64bpp ? 4 : 8;
  }
}

constexpr char kDirectHostResolveEncoderLabel[] =
    "XeniaDirectHostResolveEncoder";
constexpr uint32_t kDirectHostResolveDepthFlagHasStencil = 1u << 0;
constexpr uint32_t kDirectHostResolveDepthFlagRoundDepth = 1u << 1;

size_t DirectHostResolveBppIndex(bool is_64bpp) { return is_64bpp ? 1u : 0u; }

size_t DirectHostResolveMsaaIndex(xenos::MsaaSamples msaa_samples) {
  switch (msaa_samples) {
    case xenos::MsaaSamples::k1X:
      return 0;
    case xenos::MsaaSamples::k2X:
      return 1;
    case xenos::MsaaSamples::k4X:
      return 2;
    default:
      return 3;
  }
}

void SetEncoderLabel(MTL::CommandEncoder* encoder, const char* label) {
  if (!encoder || !label) {
    return;
  }
  encoder->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
}

void EndSharedMemoryUploadBlitEncoderForCommandBuffer(
    MetalCommandProcessor& command_processor,
    MTL::CommandBuffer* command_buffer) {
  if (command_buffer &&
      command_buffer == command_processor.GetCurrentCommandBuffer()) {
    command_processor.EndSharedMemoryUploadBlitEncoder();
  }
}

void PushEncoderDebugGroup(MTL::CommandEncoder* encoder,
                           const std::string& label) {
  if (!encoder || label.empty()) {
    return;
  }
  encoder->pushDebugGroup(
      NS::String::string(label.c_str(), NS::UTF8StringEncoding));
}

}  // namespace

void MetalRenderTargetCache::InitializeDirectHostResolvePipelines(
    bool draw_resolution_scaled) {
  struct DirectHostResolvePipelineConfig {
    const void* metallib_data;
    size_t metallib_size;
    bool is_64bpp;
    xenos::MsaaSamples msaa_samples;
    bool scaled;
    bool source_is_uint;
    const char* debug_name;
  };
#define XE_DIRECT_HOST_RESOLVE_CONFIG(id, is_64bpp, msaa, scaled, source_uint) \
  {                                                                            \
    id##_metallib, sizeof(id##_metallib), is_64bpp, msaa, scaled, source_uint, \
        #id                                                                    \
  }
#define XE_DIRECT_HOST_RESOLVE_MSAA(prefix, bpp, is_64bpp, msaa_token, msaa, \
                                    source_uint)                             \
  XE_DIRECT_HOST_RESOLVE_CONFIG(prefix##_##bpp##bpp_##msaa_token##xmsaa_cs,  \
                                is_64bpp, msaa, false, source_uint),         \
      XE_DIRECT_HOST_RESOLVE_CONFIG(                                         \
          prefix##_##bpp##bpp_##msaa_token##xmsaa_scaled_cs, is_64bpp, msaa, \
          true, source_uint)
#define XE_DIRECT_HOST_RESOLVE_BPP(prefix, bpp, is_64bpp, source_uint) \
  XE_DIRECT_HOST_RESOLVE_MSAA(prefix, bpp, is_64bpp, 1,                   \
                              xenos::MsaaSamples::k1X, source_uint),     \
      XE_DIRECT_HOST_RESOLVE_MSAA(prefix, bpp, is_64bpp, 2,              \
                                  xenos::MsaaSamples::k2X, source_uint), \
      XE_DIRECT_HOST_RESOLVE_MSAA(prefix, bpp, is_64bpp, 4,              \
                                  xenos::MsaaSamples::k4X, source_uint)
#define XE_DIRECT_HOST_RESOLVE_SOURCE(prefix, source_uint)                 \
  XE_DIRECT_HOST_RESOLVE_BPP(prefix, 32, false, source_uint),              \
      XE_DIRECT_HOST_RESOLVE_BPP(prefix, 64, true, source_uint)
  static constexpr DirectHostResolvePipelineConfig
      kDirectHostResolvePipelineConfigs[] = {
          XE_DIRECT_HOST_RESOLVE_SOURCE(resolve_host_color, false),
          XE_DIRECT_HOST_RESOLVE_SOURCE(resolve_host_color_uint, true),
      };
#undef XE_DIRECT_HOST_RESOLVE_SOURCE
#undef XE_DIRECT_HOST_RESOLVE_BPP
#undef XE_DIRECT_HOST_RESOLVE_MSAA
#undef XE_DIRECT_HOST_RESOLVE_CONFIG

  for (const DirectHostResolvePipelineConfig& cfg :
       kDirectHostResolvePipelineConfigs) {
    if (cfg.scaled && !draw_resolution_scaled) {
      continue;
    }
    MTL::ComputePipelineState* pipeline =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, cfg.metallib_data, cfg.metallib_size, cfg.debug_name);
    if (!pipeline) {
      XELOGW(
          "Metal: failed to initialize optional direct host resolve "
          "pipeline {}",
          cfg.debug_name);
      continue;
    }
    size_t msaa_index = DirectHostResolveMsaaIndex(cfg.msaa_samples);
    if (msaa_index >= kDirectHostResolveMsaaCount) {
      pipeline->release();
      continue;
    }
    direct_host_resolve_pipelines_[DirectHostResolveBppIndex(cfg.is_64bpp)]
                                  [msaa_index][cfg.scaled ? 1u : 0u]
                                  [cfg.source_is_uint ? 1u : 0u] = pipeline;
  }

  struct DirectHostColorFullResolvePipelineConfig {
    const void* metallib_data;
    size_t metallib_size;
    draw_util::ResolveCopyShaderIndex copy_shader;
    xenos::MsaaSamples msaa_samples;
    bool scaled;
    bool source_is_uint;
    const char* debug_name;
  };
#define XE_DIRECT_HOST_COLOR_FULL_RESOLVE_CONFIG(id, shader, msaa, scaled,   \
                                                 source_uint)                \
  {                                                                          \
    id##_metallib, sizeof(id##_metallib), shader, msaa, scaled, source_uint, \
        #id                                                                  \
  }
#define XE_DIRECT_HOST_COLOR_FULL_RESOLVE_MSAA(prefix, bpp, shader,          \
                                               msaa_token, msaa, source_uint) \
  XE_DIRECT_HOST_COLOR_FULL_RESOLVE_CONFIG(                                  \
      prefix##_##bpp##bpp_##msaa_token##xmsaa_cs, shader, msaa, false,       \
      source_uint),                                                          \
      XE_DIRECT_HOST_COLOR_FULL_RESOLVE_CONFIG(                              \
          prefix##_##bpp##bpp_##msaa_token##xmsaa_scaled_cs, shader, msaa,   \
          true, source_uint)
#define XE_DIRECT_HOST_COLOR_FULL_RESOLVE_DEST(prefix, bpp, shader, source_uint) \
  XE_DIRECT_HOST_COLOR_FULL_RESOLVE_MSAA(prefix, bpp, shader, 1,                \
                                         xenos::MsaaSamples::k1X, source_uint), \
      XE_DIRECT_HOST_COLOR_FULL_RESOLVE_MSAA(                                   \
          prefix, bpp, shader, 2, xenos::MsaaSamples::k2X, source_uint),        \
      XE_DIRECT_HOST_COLOR_FULL_RESOLVE_MSAA(                                   \
          prefix, bpp, shader, 4, xenos::MsaaSamples::k4X, source_uint)
#define XE_DIRECT_HOST_COLOR_FULL_RESOLVE_SOURCE(prefix, source_uint)        \
  XE_DIRECT_HOST_COLOR_FULL_RESOLVE_DEST(                                    \
      prefix, 8, draw_util::ResolveCopyShaderIndex::kFull8bpp, source_uint), \
      XE_DIRECT_HOST_COLOR_FULL_RESOLVE_DEST(                                \
          prefix, 16, draw_util::ResolveCopyShaderIndex::kFull16bpp,         \
          source_uint),                                                      \
      XE_DIRECT_HOST_COLOR_FULL_RESOLVE_DEST(                                \
          prefix, 32, draw_util::ResolveCopyShaderIndex::kFull32bpp,         \
          source_uint),                                                      \
      XE_DIRECT_HOST_COLOR_FULL_RESOLVE_DEST(                                \
          prefix, 64, draw_util::ResolveCopyShaderIndex::kFull64bpp,         \
          source_uint),                                                      \
      XE_DIRECT_HOST_COLOR_FULL_RESOLVE_DEST(                                \
          prefix, 128, draw_util::ResolveCopyShaderIndex::kFull128bpp,       \
          source_uint)
  static constexpr DirectHostColorFullResolvePipelineConfig
      kDirectHostColorFullResolvePipelineConfigs[] = {
          XE_DIRECT_HOST_COLOR_FULL_RESOLVE_SOURCE(resolve_host_color_full,
                                                   false),
          XE_DIRECT_HOST_COLOR_FULL_RESOLVE_SOURCE(resolve_host_color_full_uint,
                                                   true),
      };
#undef XE_DIRECT_HOST_COLOR_FULL_RESOLVE_SOURCE
#undef XE_DIRECT_HOST_COLOR_FULL_RESOLVE_DEST
#undef XE_DIRECT_HOST_COLOR_FULL_RESOLVE_MSAA
#undef XE_DIRECT_HOST_COLOR_FULL_RESOLVE_CONFIG

  for (const DirectHostColorFullResolvePipelineConfig& cfg :
       kDirectHostColorFullResolvePipelineConfigs) {
    if (cfg.scaled && !draw_resolution_scaled) {
      continue;
    }
    MTL::ComputePipelineState* pipeline =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, cfg.metallib_data, cfg.metallib_size, cfg.debug_name);
    if (!pipeline) {
      XELOGW(
          "Metal: failed to initialize optional direct host full color "
          "resolve pipeline {}",
          cfg.debug_name);
      continue;
    }
    size_t msaa_index = DirectHostResolveMsaaIndex(cfg.msaa_samples);
    size_t dest_index = DirectHostResolveFullDestIndex(cfg.copy_shader);
    if (msaa_index >= kDirectHostResolveMsaaCount ||
        dest_index >= kDirectHostResolveFullDestCount) {
      pipeline->release();
      continue;
    }
    direct_host_color_full_resolve_pipelines_[msaa_index][cfg.scaled ? 1u : 0u]
                                             [cfg.source_is_uint ? 1u : 0u]
                                             [dest_index] = pipeline;
  }

  struct DirectHostDepthResolvePipelineConfig {
    const void* metallib_data;
    size_t metallib_size;
    xenos::MsaaSamples msaa_samples;
    bool scaled;
    const char* debug_name;
  };
#define XE_DIRECT_HOST_DEPTH_RESOLVE_CONFIG(id, msaa, scaled) \
  {                                                           \
    id##_metallib, sizeof(id##_metallib), msaa, scaled, #id   \
  }
#define XE_DIRECT_HOST_DEPTH_RESOLVE_MSAA(msaa_token, msaa)                \
  XE_DIRECT_HOST_DEPTH_RESOLVE_CONFIG(                                     \
      resolve_host_depth_32bpp_##msaa_token##xmsaa_cs, msaa, false),       \
      XE_DIRECT_HOST_DEPTH_RESOLVE_CONFIG(                                 \
          resolve_host_depth_32bpp_##msaa_token##xmsaa_scaled_cs, msaa,    \
          true)
  static constexpr DirectHostDepthResolvePipelineConfig
      kDirectHostDepthResolvePipelineConfigs[] = {
          XE_DIRECT_HOST_DEPTH_RESOLVE_MSAA(1, xenos::MsaaSamples::k1X),
          XE_DIRECT_HOST_DEPTH_RESOLVE_MSAA(2, xenos::MsaaSamples::k2X),
          XE_DIRECT_HOST_DEPTH_RESOLVE_MSAA(4, xenos::MsaaSamples::k4X),
      };
#undef XE_DIRECT_HOST_DEPTH_RESOLVE_MSAA
#undef XE_DIRECT_HOST_DEPTH_RESOLVE_CONFIG

  for (const DirectHostDepthResolvePipelineConfig& cfg :
       kDirectHostDepthResolvePipelineConfigs) {
    if (cfg.scaled && !draw_resolution_scaled) {
      continue;
    }
    MTL::ComputePipelineState* pipeline =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, cfg.metallib_data, cfg.metallib_size, cfg.debug_name);
    if (!pipeline) {
      XELOGW(
          "Metal: failed to initialize optional direct host depth resolve "
          "pipeline {}",
          cfg.debug_name);
      continue;
    }
    size_t msaa_index = DirectHostResolveMsaaIndex(cfg.msaa_samples);
    if (msaa_index >= kDirectHostResolveMsaaCount) {
      pipeline->release();
      continue;
    }
    direct_host_depth_resolve_pipelines_[msaa_index][cfg.scaled ? 1u : 0u] =
        pipeline;
  }
}

namespace {
// Recursively release+null every MTL::ComputePipelineState* in an arbitrarily
// dimensioned C array. Replaces the per-array hand-written nested loops.
template <typename T>
void ReleaseComputePipelineArray(T& element, bool release_existing) {
  if constexpr (std::is_array_v<T>) {
    for (auto& sub : element) {
      ReleaseComputePipelineArray(sub, release_existing);
    }
  } else {
    if (release_existing && element) {
      element->release();
    }
    element = nullptr;
  }
}
}  // namespace

void MetalRenderTargetCache::ResetDirectHostResolvePipelines(
    bool release_existing) {
  ReleaseComputePipelineArray(direct_host_resolve_pipelines_, release_existing);
  ReleaseComputePipelineArray(direct_host_color_full_resolve_pipelines_,
                              release_existing);
  ReleaseComputePipelineArray(direct_host_depth_resolve_pipelines_,
                              release_existing);
}

bool MetalRenderTargetCache::PrepareResolveDestinationBuffer(
    const draw_util::ResolveInfo& resolve_info, bool draw_resolution_scaled,
    ResolveDestinationBuffer& destination) {
  destination = {};
  if (draw_resolution_scaled) {
    auto* texture_cache = command_processor_.texture_cache();
    auto* metal_texture_cache =
        texture_cache ? static_cast<MetalTextureCache*>(texture_cache)
                      : nullptr;
    if (!metal_texture_cache) {
      return false;
    }
    const uint32_t range_length = resolve_info.copy_dest_extent_start -
                                  resolve_info.copy_dest_base +
                                  resolve_info.copy_dest_extent_length;
    return metal_texture_cache->EnsureScaledResolveMemoryCommitted(
               resolve_info.copy_dest_extent_start,
               resolve_info.copy_dest_extent_length) &&
           metal_texture_cache->MakeScaledResolveRangeCurrent(
               resolve_info.copy_dest_base, range_length) &&
           metal_texture_cache->GetCurrentScaledResolveBuffer(
               destination.buffer, destination.offset, destination.length);
  }

  auto* shared = command_processor_.shared_memory();
  destination.buffer = shared ? shared->GetBuffer() : nullptr;
  if (!destination.buffer) {
    return false;
  }
  // TODO(xenios-jp): Move resolve/export destination residency into the
  // command-processor materialization path before the render encoder is opened.
  // Keeping it classified here makes the remaining active shared-memory upload
  // breaks visible as resolve_copy_dest telemetry.
  return command_processor_.RequestSharedMemoryRange(
      MetalCommandProcessor::SharedMemoryRequestReason::kResolveCopyDest,
      resolve_info.copy_dest_extent_start,
      resolve_info.copy_dest_extent_length);
}

MTL::ComputePipelineState* MetalRenderTargetCache::GetDirectHostResolvePipeline(
    bool is_64bpp, xenos::MsaaSamples msaa_samples, bool scaled,
    bool source_is_uint) const {
  size_t msaa_index = DirectHostResolveMsaaIndex(msaa_samples);
  if (msaa_index >= kDirectHostResolveMsaaCount) {
    return nullptr;
  }
  return direct_host_resolve_pipelines_[DirectHostResolveBppIndex(
      is_64bpp)][msaa_index][scaled ? 1u : 0u][source_is_uint ? 1u : 0u];
}

MTL::ComputePipelineState*
MetalRenderTargetCache::GetDirectHostColorFullResolvePipeline(
    xenos::MsaaSamples msaa_samples, bool scaled, bool source_is_uint,
    draw_util::ResolveCopyShaderIndex copy_shader) const {
  size_t msaa_index = DirectHostResolveMsaaIndex(msaa_samples);
  size_t dest_index = DirectHostResolveFullDestIndex(copy_shader);
  if (msaa_index >= kDirectHostResolveMsaaCount ||
      dest_index >= kDirectHostResolveFullDestCount) {
    return nullptr;
  }
  return direct_host_color_full_resolve_pipelines_[msaa_index][scaled ? 1u : 0u]
                                                  [source_is_uint ? 1u : 0u]
                                                  [dest_index];
}

MTL::ComputePipelineState*
MetalRenderTargetCache::GetDirectHostDepthResolvePipeline(
    xenos::MsaaSamples msaa_samples, bool scaled) const {
  size_t msaa_index = DirectHostResolveMsaaIndex(msaa_samples);
  if (msaa_index >= kDirectHostResolveMsaaCount) {
    return nullptr;
  }
  return direct_host_depth_resolve_pipelines_[msaa_index][scaled ? 1u : 0u];
}

bool MetalRenderTargetCache::TryDirectHostResolveCopy(
    const draw_util::ResolveInfo& resolve_info,
    const draw_util::ResolveCopyShaderConstants& copy_constants,
    draw_util::ResolveCopyShaderIndex copy_shader, uint32_t dump_base,
    uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch,
    MTL::CommandBuffer* command_buffer, uint32_t& written_address,
    uint32_t& written_length) {
  TelemetryStats::ResolveDirectHostTelemetry& direct_telemetry =
      telemetry_.resolve_direct_host;
  ++direct_telemetry.direct_host_attempt;

  auto reject = []() { return false; };
  auto reject_gamma = [&]() {
    ++direct_telemetry.direct_host_reject_gamma;
    return false;
  };
  auto reject_exp_bias = [&]() {
    ++direct_telemetry.direct_host_reject_exp_bias;
    return false;
  };
  auto reject_format_mismatch = [&]() {
    ++direct_telemetry.direct_host_reject_format_mismatch;
    return false;
  };
  auto reject_sample_select = [&]() {
    ++direct_telemetry.direct_host_reject_sample_select;
    return false;
  };
  auto reject_depth_no_fast = [&]() {
    ++direct_telemetry.direct_host_reject_depth_no_fast;
    return false;
  };

  if (GetPath() != Path::kHostRenderTargets) {
    return reject();
  }
  const bool resolve_is_depth = resolve_info.IsCopyingDepth();
  const bool copy_shader_is_fast =
      IsResolveDirectHostRTFastCandidate(copy_shader);
  const bool copy_shader_is_full_color =
      !resolve_is_depth && IsResolveDirectHostRTFullColorCandidate(copy_shader);

  xenos::ColorRenderTargetFormat resolve_color_format =
      xenos::ColorRenderTargetFormat::k_8_8_8_8;
  xenos::DepthRenderTargetFormat resolve_depth_format =
      xenos::DepthRenderTargetFormat::kD24S8;
  if (resolve_is_depth) {
    if (!xenos::IsSingleCopySampleSelected(
            resolve_info.copy_dest_coordinate_info.copy_sample_select)) {
      return reject_sample_select();
    }
    if (!copy_shader_is_fast) {
      return reject_depth_no_fast();
    }
    resolve_depth_format =
        xenos::DepthRenderTargetFormat(resolve_info.depth_edram_info.format);
  } else {
    resolve_color_format =
        xenos::ColorRenderTargetFormat(resolve_info.color_edram_info.format);
    if (resolve_color_format ==
        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      // TODO(xenios-jp): Support direct host gamma resolves only once the
      // shader can mirror the dump path's linear RGBA16Unorm host storage to
      // guest PWL-gamma RGBA8 conversion.
      return reject_gamma();
    }
    if (copy_shader_is_fast) {
      if (!xenos::IsSingleCopySampleSelected(
              resolve_info.copy_dest_coordinate_info.copy_sample_select)) {
        return reject_sample_select();
      }
      if (resolve_info.copy_dest_info.copy_dest_exp_bias) {
        return reject_exp_bias();
      }
      if (!xenos::IsColorResolveFormatBitwiseEquivalent(
              resolve_color_format,
              xenos::ColorFormat(
                  resolve_info.copy_dest_info.copy_dest_format))) {
        return reject_format_mismatch();
      }
    } else if (!copy_shader_is_full_color) {
      if (!xenos::IsSingleCopySampleSelected(
              resolve_info.copy_dest_coordinate_info.copy_sample_select)) {
        return reject_sample_select();
      }
      if (resolve_info.copy_dest_info.copy_dest_exp_bias) {
        return reject_exp_bias();
      }
      if (!xenos::IsColorResolveFormatBitwiseEquivalent(
              resolve_color_format,
              xenos::ColorFormat(
                  resolve_info.copy_dest_info.copy_dest_format))) {
        return reject_format_mismatch();
      }
      return reject();
    }
  }

  struct DirectHostResolveConstants {
    uint32_t dispatch_offset;
    uint32_t dump_base;
    uint32_t dump_pitch_tiles;
    uint32_t source_base_tiles;
    uint32_t source_pitch_tiles;
    uint32_t thread_count_x;
    uint32_t thread_count_y;
    uint32_t height_scaled;
    uint32_t msaa_2x_sample_0;
    uint32_t msaa_2x_sample_1;
    uint32_t flags;
  };

  struct DirectHostResolveSource {
    RenderTargetKey key;
    MTL::Texture* texture;
    MTL::Texture* stencil_texture;
    MTL::ComputePipelineState* pipeline;
    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count;
    uint32_t flags;
    uint32_t pixels_per_thread;
    bool is_64bpp;
    bool is_depth;
  };

  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, rectangles);
  if (rectangles.empty()) {
    return reject();
  }

  uint64_t covered_tiles = 0;
  std::vector<DirectHostResolveSource> sources;
  sources.reserve(rectangles.size());
  for (const ResolveCopyDumpRectangle& rect : rectangles) {
    if (!rect.rows || rect.row_last_end <= rect.row_first_start) {
      return reject();
    }
    if (rect.rows == 1) {
      covered_tiles += rect.row_last_end - rect.row_first_start;
    } else {
      covered_tiles += dump_row_length_used - rect.row_first_start;
      covered_tiles += uint64_t(rect.rows - 2) * dump_row_length_used;
      covered_tiles += rect.row_last_end;
    }

    auto* rt = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!rt) {
      return reject();
    }
    RenderTargetKey key = rt->key();
    if (key.is_depth != resolve_is_depth) {
      return reject();
    }

    bool source_is_uint = false;
    MTL::PixelFormat expected_format = MTL::PixelFormatInvalid;
    MTL::Texture* texture = nullptr;
    MTL::Texture* stencil_texture = nullptr;
    MTL::ComputePipelineState* pipeline = nullptr;
    uint32_t source_flags = 0;
    bool is_64bpp = false;
    if (resolve_is_depth) {
      if (key.GetDepthFormat() != resolve_depth_format ||
          key.msaa_samples != resolve_info.depth_edram_info.msaa_samples) {
        return reject_format_mismatch();
      }
      texture = rt->texture();
      expected_format = GetDepthPixelFormat(key.GetDepthFormat());
      if (!::cvars::depth_float24_convert_in_pixel_shader &&
          ::cvars::depth_float24_round) {
        source_flags |= kDirectHostResolveDepthFlagRoundDepth;
      }
      stencil_texture = GetStencilTextureView(rt);
      if (stencil_texture) {
        source_flags |= kDirectHostResolveDepthFlagHasStencil;
      }
      pipeline = GetDirectHostDepthResolvePipeline(key.msaa_samples,
                                                   IsDrawResolutionScaled());
    } else {
      if (key.GetColorFormat() != resolve_color_format ||
          key.msaa_samples != resolve_info.color_edram_info.msaa_samples) {
        return reject_format_mismatch();
      }
      if (copy_shader_is_full_color &&
          !IsResolveDirectHostRTFullColorSourcePackable(resolve_color_format)) {
        return reject_format_mismatch();
      }

      MTL::PixelFormat ownership_transfer_format =
          GetColorOwnershipTransferPixelFormat(key.GetColorFormat(),
                                               &source_is_uint);
      texture = source_is_uint
                    ? ((key.msaa_samples != xenos::MsaaSamples::k1X &&
                        rt->msaa_texture())
                           ? rt->msaa_transfer_texture()
                           : rt->transfer_texture())
                    : rt->texture();
      expected_format = source_is_uint
                            ? ownership_transfer_format
                            : GetColorResourcePixelFormat(key.GetColorFormat());
      is_64bpp = key.Is64bpp();
      pipeline = copy_shader_is_full_color
                     ? GetDirectHostColorFullResolvePipeline(
                           key.msaa_samples, IsDrawResolutionScaled(),
                           source_is_uint, copy_shader)
                     : GetDirectHostResolvePipeline(is_64bpp, key.msaa_samples,
                                                    IsDrawResolutionScaled(),
                                                    source_is_uint);
    }
    if (!texture) {
      return reject();
    }
    if (texture->pixelFormat() != expected_format) {
      return reject_format_mismatch();
    }
    if (!pipeline) {
      return reject();
    }

    DirectHostResolveSource source = {};
    source.key = key;
    source.texture = texture;
    source.stencil_texture = stencil_texture;
    source.pipeline = pipeline;
    source.dispatch_count =
        rect.GetDispatches(dump_pitch, dump_row_length_used, source.dispatches);
    source.flags = source_flags;
    source.pixels_per_thread = DirectHostResolvePixelsPerThread(
        copy_shader, is_64bpp, key.msaa_samples);
    source.is_64bpp = is_64bpp;
    source.is_depth = resolve_is_depth;
    if (!source.dispatch_count) {
      return reject();
    }
    const uint32_t tile_size_x =
        (source.is_64bpp ? 40u : 80u) * draw_resolution_scale_x();
    const uint32_t tile_pixel_size_x =
        tile_size_x >>
        uint32_t(source.key.msaa_samples >= xenos::MsaaSamples::k4X);
    for (uint32_t i = 0; i < source.dispatch_count; ++i) {
      uint32_t dispatch_pixel_width =
          source.dispatches[i].width_tiles * tile_pixel_size_x;
      if (dispatch_pixel_width % source.pixels_per_thread) {
        return reject();
      }
    }
    sources.push_back(source);
  }

  const uint64_t required_tiles =
      uint64_t(dump_row_length_used) * uint64_t(dump_rows);
  if (covered_tiles != required_tiles) {
    return reject();
  }

  auto* texture_cache = command_processor_.texture_cache();
  const bool draw_resolution_scaled = IsDrawResolutionScaled();
  ResolveDestinationBuffer destination = {};
  if (!PrepareResolveDestinationBuffer(resolve_info, draw_resolution_scaled,
                                       destination)) {
    return reject();
  }

  command_processor_.SetSwapDestSwap(
      resolve_info.copy_dest_base, resolve_info.copy_dest_info.copy_dest_swap);

  ScopedAutoreleasePool autorelease_pool;
  bool standalone = false;
  MTL::CommandBuffer* cmd = command_buffer;
  if (!cmd) {
    cmd = command_processor_.CreateStandaloneTransferCommandBuffer(
        "XeniaCB reason=direct-host-resolve");
    if (!cmd) {
      return reject();
    }
    standalone = true;
  }

  EndSharedMemoryUploadBlitEncoderForCommandBuffer(command_processor_, cmd);
  MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
  if (!encoder) {
    if (standalone) {
      cmd->release();
    }
    return reject();
  }

  SetEncoderLabel(encoder, kDirectHostResolveEncoderLabel);
  PushEncoderDebugGroup(
      encoder,
      fmt::format(
          "{} rects={} dest={}", kDirectHostResolveEncoderLabel, sources.size(),
          draw_resolution_scaled ? "scaled_resolve_memory" : "shared_memory"));

  if (draw_resolution_scaled) {
    encoder->setBytes(&copy_constants.dest_relative,
                      sizeof(copy_constants.dest_relative), 0);
  } else {
    encoder->setBytes(&copy_constants, sizeof(copy_constants), 0);
  }
  encoder->setBuffer(destination.buffer, destination.offset, 1);
  encoder->useResource(destination.buffer, MTL::ResourceUsageWrite);

  const uint32_t scale_x = draw_resolution_scale_x();
  const uint32_t scale_y = draw_resolution_scale_y();
  const uint32_t height_scaled =
      (resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2) *
      scale_y;
  const uint32_t msaa_2x_sample_0 =
      draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_supported_);
  const uint32_t msaa_2x_sample_1 =
      draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_supported_);

  for (const DirectHostResolveSource& source : sources) {
    encoder->setTexture(source.texture, 0);
    if (source.is_depth) {
      encoder->setTexture(source.stencil_texture, 1);
    }
    encoder->useResource(source.texture, MTL::ResourceUsageRead);
    if (source.stencil_texture) {
      encoder->useResource(source.stencil_texture, MTL::ResourceUsageRead);
    }
    encoder->setComputePipelineState(source.pipeline);

    const uint32_t tile_size_x = (source.is_64bpp ? 40u : 80u) * scale_x;
    const uint32_t tile_size_y = 16u * scale_y;
    const uint32_t tile_pixel_size_x =
        tile_size_x >>
        uint32_t(source.key.msaa_samples >= xenos::MsaaSamples::k4X);
    const uint32_t tile_pixel_size_y =
        tile_size_y >>
        uint32_t(source.key.msaa_samples >= xenos::MsaaSamples::k2X);
    const uint32_t pixels_per_thread = source.pixels_per_thread;
    for (uint32_t i = 0; i < source.dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = source.dispatches[i];
      DirectHostResolveConstants constants = {};
      constants.dispatch_offset = dispatch.offset;
      constants.dump_base = dump_base;
      constants.dump_pitch_tiles = dump_pitch;
      constants.source_base_tiles = source.key.base_tiles;
      constants.source_pitch_tiles = source.key.GetPitchTiles();
      constants.thread_count_x =
          dispatch.width_tiles * tile_pixel_size_x / pixels_per_thread;
      constants.thread_count_y = dispatch.height_tiles * tile_pixel_size_y;
      constants.height_scaled = height_scaled;
      constants.msaa_2x_sample_0 = msaa_2x_sample_0;
      constants.msaa_2x_sample_1 = msaa_2x_sample_1;
      constants.flags = source.flags;
      encoder->setBytes(&constants, sizeof(constants), 2);

      uint32_t threadgroups_x = (constants.thread_count_x + 7u) >> 3;
      uint32_t threadgroups_y = (constants.thread_count_y + 7u) >> 3;
      encoder->dispatchThreadgroups(
          MTL::Size::Make(threadgroups_x, threadgroups_y, 1),
          MTL::Size::Make(8, 8, 1));
    }
  }

  if (!draw_resolution_scaled) {
    command_processor_.MarkSharedMemoryComputeWritePending(
        resolve_info.copy_dest_extent_start,
        resolve_info.copy_dest_extent_length, encoder);
    if (auto* shared_memory = command_processor_.shared_memory()) {
      shared_memory->MarkGpuAccess(resolve_info.copy_dest_extent_start,
                                   resolve_info.copy_dest_extent_length,
                                   command_processor_.GetCurrentSubmission());
    }
  }

  encoder->popDebugGroup();
  encoder->endEncoding();
  if (standalone) {
    command_processor_.CommitStandaloneAndWait(cmd);
  }

  written_address = resolve_info.copy_dest_extent_start;
  written_length = resolve_info.copy_dest_extent_length;
  if (texture_cache) {
    texture_cache->MarkRangeAsResolved(
        written_address, written_length,
        TextureCache::ResolveProvenanceSource::kDirectHost);
  }

  ++direct_telemetry.direct_host_success;
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
