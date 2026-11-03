/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_app_delegate.h"

#include <memory>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/windowed_app.h"

#import "xenia/ui/ios/app/windowed_app_context_ios.h"
#import "xenia/ui/ios/game/ios_metal_view.h"
#import "xenia/ui/ios/app/ios_main_view_controller.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

DECLARE_path(log_file);

@implementation XeniaAppDelegate {
  std::unique_ptr<xe::ui::IOSWindowedAppContext> app_context_;
  std::unique_ptr<xe::ui::WindowedApp> app_;
}

- (void)evaluateAutomaticStikDebugJITHandoffIfNeeded:(const char*)source_tag {
  XeniaViewController* view_controller = [self xeniaViewController];
  if (!view_controller) {
    XELOGW("iOS: Skipping automatic StikDebug handoff evaluation ({}) with no view controller",
           source_tag ? source_tag : "unknown");
    return;
  }
  XELOGI("iOS: Evaluating automatic StikDebug handoff ({})", source_tag ? source_tag : "unknown");
  [view_controller evaluateAutomaticStikDebugJITHandoffIfNeeded];
}

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  (void)application;
  NSURL* launch_url = nil;
  if (launchOptions) {
    launch_url = [launchOptions objectForKey:UIApplicationLaunchOptionsURLKey];
  }
  if (@available(iOS 13.0, *)) {
    if (launch_url) {
      XELOGI("iOS: launch URL deferred to scene bootstrap");
    }
    return YES;
  }

  UIWindow* legacy_window =
      [[[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]] autorelease];
  return [self bootstrapApplicationWithWindow:legacy_window
                                    launchURL:launch_url
                                    sourceTag:"launchOptions"];
}

- (BOOL)bootstrapApplicationWithWindow:(UIWindow*)window
                             launchURL:(NSURL*)launch_url
                             sourceTag:(const char*)source_tag {
  if (!window) {
    XELOGE("iOS: Cannot bootstrap app without a UIWindow");
    return NO;
  }
  UIWindow* previous_window = self.window;
  self.window = window;
  if (app_) {
    UIViewController* existing_root = previous_window.rootViewController;
    if (!self.window.rootViewController && existing_root) {
      self.window.rootViewController = existing_root;
      [self.window makeKeyAndVisible];
    }
    if (launch_url) {
      [self handleExternalLaunchURL:launch_url sourceTag:source_tag ? source_tag : "bootstrap"];
    }
    [self
        evaluateAutomaticStikDebugJITHandoffIfNeeded:source_tag ? source_tag : "bootstrapExisting"];
    return YES;
  }

  // Initialize cvars with no arguments on iOS (arguments come from config).
  int argc = 1;
  char arg0[] = "xenios";
  char* argv[] = {arg0};
  char** argv_ptr = argv;
  cvar::ParseLaunchArguments(argc, argv_ptr, "", {});

  // Create the app context.
  app_context_ = std::make_unique<xe::ui::IOSWindowedAppContext>();

  // Set up the UIKit window and view controller FIRST, so the Metal view
  // is available when the app initializes.
  XeniaViewController* vc = [[XeniaViewController alloc] init];
  self.window.rootViewController = vc;
  [self.window makeKeyAndVisible];
  xe_request_current_orientation(vc);

  // Force layout so the Metal view is created.
  [vc.view layoutIfNeeded];

  // Store the Metal view and view controller in the app context for
  // iOSWindow to use.
  app_context_->set_metal_view(vc.metalView);
  app_context_->set_view_controller(vc);
  app_context_->set_touch_runtime_model([vc touchRuntimeModel]);
  vc.appContext = app_context_.get();
  app_context_->set_signin_ui_prompt_callback([vc](uint32_t user_index, uint32_t users_needed) {
    if ([NSThread isMainThread]) {
      [vc presentSystemSigninPromptForUserIndex:user_index
                                    usersNeeded:users_needed
                                     completion:nil];
      return true;
    }
    __block BOOL success = NO;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
      [vc presentSystemSigninPromptForUserIndex:user_index
                                    usersNeeded:users_needed
                                     completion:^(BOOL prompt_success) {
                                       success = prompt_success;
                                       dispatch_semaphore_signal(sem);
                                     }];
    });
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    dispatch_release(sem);
    return success ? true : false;
  });
  app_context_->set_achievements_ui_prompt_callback([vc](uint32_t user_index, uint32_t title_id) {
    if ([NSThread isMainThread]) {
      [vc presentAchievementsForUserIndex:user_index titleID:title_id completion:nil];
      return true;
    }
    __block BOOL success = NO;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
      [vc presentAchievementsForUserIndex:user_index
                                  titleID:title_id
                               completion:^(BOOL prompt_success) {
                                 success = prompt_success;
                                 dispatch_semaphore_signal(sem);
                               }];
    });
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    dispatch_release(sem);
    return success ? true : false;
  });
  app_context_->set_achievement_notification_callback(
      [vc](const xe::ui::AchievementNotificationPayload& payload) {
        if (!vc) {
          return false;
        }
        xe::ui::AchievementNotificationPayload payload_copy = payload;
        dispatch_async(dispatch_get_main_queue(), ^{
          [vc presentAchievementNotification:payload_copy];
        });
        return true;
      });
  app_context_->set_keyboard_prompt_callback(
      [vc](const std::string& title, const std::string& description,
           const std::string& default_text, size_t max_length, std::string* text_out,
           bool* cancelled_out) {
        if ([NSThread isMainThread]) {
          if (cancelled_out) {
            *cancelled_out = true;
          }
          return false;
        }
        __block BOOL cancelled = YES;
        __block NSString* typed_text = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_main_queue(), ^{
          [vc presentSystemKeyboardPromptWithTitle:ToNSString(title)
                                       description:ToNSString(description)
                                       defaultText:ToNSString(default_text)
                                         maxLength:max_length
                                        completion:^(BOOL prompt_cancelled, NSString* prompt_text) {
                                          cancelled = prompt_cancelled;
                                          typed_text = [(prompt_text ?: @"") copy];
                                          dispatch_semaphore_signal(sem);
                                        }];
        });
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        dispatch_release(sem);
        if (text_out) {
          *text_out = std::string([typed_text UTF8String]);
        }
        if (cancelled_out) {
          *cancelled_out = cancelled ? true : false;
        }
        [typed_text release];
        return true;
      });
  app_context_->set_game_exited_callback([vc]() { [vc showLauncherOverlay]; });
  app_context_->set_profile_services_ready_callback([vc]() {
    dispatch_async(dispatch_get_main_queue(), ^{
      [vc refreshSignedInProfileUI];
      if ([vc.statusLabel.text isEqualToString:@"Initializing profile services..."]) {
        vc.statusLabel.text = @"";
      }
    });
  });
  XELOGI("iOS: Metal view ready ({}x{})",
         static_cast<uint32_t>(vc.metalView.bounds.size.width * vc.metalView.contentScaleFactor),
         static_cast<uint32_t>(vc.metalView.bounds.size.height * vc.metalView.contentScaleFactor));

  // Create and initialize the Xenia app.
  app_ = xe::ui::GetWindowedAppCreator()(*app_context_);
  if (cvars::log_file.empty()) {
    cvars::log_file = xe_get_ios_documents_path() / "xenia.log";
  }
  xe::InitializeLogging(app_->GetName());

  if (!app_->OnInitialize()) {
    XELOGE("iOS: App initialization failed");
    return NO;
  }

  [vc refreshImportedGames];
  [vc refreshSignedInProfileUI];
  if (vc.appContext) {
    vc.statusLabel.text = @"Initializing profile services...";
    vc.appContext->LaunchGame(std::string());
  }

  if (launch_url) {
    [self handleExternalLaunchURL:launch_url sourceTag:source_tag ? source_tag : "bootstrap"];
  }
  [self evaluateAutomaticStikDebugJITHandoffIfNeeded:source_tag ? source_tag : "bootstrapNew"];

  XELOGI("iOS: Application launched successfully");
  return YES;
}

- (XeniaViewController*)xeniaViewController {
  UIViewController* root_view_controller = self.window.rootViewController;
  if ([root_view_controller isKindOfClass:[XeniaViewController class]]) {
    return (XeniaViewController*)root_view_controller;
  }
  return nil;
}

- (BOOL)handleExternalLaunchURL:(NSURL*)url sourceTag:(const char*)source_tag {
  if (!url) {
    return NO;
  }

  NSString* absolute_url = [url absoluteString];
  XELOGI("iOS: Received app URL ({}) {}", source_tag ? source_tag : "unknown",
         absolute_url ? [absolute_url UTF8String] : "");

  XeniaViewController* view_controller = [self xeniaViewController];
  if (!view_controller) {
    XELOGW("iOS: Ignoring URL launch; root view controller unavailable");
    return NO;
  }

  BOOL handled = [view_controller handleExternalLaunchURL:url];
  if (!handled) {
    XELOGW("iOS: URL launch was not handled");
  }
  return handled ? YES : NO;
}

- (BOOL)application:(UIApplication*)application
            openURL:(NSURL*)url
            options:(NSDictionary<UIApplicationOpenURLOptionsKey, id>*)options {
  (void)application;
  (void)options;
  return [self handleExternalLaunchURL:url sourceTag:"openURL"];
}

- (void)applicationDidBecomeActive:(UIApplication*)application {
  (void)application;
  [self evaluateAutomaticStikDebugJITHandoffIfNeeded:"applicationDidBecomeActive"];
}

- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)connectingSceneSession
                                   options:(UISceneConnectionOptions*)options {
  (void)application;
  (void)options;
  if (@available(iOS 13.0, *)) {
    UISceneConfiguration* configuration =
        [[[UISceneConfiguration alloc] initWithName:@"Default Configuration"
                                        sessionRole:connectingSceneSession.role] autorelease];
    configuration.delegateClass = [XeniaSceneDelegate class];
    return configuration;
  }
  return nil;
}

- (UIInterfaceOrientationMask)application:(UIApplication*)application
    supportedInterfaceOrientationsForWindow:(UIWindow*)window {
  UIViewController* root = window.rootViewController;
  if (root) {
    return [root supportedInterfaceOrientations];
  }
  return UIInterfaceOrientationMaskAll;
}

- (void)applicationWillTerminate:(UIApplication*)application {
  XELOGI("iOS lifecycle: applicationWillTerminate");
  if (app_) {
    app_->InvokeOnDestroy();
    app_.reset();
  }
  app_context_.reset();
}

@end

@implementation XeniaSceneDelegate

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)connectionOptions {
  (void)session;
  if (![scene isKindOfClass:[UIWindowScene class]]) {
    XELOGE("iOS: scene connection ignored because scene is not a UIWindowScene");
    return;
  }

  UIWindowScene* window_scene = (UIWindowScene*)scene;
  UIWindow* scene_window = [[[UIWindow alloc] initWithWindowScene:window_scene] autorelease];
  self.window = scene_window;

  NSURL* launch_url = xe_first_open_url_context_url(connectionOptions.URLContexts);
  XeniaAppDelegate* app_delegate = (XeniaAppDelegate*)[UIApplication sharedApplication].delegate;
  if (!app_delegate || ![app_delegate bootstrapApplicationWithWindow:scene_window
                                                           launchURL:launch_url
                                                           sourceTag:"sceneConnect"]) {
    XELOGE("iOS: scene bootstrap failed");
  }
}

- (void)scene:(UIScene*)scene openURLContexts:(NSSet<UIOpenURLContext*>*)URLContexts {
  (void)scene;
  NSURL* url = xe_first_open_url_context_url(URLContexts);
  XeniaAppDelegate* app_delegate = (XeniaAppDelegate*)[UIApplication sharedApplication].delegate;
  if (app_delegate) {
    [app_delegate handleExternalLaunchURL:url sourceTag:"sceneOpenURL"];
  }
}

- (void)sceneDidBecomeActive:(UIScene*)scene {
  (void)scene;
  XeniaAppDelegate* app_delegate = (XeniaAppDelegate*)[UIApplication sharedApplication].delegate;
  if (app_delegate) {
    [app_delegate evaluateAutomaticStikDebugJITHandoffIfNeeded:"sceneDidBecomeActive"];
  }
}

@end
