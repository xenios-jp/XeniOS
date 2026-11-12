/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_app_delegate.h"
#import "xenia/ui/ios/app/ios_external_display.h"

#include <unistd.h>

#include <memory>
#include <string>

#import "xenia/ui/ios/shared/ios_background_refresh.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/windowed_app.h"

#import "xenia/ui/ios/app/ios_lifecycle_observer.h"
#import "xenia/ui/ios/app/ios_main_view_controller.h"
#import "xenia/ui/ios/app/windowed_app_context_ios.h"
#import "xenia/ui/ios/game/ios_metal_view.h"
#import "xenia/ui/ios/launcher/ios_game_library_store.h"
#import "xenia/ui/ios/launcher/ios_storage.h"
#import "xenia/ui/ios/shared/ios_crash_reporting.h"
#import "xenia/ui/ios/shared/ios_network_status.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

DECLARE_path(log_file);
DECLARE_bool(log_to_stdout);

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
  // Before launching ends, as iOS requires: a closed app may be woken for it.
  xe_ios_background_refresh_register();
  xe_ios_network_status_start();
  xe_ios_storage_launch_maintenance();
  NSURL* launch_url = nil;
  if (launchOptions) {
    launch_url = [launchOptions objectForKey:UIApplicationLaunchOptionsURLKey];
  }
  if (launch_url) {
    XELOGI("iOS: launch URL deferred to scene bootstrap");
  }
  return YES;
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
  // One accent everywhere: bar buttons, checkmarks and switches use the XeniOS mint instead of
  // system blue and green.
  window.tintColor = [XeniaTheme accent];
  [UISwitch appearance].onTintColor = [XeniaTheme accent];
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
  app_context_->set_gamer_card_ui_prompt_callback([vc](uint32_t, uint64_t xuid) {
    if ([NSThread isMainThread]) {
      [vc presentGamerCardForXuid:xuid completion:nil];
      return true;
    }
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
      [vc presentGamerCardForXuid:xuid
                       completion:^{
                         dispatch_semaphore_signal(sem);
                       }];
    });
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    dispatch_release(sem);
    return true;
  });
  app_context_->set_online_panel_prompt_callback([vc](uint32_t) {
    if ([NSThread isMainThread]) {
      [vc presentOnlineSheetWithCompletion:nil];
      return true;
    }
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
      [vc presentOnlineSheetWithCompletion:^{
        dispatch_semaphore_signal(sem);
      }];
    });
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    dispatch_release(sem);
    return true;
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
  app_context_->set_game_launch_failed_callback([vc](const std::string& reason) {
    [vc presentGameLaunchFailureAlertWithMessage:ToNSString(reason)];
  });
  app_context_->set_game_stop_stuck_callback([vc]() { [vc presentGameStopStuckAlert]; });
  xe::ui::IOSLifecycleObserverInstall(app_context_.get());
  app_context_->set_profile_services_ready_callback([vc]() {
    dispatch_async(dispatch_get_main_queue(), ^{
      [vc refreshSignedInProfileUI];
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
  // Keeps the previous session's log and notes how that session ended, so a crash can be
  // reported on this launch. Must run before the log file is opened.
  xe_ios_begin_session_log(cvars::log_file);
  // Launched by a JIT tool, stdout feeds that tool's live console, which renders every line.
  // A game's log keeps StikDebug busy until it stops serving JIT requests and iOS kills it for
  // CPU use, leaving the game stopped at its next one. The log file keeps everything;
  // XENIA_LOG_TO_STDOUT=1 (an Xcode scheme, say) mirrors it to stdout anyway.
  if (!getenv("XENIA_LOG_TO_STDOUT")) {
    cvars::log_to_stdout = false;
  }
  xe::InitializeLogging(app_->GetName());

  if (!app_->OnInitialize()) {
    XELOGE("iOS: App initialization failed");
    // After the scene's window is on screen.
    dispatch_async(dispatch_get_main_queue(), ^{
      xe_ios_present_init_failure_alert(
          vc, NSLocalizedString(
                  @"The emulator could not be initialized. Sharing the log helps find out why.",
                  @"Alert message when the emulator fails to initialize"));
    });
    return NO;
  }

  [vc refreshSignedInProfileUI];
  if (vc.appContext) {
    vc.appContext->LaunchGame(std::string());
  }

  {
    // Leftover copies of opened files; keep the one this launch is about to import.
    const std::filesystem::path keep_path =
        launch_url.isFileURL ? std::filesystem::path([launch_url.path UTF8String])
                             : std::filesystem::path();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      xe::ui::CleanupIOSDocumentsInbox(keep_path);
    });
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

  // Scheme and host only: file URLs and launch queries carry paths.
  XELOGI("iOS: Received app URL ({}) scheme={} host={}", source_tag ? source_tag : "unknown",
         url.scheme ? [url.scheme UTF8String] : "", url.host ? [url.host UTF8String] : "");

  XeniaViewController* view_controller = [self xeniaViewController];
  if (!view_controller) {
    XELOGW("iOS: Ignoring URL launch; root view controller unavailable");
    return NO;
  }

  // Files opened from Files/AirDrop go through the import flow (security scope,
  // copy or link, Inbox cleanup); xenios:// URLs keep the launch path.
  BOOL handled = url.isFileURL ? [view_controller handleOpenedFileURL:url]
                               : [view_controller handleExternalLaunchURL:url];
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
  if ([connectingSceneSession.role
          isEqualToString:UIWindowSceneSessionRoleExternalDisplayNonInteractive]) {
    UISceneConfiguration* configuration =
        [[[UISceneConfiguration alloc] initWithName:@"External Display"
                                        sessionRole:connectingSceneSession.role] autorelease];
    configuration.delegateClass = [XeniaExternalDisplaySceneDelegate class];
    return configuration;
  }
  UISceneConfiguration* configuration =
      [[[UISceneConfiguration alloc] initWithName:@"Default Configuration"
                                      sessionRole:connectingSceneSession.role] autorelease];
  configuration.delegateClass = [XeniaSceneDelegate class];
  return configuration;
}

- (UIInterfaceOrientationMask)application:(UIApplication*)application
    supportedInterfaceOrientationsForWindow:(UIWindow*)window {
  // This replaces the Info.plist list, so keep its iPhone exclusion of upside-down portrait,
  // which Face ID iPhones don't support.
  UIInterfaceOrientationMask allowed =
      UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone
          ? UIInterfaceOrientationMaskAllButUpsideDown
          : UIInterfaceOrientationMaskAll;
  UIViewController* root = window.rootViewController;
  if (root) {
    return [root supportedInterfaceOrientations] & allowed;
  }
  return allowed;
}

- (void)applicationWillTerminate:(UIApplication*)application {
  XELOGI("iOS lifecycle: applicationWillTerminate");
  // iOS ends an app that takes 5 s to quit and reports it as a hang. Teardown has its own limit,
  // but what runs around it (observers, destructors, exit handlers) can stall too.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, INT64_C(4) * NSEC_PER_SEC),
                 dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                   XELOGE("iOS: still quitting after 4 s; exiting");
                   xe::FlushLog();
                   _exit(0);
                 });
  xe::ui::IOSLifecycleObserverRemove();
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
