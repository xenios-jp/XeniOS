/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOW_IOS_H_
#define XENIA_UI_WINDOW_IOS_H_

#include <atomic>
#include <memory>
#include <string>

#include "xenia/ui/window.h"

#ifdef __OBJC__
@class UIView;
@class UIViewController;
@class CADisplayLink;
@class UIScreen;
#else
typedef struct objc_object UIView;
typedef struct objc_object UIViewController;
typedef struct objc_object CADisplayLink;
typedef struct objc_object UIScreen;
#endif

namespace xe {
namespace ui {

class IOSWindowedAppContext;

class iOSWindow : public Window {
 public:
  iOSWindow(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~iOSWindow() override;

  UIView* view() const { return view_; }
  UIViewController* view_controller() const { return view_controller_; }

  // Called from the app delegate when the view becomes available.
  void SetNativeView(UIView* view, UIViewController* view_controller);

  // Called from display link or other paint trigger.
  void TriggerPaint();

  void SetGuestDisplayRefreshCapFromUIThread(bool capped);

  // Called from the view controller on layout changes.
  void HandleSizeChange();

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;

  uint32_t GetLatestDpiImpl() const override;

  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override {}
  void LoadAndApplyIcon(const void* buffer, size_t size,
                        bool can_apply_state_in_current_phase) override {}

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  void SetupDisplayLink();
  void TeardownDisplayLink();
  void ApplyDisplayLinkFrameRateRangeFromUIThread();
  UIScreen* GetViewScreen() const;
  void FollowViewScreenFromUIThread();

  UIView* view_ = nullptr;
  UIViewController* view_controller_ = nullptr;
  CADisplayLink* display_link_ = nullptr;
  // The screen display_link_ is synchronized with, compared by identity only
  // (not retained). The view moves to an external display and back.
  UIScreen* display_link_screen_ = nullptr;
  bool display_link_screen_change_pending_ = false;
  std::atomic<bool> paint_request_pending_{false};
  std::atomic<bool> display_link_driving_paint_{false};
  bool guest_display_refresh_capped_ = true;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOW_IOS_H_
