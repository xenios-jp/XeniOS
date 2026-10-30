/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/metal/metal_presenter.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/ios/game/surface_ios.h"

DECLARE_bool(metal_presenter_use_backing_scale);

namespace xe::ui::metal {

Surface::TypeFlags MetalPresenter::GetSupportedSurfaceTypes() const {
  return Surface::kTypeFlag_iOSUIView;
}

Presenter::SurfacePaintConnectResult
MetalPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t new_surface_width,
    uint32_t new_surface_height, bool was_paintable,
    bool& is_vsync_implicit_out) {
  if (new_surface.GetType() != Surface::kTypeIndex_iOSUIView) {
    return SurfacePaintConnectResult::kFailure;
  }
  auto& surface = static_cast<iOSUIViewSurface&>(new_surface);
  CAMetalLayer* layer = surface.GetOrCreateMetalLayer();
  if (!layer) {
    XELOGE("Metal presenter failed to obtain the iOS surface layer");
    return SurfacePaintConnectResult::kFailure;
  }
  UIView* view = surface.view();
  CGFloat scale = view.window.screen.scale;
  if (scale <= 0.0 || !cvars::metal_presenter_use_backing_scale) {
    scale = 1.0;
  }
  layer.device = (__bridge id<MTLDevice>)device_;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  surface_scale_ = static_cast<float>(scale);
  surface_width_in_points_ = new_surface_width;
  surface_height_in_points_ = new_surface_height;
  layer.contentsScale = scale;
  layer.drawableSize =
      CGSizeMake(new_surface_width * scale, new_surface_height * scale);
  metal_layer_ = layer;
  // iOS synchronizes drawable presentation; displaySyncEnabled is macOS-only.
  is_vsync_implicit_out = true;
  return SurfacePaintConnectResult::kSuccess;
}

}  // namespace xe::ui::metal
