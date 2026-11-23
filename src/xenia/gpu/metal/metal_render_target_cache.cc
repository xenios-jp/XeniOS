/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_render_target_cache.h"
#include "xenia/gpu/gpu_flags.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "third_party/stb/stb_image_write.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/metal/metal_transfer_encoder.h"
#include "xenia/gpu/metal/metal_util.h"
#include "xenia/gpu/strip_merge.h"

#include "xenia/gpu/edram_dump_shader.h"
#include "xenia/gpu/edram_transfer_shader.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(metal_transfer_in_draw_pass, true,
            "Encode render target ownership transfers at the head of the "
            "guest's own render pass instead of in passes of their own",
            "Metal");
DEFINE_bool(metal_defer_resolve_clears, true,
            "Leave a resolve clear of a whole render target to the next pass "
            "that binds it, as its clear load action, rather than clearing "
            "it in a pass of its own",
            "Metal");
DEFINE_bool(metal_resolve_store_dontcare, true,
            "Don't store the attachments of a render pass ended by a resolve "
            "that clears them whole without reading them",
            "Metal");
DEFINE_bool(
    metal_gamma_fetch_blend, true,
    "Store 8_8_8_8_GAMMA render targets as 8-bit piecewise linear gamma, as "
    "the console's EDRAM holds them, in the same resources as 8_8_8_8, and "
    "blend draws into them in the pixel shader: read the target through "
    "framebuffer fetch, convert both colors to 10-bit linear, blend with the "
    "draw's factors and encode, as the console's render backend does. Needs "
    "programmable blending (Apple GPUs) and the spirv2air shader compiler; "
    "replaces gamma_render_target_as_unorm16 where it applies.",
    "Metal");
DEFINE_bool(
    metal_rt16_fetch_blend, true,
    "Write 16_16 and 16_16_16_16 render targets with the console's fixed "
    "point encode into their snorm attachments, which hold the EDRAM's bits, "
    "and blend draws into them in the pixel shader where fixed-function "
    "blending isn't exact (factors other than ONE and ZERO, and 16_16's green, "
    "which the console blends under the alpha control): read the target "
    "through framebuffer fetch and blend with the console's 16-bit fixed "
    "point blender. Needs programmable blending (Apple GPUs), the spirv2air "
    "shader compiler and snorm16_render_target_full_range.",
    "Metal");
DEFINE_int32(metal_memory_log_rate, 0,
             "Log Metal render target/pipeline/instance buffer sizes every N "
             "frames (0 to disable)",
             "Metal");

namespace xe {
namespace gpu {
namespace metal {

// MetalRenderTarget implementation
MetalRenderTargetCache::MetalRenderTarget::~MetalRenderTarget() {
  auto release = [](MTL::Texture*& texture) {
    if (texture) {
      texture->release();
      texture = nullptr;
    }
  };
  release(spare_depth_stencil_view_);
  release(spare_depth_texture_);
  release(stencil_view_);
  if (transfer_texture_ != texture_) {
    release(transfer_texture_);
  }
  release(texture_);
}

// MetalRenderTargetCache implementation
MetalRenderTargetCache::MetalRenderTargetCache(
    const RegisterFile& register_file, const Memory& memory,
    TraceWriter* trace_writer, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y, MetalCommandProcessor& command_processor)
    : RenderTargetCache(register_file, memory, trace_writer,
                        draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      trace_writer_(trace_writer),
      resolve_encoder_(*this, command_processor),
      transfer_encoder_(
          std::make_unique<MetalTransferEncoder>(*this, command_processor)) {}

MetalRenderTargetCache::~MetalRenderTargetCache() { Shutdown(true); }

RenderTargetCache::Path MetalRenderTargetCache::GetPath() const {
  return Path::kHostRenderTargets;
}

bool MetalRenderTargetCache::Initialize() {
  device_ = command_processor_.GetMetalDevice();
  if (!device_) {
    XELOGE("MetalRenderTargetCache: No Metal device available");
    return false;
  }

  // 2x msaa and unorm16 support virtually guarunteed as minimum OS target /
  // Metal version currently is MacOS 15 / Metal 3
  msaa_2x_supported_ = device_->supportsTextureSampleCount(2);

  gamma_fetch_blend_ = ::cvars::metal_gamma_fetch_blend &&
                       command_processor_.SupportsHostRenderTargetFeedback();
  // 8-bit storage blends correctly when the shader blends, so the 16-bit
  // linear storage is only for when it can't.
  gamma_render_target_as_unorm16_ =
      ::cvars::gamma_render_target_as_unorm16 && !gamma_fetch_blend_;
  fixed16_fetch_blend_ = ::cvars::metal_rt16_fetch_blend &&
                         ::cvars::snorm16_render_target_full_range &&
                         command_processor_.SupportsHostRenderTargetFeedback();
  XELOGI("MetalRenderTargetCache: 16_16 and 16_16_16_16 {}",
         fixed16_fetch_blend_ ? "encoded and blended in the shader"
                              : "blended fixed-function");
  XELOGI("MetalRenderTargetCache: 8_8_8_8_GAMMA as {}",
         gamma_fetch_blend_ ? "8-bit gamma with shader blending"
         : gamma_render_target_as_unorm16_ ? "16-bit linear"
                                           : "8-bit gamma");

  // Create the EDRAM buffer.
  //
  // The guest has 10 MiB of EDRAM for samples, but with host resolution
  // scaling enabled the compute path addresses a scaled EDRAM layout (the
  // shaders multiply the tile dimensions by resolution_scale_x/y). The buffer
  // therefore must be scaled by the same factor to avoid out-of-bounds writes.
  const uint32_t scale_x = std::max<uint32_t>(1u, draw_resolution_scale_x());
  const uint32_t scale_y = std::max<uint32_t>(1u, draw_resolution_scale_y());
  const size_t edram_dwords = size_t(xenos::kEdramTileCount) *
                              size_t(xenos::kEdramTileWidthSamples) *
                              size_t(xenos::kEdramTileHeightSamples) *
                              size_t(scale_x) * size_t(scale_y);
  const size_t edram_size_bytes = edram_dwords * sizeof(uint32_t);
  const bool edram_cpu_visible = false;
  const MTL::ResourceOptions edram_storage_mode =
      edram_cpu_visible ? MTL::ResourceStorageModeShared
                        : MTL::ResourceStorageModePrivate;
  edram_buffer_ = device_->newBuffer(edram_size_bytes, edram_storage_mode);
  if (!edram_buffer_) {
    XELOGE("MetalRenderTargetCache: Failed to create EDRAM buffer");
    return false;
  }
  edram_buffer_->setLabel(
      NS::String::string("EDRAM Buffer", NS::UTF8StringEncoding));
  if (edram_cpu_visible) {
    void* edram_contents = edram_buffer_->contents();
    if (edram_contents) {
      std::memset(edram_contents, 0, edram_size_bytes);
    }
  } else {
    ScopedAutoreleasePool autorelease_pool;
    MTL::CommandBuffer* cmd = command_processor_.CreateAccountedCommandBuffer(
        MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
    if (cmd) {
      command_processor_.EndSharedComputeEncoder(cmd);
      MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
      if (blit) {
        blit->fillBuffer(
            edram_buffer_,
            NS::Range::Make(0, static_cast<NS::UInteger>(edram_size_bytes)), 0);
        blit->endEncoding();
        cmd->commit();
      } else {
        command_processor_.DiscardAccountedCommandBuffer(
            cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
      }
    }
  }
  // Initialize EDRAM compute shaders
  if (!InitializeEdramComputeShaders()) {
    XELOGE(
        "MetalRenderTargetCache: Failed to initialize EDRAM compute shaders");
    return false;
  }

  // Initialize base class
  InitializeCommon();

  return true;
}

void MetalRenderTargetCache::Shutdown(bool from_destructor) {
  if (pass_stats_.draw_passes || pass_stats_.tile_resolves ||
      pass_stats_.direct_resolves || pass_stats_.edram_resolves) {
    LogPassStats();
    pass_stats_ = {};
  }
  ClearPendingDrawPassTransfers();
  post_pass_resolves_.clear();
  ClearPostPassRefreshCopies();
  for (MTL::Texture* scratch : refresh_scratch_pool_) {
    scratch->release();
  }
  refresh_scratch_pool_.clear();
  if (!from_destructor) {
    ClearCache();
  }

  // Clean up dummy target
  dummy_color_targets_.clear();
  dummy_color_target_ = nullptr;
  if (cached_render_pass_descriptor_) {
    cached_render_pass_descriptor_->release();
    cached_render_pass_descriptor_ = nullptr;
  }

  for (auto& retarget_clear_state : retarget_clear_states_) {
    if (retarget_clear_state.second) {
      retarget_clear_state.second->release();
    }
  }
  retarget_clear_states_.clear();

  // Clean up EDRAM compute shaders
  ShutdownEdramComputeShaders();

  if (edram_buffer_) {
    edram_buffer_->release();
    edram_buffer_ = nullptr;
  }

  // Destroy all render targets
  constant_contents_.clear();
  DestroyAllRenderTargets(!from_destructor);
  render_target_map_.clear();

  // Shutdown base class
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool MetalRenderTargetCache::InitializeEdramComputeShaders() {
  // Initialize the resolve / EDRAM compute and transfer pipelines used by the
  // Metal backend.
  const bool draw_resolution_scaled = IsDrawResolutionScaled();
  if (!resolve_encoder_.Initialize(device_, draw_resolution_scaled)) {
    return false;
  }
  if (!transfer_encoder_->Initialize(device_)) {
    return false;
  }
  return true;
}

void MetalRenderTargetCache::ShutdownEdramComputeShaders() {
  resolve_encoder_.Shutdown();
  if (transfer_encoder_) {
    transfer_encoder_->Shutdown();
  }
}

void MetalRenderTargetCache::ClearCache() {
  ClearPendingDrawPassTransfers();
  // It names render targets about to be destroyed.
  post_pass_resolves_.clear();
  ClearPostPassRefreshCopies();
  dead_store_render_targets_.clear();
  constant_contents_.clear();

  // Clear current bindings
  for (uint32_t i = 0; i < 4; ++i) {
    current_color_targets_[i] = nullptr;
  }
  current_depth_target_ = nullptr;
  render_pass_descriptor_dirty_ = true;
  render_pass_encoder_created_since_targets_changed_ = false;

  // Clear the tracking of which render targets have been cleared
  cleared_render_targets_this_frame_.clear();
  dummy_color_targets_.clear();
  dummy_color_target_ = nullptr;
  render_target_map_.clear();

  // Call base implementation
  RenderTargetCache::ClearCache();
}

void MetalRenderTargetCache::RenderTargetReused(RenderTarget& render_target,
                                                RenderTargetKey old_key) {
  MetalRenderTarget& metal_render_target =
      static_cast<MetalRenderTarget&>(render_target);
  ForgetConstantContent(&metal_render_target);
  auto it = render_target_map_.find(old_key.key);
  if (it != render_target_map_.end() && it->second == &metal_render_target) {
    render_target_map_.erase(it);
  }
  render_target_map_[render_target.key().key] = &metal_render_target;
  // Cleared on the first use like a new one.
  metal_render_target.SetNeedsInitialClear(true);
}

bool MetalRenderTargetCache::PerformStripMergeTransfers(
    RenderTarget* dest, const std::vector<Transfer>& transfers) {
  // Outside any pass, after what is queued for one: a twin may be made in the
  // middle of an update.
  command_processor_.EndRenderEncoder();
  if (!FlushPendingDrawPassTransfers()) {
    return false;
  }
  RenderTarget* render_targets[] = {dest};
  bool result = PerformTransfersAndResolveClears(1, render_targets, &transfers);
  render_pass_descriptor_dirty_ = true;
  return result;
}

bool MetalRenderTargetCache::CanReuseRenderTarget(
    const RenderTarget& render_target) const {
  // Transfers that failed to flush still reference their sources.
  if (HasPendingDrawPassTransfers()) {
    return false;
  }
  // A deferred resolve clear is looked up by the key it was deferred at.
  if (static_cast<const MetalRenderTarget&>(render_target).clear_deferred()) {
    return false;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_color_targets_[i] == &render_target ||
        last_real_color_targets_[i] == &render_target) {
      return false;
    }
  }
  if (current_depth_target_ == &render_target ||
      last_real_depth_target_ == &render_target) {
    return false;
  }
  for (const RenderTarget* pending_render_target :
       pending_draw_pass_render_targets_) {
    if (pending_render_target == &render_target) {
      return false;
    }
  }
  return true;
}

void MetalRenderTargetCache::BeginFrame() {
  SCOPE_profile_cpu_f("gpu");
  (void)FlushPendingDrawPassTransfers();

  ++frame_id_;

  // Clear the tracking of which render targets have been cleared this frame
  cleared_render_targets_this_frame_.clear();

  // Call base implementation
  RenderTargetCache::BeginFrame();

  if (::cvars::metal_memory_log_rate > 0 &&
      (frame_id_ % uint64_t(::cvars::metal_memory_log_rate)) == 0) {
    XELOGI(
        "Metal mem: frame={} rt={} map={} dummy={} pipelines={} "
        "transfer_shaders={}",
        frame_id_, render_target_map_.size(), render_target_map_.size(),
        dummy_color_targets_.size(), transfer_encoder_->transfer_pipeline_count(),
        transfer_encoder_->transfer_fragment_function_count());
  }
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetWidth() const {
  // Metal maximum texture dimension
  return 16384;
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetHeight() const {
  // Metal maximum texture dimension
  return 16384;
}

bool MetalRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

RenderTargetCache::RenderTarget* MetalRenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  SCOPE_profile_cpu_f("gpu");
  // Calculate dimensions
  uint32_t width = key.GetWidth();
  uint32_t height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples);
  if (key.strip_merged) {
    // Every strip's band (strip carries the same shape).
    height = strip_merge_bands_.count * GetStripMergeBandRows(key);
  }

  // Apply resolution scaling
  width *= draw_resolution_scale_x();
  height *= draw_resolution_scale_y();

  // Create Metal render target
  auto* render_target = new MetalRenderTarget(key);

  // Create the texture based on format
  MTL::Texture* texture = nullptr;
  uint32_t samples = 1 << uint32_t(key.msaa_samples);

  MTL::Texture* stencil_texture = nullptr;
  if (key.is_depth) {
    texture = CreateDepthTexture(width, height, key.GetDepthFormat(), samples,
                                 &stencil_texture);
  } else {
    texture = CreateColorTexture(width, height, key.GetColorFormat(), samples);
  }

  if (!texture) {
    delete render_target;
    return nullptr;
  }

  render_target->SetTexture(texture);
  if (stencil_texture) {
    render_target->SetStencilView(stencil_texture);
  }
  if (!key.is_depth) {
    MTL::PixelFormat resource_format =
        GetColorResourcePixelFormat(key.GetColorFormat());
    MTL::PixelFormat transfer_format =
        GetColorOwnershipTransferPixelFormat(key.GetColorFormat(), nullptr);
    if (transfer_format != resource_format) {
      MTL::Texture* transfer_view = texture->newTextureView(transfer_format);
      RecordRenderTargetViewCreated();
      render_target->SetTransferTexture(transfer_view);
    }
  }

  // NOTE: Unlike the previous implementation, we do NOT load EDRAM data here.
  // This matches D3D12's approach where:
  // 1. CreateRenderTarget creates an empty texture
  // 2. Data transfer happens via ownership transfers in
  // PerformTransfersAndResolveClears
  // 3. The EDRAM buffer is only used as scratch space for resolves
  //
  // The ownership transfer system (called from Update()) handles copying data
  // between render target textures when EDRAM regions are aliased between
  // different RT configurations.

  // Store in our map for later retrieval
  render_target_map_[key.key] = render_target;

  return render_target;
}

bool MetalRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  // Metal uses different depth encoding than Xbox 360
  // D24S8 on Xbox 360 vs D32Float_S8 on Metal
  return format == xenos::DepthRenderTargetFormat::kD24S8 ||
         format == xenos::DepthRenderTargetFormat::kD24FS8;
}

void MetalRenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  // Every backing is rewritten from the snapshot.
  tile_transfer_records_.fill({});
  if (!snapshot) {
    return;
  }

  if (IsDrawResolutionScaled()) {
    return;
  }

  RenderTarget* full_edram_rt =
      PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
          xenos::ColorRenderTargetFormat::k_32_FLOAT);
  if (!full_edram_rt) {
    return;
  }

  MetalRenderTarget* metal_rt = static_cast<MetalRenderTarget*>(full_edram_rt);
  MTL::Texture* texture = metal_rt->texture();
  if (!texture) {
    return;
  }

  constexpr uint32_t kPitchTilesAt32bpp = 16;
  constexpr uint32_t kWidth =
      kPitchTilesAt32bpp * xenos::kEdramTileWidthSamples;
  constexpr uint32_t kTileRows = xenos::kEdramTileCount / kPitchTilesAt32bpp;
  constexpr uint32_t kHeight = kTileRows * xenos::kEdramTileHeightSamples;

  size_t staging_size = size_t(kWidth) * size_t(kHeight) * sizeof(uint32_t);
  MTL::Buffer* staging =
      device_->newBuffer(staging_size, MTL::ResourceStorageModeShared);
  if (!staging) {
    return;
  }

  auto* dst_base = static_cast<uint8_t*>(staging->contents());
  const uint8_t* src = static_cast<const uint8_t*>(snapshot);
  uint32_t bytes_per_row = kWidth * sizeof(uint32_t);

  for (uint32_t y_tile = 0; y_tile < kTileRows; ++y_tile) {
    for (uint32_t x_tile = 0; x_tile < kPitchTilesAt32bpp; ++x_tile) {
      uint32_t tile_index = y_tile * kPitchTilesAt32bpp + x_tile;
      const uint8_t* tile_src =
          src + tile_index * xenos::kEdramTileWidthSamples *
                    xenos::kEdramTileHeightSamples * sizeof(uint32_t);

      for (uint32_t sample_row = 0; sample_row < xenos::kEdramTileHeightSamples;
           ++sample_row) {
        uint32_t dst_y = y_tile * xenos::kEdramTileHeightSamples + sample_row;
        uint32_t dst_x = x_tile * xenos::kEdramTileWidthSamples;

        uint8_t* dst_row =
            dst_base + dst_y * bytes_per_row + dst_x * sizeof(uint32_t);
        const uint8_t* src_row = tile_src + sample_row *
                                                xenos::kEdramTileWidthSamples *
                                                sizeof(uint32_t);

        std::memcpy(dst_row, src_row,
                    xenos::kEdramTileWidthSamples * sizeof(uint32_t));
      }
    }
  }

  ScopedAutoreleasePool autorelease_pool;
  MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
  if (!queue) {
    staging->release();
    return;
  }

  MTL::CommandBuffer* cmd = command_processor_.CreateAccountedCommandBuffer(
      MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
  if (!cmd) {
    staging->release();
    return;
  }

  command_processor_.EndSharedComputeEncoder(cmd);
  MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
  if (!blit) {
    command_processor_.DiscardAccountedCommandBuffer(
        cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
    staging->release();
    return;
  }

  blit->copyFromBuffer(staging, 0, bytes_per_row, 0,
                       MTL::Size::Make(kWidth, kHeight, 1), texture, 0, 0,
                       MTL::Origin::Make(0, 0, 0));
  blit->endEncoding();
  command_processor_.CommitAndAwait(
      cmd, MetalCommandProcessor::GpuWaitReason::kSnapshot);
  // cmd is autoreleased from commandBuffer() - do not release
  staging->release();
  if (metal_rt->needs_initial_clear()) {
    metal_rt->SetNeedsInitialClear(false);
    render_pass_descriptor_dirty_ = true;
  }

  // Seed edram_buffer_ with the restored full-EDRAM render target contents
  // so subsequent DumpRenderTargets and resolve passes see the same initial
  // EDRAM state as D3D12/Vulkan.
  DumpRenderTargets(0, kPitchTilesAt32bpp, kTileRows, kPitchTilesAt32bpp);
}

MTL::Texture* MetalRenderTargetCache::CreateColorTexture(
    uint32_t width, uint32_t height, xenos::ColorRenderTargetFormat format,
    uint32_t samples, bool transient_render_target_only) {
  MTL::PixelFormat resource_format = GetColorResourcePixelFormat(format);
  MTL::PixelFormat transfer_format =
      GetColorOwnershipTransferPixelFormat(format, nullptr);
  // PixelFormatView turns lossless compression off on Apple5+, so only the
  // formats with a separate transfer view get it.
  bool needs_pixel_format_view = transfer_format != resource_format;

  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);  // Default height if not specified
  desc->setPixelFormat(resource_format);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  MTL::TextureUsage usage = MTL::TextureUsageRenderTarget;
  if (!transient_render_target_only) {
    usage |= MTL::TextureUsageShaderRead;
  }
  if (needs_pixel_format_view) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);

  MTL::Texture* texture = nullptr;
  const bool can_use_memoryless =
      transient_render_target_only && !needs_pixel_format_view &&
      device_->supportsFamily(MTL::GPUFamilyApple1);
  if (can_use_memoryless) {
    // Dummy fallback color targets are transient (load/store don't care) and
    // never sampled - memoryless is optimal on Apple TBDR GPUs.
    desc->setStorageMode(MTL::StorageModeMemoryless);
    texture = device_->newTexture(desc);
  }
  if (!texture) {
    desc->setStorageMode(MTL::StorageModePrivate);
    texture = device_->newTexture(desc);
  }
  desc->release();
  // Initial clear is handled on first bind via load actions; avoid
  // synchronous clears here to keep the host RT path fast.
  return texture;
}

MTL::Texture* MetalRenderTargetCache::CreateDummyColorTexture(
    uint32_t width, uint32_t height, uint32_t samples) {
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);
  // Nothing reads this and the write mask is empty, so the narrowest renderable
  // format will do - it still costs tile memory on every depth-only pass.
  desc->setPixelFormat(MTL::PixelFormatR8Unorm);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  desc->setUsage(MTL::TextureUsageRenderTarget);

  MTL::Texture* texture = nullptr;
  if (device_->supportsFamily(MTL::GPUFamilyApple1)) {
    desc->setStorageMode(MTL::StorageModeMemoryless);
    texture = device_->newTexture(desc);
  }
  if (!texture) {
    desc->setStorageMode(MTL::StorageModePrivate);
    texture = device_->newTexture(desc);
  }
  desc->release();
  return texture;
}

MTL::Texture* MetalRenderTargetCache::CreateDepthTexture(
    uint32_t width, uint32_t height, xenos::DepthRenderTargetFormat format,
    uint32_t samples, MTL::Texture** stencil_out) {
  *stencil_out = nullptr;
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);  // Default height if not specified
  desc->setPixelFormat(GetDepthPixelFormat(format));
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  // No PixelFormatView (or ShaderWrite): on Apple5+ either turns lossless
  // compression off. Shaders read the stencil from the Stencil8 texture
  // directly rather than through an X32_Stencil8 view, which would need it.
  desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  desc->setStorageMode(MTL::StorageModePrivate);

  MTL::Texture* texture = device_->newTexture(desc);
  if (texture) {
    desc->setPixelFormat(MTL::PixelFormatStencil8);
    MTL::Texture* stencil = device_->newTexture(desc);
    if (stencil) {
      *stencil_out = stencil;
    } else {
      texture->release();
      texture = nullptr;
    }
  }
  desc->release();
  // Initial clear is handled on first bind via load actions; avoid
  // synchronous clears here to keep the host RT path fast.
  return texture;
}

MTL::PixelFormat MetalRenderTargetCache::GetColorResourcePixelFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return MTL::PixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      // Updated to suport unorm16 for gamma render targets.
      return gamma_render_target_as_unorm16_ ? MTL::PixelFormatRGBA16Unorm
                                             : MTL::PixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return MTL::PixelFormatRGB10A2Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      // Match D3D12 behavior: store as RGBA16F and pack to float10 on dump.
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return MTL::PixelFormatRG16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return MTL::PixelFormatRGBA16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    default:
      XELOGE("MetalRenderTargetCache: Unsupported color format {}",
             static_cast<uint32_t>(format));
      return MTL::PixelFormatRGBA8Unorm;
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetColorOwnershipTransferPixelFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Uint;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Uint;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Uint;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Uint;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      // Ownership transfers must use a linear resource view to avoid
      // implicit sRGB conversion for gamma render targets.
      return GetColorResourcePixelFormat(format);
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetDepthPixelFormat(
    xenos::DepthRenderTargetFormat format) const {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
    case xenos::DepthRenderTargetFormat::kD24FS8:
      // Metal doesn't have D24S8. The stencil is a separate Stencil8 texture
      // (see CreateDepthTexture).
      return MTL::PixelFormatDepth32Float;
    default:
      XELOGE("MetalRenderTargetCache: Unsupported depth format {}",
             static_cast<uint32_t>(format));
      return MTL::PixelFormatDepth32Float;
  }
}

MTL::Texture* MetalRenderTargetCache::GetStencilTextureView(
    MetalRenderTarget* render_target) {
  // Depth targets keep their separate Stencil8 texture there; color targets
  // have none.
  return render_target ? render_target->stencil_view() : nullptr;
}

MTL::RenderPassDescriptor* MetalRenderTargetCache::GetRenderPassDescriptor(
    uint32_t expected_sample_count, bool render_encoder_pending) {
  if (!render_pass_descriptor_dirty_ && cached_render_pass_descriptor_ &&
      cached_render_pass_descriptor_sample_count_ == expected_sample_count) {
    // Queuing transfers deliberately does not dirty the descriptor, so their
    // load actions are the one thing this path still has to bring up to date.
    if (render_encoder_pending) {
      ApplyPendingDrawPassLoadActions();
    }
    return cached_render_pass_descriptor_;
  }
  if (cached_render_pass_descriptor_sample_count_ != expected_sample_count) {
    render_pass_descriptor_dirty_ = true;
  }

  // Release old descriptor
  if (cached_render_pass_descriptor_) {
    cached_render_pass_descriptor_->release();
    cached_render_pass_descriptor_ = nullptr;
  }

  // Create new descriptor
  cached_render_pass_descriptor_ =
      MTL::RenderPassDescriptor::renderPassDescriptor();
  if (!cached_render_pass_descriptor_) {
    XELOGE("MetalRenderTargetCache: Failed to create render pass descriptor");
    return nullptr;
  }
  cached_render_pass_descriptor_->retain();
  cached_render_pass_descriptor_sample_count_ = expected_sample_count;
  cached_render_pass_descriptor_pending_clears_.fill(nullptr);

  // Queued transfers that rewrite a destination in full make loading its old
  // contents into tile memory pointless, but only for the pass that actually
  // encodes them - ClearPendingDrawPassTransfers restores this if it doesn't.
  pending_draw_pass_load_dontcare_mask_ = GetPendingDrawPassLoadDontCareMask();
  // The store actions are chosen when the pass ends, in
  // FinalizeRenderPassStoreActions, once it's known whether anything still
  // needs the contents.
  const MTL::StoreAction draw_pass_store_action =
      ::cvars::metal_resolve_store_dontcare ? MTL::StoreActionUnknown
                                            : MTL::StoreActionStore;
  auto pending_load_dontcare = [this](uint32_t pending_index) {
    return (pending_draw_pass_load_dontcare_mask_ &
            (uint32_t(1) << pending_index)) != 0;
  };

  bool has_any_render_target = false;
  bool has_any_color_target = false;
  uint32_t coverage_width = 0;
  uint32_t coverage_height = 0;
  uint32_t coverage_samples = std::max(1u, expected_sample_count);

  // Bind the actual render targets retrieved from base class in Update()

  // Bind depth target if present
  if (current_depth_target_ && current_depth_target_->texture()) {
    auto* depth_attachment = cached_render_pass_descriptor_->depthAttachment();
    depth_attachment->setTexture(current_depth_target_->texture());

    // Clear on first bind to avoid synchronous clears at creation.
    uint32_t depth_key = current_depth_target_->key().key;
    bool depth_needs_clear = current_depth_target_->needs_initial_clear();
    bool depth_load_dontcare = pending_load_dontcare(0);
    if (depth_needs_clear) {
      depth_attachment->setLoadAction(MTL::LoadActionClear);
      depth_attachment->setClearDepth(current_depth_target_->clear_depth());
      cached_render_pass_descriptor_pending_clears_[0] = current_depth_target_;
    } else {
      depth_attachment->setLoadAction(
          depth_load_dontcare ? MTL::LoadActionDontCare : MTL::LoadActionLoad);
    }
    depth_attachment->setStoreAction(draw_pass_store_action);

    // Bind the depth target's stencil texture to the stencil attachment too
    // (Metal requires explicit stencil attachment binding to match pipeline
    // state).
    if (MTL::Texture* stencil_texture =
            current_depth_target_->stencil_attachment_texture()) {
      auto* stencil_attachment =
          cached_render_pass_descriptor_->stencilAttachment();
      stencil_attachment->setTexture(stencil_texture);
      if (depth_needs_clear) {
        stencil_attachment->setLoadAction(MTL::LoadActionClear);
        stencil_attachment->setClearStencil(
            current_depth_target_->clear_stencil());
      } else {
        // The queued depth transfers clear and rewrite stencil over the same
        // rectangles they cover.
        stencil_attachment->setLoadAction(depth_load_dontcare
                                              ? MTL::LoadActionDontCare
                                              : MTL::LoadActionLoad);
      }
      stencil_attachment->setStoreAction(draw_pass_store_action);
    }

    has_any_render_target = true;

    // Track this as a real render target for capture
    last_real_depth_target_ = current_depth_target_;

    if (!coverage_width && current_depth_target_->texture()) {
      coverage_width =
          static_cast<uint32_t>(current_depth_target_->texture()->width());
      coverage_height =
          static_cast<uint32_t>(current_depth_target_->texture()->height());
      if (current_depth_target_->texture()->sampleCount() > 0) {
        coverage_samples = std::max<uint32_t>(
            coverage_samples,
            static_cast<uint32_t>(
                current_depth_target_->texture()->sampleCount()));
      }
    }
  }

  // Bind color targets
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_color_targets_[i] && current_color_targets_[i]->texture()) {
      auto* color_attachment =
          cached_render_pass_descriptor_->colorAttachments()->object(i);
      color_attachment->setTexture(current_color_targets_[i]->texture());

      // Clear on first bind to avoid synchronous clears at creation.
      bool color_needs_clear = current_color_targets_[i]->needs_initial_clear();
      if (color_needs_clear) {
        color_attachment->setLoadAction(MTL::LoadActionClear);
        color_attachment->setClearColor(
            current_color_targets_[i]->clear_color());
        cached_render_pass_descriptor_pending_clears_[1 + i] =
            current_color_targets_[i];
      } else {
        color_attachment->setLoadAction(pending_load_dontcare(i + 1)
                                            ? MTL::LoadActionDontCare
                                            : MTL::LoadActionLoad);
      }
      color_attachment->setStoreAction(draw_pass_store_action);

      has_any_render_target = true;
      has_any_color_target = true;

      // Track this as a real render target for capture
      last_real_color_targets_[i] = current_color_targets_[i];

      if (!coverage_width) {
        coverage_width = static_cast<uint32_t>(
            current_color_targets_[i]->texture()->width());
        coverage_height = static_cast<uint32_t>(
            current_color_targets_[i]->texture()->height());
        if (current_color_targets_[i]->texture()->sampleCount() > 0) {
          coverage_samples = std::max<uint32_t>(
              coverage_samples,
              static_cast<uint32_t>(
                  current_color_targets_[i]->texture()->sampleCount()));
        }
      }
    }
  }

  // With neither a color nor a depth target bound, attach a dummy color target,
  // like the RTV D3D12 and Vulkan always bind when drawing. A depth-only pass
  // has no color attachment.
  const bool omit_dummy_color =
      current_depth_target_ && current_depth_target_->texture();
  if (!has_any_color_target && !omit_dummy_color) {
    uint32_t samples = std::max(1u, expected_sample_count);

    uint32_t width = 1280;
    uint32_t height = 720;
    if (current_depth_target_ && current_depth_target_->texture()) {
      width = static_cast<uint32_t>(current_depth_target_->texture()->width());
      height =
          static_cast<uint32_t>(current_depth_target_->texture()->height());
      if (current_depth_target_->texture()->sampleCount() > 0) {
        samples = std::max<uint32_t>(
            samples, static_cast<uint32_t>(
                         current_depth_target_->texture()->sampleCount()));
      }
    } else if (last_real_color_targets_[0] &&
               last_real_color_targets_[0]->texture()) {
      width = static_cast<uint32_t>(
          last_real_color_targets_[0]->texture()->width());
      height = static_cast<uint32_t>(
          last_real_color_targets_[0]->texture()->height());
    } else if (last_real_depth_target_ && last_real_depth_target_->texture()) {
      width =
          static_cast<uint32_t>(last_real_depth_target_->texture()->width());
      height =
          static_cast<uint32_t>(last_real_depth_target_->texture()->height());
      if (last_real_depth_target_->texture()->sampleCount() > 0) {
        samples = std::max<uint32_t>(
            samples, static_cast<uint32_t>(
                         last_real_depth_target_->texture()->sampleCount()));
      }
    }

    uint32_t dummy_sample_count =
        samples >= 4u ? 4u : (samples == 2u ? 2u : 1u);
    // Cache dummy color targets by shape only so depth-only passes with
    // changing EDRAM bases can reuse the same transient attachment.
    uint64_t dummy_key = uint64_t(width & 0xFFFFu) |
                         (uint64_t(height & 0xFFFFu) << 16) |
                         (uint64_t(dummy_sample_count & 0xFFu) << 32);
    auto evict_oldest_dummy_target = [&](uint64_t keep_key) -> bool {
      uint64_t oldest_key = 0;
      uint64_t oldest_frame = frame_id_;
      bool found = false;
      for (const auto& it : dummy_color_targets_) {
        if (it.first == keep_key) {
          continue;
        }
        if (!found || it.second.last_used_frame < oldest_frame) {
          oldest_frame = it.second.last_used_frame;
          oldest_key = it.first;
          found = true;
        }
      }
      if (found) {
        dummy_color_targets_.erase(oldest_key);
      }
      return found;
    };

    auto& entry = dummy_color_targets_[dummy_key];
    if (!entry.target || !entry.target->texture()) {
      RenderTargetKey dummy_rt_key;
      dummy_rt_key.key = 0;
      dummy_rt_key.is_depth = 0;
      dummy_rt_key.msaa_samples =
          dummy_sample_count >= 4u   ? xenos::MsaaSamples::k4X
          : dummy_sample_count == 2u ? xenos::MsaaSamples::k2X
                                     : xenos::MsaaSamples::k1X;
      entry.target = std::make_unique<MetalRenderTarget>(dummy_rt_key);
      entry.last_cleared_frame = frame_id_ - 1;
      MTL::Texture* tex =
          CreateDummyColorTexture(width, height, dummy_sample_count);
      while (!tex && dummy_color_targets_.size() > 1 &&
             evict_oldest_dummy_target(dummy_key)) {
        tex = CreateDummyColorTexture(width, height, dummy_sample_count);
      }
      entry.target->SetTexture(tex);
    }

    entry.last_used_frame = frame_id_;
    dummy_color_target_ = entry.target.get();

    // Keep this cache small - dummy targets are transient fallback attachments.
#if XE_PLATFORM_IOS
    constexpr size_t kMaxDummyColorTargets = 4;
#else
    constexpr size_t kMaxDummyColorTargets = 8;
#endif
    while (dummy_color_targets_.size() > kMaxDummyColorTargets) {
      if (!evict_oldest_dummy_target(dummy_key)) {
        break;
      }
    }

    auto* color_attachment =
        cached_render_pass_descriptor_->colorAttachments()->object(0);
    color_attachment->setTexture(dummy_color_target_->texture());
    color_attachment->setLoadAction(MTL::LoadActionDontCare);
    color_attachment->setStoreAction(MTL::StoreActionDontCare);

    has_any_render_target = true;
    if (!coverage_width && dummy_color_target_->texture()) {
      coverage_width =
          static_cast<uint32_t>(dummy_color_target_->texture()->width());
      coverage_height =
          static_cast<uint32_t>(dummy_color_target_->texture()->height());
      if (dummy_color_target_->texture()->sampleCount() > 0) {
        coverage_samples = std::max<uint32_t>(
            coverage_samples,
            static_cast<uint32_t>(
                dummy_color_target_->texture()->sampleCount()));
      }
    }
  } else if (!has_any_color_target) {
    dummy_color_target_ = nullptr;
  }

  render_pass_descriptor_dirty_ = false;
  return cached_render_pass_descriptor_;
}

void MetalRenderTargetCache::ConsumeRenderPassDescriptorClears(
    MTL::RenderPassDescriptor* pass_descriptor) {
  if (!pass_descriptor || pass_descriptor != cached_render_pass_descriptor_) {
    return;
  }
  render_pass_encoder_created_since_targets_changed_ = true;
  bool any_consumed = false;
  for (MetalRenderTarget*& render_target :
       cached_render_pass_descriptor_pending_clears_) {
    if (!render_target) {
      continue;
    }
    render_target->SetNeedsInitialClear(false);
    render_target = nullptr;
    any_consumed = true;
  }
  if (any_consumed) {
    // The cached descriptor still has clear load actions. Rebuild it before the
    // next encoder so the cleared contents are loaded rather than cleared
    // again.
    render_pass_descriptor_dirty_ = true;
  }
}

void MetalRenderTargetCache::SetRenderPassArea(
    MTL::RenderPassDescriptor* pass_descriptor, uint32_t min_height) const {
  if (!pass_descriptor) {
    return;
  }
  if (pass_descriptor != cached_render_pass_descriptor_) {
    pass_descriptor->setRenderTargetWidth(0);
    pass_descriptor->setRenderTargetHeight(0);
    return;
  }
  RenderTargetKey keys[1 + xenos::kMaxColorRenderTargets];
  uint32_t key_count = 0;
  if (current_depth_target_ && current_depth_target_->texture()) {
    keys[key_count++] = current_depth_target_->key();
  }
  for (const MetalRenderTarget* color_target : current_color_targets_) {
    if (color_target && color_target->texture()) {
      keys[key_count++] = color_target->key();
    }
  }
  RestrictRenderArea(pass_descriptor, keys, key_count, min_height);
}

void MetalRenderTargetCache::RestrictRenderArea(
    MTL::RenderPassDescriptor* pass_descriptor, const RenderTargetKey* keys,
    uint32_t key_count, uint32_t min_height) const {
  pass_descriptor->setRenderTargetWidth(0);
  pass_descriptor->setRenderTargetHeight(0);
  if (!key_count) {
    return;
  }
  // The render area can't exceed any attachment, a dummy color one too.
  uint32_t width = UINT32_MAX;
  uint32_t full_height = UINT32_MAX;
  auto constrain = [&](MTL::RenderPassAttachmentDescriptor* attachment) {
    if (MTL::Texture* texture = attachment->texture()) {
      width = std::min(width, uint32_t(texture->width()));
      full_height = std::min(full_height, uint32_t(texture->height()));
    }
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    constrain(pass_descriptor->colorAttachments()->object(i));
  }
  constrain(pass_descriptor->depthAttachment());
  constrain(pass_descriptor->stencilAttachment());
  if (width == UINT32_MAX) {
    return;
  }
  const uint32_t draw_scale_y = std::max<uint32_t>(1u, draw_resolution_scale_y());
  uint32_t height = min_height;
  for (uint32_t i = 0; i < key_count; ++i) {
    uint32_t owned_height = GetRenderTargetOwnedHeight(keys[i]);
    if (!owned_height) {
      return;
    }
    const uint32_t scale_y = keys[i].scale_native ? 1u : draw_scale_y;
    height = std::max(height, owned_height * scale_y);
  }
  if (height >= full_height) {
    return;
  }
  pass_descriptor->setRenderTargetWidth(width);
  pass_descriptor->setRenderTargetHeight(height);
}

bool MetalRenderTargetCache::IsRenderPassDescriptorCompatible(
    MTL::RenderPassDescriptor* pass_descriptor,
    uint32_t expected_sample_count) const {
  if (pass_descriptor && pass_descriptor == cached_render_pass_descriptor_ &&
      !render_pass_descriptor_dirty_ &&
      cached_render_pass_descriptor_sample_count_ == expected_sample_count) {
    return true;
  }
  if (!pass_descriptor) {
    return false;
  }

  auto* depth_attachment = pass_descriptor->depthAttachment();
  auto* stencil_attachment = pass_descriptor->stencilAttachment();
  MTL::Texture* expected_depth =
      current_depth_target_ ? current_depth_target_->texture() : nullptr;
  if (expected_depth) {
    if (!depth_attachment || depth_attachment->texture() != expected_depth) {
      return false;
    }
    MTL::Texture* expected_stencil =
        current_depth_target_->stencil_attachment_texture();
    if (expected_stencil) {
      if (!stencil_attachment ||
          stencil_attachment->texture() != expected_stencil) {
        return false;
      }
    } else if (stencil_attachment && stencil_attachment->texture()) {
      return false;
    }
  } else if ((depth_attachment && depth_attachment->texture()) ||
             (stencil_attachment && stencil_attachment->texture())) {
    return false;
  }

  bool has_current_color_target = false;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (current_color_targets_[i] && current_color_targets_[i]->texture()) {
      has_current_color_target = true;
      break;
    }
  }
  auto* color_attachments = pass_descriptor->colorAttachments();
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MTL::Texture* actual_color = color_attachments->object(i)->texture();
    MTL::Texture* expected_color = current_color_targets_[i]
                                       ? current_color_targets_[i]->texture()
                                       : nullptr;
    if (expected_color) {
      if (actual_color != expected_color) {
        return false;
      }
    } else if (actual_color && (has_current_color_target || i != 0)) {
      return false;
    }
  }

  if (has_current_color_target) {
    return true;
  }
  // A depth-only pass has no dummy color attachment.
  if (expected_depth) {
    return color_attachments->object(0)->texture() == nullptr;
  }
  MTL::Texture* expected_dummy =
      dummy_color_target_ ? dummy_color_target_->texture() : nullptr;
  return expected_dummy &&
         color_attachments->object(0)->texture() == expected_dummy;
}

void MetalRenderTargetCache::RecordRenderTargetViewCreated() {
  render_target_views_created_.fetch_add(1, std::memory_order_relaxed);
}

MetalRenderTargetCache::MetalRenderTarget*
MetalRenderTargetCache::GetColorRenderTarget(uint32_t index) const {
  if (index >= 4) {
    return nullptr;
  }
  return current_color_targets_[index];
}

MTL::Texture* MetalRenderTargetCache::GetColorTargetForDraw(
    uint32_t index) const {
  if (index >= 4 || !current_color_targets_[index]) {
    return nullptr;
  }
  return current_color_targets_[index]->texture();
}

MTL::Texture* MetalRenderTargetCache::GetDepthTargetForDraw() const {
  if (!current_depth_target_) {
    return nullptr;
  }
  return current_depth_target_->texture();
}

MTL::Texture* MetalRenderTargetCache::GetDummyColorTargetForDraw() const {
  if (dummy_color_target_ && dummy_color_target_->texture()) {
    return dummy_color_target_->texture();
  }
  return nullptr;
}

MTL::Texture* MetalRenderTargetCache::GetRenderTargetTexture(
    RenderTargetKey key) const {
  auto it = render_target_map_.find(key.key);
  if (it == render_target_map_.end()) {
    return nullptr;
  }
  MetalRenderTarget* target = it->second;
  return target ? target->texture() : nullptr;
}

MTL::ComputePipelineState* MetalRenderTargetCache::GetOrCreateDumpPipeline(
    EdramDumpShaderKey key) {
  return transfer_encoder_->GetOrCreateDumpPipeline(key);
}

void MetalRenderTargetCache::DumpAllRenderTargetsToEdram() {
  DumpRenderTargets(0, xenos::kEdramTileCount, 1, xenos::kEdramTileCount);
}

bool MetalRenderTargetCache::BeginEdramSnapshotReadback() {
  if (!edram_buffer_ || !device_) {
    return false;
  }
  if (!edram_snapshot_download_buffer_) {
    edram_snapshot_download_buffer_ = device_->newBuffer(
        xenos::kEdramSizeBytes, MTL::ResourceStorageModeShared);
    if (!edram_snapshot_download_buffer_) {
      XELOGE(
          "MetalRenderTargetCache: Failed to create the EDRAM snapshot "
          "download buffer");
      return false;
    }
    edram_snapshot_download_buffer_->setLabel(
        NS::String::string("EDRAM Snapshot Download", NS::UTF8StringEncoding));
  }

  // Nothing brackets this submission, so the copy is awaited here.
  ScopedAutoreleasePool autorelease_pool;
  MTL::CommandBuffer* cmd = command_processor_.CreateAccountedCommandBuffer(
      MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
  command_processor_.EndSharedComputeEncoder(cmd);
  MTL::BlitCommandEncoder* blit = cmd ? cmd->blitCommandEncoder() : nullptr;
  if (!blit) {
    command_processor_.DiscardAccountedCommandBuffer(
        cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
    return false;
  }
  blit->copyFromBuffer(edram_buffer_, 0, edram_snapshot_download_buffer_, 0,
                       xenos::kEdramSizeBytes);
  blit->endEncoding();
  command_processor_.CommitAndAwait(
      cmd, MetalCommandProcessor::GpuWaitReason::kSnapshot);
  return true;
}

const void* MetalRenderTargetCache::MapEdramSnapshotReadback() {
  return edram_snapshot_download_buffer_
             ? edram_snapshot_download_buffer_->contents()
             : nullptr;
}

void MetalRenderTargetCache::EndEdramSnapshotReadback() {
  if (edram_snapshot_download_buffer_) {
    edram_snapshot_download_buffer_->release();
    edram_snapshot_download_buffer_ = nullptr;
  }
}

void MetalRenderTargetCache::DumpRenderTargets(
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, MTL::CommandBuffer* command_buffer) {
  // The dump reads the backings.
  PerformDeferredClears(command_buffer);
  transfer_encoder_->DumpRenderTargets(dump_base, dump_row_length_used,
                                       dump_rows, dump_pitch, command_buffer);
}

bool MetalRenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const Transfer::Rectangle* const* transfer_cutouts,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle,
    MTL::CommandBuffer* command_buffer,
    MTL::RenderCommandEncoder* active_render_encoder,
    MTL::RenderPassDescriptor* active_render_pass_descriptor,
    DrawPassTransferEncoderMutationMask* mutations_out,
    bool depth_keep_dest_depth) {
  if (!render_target_count || !render_targets || !render_target_transfers) {
    return true;
  }
  const bool resolve_clear_needed =
      render_target_resolve_clear_values != nullptr &&
      resolve_clear_rectangle != nullptr;
  assert_false(resolve_clear_needed && transfer_cutouts);

  // Resolve clears build a pass of their own around a clear load action, which
  // the guest's already-started pass cannot provide.
  const bool use_active_render_encoder = active_render_encoder != nullptr;
  if (use_active_render_encoder &&
      (resolve_clear_needed || !active_render_pass_descriptor)) {
    return false;
  }
  TransferAttachmentFormats active_attachment_formats;
  if (use_active_render_encoder &&
      !transfer_encoder_->GetActiveTransferAttachmentFormats(
          active_render_pass_descriptor, active_attachment_formats)) {
    return false;
  }

  MetalRenderTarget* renamed_depth_dest =
      render_target_transfers == pending_draw_pass_transfers_.data()
          ? pending_draw_pass_rotated_depth_
          : nullptr;

  bool any_work = false;
  bool host_depth_store_needed = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    if (resolve_clear_needed) {
      any_work = true;
    }
    const std::vector<Transfer>& transfers = render_target_transfers[i];
    if (transfers.empty()) {
      continue;
    }
    any_work = true;
    if (!dest_rt->key().is_depth || dest_rt == renamed_depth_dest) {
      continue;
    }
    for (const Transfer& transfer : transfers) {
      if (transfer.host_depth_source == dest_rt) {
        host_depth_store_needed = true;
        break;
      }
    }
  }
  if (!any_work) {
    return true;
  }
  // The host depth store is a compute dispatch, which cannot be encoded into a
  // render pass.
  if (use_active_render_encoder && host_depth_store_needed) {
    return false;
  }

  // Encoding into the guest's pass touches no encoder of its own, so it needs
  // no command buffer either.
  MTL::CommandBuffer* cmd = nullptr;
  if (!use_active_render_encoder) {
    cmd = command_buffer ? command_buffer
                         : command_processor_.EnsureCommandBuffer();
    if (!cmd) {
      XELOGE(
          "MetalRenderTargetCache::PerformTransfersAndResolveClears: no "
          "command buffer");
      return false;
    }
    command_processor_.EndRenderEncoder();
    // The passes below read and partially write the backings.
    PerformDeferredClears(cmd);
  }

  // Past every fallback to another path, so each transfer is logged once.
  LogTransfers(render_target_count, render_targets, render_target_transfers,
               resolve_clear_needed ? resolve_clear_rectangle : nullptr);

  if (resolve_clear_needed && !use_active_render_encoder) {
    const PairedClearResult result = TryPerformPairedResolveClear(
        render_target_count, render_targets, render_target_transfers,
        render_target_resolve_clear_values, *resolve_clear_rectangle, cmd);
    if (result != PairedClearResult::kNotApplicable) {
      return result == PairedClearResult::kEncoded;
    }
  }

  // Self-sourced host depth: rather than snapshotting the depth into a buffer
  // with a compute dispatch, keep the current backing as the read-only host
  // depth source and render the transfer into a spare backing that becomes
  // the active one.
  if (host_depth_store_needed && !use_active_render_encoder) {
    auto* dest = static_cast<MetalRenderTarget*>(render_targets[0]);
    if (CanRotateHostDepth(render_target_count, render_targets,
                           render_target_transfers, resolve_clear_needed) &&
        AcquireSpareDepthBacking(dest, render_target_transfers[0])) {
      dest->SwapDepthBackings();
      MarkContentChanged(dest);
      renamed_depth_dest = dest;
      host_depth_store_needed = false;
      render_pass_descriptor_dirty_ = true;
    }
  }

  MetalTransferEncoder::ExecutionOutput output;
  const bool result = transfer_encoder_->PerformTransfersAndResolveClears(
      render_target_count, render_targets, render_target_transfers,
      transfer_cutouts, render_target_resolve_clear_values,
      resolve_clear_rectangle, cmd, active_render_encoder,
      active_render_pass_descriptor,
      use_active_render_encoder ? &active_attachment_formats : nullptr,
      depth_keep_dest_depth, host_depth_store_needed, renamed_depth_dest,
      &output);
  if (mutations_out) {
    *mutations_out = output.mutations;
  }
  if (output.render_pass_descriptor_dirty) {
    render_pass_descriptor_dirty_ = true;
  }
  for (uint32_t i = 0; i < render_target_count; ++i) {
    if ((output.transferred_target_mask & (uint32_t(1) << i)) &&
        render_targets[i]) {
      RecordPerformedTransfers(
          static_cast<MetalRenderTarget*>(render_targets[i]),
          render_target_transfers[i]);
    }
  }
  for (const RenderTargetKey& key : output.deferred_clear_targets) {
    deferred_clear_targets_.push_back(key);
  }
  pass_stats_.deferred_resolve_clears += output.deferred_clear_targets.size();
  return result;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
