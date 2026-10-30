/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SURFACE_IOS_H_
#define XENIA_UI_SURFACE_IOS_H_

#include <cstdint>

#include "xenia/ui/surface.h"

#ifdef __OBJC__
@class UIView;
@class CAMetalLayer;
#else
struct UIView;
#ifndef VULKAN_METAL_H_
struct CAMetalLayer;
#endif
#endif

namespace xe {
namespace ui {

class iOSUIViewSurface final : public Surface {
 public:
  explicit iOSUIViewSurface(UIView* view) : view_(view) {}
  TypeIndex GetType() const override { return kTypeIndex_iOSUIView; }
  UIView* view() const { return view_; }
  void ConfigureMetalLayer(uint32_t drawable_width, uint32_t drawable_height,
                           double contents_scale) const;
  CAMetalLayer* GetOrCreateMetalLayer() const;

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  UIView* view_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SURFACE_IOS_H_
