/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TRANSFER_ENCODER_H_
#define XENIA_GPU_METAL_METAL_TRANSFER_ENCODER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "third_party/metal-cpp/Metal/Metal.hpp"
#include "xenia/base/assert.h"
#include "xenia/gpu/edram_dump_shader.h"
#include "xenia/gpu/edram_transfer_shader.h"
#include "xenia/gpu/metal/metal_render_target_cache.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;

// Matches TransferVertexConstants / TransferRectInstance / clear constants in
// the transfer library's MSL.
struct TransferVertexConstants {
  float pixel_to_ndc_x;
  float pixel_to_ndc_y;
};

struct TransferRectInstance {
  float origin_x;
  float origin_y;
  float size_x;
  float size_y;
};

struct TransferClearColorFloatConstants {
  float color[4];
};

struct TransferClearColorUintConstants {
  uint32_t color[4];
};

struct TransferClearDepthConstants {
  float depth;
  float padding[3];
};

// Stateless-with-respect-to-pass-state encoder for Metal EDRAM ownership
// transfers, resolve clears, host-depth stores, and EDRAM dumps.
class MetalTransferEncoder {
 public:
  using RenderTarget = MetalRenderTargetCache::RenderTarget;
  using RenderTargetKey = MetalRenderTargetCache::RenderTargetKey;
  using MetalRenderTarget = MetalRenderTargetCache::MetalRenderTarget;
  using Transfer = MetalRenderTargetCache::Transfer;
  using TransferColorAttachmentFormats =
      MetalRenderTargetCache::TransferColorAttachmentFormats;
  using TransferAttachmentFormats =
      MetalRenderTargetCache::TransferAttachmentFormats;
  using DrawPassTransferEncoderMutationMask =
      MetalRenderTargetCache::DrawPassTransferEncoderMutationMask;

  struct TransferRectanglePlan {
    uint32_t transfer_index = 0;
    std::array<Transfer::Rectangle,
               MetalRenderTargetCache::kMaxBandedRectanglesWithCutout>
        rectangles = {};
    uint32_t rectangle_count = 0;
  };

  struct ExecutionOutput {
    DrawPassTransferEncoderMutationMask mutations = 0;
    bool render_pass_descriptor_dirty = false;
    std::vector<RenderTargetKey> deferred_clear_targets;
  };

  MetalTransferEncoder(MetalRenderTargetCache& render_target_cache,
                       MetalCommandProcessor& command_processor);
  ~MetalTransferEncoder();

  bool Initialize(MTL::Device* device);
  void Shutdown();

  size_t transfer_pipeline_count() const { return transfer_pipelines_.size(); }
  size_t transfer_fragment_function_count() const {
    return transfer_fragment_functions_.size();
  }

  // The host depth a resolve clears a depth target to.
  static float GetResolveClearDepth(xenos::DepthRenderTargetFormat format,
                                    uint64_t clear_value);
  // The normalized color a resolve clears an 8_8_8_8 target to.
  static void GetResolveClearColor8888(uint64_t clear_value,
                                       float color_out[4]);
  static bool IsColorTransferFloatBitsExact(
      xenos::ColorRenderTargetFormat format);

  bool UseNativeStencilOutputInTransfers() const {
    return native_stencil_output_probed_;
  }
  bool TransferDrawsSamplesSeparately(EdramTransferShaderKey key) const;

  MTL::ComputePipelineState* GetOrCreateDumpPipeline(EdramDumpShaderKey key);

  // Writes contents of host render targets within rectangles from
  // ResolveInfo::GetCopyEdramTileSpan to edram_buffer_.
  void DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                         uint32_t dump_rows, uint32_t dump_pitch,
                         MTL::CommandBuffer* command_buffer = nullptr);

  // Encodes ownership transfers and/or resolve clears (standalone passes or
  // into an active guest render pass encoder). Planner state changes are
  // reported via |output| rather than mutated implicitly.
  bool PerformTransfersAndResolveClears(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const Transfer::Rectangle* const* transfer_cutouts,
      const uint64_t* render_target_resolve_clear_values,
      const Transfer::Rectangle* resolve_clear_rectangle,
      MTL::CommandBuffer* command_buffer,
      MTL::RenderCommandEncoder* active_render_encoder,
      MTL::RenderPassDescriptor* active_render_pass_descriptor,
      bool depth_keep_dest_depth, MetalRenderTarget* pre_rotated_depth_target,
      ExecutionOutput* output);

  EdramTransferShaderKey GetTransferShaderKey(
      RenderTargetKey source_key, RenderTargetKey dest_key,
      const RenderTargetKey* host_depth_source_key,
      bool host_depth_source_is_copy, bool stencil_bit,
      uint32_t dest_color_rt_index, const Transfer& transfer) const;
  bool MakeTransferAddressConstant(
      RenderTargetKey source_key, RenderTargetKey dest_key,
      const Transfer& transfer,
      EdramTransferAddressConstant& constant_out) const;
  int32_t GetStripMergeTransferOffset(RenderTargetKey source_key,
                                      RenderTargetKey dest_key,
                                      const Transfer& transfer) const;
  static bool IsStripMergeTransferFar(int32_t offset) {
    return offset >= int32_t(xenos::kEdramTileCount);
  }
  bool IsStripMergeAddressingSame(const Transfer& transfer,
                                  const Transfer& other_transfer,
                                  RenderTargetKey dest_key) const;
  bool BuildTransferRectanglePlans(
      RenderTargetKey dest_key, const std::vector<Transfer>& transfers,
      const Transfer::Rectangle* cutout, bool require_all_rectangles,
      std::vector<TransferRectanglePlan>& transfer_rectangles_out) const;
  bool GetActiveTransferAttachmentFormats(
      MTL::RenderPassDescriptor* pass_descriptor,
      TransferAttachmentFormats& attachment_formats_out) const;
  bool GetCurrentTransferAttachmentFormats(
      TransferAttachmentFormats& attachment_formats_out) const;

  MTL::RenderPipelineState* GetOrCreateTransferPipelines(
      const EdramTransferShaderKey& key, MTL::PixelFormat dest_format,
      bool dest_is_uint, bool native_stencil_output = false,
      uint32_t color_attachment_index = 0,
      const TransferColorAttachmentFormats* color_attachment_formats = nullptr,
      MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid,
      MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid);
  MTL::RenderPipelineState* GetOrCreateTransferClearPipeline(
      MTL::PixelFormat dest_format, bool dest_is_uint, bool is_depth,
      uint32_t sample_count, uint32_t color_attachment_index = 0,
      const TransferColorAttachmentFormats* color_attachment_formats = nullptr,
      MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid,
      MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid);

  MTL::DepthStencilState* GetTransferDepthStencilState(bool depth_write);
  MTL::DepthStencilState* GetTransferDepthAndStencilOutputState();
  MTL::DepthStencilState* GetTransferNoDepthStencilState();
  MTL::DepthStencilState* GetTransferDepthClearState();
  MTL::DepthStencilState* GetTransferStencilClearState();
  MTL::DepthStencilState* GetTransferStencilBitState(uint32_t bit);

 private:
  // A pipeline is only usable in a pass whose attachment formats it was built
  // against, so the whole set is part of its identity. Color attachments other
  // than the destination get an empty write mask.
  struct TransferPipelineKey {
    EdramTransferShaderKey shader_key;
    uint32_t color_attachment_index = 0;
    uint32_t native_stencil_output = 0;
    TransferColorAttachmentFormats color_attachment_formats = {};
    MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid;
    MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid;

    bool operator==(const TransferPipelineKey& other) const = default;

    struct Hasher {
      size_t operator()(const TransferPipelineKey& key) const {
        auto combine = [](size_t seed, size_t value) {
          return seed ^ (value + 0x9E3779B9 + (seed << 6) + (seed >> 2));
        };
        size_t h = EdramTransferShaderKey::Hasher()(key.shader_key);
        h = combine(h, key.color_attachment_index);
        h = combine(h, key.native_stencil_output);
        h = combine(h, size_t(key.depth_attachment_format));
        h = combine(h, size_t(key.stencil_attachment_format));
        for (MTL::PixelFormat color_format : key.color_attachment_formats) {
          h = combine(h, size_t(color_format));
        }
        return h;
      }
    };
  };

  struct TransferClearPipelineKey {
    uint32_t color_attachment_index = 0;
    uint32_t sample_count = 1;
    uint32_t dest_is_uint = 0;
    uint32_t is_depth = 0;
    TransferColorAttachmentFormats color_attachment_formats = {};
    MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid;
    MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid;

    bool operator==(const TransferClearPipelineKey& other) const = default;

    struct Hasher {
      size_t operator()(const TransferClearPipelineKey& key) const {
        auto combine = [](size_t seed, size_t value) {
          return seed ^ (value + 0x9E3779B9 + (seed << 6) + (seed >> 2));
        };
        size_t h = key.color_attachment_index;
        h = combine(h, key.sample_count);
        h = combine(h, key.dest_is_uint);
        h = combine(h, key.is_depth);
        h = combine(h, size_t(key.depth_attachment_format));
        h = combine(h, size_t(key.stencil_attachment_format));
        for (MTL::PixelFormat color_format : key.color_attachment_formats) {
          h = combine(h, size_t(color_format));
        }
        return h;
      }
    };
  };

  struct TransferInvocation {
    Transfer transfer;
    EdramTransferShaderKey shader_key;
    TransferInvocation(const Transfer& transfer,
                       const EdramTransferShaderKey& shader_key)
        : transfer(transfer), shader_key(shader_key) {}
    bool operator<(const TransferInvocation& other) const {
      if (shader_key != other.shader_key) {
        return shader_key < other.shader_key;
      }
      assert_not_null(transfer.source);
      assert_not_null(other.transfer.source);
      uint32_t source_index =
          static_cast<const MetalRenderTarget*>(transfer.source)
              ->temporary_sort_index();
      uint32_t other_source_index =
          static_cast<const MetalRenderTarget*>(other.transfer.source)
              ->temporary_sort_index();
      if (source_index != other_source_index) {
        return source_index < other_source_index;
      }
      return transfer.start_tiles < other.transfer.start_tiles;
    }
    bool CanBeMergedIntoOneDraw(const TransferInvocation& other) const {
      return shader_key == other.shader_key &&
             transfer.AreSourcesSame(other.transfer);
    }
  };

  bool ProbeNativeStencilOutputSupport();
  MTL::Library* GetOrCreateTransferLibrary();
  MTL::Function* GetTransferRectVertexFunction();
  MTL::Function* GetOrCreateTransferFragmentFunction(
      EdramTransferShaderKey key);
  MTL::Buffer* GetTransferDummyBuffer();

  MetalRenderTargetCache& render_target_cache_;
  MetalCommandProcessor& command_processor_;
  MTL::Device* device_ = nullptr;

  // EDRAM dump compute pipelines (host RT -> EDRAM buffer).
  std::unordered_map<EdramDumpShaderKey, MTL::ComputePipelineState*,
                     EdramDumpShaderKey::Hasher>
      dump_pipelines_;

  // Host depth store compute shaders (1x/2x/4x MSAA).
  MTL::ComputePipelineState* host_depth_store_pipelines_[3] = {};

  // Ownership transfer & clear pipelines and states.
  std::unordered_map<TransferPipelineKey, MTL::RenderPipelineState*,
                     TransferPipelineKey::Hasher>
      transfer_pipelines_;
  std::unordered_map<EdramTransferShaderKey, MTL::Function*,
                     EdramTransferShaderKey::Hasher>
      transfer_fragment_functions_;
  std::vector<TransferInvocation> transfer_invocations_;
  MTL::Library* transfer_library_ = nullptr;
  MTL::Function* transfer_rect_vertex_function_ = nullptr;
  std::unordered_map<TransferClearPipelineKey, MTL::RenderPipelineState*,
                     TransferClearPipelineKey::Hasher>
      transfer_clear_pipelines_;
  MTL::DepthStencilState* transfer_depth_state_ = nullptr;
  MTL::DepthStencilState* transfer_depth_stencil_output_state_ = nullptr;
  MTL::DepthStencilState* transfer_depth_state_none_ = nullptr;
  MTL::DepthStencilState* transfer_depth_clear_state_ = nullptr;
  MTL::DepthStencilState* transfer_stencil_clear_state_ = nullptr;
  MTL::DepthStencilState* transfer_stencil_bit_states_[8] = {};
  bool native_stencil_output_probed_ = false;
  MTL::Buffer* transfer_dummy_buffer_ = nullptr;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_TRANSFER_ENCODER_H_
