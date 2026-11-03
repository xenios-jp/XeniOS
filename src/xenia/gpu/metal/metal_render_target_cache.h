/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_RENDER_TARGET_CACHE_H_
#define XENIA_GPU_METAL_METAL_RENDER_TARGET_CACHE_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "xenia/gpu/edram_dump_shader.h"
#include "xenia/gpu/edram_transfer_shader.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;

class MetalRenderTargetCache final : public gpu::RenderTargetCache {
  friend class MetalPairedClearTest;

 public:
  // Metal-specific render target - defined inside cache class to access
  // protected RenderTarget
  class MetalRenderTarget final : public RenderTarget {
   public:
    ~MetalRenderTarget() override;

    MTL::Texture* texture() const { return texture_; }
    MTL::Texture* transfer_texture() const {
      return transfer_texture_ ? transfer_texture_ : texture_;
    }
    MTL::Texture* stencil_view() const { return stencil_view_; }
    void SetStencilView(MTL::Texture* view) { stencil_view_ = view; }
    MTL::Texture* spare_depth_texture() const { return spare_depth_texture_; }
    void SetSpareDepthTexture(MTL::Texture* texture) {
      assert_null(spare_depth_texture_);
      spare_depth_texture_ = texture;
    }
    void SwapDepthBackings() {
      assert_not_null(texture_);
      assert_not_null(spare_depth_texture_);
      assert_null(transfer_texture_);
      std::swap(texture_, spare_depth_texture_);
      std::swap(stencil_view_, spare_depth_stencil_view_);
    }

    void SetTemporarySortIndex(uint32_t index) {
      temporary_sort_index_ = index;
    }
    uint32_t temporary_sort_index() const { return temporary_sort_index_; }

    void SetTexture(MTL::Texture* texture) {
      if (texture_ != texture) {
        if (stencil_view_) {
          stencil_view_->release();
          stencil_view_ = nullptr;
        }
        texture_ = texture;
      }
    }
    void SetTransferTexture(MTL::Texture* texture) {
      transfer_texture_ = texture;
    }
    // The allocation the backing lives in: the texture itself, or for a color
    // target aliased onto another key's allocation, that allocation.
    MTL::Texture* storage_root() const {
      return alias_storage_root_ ? alias_storage_root_.get() : texture_;
    }
    void SetAliasStorageRoot(MTL::Texture* root) {
      alias_storage_root_ = NS::RetainPtr(root);
    }
    bool has_alias_storage_partner_candidates() const {
      return !key().is_depth && key().msaa_samples == xenos::MsaaSamples::k4X;
    }
    bool needs_initial_clear() const { return needs_initial_clear_; }
    void SetNeedsInitialClear(bool needs_initial_clear) {
      needs_initial_clear_ = needs_initial_clear;
      if (!needs_initial_clear) {
        clear_deferred_ = false;
        clear_depth_ = 1.0f;
        clear_stencil_ = 0;
        clear_color_[0] = clear_color_[1] = clear_color_[2] =
            clear_color_[3] = 0.0f;
      }
    }
    // A full resolve clear left for the next pass to perform as its clear load
    // action. Unlike a first-bind clear, the target owns EDRAM ranges, so
    // anything reading its backing first has to encode the clear.
    bool clear_deferred() const { return clear_deferred_; }
    void DeferDepthClear(float depth, uint32_t stencil) {
      needs_initial_clear_ = true;
      clear_deferred_ = true;
      clear_depth_ = depth;
      clear_stencil_ = stencil;
    }
    void DeferColorClear(const float color[4]) {
      needs_initial_clear_ = true;
      clear_deferred_ = true;
      for (uint32_t i = 0; i < 4; ++i) {
        clear_color_[i] = color[i];
      }
    }
    // The values the clear load action writes.
    float clear_depth() const { return clear_depth_; }
    uint32_t clear_stencil() const { return clear_stencil_; }
    MTL::ClearColor clear_color() const {
      return MTL::ClearColor(clear_color_[0], clear_color_[1], clear_color_[2],
                             clear_color_[3]);
    }
    // Changes whenever the backing's content may have changed: draws that may
    // write it, clears, transfers into it, backing swaps. Values come from one
    // cache-wide counter, so no two render targets ever share one.
    uint64_t content_generation() const { return content_generation_; }
    void SetContentGeneration(uint64_t generation) {
      content_generation_ = generation;
    }

    // Public constructor for creating render targets
    MetalRenderTarget(RenderTargetKey key) : RenderTarget(key) {}

   private:
    MTL::Texture* texture_ = nullptr;
    MTL::Texture* transfer_texture_ = nullptr;
    MTL::Texture* stencil_view_ = nullptr;
    MTL::Texture* spare_depth_texture_ = nullptr;
    MTL::Texture* spare_depth_stencil_view_ = nullptr;
    NS::SharedPtr<MTL::Texture> alias_storage_root_;
    uint32_t temporary_sort_index_ = UINT32_MAX;
    uint64_t content_generation_ = 0;
    bool needs_initial_clear_ = true;
    bool clear_deferred_ = false;
    float clear_depth_ = 1.0f;
    uint32_t clear_stencil_ = 0;
    float clear_color_[4] = {};
  };

 public:
  MetalRenderTargetCache(const RegisterFile& register_file,
                         const Memory& memory, TraceWriter* trace_writer,
                         uint32_t draw_resolution_scale_x,
                         uint32_t draw_resolution_scale_y,
                         MetalCommandProcessor& command_processor);
  ~MetalRenderTargetCache() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  // RenderTargetCache implementation
  Path GetPath() const override;

  // Fixed-point render targets (k_16_16 / k_16_16_16_16) are backed by *_SNORM
  // formats in the host render targets path, which are -1...1 rather than the
  // Xbox 360's -32...32 range. When this is true, resolve/copy must compensate
  // to match the guest packing expectations.
  bool IsFixedRG16TruncatedToMinus1To1() const {
    return !cvars::snorm16_render_target_full_range;
  }
  bool IsFixedRGBA16TruncatedToMinus1To1() const {
    return !cvars::snorm16_render_target_full_range;
  }

  // Whether 2x MSAA is supported on this device.
  bool msaa_2x_supported() const { return msaa_2x_supported_; }

  // Whether gamma render targets use UNORM16 storage (separate from sRGB).
  // When true, gamma correction is done in shaders rather than via sRGB format.
  bool gamma_render_target_as_unorm16() const {
    return gamma_render_target_as_unorm16_;
  }

  bool IsGammaFormatHostStorageSeparate() const override;

  void ClearCache() override;
  void BeginFrame() override;

  bool Update(bool is_rasterization_done,
              reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask, const Shader& vertex_shader,
              int32_t window_offset_tiles) override;

  // Metal-specific methods
  // render_encoder_pending tells whether a render encoder is still to be
  // created from the result - load actions only take effect for one, and an
  // encoder already recording holds the attachments in tile memory instead.
  MTL::RenderPassDescriptor* GetRenderPassDescriptor(
      uint32_t expected_sample_count, bool render_encoder_pending);
  // Whether an existing encoder's descriptor still binds the textures needed
  // by the current guest render-target configuration.
  bool IsRenderPassDescriptorCompatible(
      MTL::RenderPassDescriptor* pass_descriptor,
      uint32_t expected_sample_count) const;
  // Marks first-use clears baked into this descriptor as executed. Call only
  // after a render encoder has been created successfully from the descriptor.
  void ConsumeRenderPassDescriptorClears(
      MTL::RenderPassDescriptor* pass_descriptor);
  // Sets the store actions the descriptor left undecided, right before the
  // command processor ends the encoder: DontCare for the attachments a
  // resolve is about to clear whole without reading, Store for the rest.
  void FinalizeRenderPassStoreActions(
      MTL::RenderCommandEncoder* encoder,
      MTL::RenderPassDescriptor* pass_descriptor);
  // Whether the descriptor's depth clear is a deferred resolve clear, whose
  // value has to stay as the descriptor has it.
  bool HasDeferredDepthClear(MTL::RenderPassDescriptor* pass_descriptor) const {
    return pass_descriptor &&
           pass_descriptor == cached_render_pass_descriptor_ &&
           current_depth_target_ && current_depth_target_->clear_deferred() &&
           pass_descriptor->depthAttachment() &&
           pass_descriptor->depthAttachment()->loadAction() ==
               MTL::LoadActionClear;
  }

  bool IsRenderPassDescriptorDirty() const {
    return render_pass_descriptor_dirty_;
  }

  // Get current render targets for capture
  MetalRenderTarget* GetColorRenderTarget(uint32_t index) const;
  // Get current render targets for pipeline attachment formats.
  MTL::Texture* GetColorTargetForDraw(uint32_t index) const;
  MTL::Texture* GetDepthTargetForDraw() const;
  MTL::Texture* GetDummyColorTargetForDraw() const;

  // Get the last REAL (non-dummy) render targets for capture
  MTL::Texture* GetLastRealColorTarget(uint32_t index) const;
  MTL::Texture* GetLastRealDepthTarget() const;

  // Look up a render target texture by key for debug/trace viewer use.
  MTL::Texture* GetRenderTargetTexture(RenderTargetKey key) const;
  // Look up a color render target texture by key components for the trace
  // viewer without exposing RenderTargetKey.
  MTL::Texture* GetColorRenderTargetTexture(
      uint32_t pitch, xenos::MsaaSamples samples, uint32_t base,
      xenos::ColorRenderTargetFormat format) const;

  // Restore EDRAM contents from snapshot (for trace playback), matching
  // D3D12RenderTargetCache::RestoreEdramSnapshot.
  void RestoreEdramSnapshot(const void* snapshot);

  MTL::Buffer* GetEdramBuffer() const { return edram_buffer_; }

  // Resolve (copy) render targets to shared memory
  bool Resolve(Memory& memory, uint32_t& written_address,
               uint32_t& written_length,
               MTL::CommandBuffer* command_buffer = nullptr);

  // Render encoder state the transfer draws overwrite when they are encoded
  // into the guest's own render pass.
  using DrawPassTransferEncoderMutationMask = uint32_t;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationNone = 0;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationPipeline = 1u << 0;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationDepthStencil = 1u << 1;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationStencilReference = 1u << 2;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationViewport = 1u << 3;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationScissor = 1u << 4;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationRasterizer = 1u << 5;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationVertexSlot0 = 1u << 6;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationVertexSlot1 = 1u << 7;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationFragmentSlot0 = 1u << 8;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationFragmentSlot1 = 1u << 9;
  static constexpr DrawPassTransferEncoderMutationMask
      kDrawPassTransferEncoderMutationFragmentTextures = 1u << 10;

  bool HasPendingDrawPassTransfers() const {
    return pending_draw_pass_transfer_mask_ != 0;
  }
  // Whether a draw may prepare its uploads before ending a changed pass with
  // these transfers queued for the next one: nothing queued, only transfers
  // into depth, or only color-to-color transfers. Depth-to-color and mixed
  // queues keep the eager pass boundary.
  bool PendingDrawPassTransfersPrepareable() const;
  // Whether the queue can be encoded into a pass with this descriptor, checked
  // before the encoder exists so a rejected queue can still be run standalone.
  bool PreflightPendingDrawPassTransfers(
      MTL::RenderPassDescriptor* pass_descriptor);
  // Encodes the queued transfers as draws at the head of the guest's pass.
  // Failure leaves the queue intact for FlushPendingDrawPassTransfers;
  // mutations_out describes what was already encoded either way.
  bool EncodePendingDrawPassTransfers(
      MTL::RenderCommandEncoder* encoder,
      MTL::RenderPassDescriptor* pass_descriptor,
      DrawPassTransferEncoderMutationMask* mutations_out);
  // Runs the queued transfers in standalone passes instead. Every path that
  // abandons the queue has to come through here - ownership is already marked
  // transferred, so dropping it corrupts the destination.
  bool FlushPendingDrawPassTransfers();
  // Accounts for an encoded draw possibly having written its attachments;
  // called after the draw so the pass-head transfers it followed are already
  // recorded.
  void NoteDrawWrites();

 protected:
  // Virtual methods from RenderTargetCache
  uint32_t GetMaxRenderTargetWidth() const override;
  uint32_t GetMaxRenderTargetHeight() const override;

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;
  bool CanReuseRenderTarget(const RenderTarget& render_target) const override;
  void RenderTargetReused(RenderTarget& render_target,
                          RenderTargetKey old_key) override;

  bool IsHostDepthEncodingDifferent(
      xenos::DepthRenderTargetFormat format) const override;

 private:
  void RecordRenderTargetViewCreated();

  MTL::Library* GetOrCreateEdramLoadLibrary(bool msaa);
  MTL::RenderPipelineState* GetOrCreateEdramLoadPipeline(
      MTL::PixelFormat dest_format, uint32_t sample_count);

  MetalCommandProcessor& command_processor_;
  TraceWriter* trace_writer_;

  std::atomic<uint64_t> render_target_views_created_{0};

  // Metal device reference
  MTL::Device* device_ = nullptr;
  bool gamma_render_target_as_srgb_ = false;
  bool gamma_render_target_as_unorm16_ = false;

  // EDRAM buffer (10MB embedded DRAM)
  MTL::Buffer* edram_buffer_ = nullptr;
  MTL::Buffer* edram_snapshot_download_buffer_ = nullptr;

  // EDRAM compute shaders for tile operations
  MTL::ComputePipelineState* edram_load_pipeline_ = nullptr;   // Tiled → Linear
  MTL::ComputePipelineState* edram_store_pipeline_ = nullptr;  // Linear → Tiled
  std::unordered_map<uint64_t, MTL::RenderPipelineState*> edram_load_pipelines_;
  MTL::Library* edram_load_library_ = nullptr;
  MTL::Library* edram_load_library_msaa_ = nullptr;

  // EDRAM dump compute shaders for host render target -> EDRAM copies, built
  // on demand from the shared SPIR-V emitter by way of DXIL and the Metal
  // Shader Converter.
  std::unordered_map<EdramDumpShaderKey, MTL::ComputePipelineState*,
                     EdramDumpShaderKey::Hasher>
      dump_pipelines_;

  // Resolve compute shaders (Metal XeSL → MSL metallib)
  MTL::ComputePipelineState* resolve_full_8bpp_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_16bpp_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_pipeline_ = nullptr;
  // The rgb10-4x copy reading the 4x render target directly.
  MTL::ComputePipelineState* resolve_full_32bpp_rgb10_4x_direct_pipeline_ =
      nullptr;
  MTL::ComputePipelineState*
      resolve_full_32bpp_rgb10_4x_direct_refresh_pipeline_ = nullptr;
  // The generic 32bpp copy of one sample of a 1x 8_8_8_8, 2_10_10_10 or
  // 2_10_10_10_FLOAT render target, reading it directly.
  MTL::ComputePipelineState* resolve_full_32bpp_1x_direct_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_1x_direct_refresh_pipeline_ =
      nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_rgb10f_1x_direct_pipeline_ =
      nullptr;
  MTL::ComputePipelineState*
      resolve_full_32bpp_rgb10f_1x_direct_refresh_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_64bpp_pipeline_ = nullptr;
  // The rgba16-1x copy reading the 1x render target directly.
  MTL::ComputePipelineState* resolve_full_64bpp_rgba16_1x_direct_pipeline_ =
      nullptr;
  // The rgb10f-to-rgba16 1x copy reading the 1x render target directly.
  MTL::ComputePipelineState*
      resolve_full_64bpp_rgb10f_rgba16_1x_direct_pipeline_ = nullptr;
  // Its 2x and 4x averages, reading the multisampled render target directly.
  MTL::ComputePipelineState*
      resolve_full_64bpp_rgb10f_rgba16_msaa_direct_pipeline_ = nullptr;
  // One per kResolveSpecializations entry, in its order.
  std::vector<MTL::ComputePipelineState*> resolve_specialized_pipelines_;
  MTL::ComputePipelineState* resolve_full_128bpp_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_32bpp_1x2xmsaa_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_32bpp_4xmsaa_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_64bpp_1x2xmsaa_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_64bpp_4xmsaa_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_8bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_16bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_32bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_64bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_full_128bpp_scaled_pipeline_ = nullptr;
  MTL::ComputePipelineState* resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ =
      nullptr;
  MTL::ComputePipelineState* resolve_fast_32bpp_4xmsaa_scaled_pipeline_ =
      nullptr;
  MTL::ComputePipelineState* resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ =
      nullptr;
  MTL::ComputePipelineState* resolve_fast_64bpp_4xmsaa_scaled_pipeline_ =
      nullptr;

  // Host depth store compute shaders (1x/2x/4x MSAA).
  MTL::ComputePipelineState* host_depth_store_pipelines_[3] = {};

  // Transfer shaders (host RT ownership transfers) - modeled after D3D12.

  using TransferColorAttachmentFormats =
      std::array<MTL::PixelFormat, xenos::kMaxColorRenderTargets>;

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

  struct TransferAttachmentFormats {
    TransferColorAttachmentFormats color_attachment_formats = {};
    MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid;
    MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid;
  };

  struct TransferRectanglePlan {
    uint32_t transfer_index = 0;
    std::array<Transfer::Rectangle, Transfer::kMaxRectanglesWithCutout>
        rectangles = {};
    uint32_t rectangle_count = 0;
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

  std::unordered_map<TransferPipelineKey, MTL::RenderPipelineState*,
                     TransferPipelineKey::Hasher>
      transfer_pipelines_;
  // The fragment half comes from the shared SPIR-V emitter, through either
  // spirv_to_dxil and the Metal Shader Converter or SPIRV-Cross. A key that
  // failed is kept as null so it is not retried on every transfer.
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
  MTL::Texture* transfer_dummy_color_float_[3] = {};
  MTL::Texture* transfer_dummy_color_uint_[3] = {};
  MTL::Texture* transfer_dummy_depth_[3] = {};
  MTL::Texture* transfer_dummy_stencil_[3] = {};
  bool msaa_2x_supported_ = true;

  // Current render targets - updated by base class Update() call

  MetalRenderTarget* current_color_targets_[4] = {};
  MetalRenderTarget* current_depth_target_ = nullptr;

  // Ownership transfers deferred to the head of the guest's own render pass,
  // indexed like the base class's accumulated targets (0 depth, 1..4 color).
  std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
      pending_draw_pass_transfers_;
  // The last ownership transfer that wrote each EDRAM tile, performed or
  // elided, with the content generations of both ends at that time. A
  // transfer straight back that finds both unchanged is elided: the
  // destination's backing still holds exactly what it would rebuild.
  struct TileTransferRecord {
    RenderTargetKey dest;
    RenderTargetKey source;
    uint64_t source_generation = 0;
    uint64_t dest_generation = 0;
  };
  std::array<TileTransferRecord, xenos::kEdramTileCount>
      tile_transfer_records_ = {};
  uint64_t next_content_generation_ = 1;
  // What the draw of the last Update may write, for NoteDrawWrites.
  bool draw_may_write_depth_ = false;
  bool draw_may_write_stencil_ = false;
  uint32_t draw_color_write_mask_ = 0;
  void MarkContentChanged(MetalRenderTarget* render_target) {
    render_target->SetContentGeneration(next_content_generation_++);
  }
  // Records that the transfer (or elided transfer) into dest from source was
  // the last one to write these tiles.
  void RecordTileTransfer(uint32_t start_tiles, uint32_t end_tiles,
                          const MetalRenderTarget* dest,
                          const MetalRenderTarget* source);
  // Drops the transfers of the last update whose result the destination's
  // backing already holds.
  void ElideRedundantTransfers();
  // Drops color transfers into dest between two keys backed by one
  // allocation.
  void ElideAliasedStorageTransfers(MetalRenderTarget* dest,
                                    std::vector<Transfer>& transfers);
  MetalRenderTarget* FindUnorm32ColorStoragePartner(RenderTargetKey key,
                                                    uint32_t width,
                                                    uint32_t height) const;
  // Accounts for dest being written by these transfers (or a clear).
  void RecordPerformedTransfers(MetalRenderTarget* dest,
                                const std::vector<Transfer>& transfers);
  std::array<RenderTarget*, 1 + xenos::kMaxColorRenderTargets>
      pending_draw_pass_render_targets_ = {};
  uint32_t pending_draw_pass_transfer_mask_ = 0;
  uint32_t pending_draw_pass_full_overwrite_mask_ = 0;
  // Subset of the above that the current render pass descriptor was built with
  // a DontCare load action for.
  uint32_t pending_draw_pass_load_dontcare_mask_ = 0;

  // Track the last REAL (non-dummy) render targets for capture
  MetalRenderTarget* last_real_color_targets_[4] = {};
  MetalRenderTarget* last_real_depth_target_ = nullptr;

  // Track all created render targets so we can find them
  std::unordered_map<uint32_t, MetalRenderTarget*> render_target_map_;

  // Render pass descriptor cache
  MTL::RenderPassDescriptor* cached_render_pass_descriptor_ = nullptr;
  bool render_pass_descriptor_dirty_ = true;
  uint32_t cached_render_pass_descriptor_sample_count_ = 0;
  // First-use clears are not consumed until an encoder has actually been
  // created from the cached descriptor. Index 0 is depth, 1 + i are colors.
  std::array<MetalRenderTarget*, 1 + xenos::kMaxColorRenderTargets>
      cached_render_pass_descriptor_pending_clears_ = {};
  // Whether an encoder has already been created for the current render target
  // set. A later one reopens attachments that encoder wrote, so their contents
  // have to be loaded back rather than discarded.
  bool render_pass_encoder_created_since_targets_changed_ = false;

  // Dummy render target for when no render targets are bound
  struct DummyColorTargetEntry {
    std::unique_ptr<MetalRenderTarget> target;
    uint64_t last_used_frame = 0;
    uint64_t last_cleared_frame = 0;
  };
  mutable std::unordered_map<uint64_t, DummyColorTargetEntry>
      dummy_color_targets_;
  mutable MetalRenderTarget* dummy_color_target_ = nullptr;
  uint64_t frame_id_ = 0;

  // Track which render targets have been cleared this frame
  std::unordered_set<uint32_t> cleared_render_targets_this_frame_;

  // Debug helper to log a small region of the current color RT0.
  // Helper methods
  MTL::Texture* CreateColorTexture(uint32_t width, uint32_t height,
                                   xenos::ColorRenderTargetFormat format,
                                   uint32_t samples,
                                   bool transient_render_target_only = false);
  MTL::Texture* CreateDummyColorTexture(uint32_t width, uint32_t height,
                                        uint32_t samples);
  MTL::Texture* CreateDepthTexture(uint32_t width, uint32_t height,
                                   xenos::DepthRenderTargetFormat format,
                                   uint32_t samples);
  MTL::Texture* GetStencilTextureView(MetalRenderTarget* render_target);

  MTL::PixelFormat GetColorResourcePixelFormat(
      xenos::ColorRenderTargetFormat format) const;
  MTL::PixelFormat GetColorOwnershipTransferPixelFormat(
      xenos::ColorRenderTargetFormat format, bool* is_integer_out) const;
  MTL::PixelFormat GetDepthPixelFormat(
      xenos::DepthRenderTargetFormat format) const;

  // EDRAM compute shader setup
  bool InitializeEdramComputeShaders();
  void ShutdownEdramComputeShaders();

  // Transfer pipeline setup (host RT ownership transfers) - Metal analogue of
  // D3D12RenderTargetCache::GetOrCreateTransferPipelines.
  // A null color_attachment_formats builds for a pass whose only color
  // attachment is the destination, at color_attachment_index; an invalid
  // depth/stencil format on a depth destination means the destination's own.
  // The fragment shader comes from the shared SPIR-V emitter; the vertex half
  // only places the rectangle, so it is one function for every key.
  MTL::Function* GetOrCreateTransferFragmentFunction(
      EdramTransferShaderKey key);
  // Whether a key's destination samples are drawn one at a time, with the
  // sample index in the push constants so that one pipeline serves them all,
  // rather than covered by one sample-rate draw. Sample-rate shading only pays
  // for itself when a multisampled source is being read per sample.
  bool TransferDrawsSamplesSeparately(EdramTransferShaderKey key) const;
  MTL::Function* GetTransferRectVertexFunction();
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
  MTL::Library* GetOrCreateTransferLibrary();
  MTL::Texture* GetTransferDummyTexture(MTL::PixelFormat format,
                                        uint32_t sample_count);
  MTL::Texture* GetTransferDummyColorFloatTexture(uint32_t sample_count);
  MTL::Texture* GetTransferDummyColorUintTexture(uint32_t sample_count);
  MTL::Texture* GetTransferDummyDepthTexture(uint32_t sample_count);
  MTL::Texture* GetTransferDummyStencilTexture(uint32_t sample_count);
  MTL::Buffer* GetTransferDummyBuffer();
  MTL::DepthStencilState* GetTransferDepthStencilState(bool depth_write);
  MTL::DepthStencilState* GetTransferDepthAndStencilOutputState();
  MTL::DepthStencilState* GetTransferNoDepthStencilState();
  MTL::DepthStencilState* GetTransferDepthClearState();
  MTL::DepthStencilState* GetTransferStencilClearState();
  // Whether the depth transfer draw writes the guest stencil as well, rather
  // than a stencil clear plus one masked draw per bit following it.
  bool UseNativeStencilOutputInTransfers() const;
  bool ProbeNativeStencilOutputSupport();
  MTL::DepthStencilState* GetTransferStencilBitState(uint32_t bit);

  // EDRAM tile operations

  void LoadTiledData(MTL::CommandBuffer* command_buffer, MTL::Texture* texture,
                     uint32_t edram_base, uint32_t pitch_tiles,
                     uint32_t height_tiles, bool is_depth);

  void StoreTiledData(MTL::CommandBuffer* command_buffer, MTL::Texture* texture,
                      uint32_t edram_base, uint32_t pitch_tiles,
                      uint32_t height_tiles, bool is_depth);

  // Ownership transfer support - copies data between render targets when
  // EDRAM regions are aliased between different RT configurations.
  // This mirrors D3D12/Vulkan's PerformTransfersAndResolveClears.
  // With active_render_encoder the transfers are encoded as draws into that
  // encoder's pass rather than into standalone passes of their own, which rules
  // out resolve clears, host depth stores and the blit fast path.
  enum class PairedClearResult { kNotApplicable, kEncoded, kFailed };
  PairedClearResult TryPerformPairedResolveClear(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* transfers, const uint64_t* clear_values,
      const Transfer::Rectangle& rectangle, MTL::CommandBuffer* cmd);
  // Encodes the clears deferred to the next pass's load actions, for anything
  // about to read the backings or write them outside a whole-attachment pass.
  // There must be no render encoder open on cmd.
  void PerformDeferredClears(MTL::CommandBuffer* cmd = nullptr);
  // Whether another color key's view shares the target's allocation.
  bool IsColorStorageShared(const MetalRenderTarget* render_target) const;
  // Before a resolve ends the guest's pass: the attachments of that pass the
  // resolve clears whole and doesn't read, for FinalizeRenderPassStoreActions.
  void PlanResolveStoreActions(const draw_util::ResolveInfo& resolve_info);
  std::vector<MetalRenderTarget*> dead_store_render_targets_;
  bool AnyDeferredClear(const RenderTarget* render_target) const {
    return render_target &&
           static_cast<const MetalRenderTarget*>(render_target)
               ->clear_deferred();
  }
  // Keys of the render targets that may have a deferred clear.
  std::vector<RenderTargetKey> deferred_clear_targets_;

  bool PerformTransfersAndResolveClears(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const uint64_t* render_target_resolve_clear_values = nullptr,
      const Transfer::Rectangle* resolve_clear_rectangle = nullptr,
      MTL::CommandBuffer* command_buffer = nullptr,
      MTL::RenderCommandEncoder* active_render_encoder = nullptr,
      MTL::RenderPassDescriptor* active_render_pass_descriptor = nullptr,
      DrawPassTransferEncoderMutationMask* mutations_out = nullptr);

  // Whether the depth destination's self-sourced host depth can be preserved
  // by rotating its backings (render the transfer into a spare backing that
  // then becomes active) instead of copying it into the snapshot buffer. The
  // transfers must be unscaled SPIR-V Cross draws with native stencil output
  // covering every tile the destination owns, and nothing else in this update,
  // including transfers already queued for the draw pass, may read the
  // destination.
  bool CanRotateHostDepth(uint32_t render_target_count,
                          RenderTarget* const* render_targets,
                          const std::vector<Transfer>* transfers,
                          bool resolve_clear) const;
  // Ensures the transfer pipelines and a matching spare depth backing exist
  // for the rotation; false keeps the snapshot path.
  bool AcquireSpareDepthBacking(MetalRenderTarget* dest,
                                const std::vector<Transfer>& transfers);

  EdramTransferShaderKey GetTransferShaderKey(
      RenderTargetKey source_key, RenderTargetKey dest_key,
      const RenderTargetKey* host_depth_source_key,
      bool host_depth_source_is_copy, bool stencil_bit,
      uint32_t dest_color_rt_index, const Transfer& transfer) const;
  bool BuildTransferRectanglePlans(
      RenderTargetKey dest_key, const std::vector<Transfer>& transfers,
      const Transfer::Rectangle* cutout, bool require_all_rectangles,
      std::vector<TransferRectanglePlan>& transfer_rectangles_out) const;
  bool GetActiveTransferAttachmentFormats(
      MTL::RenderPassDescriptor* pass_descriptor,
      TransferAttachmentFormats& attachment_formats_out) const;
  bool GetCurrentTransferAttachmentFormats(
      TransferAttachmentFormats& attachment_formats_out) const;
  bool CanQueueDrawPassTransfers(uint32_t render_target_index,
                                 RenderTarget* const* render_targets,
                                 const std::vector<Transfer>& transfers) const;
  bool PendingDrawPassTransfersFullyOverwriteTarget(
      uint32_t render_target_index, RenderTarget* render_target,
      const std::vector<Transfer>& transfers) const;
  bool PreflightPendingDrawPassTransfers(
      const TransferAttachmentFormats& attachment_formats);
  void ClearPendingDrawPassTransfers();
  // Which attachments of the queued transfers rewrite their destination in
  // full, and so have nothing worth loading into tile memory.
  uint32_t GetPendingDrawPassLoadDontCareMask();
  // Bit 0 is the depth/stencil attachment, bits 1 and up the color ones.
  void SetCachedRenderPassLoadActions(uint32_t attachment_mask,
                                      MTL::LoadAction load_action);
  void ApplyPendingDrawPassLoadActions();

  // Writes contents of host render targets within rectangles from
  // Returns the dump pipeline for a key, compiling it on the first use, or null
  // if it could not be built. A failed key is cached as null so it is not
  // retried every resolve.
  MTL::ComputePipelineState* GetOrCreateDumpPipeline(EdramDumpShaderKey key);

  // Writes contents of the host render targets within those same rectangles
  // straight into shared memory in the destination's guest texture layout,
  // skipping edram_buffer_ and the resolve copy that would read it back again.
  // With refresh_texture, also writes the texels into that texture. A
  // 2_10_10_10 four-sample average instead runs the rgb10-4x copy reading the
  // samples from the one 4x render target owning the source, with the group
  // counts of the copy. Returns false if it can't, leaving the caller to fall
  // back to the round trip, which rewrites the whole destination.
  bool DirectResolveRenderTargets(
      const draw_util::ResolveInfo& resolve_info,
      draw_util::ResolveCopyShaderIndex copy_shader,
      const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
      uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
      uint32_t dump_pitch, uint32_t group_count_x, uint32_t group_count_y,
      MTL::CommandBuffer* command_buffer,
      std::span<MTL::Texture* const> refresh_textures = {},
      uint32_t refresh_offset_y = 0, bool refresh_depth_float = false);

  // ResolveInfo::GetCopyEdramTileSpan to edram_buffer_.
  void DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                         uint32_t dump_rows, uint32_t dump_pitch,
                         MTL::CommandBuffer* command_buffer = nullptr);

  void DumpAllRenderTargetsToEdram() override;
  bool BeginEdramSnapshotReadback() override;
  const void* MapEdramSnapshotReadback() override;
  void EndEdramSnapshotReadback() override;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_RENDER_TARGET_CACHE_H_
