/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_profile_import.h"

#include <filesystem>

#include "xenia/kernel/xam/profile_package.h"
#import "xenia/ui/ios/shared/ios_live_account.h"
#import "xenia/ui/ios/shared/ios_profile_data.h"
#import "xenia/ui/ios/shared/ios_status_toast.h"

namespace {

using xe::kernel::xam::ProfilePackageError;
using xe::kernel::xam::ProfilePackageManifest;

// The service is installed on the main queue at startup, which a file opened to launch the
// app can beat: wait a little for it.
constexpr int kServiceWaits = 10;
constexpr double kServiceWaitSeconds = 0.5;

UIViewController* TopOf(UIViewController* controller) {
  while (controller.presentedViewController) {
    controller = controller.presentedViewController;
  }
  return controller;
}

XeniaIOSStatusToastPresenter* Toasts() {
  static XeniaIOSStatusToastPresenter* toasts = [[XeniaIOSStatusToastPresenter alloc] init];
  return toasts;
}

void ShowAlert(UIViewController* presenter, NSString* title, NSString* message) {
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title
                                          message:message
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"OK", @"Profile import: alert button")
                                 style:UIAlertActionStyleDefault
                               handler:nil]];
  [TopOf(presenter) presentViewController:alert animated:YES completion:nil];
}

NSString* CouldNotImportTitle() {
  return NSLocalizedString(@"Couldn’t Import", @"Profile import: alert title");
}

NSString* ToNS(const std::string& text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

void RunImport(NSURL* url, NSString* gamertag, UIViewController* presenter, void (^finish)(void)) {
  id<XeniaLiveAccountService> service = xe_ios_live_account_service();
  UIView* toast_view = TopOf(presenter).view;
  [Toasts()
      presentMessage:[NSString stringWithFormat:NSLocalizedString(@"Importing “%@”…",
                                                                  @"Profile import: toast; %@ "
                                                                  @"is the gamertag"),
                                                gamertag]
               style:XeniaIOSStatusToastStyleInfo
              inView:toast_view
            duration:0];
  id observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:XeniaProfileDataProgressNotification
                  object:nil
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification* note) {
                const double fraction =
                    [note.userInfo[XeniaProfileDataProgressFractionKey] doubleValue];
                [Toasts()
                    updateMessage:[NSString stringWithFormat:NSLocalizedString(
                                                                 @"Importing “%@”… %d%%",
                                                                 @"Profile import: toast with "
                                                                 @"progress; %@ is the gamertag, "
                                                                 @"%d the percentage"),
                                                             gamertag, int(fraction * 100.0)]];
              }];
  [observer retain];

  // Let a long import finish if the player leaves the app.
  UIApplication* app = [UIApplication sharedApplication];
  __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
  task = [app beginBackgroundTaskWithName:@"Profile import"
                        expirationHandler:^{
                          if (task != UIBackgroundTaskInvalid) {
                            [app endBackgroundTask:task];
                            task = UIBackgroundTaskInvalid;
                          }
                        }];

  [service
      importProfileFromURL:url
                completion:^(NSError* error) {
                  [[NSNotificationCenter defaultCenter] removeObserver:observer];
                  [observer release];
                  if (task != UIBackgroundTaskInvalid) {
                    [app endBackgroundTask:task];
                    task = UIBackgroundTaskInvalid;
                  }
                  finish();
                  if (error) {
                    [Toasts() dismiss];
                    ShowAlert(presenter, CouldNotImportTitle(), error.localizedDescription);
                    return;
                  }
                  [Toasts()
                      presentMessage:[NSString stringWithFormat:NSLocalizedString(
                                                                    @"Imported “%@”",
                                                                    @"Profile import: toast; %@ is "
                                                                    @"the gamertag"),
                                                                gamertag]
                               style:XeniaIOSStatusToastStyleSuccess
                              inView:TopOf(presenter).view];
                }];
}

void Confirm(NSURL* url, UIViewController* presenter, NSString* gamertag, NSString* message,
             void (^finish)(void)) {
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:[NSString stringWithFormat:NSLocalizedString(
                                                              @"Import “%@”?",
                                                              @"Profile import: confirm title; %@ "
                                                              @"is the gamertag"),
                                                          gamertag]
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(
                                                      @"Cancel", @"Profile import: confirm button")
                                            style:UIAlertActionStyleCancel
                                          handler:^(UIAlertAction* __unused action) {
                                            finish();
                                          }]];
  UIAlertAction* import =
      [UIAlertAction actionWithTitle:NSLocalizedString(@"Import", @"Profile import: confirm button")
                               style:UIAlertActionStyleDefault
                             handler:^(UIAlertAction* __unused action) {
                               RunImport(url, gamertag, presenter, finish);
                             }];
  [alert addAction:import];
  alert.preferredAction = import;
  [TopOf(presenter) presentViewController:alert animated:YES completion:nil];
}

// What the file holds, read off the main thread, then the confirm sheet.
void InspectAndConfirm(NSURL* url, UIViewController* presenter, void (^finish)(void)) {
  NSString* file_name = url.lastPathComponent.stringByDeletingPathExtension;
  // The Simulator's preview service fakes the file; there's nothing to read.
  if (xe_ios_live_account_service_is_stub()) {
    Confirm(url, presenter, file_name,
            NSLocalizedString(@"Its saves, achievements and settings are added to this device.",
                              @"Profile import: confirm message without details"),
            finish);
    return;
  }
  const XeniaIOSProfileDataHost* host = xe_ios_profile_data_host();
  const std::filesystem::path content_root =
      host && host->content_root ? host->content_root() : std::filesystem::path();
  const std::filesystem::path path(url.fileSystemRepresentation);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    ProfilePackageManifest manifest;
    const ProfilePackageError error = xe::kernel::xam::InspectProfilePackage(path, &manifest);
    const bool here = error == ProfilePackageError::kNone && !content_root.empty() &&
                      xe::kernel::xam::FindProfileForPackage(content_root, manifest) != 0;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (error != ProfilePackageError::kNone) {
        finish();
        ShowAlert(presenter, CouldNotImportTitle(),
                  xe_ios_profile_package_error(error, NO).localizedDescription);
        return;
      }
      NSString* gamertag = ToNS(manifest.gamertag);
      if (!gamertag.length) {
        gamertag = file_name;
      }
      NSString* contents = xe_ios_profile_package_contents_text(manifest);
      NSString* format;
      if (!here) {
        format = NSLocalizedString(@"%@ It’s added to this device as a new profile.",
                                   @"Profile import: confirm message for a new profile; %@ is "
                                   @"what the file holds");
      } else if (manifest.saves && !manifest.titles.empty()) {
        format = NSLocalizedString(@"%@ Your current saves are kept if they’re newer; replaced "
                                   @"ones are backed up.",
                                   @"Profile import: confirm message for a profile on this "
                                   @"device; %@ is what the file holds");
      } else {
        // No saves to merge: say nothing about keeping them.
        format = NSLocalizedString(@"%@ Anything it replaces is backed up first.",
                                   @"Profile import: confirm message for a profile on this "
                                   @"device, file without saves; %@ is what the file holds");
      }
      NSString* message = [NSString stringWithFormat:format, contents];
      Confirm(url, presenter, gamertag, message, finish);
    });
  });
}

void ImportWhenReady(NSURL* url, UIViewController* presenter, void (^finish)(void),
                     int waits_left) {
  id<XeniaLiveAccountService> service = xe_ios_live_account_service();
  if (!service && waits_left > 0) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(kServiceWaitSeconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     ImportWhenReady(url, presenter, finish, waits_left - 1);
                   });
    return;
  }
  if (!service || !xe_ios_live_account_supports_profile_data()) {
    finish();
    ShowAlert(presenter, NSLocalizedString(@"Can’t Import Yet", @"Profile import: alert title"),
              NSLocalizedString(@"Profile files aren’t supported in this build.",
                                @"Profile import: alert message"));
    return;
  }
  InspectAndConfirm(url, presenter, finish);
}

}  // namespace

BOOL xe_ios_profile_import_handle_url(NSURL* url, UIViewController* presenter) {
  if (!url.isFileURL || ![url.pathExtension.lowercaseString isEqualToString:@"xeniosprofile"]) {
    return NO;
  }
  // A file opened in place in Files needs its security scope until the import is done.
  const BOOL scoped = [url startAccessingSecurityScopedResource];
  __block BOOL finished = NO;
  void (^finish)(void) = ^{
    if (finished) {
      return;
    }
    finished = YES;
    if (scoped) {
      [url stopAccessingSecurityScopedResource];
    }
    // An Inbox copy (AirDrop, "Open in") is ours to remove; a file in Files stays.
    if ([url.path containsString:@"/Documents/Inbox/"]) {
      [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    }
  };
  ImportWhenReady(url, presenter, [[finish copy] autorelease], kServiceWaits);
  return YES;
}
