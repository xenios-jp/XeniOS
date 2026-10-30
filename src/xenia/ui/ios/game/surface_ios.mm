/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/game/surface_ios.h"

#import "xenia/ui/ios/game/ios_metal_view.h"

#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>

namespace xe {
namespace ui {

namespace {
constexpr uint32_t kBaseDrawableHeight = 720;
constexpr double kDefaultDrawableAspect = 16.0 / 9.0;

double GetUIViewDrawableAspect(UIView* view, CGRect bounds) {
  if ([view respondsToSelector:@selector(xeniaDrawableAspectRatio)]) {
    const CGFloat configured_aspect =
        [(XeniaMetalView*)view xeniaDrawableAspectRatio];
    if (std::isfinite(static_cast<double>(configured_aspect)) &&
        configured_aspect > 0.0) {
      return static_cast<double>(configured_aspect);
    }
  }

  double aspect = static_cast<double>(bounds.size.width) /
                  std::max(1.0, static_cast<double>(bounds.size.height));
  if (!std::isfinite(aspect) || aspect <= 0.0) {
    aspect = kDefaultDrawableAspect;
  }
  return aspect;
}

bool GetUIViewFixedDrawableSize(UIView* view, uint32_t& width_out,
                                uint32_t& height_out) {
  if (!view) {
    width_out = 0;
    height_out = 0;
    return false;
  }
  CGRect bounds = view.bounds;
  if (CGRectIsEmpty(bounds)) {
    width_out = 0;
    height_out = 0;
    return false;
  }
  const double aspect = GetUIViewDrawableAspect(view, bounds);
  width_out =
      std::max<uint32_t>(1, uint32_t(std::llround(kBaseDrawableHeight * aspect)));
  height_out = kBaseDrawableHeight;
  return true;
}
}  // namespace

bool iOSUIViewSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  // Keep presenter work at a stable 720p-class target. The UIView frame owns
  // final screen placement and aspect changes from Fit / Stretch / Zoom.
  return GetUIViewFixedDrawableSize(view_, width_out, height_out);
}

void iOSUIViewSurface::ConfigureMetalLayer(uint32_t drawable_width, uint32_t drawable_height,
                                           double contents_scale) const {
  CAMetalLayer* metal_layer = GetOrCreateMetalLayer();
  if (!metal_layer) {
    return;
  }
  if (contents_scale <= 0.0) {
    contents_scale = 1.0;
  }
  CALayer* view_layer = view_.layer;
  if (metal_layer != view_layer) {
    metal_layer.frame = view_layer.bounds;
  }
  metal_layer.contentsScale = contents_scale;
  metal_layer.drawableSize =
      CGSizeMake(drawable_width * contents_scale, drawable_height * contents_scale);
}

CAMetalLayer* iOSUIViewSurface::GetOrCreateMetalLayer() const {
  if (!view_) {
    return nullptr;
  }

  // On iOS, UIView's layer can be set to a CAMetalLayer via layerClass
  // or by replacing the layer directly.
  CALayer* layer = [view_ layer];
  if ([layer isKindOfClass:[CAMetalLayer class]]) {
    CAMetalLayer* metal_layer = static_cast<CAMetalLayer*>(layer);
    // The presenter owns guest aspect correction. The layer should only map the
    // drawable to the current UIView bounds.
    metal_layer.contentsGravity = kCAGravityResize;
    return metal_layer;
  }

  // Create and set a new CAMetalLayer.
  CAMetalLayer* metalLayer = [CAMetalLayer layer];
  [view_ setNeedsLayout];
  [view_.layer addSublayer:metalLayer];
  metalLayer.frame = view_.bounds;
  metalLayer.contentsScale = [UIScreen mainScreen].scale;
  metalLayer.contentsGravity = kCAGravityResize;
  metalLayer.framebufferOnly = YES;
  metalLayer.opaque = YES;

  return metalLayer;
}

}  // namespace ui
}  // namespace xe
