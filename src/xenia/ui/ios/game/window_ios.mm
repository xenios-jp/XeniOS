/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/game/window_ios.h"

#import "xenia/ui/ios/game/ios_metal_view.h"

#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>

#include "xenia/base/logging.h"
#include "xenia/ui/ios/game/surface_ios.h"
#include "xenia/ui/ios/app/windowed_app_context_ios.h"

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

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  auto window = std::make_unique<iOSWindow>(app_context, title, desired_logical_width,
                                            desired_logical_height);
  return window;
}

iOSWindow::iOSWindow(WindowedAppContext& app_context, const std::string_view title,
                     uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

iOSWindow::~iOSWindow() {
  EnterDestructor();
  TeardownDisplayLink();
}

void iOSWindow::SetNativeView(UIView* view, UIViewController* view_controller) {
  view_ = view;
  view_controller_ = view_controller;
}

void iOSWindow::HandleSizeChange() {
  uint32_t width = 0;
  uint32_t height = 0;
  if (!GetUIViewFixedDrawableSize(view_, width, height)) {
    return;
  }

  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(width, height, destruction_receiver);
}

void iOSWindow::TriggerPaint() {
  paint_request_pending_.store(false, std::memory_order_release);
  OnPaint();
}

void iOSWindow::SetGuestDisplayRefreshCapFromUIThread(bool capped) {
  guest_display_refresh_capped_ = capped;
  ApplyDisplayLinkFrameRateRangeFromUIThread();
}

bool iOSWindow::OpenImpl() {
  auto& ios_context = static_cast<IOSWindowedAppContext&>(app_context());
  view_ = ios_context.metal_view();
  view_controller_ = ios_context.view_controller();

  if (!view_) {
    XELOGE("iOSWindow::OpenImpl: No Metal view available from app context");
    return false;
  }

  // On iOS, the window is always effectively fullscreen.
  OnDesiredFullscreenUpdate(true);

  // Report initial monitor info.
  {
    MonitorUpdateEvent monitor_event(this, true, true);
    OnMonitorUpdate(monitor_event);
  }

  CGRect bounds = [view_ bounds];
  CGFloat scale = [view_ contentScaleFactor];
  uint32_t width = 0;
  uint32_t height = 0;
  if (!GetUIViewFixedDrawableSize(view_, width, height)) {
    XELOGE("iOSWindow::OpenImpl: Metal view has no drawable area");
    return false;
  }
  {
    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(width, height, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return true;
    }
  }

  // Report initial focus (iOS app is always in focus when visible).
  {
    WindowDestructionReceiver destruction_receiver(this);
    OnFocusUpdate(true, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return true;
    }
  }

  SetupDisplayLink();

  XELOGI("iOSWindow: Opened ({}x{} drawable, view_bounds={}x{} @ {:.0f}x scale)", width, height,
         static_cast<uint32_t>(bounds.size.width), static_cast<uint32_t>(bounds.size.height),
         scale);
  return true;
}

void iOSWindow::RequestCloseImpl() {
  // iOS apps don't close windows in the traditional sense.
  // Signal the close lifecycle for proper cleanup.
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
  TeardownDisplayLink();
  OnAfterClose();
}

uint32_t iOSWindow::GetLatestDpiImpl() const {
  if (view_) {
    // iOS points are 1/163 of an inch at 1x scale.
    // Standard DPI: 163 * scale factor.
    CGFloat scale = [view_ contentScaleFactor];
    return static_cast<uint32_t>(163.0 * scale);
  }
  // Default: 2x Retina = 326 DPI.
  return 326;
}

void iOSWindow::ApplyNewFullscreen() {
  // iOS is always fullscreen. Update status bar/home indicator visibility.
  if (view_controller_) {
    [view_controller_ setNeedsStatusBarAppearanceUpdate];
    [view_controller_ setNeedsUpdateOfHomeIndicatorAutoHidden];
  }
}

std::unique_ptr<Surface> iOSWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!view_) {
    return nullptr;
  }
  if (allowed_types & (1 << Surface::kTypeIndex_iOSUIView)) {
    return std::make_unique<iOSUIViewSurface>(view_);
  }
  return nullptr;
}

void iOSWindow::RequestPaintImpl() {
  // Painting is driven by CADisplayLink while the window is visible. Don't
  // enqueue extra immediate paints in that mode - they can pile up under fast
  // guest refresh and race CoreAnimation drawable lifetime tracking.
  if (display_link_driving_paint_.load(std::memory_order_acquire)) {
    return;
  }
  if (paint_request_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    if (!paint_request_pending_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    OnPaint();
  });
}

void iOSWindow::ApplyDisplayLinkFrameRateRangeFromUIThread() {
  if (!display_link_) {
    return;
  }
  UIScreen* screen = view_ && view_.window ? view_.window.screen : UIScreen.mainScreen;
  const float screen_max_fps =
      screen ? static_cast<float>(screen.maximumFramesPerSecond) : 60.0f;
  const float maximum_fps = std::max(30.0f, screen_max_fps);
  const float capped_fps = std::min(60.0f, maximum_fps);
  const float preferred_fps =
      guest_display_refresh_capped_ ? capped_fps : maximum_fps;
  const float maximum_range_fps =
      guest_display_refresh_capped_ ? capped_fps : maximum_fps;
  display_link_.preferredFrameRateRange =
      CAFrameRateRangeMake(capped_fps, maximum_range_fps, preferred_fps);
}

}  // namespace ui
}  // namespace xe

// Helper Objective-C class for display link callback.
// Must be at global scope (ObjC declarations cannot appear inside C++
// namespaces).
@interface XeniaDisplayLinkTarget : NSObject {
  xe::ui::iOSWindow* _window;
  CADisplayLink* _displayLink;
}
- (instancetype)initWithWindow:(xe::ui::iOSWindow*)window;
- (void)displayLinkFired:(CADisplayLink*)link;
- (void)setDisplayLink:(CADisplayLink*)link;
- (void)startObservingLifecycle;
- (void)stopObservingLifecycle;
@end

@implementation XeniaDisplayLinkTarget
- (instancetype)initWithWindow:(xe::ui::iOSWindow*)window {
  if (self = [super init]) {
    _window = window;
    _displayLink = nil;
  }
  return self;
}

- (void)setDisplayLink:(CADisplayLink*)link {
  _displayLink = link;
}

- (void)displayLinkFired:(CADisplayLink*)link {
  if (_window) {
    _window->TriggerPaint();
  }
}

- (void)startObservingLifecycle {
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(appWillResignActive:)
                                               name:UIApplicationWillResignActiveNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(appDidBecomeActive:)
                                               name:UIApplicationDidBecomeActiveNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(appDidEnterBackground:)
                                               name:UIApplicationDidEnterBackgroundNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(appWillEnterForeground:)
                                               name:UIApplicationWillEnterForegroundNotification
                                             object:nil];
}

- (void)stopObservingLifecycle {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)appWillResignActive:(NSNotification*)notification {
  // Transient resign-active transitions can happen while remaining visible
  // (for example around debugger / system overlays), so don't pause here.
  XELOGI("iOS lifecycle: app resigned active");
}

- (void)appDidBecomeActive:(NSNotification*)notification {
  // Resume the display link when the app returns to foreground.
  if (_displayLink) {
    _displayLink.paused = NO;
  }
  // Notify the window of potential size changes (e.g. split-view transitions).
  if (_window) {
    _window->HandleSizeChange();
  }
  XELOGI("iOS lifecycle: display link resumed (app became active)");
}

- (void)appDidEnterBackground:(NSNotification*)notification {
  // Pause rendering only once the app is actually backgrounded.
  if (_displayLink) {
    _displayLink.paused = YES;
  }
  XELOGI("iOS lifecycle: display link paused (app entered background)");
}

- (void)appWillEnterForeground:(NSNotification*)notification {
  XELOGI("iOS lifecycle: app will enter foreground");
}

- (void)dealloc {
  [self stopObservingLifecycle];
  [super dealloc];
}
@end

static XeniaDisplayLinkTarget* g_display_link_target = nil;

namespace xe {
namespace ui {

void iOSWindow::SetupDisplayLink() {
  if (display_link_) {
    return;
  }

  g_display_link_target = [[XeniaDisplayLinkTarget alloc] initWithWindow:this];
  display_link_ = [CADisplayLink displayLinkWithTarget:g_display_link_target
                                              selector:@selector(displayLinkFired:)];
  [g_display_link_target setDisplayLink:display_link_];
  // Keep presenter pacing display-link driven, with the preferred refresh rate
  // following the guest display cap toggle.
  ApplyDisplayLinkFrameRateRangeFromUIThread();
  [display_link_ addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
  display_link_driving_paint_.store(true, std::memory_order_release);

  // Observe iOS lifecycle events to pause/resume display link.
  [g_display_link_target startObservingLifecycle];
}

void iOSWindow::TeardownDisplayLink() {
  if (g_display_link_target) {
    [g_display_link_target stopObservingLifecycle];
  }
  if (display_link_) {
    [display_link_ invalidate];
    display_link_ = nil;
  }
  display_link_driving_paint_.store(false, std::memory_order_release);
  paint_request_pending_.store(false, std::memory_order_release);
  g_display_link_target = nil;
}

}  // namespace ui
}  // namespace xe
