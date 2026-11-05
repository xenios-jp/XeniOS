/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_external_display.h"

#include <cmath>

#include "xenia/base/logging.h"

NSNotificationName const XeniaExternalDisplayHostDidChangeNotification =
    @"XeniaExternalDisplayHostDidChangeNotification";

// The external display's root view controller: the idle XeniOS screen, and a
// full-screen view that the game's Metal view moves into while a game runs.
@interface XeniaExternalDisplayViewController : UIViewController
@property(nonatomic, strong) UIView* gameHostView;
@property(nonatomic, strong) UIView* idleView;
@property(nonatomic, assign) BOOL hostingGame;
@end

namespace {
// Owned by the scene's window; cleared when the scene disconnects.
XeniaExternalDisplayViewController* g_external_display_view_controller = nil;

void PostHostDidChange() {
  [[NSNotificationCenter defaultCenter]
      postNotificationName:XeniaExternalDisplayHostDidChangeNotification
                    object:nil];
}
}  // namespace

@implementation XeniaExternalDisplayViewController {
  CGSize last_layout_size_;
}

- (void)loadView {
  UIView* root = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  root.backgroundColor = [UIColor systemBackgroundColor];
  self.view = root;

  UIView* idle = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  idle.translatesAutoresizingMaskIntoConstraints = NO;
  [root addSubview:idle];

  UILabel* title = [[[UILabel alloc] initWithFrame:CGRectZero] autorelease];
  title.translatesAutoresizingMaskIntoConstraints = NO;
  title.text = @"XeniOS";
  UIFontDescriptor* title_descriptor =
      [[UIFont systemFontOfSize:72.0 weight:UIFontWeightBold].fontDescriptor
          fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded];
  title.font = title_descriptor ? [UIFont fontWithDescriptor:title_descriptor size:72.0]
                                : [UIFont systemFontOfSize:72.0 weight:UIFontWeightBold];
  title.textColor = [UIColor labelColor];
  title.textAlignment = NSTextAlignmentCenter;
  [idle addSubview:title];

  UILabel* subtitle = [[[UILabel alloc] initWithFrame:CGRectZero] autorelease];
  subtitle.translatesAutoresizingMaskIntoConstraints = NO;
  NSString* format =
      NSLocalizedString(@"Choose a game on your %@ to play it here.",
                        @"External display idle screen; the argument is iPhone or iPad");
  subtitle.text = [NSString stringWithFormat:format, [UIDevice currentDevice].model];
  subtitle.font = [UIFont systemFontOfSize:28.0 weight:UIFontWeightRegular];
  subtitle.textColor = [UIColor secondaryLabelColor];
  subtitle.textAlignment = NSTextAlignmentCenter;
  subtitle.numberOfLines = 0;
  [idle addSubview:subtitle];

  [NSLayoutConstraint activateConstraints:@[
    [idle.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
    [idle.centerYAnchor constraintEqualToAnchor:root.centerYAnchor],
    [idle.widthAnchor constraintLessThanOrEqualToAnchor:root.widthAnchor multiplier:0.8],
    [title.topAnchor constraintEqualToAnchor:idle.topAnchor],
    [title.leadingAnchor constraintEqualToAnchor:idle.leadingAnchor],
    [title.trailingAnchor constraintEqualToAnchor:idle.trailingAnchor],
    [subtitle.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:16.0],
    [subtitle.leadingAnchor constraintEqualToAnchor:idle.leadingAnchor],
    [subtitle.trailingAnchor constraintEqualToAnchor:idle.trailingAnchor],
    [subtitle.bottomAnchor constraintEqualToAnchor:idle.bottomAnchor],
  ]];
  self.idleView = idle;

  // The game's Metal view is placed in here with an explicit frame, so it
  // follows the same Fit / Stretch / Zoom layout as on the device.
  UIView* game_host = [[[UIView alloc] initWithFrame:root.bounds] autorelease];
  game_host.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  game_host.backgroundColor = [UIColor clearColor];
  game_host.clipsToBounds = YES;
  game_host.userInteractionEnabled = NO;
  [root addSubview:game_host];
  self.gameHostView = game_host;
  last_layout_size_ = CGSizeZero;
}

- (void)setHostingGame:(BOOL)hosting_game {
  _hostingGame = hosting_game;
  if (!self.isViewLoaded) {
    return;
  }
  self.idleView.hidden = hosting_game;
  self.view.backgroundColor = hosting_game ? [UIColor blackColor] : [UIColor systemBackgroundColor];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  const CGSize size = self.gameHostView.bounds.size;
  if (!CGSizeEqualToSize(size, last_layout_size_)) {
    last_layout_size_ = size;
    PostHostDidChange();
  }
}

- (void)dealloc {
  [_gameHostView release];
  [_idleView release];
  [super dealloc];
}

@end

@implementation XeniaExternalDisplaySceneDelegate

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)connectionOptions {
  (void)session;
  (void)connectionOptions;
  if (![scene isKindOfClass:[UIWindowScene class]]) {
    return;
  }
  UIWindowScene* window_scene = (UIWindowScene*)scene;
  UIWindow* window = [[[UIWindow alloc] initWithWindowScene:window_scene] autorelease];
  XeniaExternalDisplayViewController* root =
      [[[XeniaExternalDisplayViewController alloc] init] autorelease];
  window.rootViewController = root;
  // A TV is usually in a dim room and shows the dark game; keep its screen dark
  // whatever the device's appearance.
  window.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  window.hidden = NO;
  self.window = window;
  g_external_display_view_controller = root;

  UIScreen* screen = window_scene.screen;
  XELOGI("iOS: external display connected ({}x{} native pixels, scale {:.1f}, up to {} Hz)",
         static_cast<uint32_t>(std::lround(screen.nativeBounds.size.width)),
         static_cast<uint32_t>(std::lround(screen.nativeBounds.size.height)), screen.nativeScale,
         static_cast<int>(screen.maximumFramesPerSecond));
  PostHostDidChange();
}

- (void)sceneDidDisconnect:(UIScene*)scene {
  (void)scene;
  XELOGI("iOS: external display disconnected");
  // Observers move the game back to the device synchronously, before the
  // window and its views go away.
  g_external_display_view_controller = nil;
  PostHostDidChange();
  self.window.hidden = YES;
  self.window = nil;
}

- (void)dealloc {
  [_window release];
  [super dealloc];
}

@end

NSObject* XeniaExternalDisplayRegisterSceneAccessory(UIViewController* view_controller) {
  if (@available(iOS 27.0, *)) {
    UISceneConfiguration* configuration = [[[UISceneConfiguration alloc]
        initWithName:@"External Display"
         sessionRole:UIWindowSceneSessionRoleExternalDisplayNonInteractive] autorelease];
    configuration.delegateClass = [XeniaExternalDisplaySceneDelegate class];
    UISceneAccessoryRegistration* registration = [view_controller
        registerSceneAccessory:[UISceneAccessory
                                   externalNonInteractiveSceneAccessoryWithConfiguration:
                                       configuration]];
    registration.enabled = YES;
    return [registration retain];
  }
  return nil;
}

UIView* XeniaExternalDisplayHostView(void) {
  XeniaExternalDisplayViewController* controller = g_external_display_view_controller;
  if (!controller) {
    return nil;
  }
  [controller loadViewIfNeeded];
  return controller.gameHostView;
}

void XeniaExternalDisplaySetHostingGame(BOOL hosting_game) {
  g_external_display_view_controller.hostingGame = hosting_game;
}
