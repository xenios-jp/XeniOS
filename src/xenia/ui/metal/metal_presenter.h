/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_METAL_METAL_PRESENTER_H_
#define XENIA_UI_METAL_METAL_PRESENTER_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "xenia/ui/metal/metal_provider.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/surface.h"

#ifdef __OBJC__
@class CAMetalLayer;
@protocol MTLCommandQueue;
@protocol MTLTexture;
#else
typedef struct objc_object CAMetalLayer;
typedef struct objc_object* id;
#endif

namespace xe {
namespace ui {
namespace metal {

class MetalProvider;

class MetalGuestOutputRefreshContext final
    : public Presenter::GuestOutputRefreshContext {
 public:
  MetalGuestOutputRefreshContext(bool& is_8bpc_out_ref, id resource)
      : Presenter::GuestOutputRefreshContext(is_8bpc_out_ref),
        resource_(resource) {}

  // The guest output Metal texture that the refresher should write to.
  // Initial state is undefined, refresher must write all pixels.
  id resource_uav_capable() const { return resource_; }
  void SetSubmissionId(uint64_t submission_id) {
    submission_id_ = submission_id;
  }
  uint64_t submission_id() const { return submission_id_; }

 private:
  id resource_;
  uint64_t submission_id_ = 0;
};

class MetalUIDrawContext final : public UIDrawContext {
 public:
  MetalUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                     uint32_t render_target_height,
                     id command_buffer,  // id<MTLCommandBuffer>
                     id render_encoder)  // id<MTLRenderCommandEncoder>
      : UIDrawContext(presenter, render_target_width, render_target_height),
        command_buffer_(command_buffer),
        render_encoder_(render_encoder) {}

  id command_buffer() const { return command_buffer_; }  // id<MTLCommandBuffer>
  id render_encoder() const {
    return render_encoder_;
  }  // id<MTLRenderCommandEncoder>

 private:
  id command_buffer_;  // id<MTLCommandBuffer>
  id render_encoder_;  // id<MTLRenderCommandEncoder>
};

class MetalPresenter : public Presenter {
 public:
  MetalPresenter(MetalProvider* provider,
                 HostGpuLossCallback host_gpu_loss_callback);
  ~MetalPresenter() override;

  bool Initialize();
  void Shutdown();

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(RawImage& image_out) override;

  // wait_event/wait_event_value: optional MTLSharedEvent edge the copy
  // command buffer waits on before reading source_texture. The copy runs on
  // the presenter's own queue, and fences / hazard tracking do not order
  // across queues, so the producing queue must publish its progress through
  // an event for the copy to be ordered after the source texture's writes.
  bool CopyTextureToGuestOutput(MTL::Texture* source_texture, id dest_texture,
                                uint32_t source_width, uint32_t source_height,
                                bool force_swap_rb, bool use_pwl_gamma_ramp,
                                uint64_t* submission_out = nullptr,
                                MTL::SharedEvent* wait_event = nullptr,
                                uint64_t wait_event_value = 0);

  bool UpdateGammaRamp(const void* table_data, size_t table_bytes,
                       const void* pwl_data, size_t pwl_bytes);

 protected:
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width,
      uint32_t new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(
      uint32_t mailbox_index, uint32_t frontbuffer_width,
      uint32_t frontbuffer_height,
      std::function<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out_ref) override;

 private:
  // Metal's blit encoder copyFromTexture:toTexture requires identical pixel
  // formats. The swap surface may be 10-bit or BGRA while the guest output is
  // RGBA8 for PNG capture, so a shader conversion path is required.
  bool EnsureCopyTextureConvertPipelines();
  bool EnsureApplyGammaPipelines();
  bool EnsureGuestOutputPaintResources(uint32_t pixel_format);

  struct PresenterTextureViewCacheEntry {
    MTL::Texture* parent = nullptr;
    MTL::Texture* view = nullptr;
    bool full_descriptor = false;
    MTL::PixelFormat pixel_format = MTL::PixelFormatInvalid;
    MTL::TextureType texture_type = MTL::TextureType2D;
    uint64_t level_location = 0;
    uint64_t level_length = 0;
    uint64_t slice_location = 0;
    uint64_t slice_length = 0;
    MTL::TextureSwizzleChannels swizzle = {};
  };

  MTL::Texture* GetCachedPresenterPixelFormatView(
      PresenterTextureViewCacheEntry& entry, MTL::Texture* parent,
      MTL::PixelFormat pixel_format);
  MTL::Texture* GetCachedPresenterTextureView(
      PresenterTextureViewCacheEntry& entry, MTL::Texture* parent,
      MTL::PixelFormat pixel_format, MTL::TextureType texture_type,
      NS::Range level_range, NS::Range slice_range,
      MTL::TextureSwizzleChannels swizzle);
  void ReleaseCachedPresenterTextureView(PresenterTextureViewCacheEntry& entry);
  void ReleaseCachedPresenterTextureViews();

  MetalProvider* provider_;
  MTL::Device* device_ = nullptr;

  CAMetalLayer* metal_layer_ = nullptr;
  id command_queue_ = nullptr;  // id<MTLCommandQueue>
  id shared_event_ = nullptr;   // id<MTLSharedEvent>

  // Compute pipeline state used to convert/copy from swap formats to the
  // RGBA8 guest output texture.
  id copy_texture_convert_pipeline_2d_ =
      nullptr;  // id<MTLComputePipelineState>
  id copy_texture_convert_pipeline_2d_array_ =
      nullptr;                               // id<MTLComputePipelineState>
  id apply_gamma_table_pipeline_ = nullptr;  // id<MTLComputePipelineState>
  id apply_gamma_pwl_pipeline_ = nullptr;    // id<MTLComputePipelineState>
  id gamma_output_texture_ = nullptr;        // id<MTLTexture>
  uint32_t gamma_output_width_ = 0;
  uint32_t gamma_output_height_ = 0;
  id gamma_ramp_buffer_ = nullptr;         // id<MTLBuffer>
  id gamma_ramp_table_texture_ = nullptr;  // id<MTLTexture>
  id gamma_ramp_pwl_texture_ = nullptr;    // id<MTLTexture>
  uint32_t gamma_ramp_buffer_size_ = 0;
  bool gamma_ramp_table_valid_ = false;
  bool gamma_ramp_pwl_valid_ = false;
  PresenterTextureViewCacheEntry linear_presenter_view_;
  PresenterTextureViewCacheEntry array_presenter_view_;
  PresenterTextureViewCacheEntry swizzle_presenter_view_;
  id guest_output_pipeline_bilinear_ = nullptr;  // id<MTLRenderPipelineState>
  id guest_output_pipeline_bilinear_dither_ =
      nullptr;                         // id<MTLRenderPipelineState>
  id guest_output_sampler_ = nullptr;  // id<MTLSamplerState>
  uint32_t guest_output_pipeline_format_ = 0;

  id metalfx_scaler_ = nullptr;          // id<MTLFXSpatialScaler>
  id metalfx_output_texture_ = nullptr;  // id<MTLTexture>
  uint32_t metalfx_input_width_ = 0;
  uint32_t metalfx_input_height_ = 0;
  uint32_t metalfx_output_width_ = 0;
  uint32_t metalfx_output_height_ = 0;
  uint32_t metalfx_color_format_ = 0;
  uint32_t metalfx_color_processing_mode_ = 0;
  float surface_scale_ = 1.0f;
  uint32_t surface_width_in_points_ = 0;
  uint32_t surface_height_in_points_ = 0;

  // Guest output textures for PNG capture (mailbox system)
  std::array<id, kGuestOutputMailboxSize> guest_output_textures_;
  std::atomic<uint32_t> last_guest_output_mailbox_index_{UINT32_MAX};
  std::array<uint64_t, kGuestOutputMailboxSize> guest_output_submissions_{};
  uint64_t guest_output_waited_submission_ = 0;
  std::atomic<uint64_t> guest_output_submission_counter_{0};
};

}  // namespace metal
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_METAL_METAL_PRESENTER_H_
