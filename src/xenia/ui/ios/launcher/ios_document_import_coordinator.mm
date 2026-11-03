/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_document_import_coordinator.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstdint>

#include "xenia/base/logging.h"

#import "xenia/ui/ios/launcher/ios_content_management.h"
#import "xenia/ui/ios/launcher/ios_file_transfer.h"
#import "xenia/ui/ios/launcher/ios_game_library.h"
#import "xenia/ui/ios/launcher/ios_game_library_store.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

enum class IOSDocumentImportMode : uint8_t {
  kGameImport = 0,
  kExternalLibraryFolder,
  kTouchLayoutImport,
};

void AddFilenameExtensionContentType(NSMutableArray<UTType*>* content_types, NSString* extension) {
  UTType* type = [UTType typeWithFilenameExtension:extension conformingToType:UTTypeData];
  if (!type) {
    type = [UTType typeWithFilenameExtension:extension];
  }
  if (type) {
    [content_types addObject:type];
  }
}

}  // namespace

@implementation XeniaIOSDocumentImportCoordinator {
  id<XeniaIOSDocumentImportCoordinatorHost> _host;
  IOSDocumentImportMode _mode;
  BOOL _importInProgress;
}

- (instancetype)initWithHost:(id<XeniaIOSDocumentImportCoordinatorHost>)host {
  if (!(self = [super init])) {
    return nil;
  }
  _host = host;
  _mode = IOSDocumentImportMode::kGameImport;
  return self;
}

- (void)refreshImportedGamesWithCompletion:(void (^)(void))completion {
  if ([_host
          respondsToSelector:@selector(
                                 documentImportCoordinatorRefreshImportedGamesWithCompletion:)]) {
    [_host documentImportCoordinatorRefreshImportedGamesWithCompletion:completion];
    return;
  }

  [_host documentImportCoordinatorRefreshImportedGames];
  if (completion) {
    dispatch_async(dispatch_get_main_queue(), completion);
  }
}

- (void)refreshImportedGamesWithScannedGamesCompletion:
    (void (^)(const std::vector<xe::ui::IOSDiscoveredGame>& games))completion {
  if ([_host respondsToSelector:@selector(
                                    documentImportCoordinatorRefreshImportedGamesWithScannedGamesCompletion:)]) {
    [_host documentImportCoordinatorRefreshImportedGamesWithScannedGamesCompletion:completion];
    return;
  }

  [self refreshImportedGamesWithCompletion:^{
    if (completion) {
      const std::vector<xe::ui::IOSDiscoveredGame> empty_games;
      completion(empty_games);
    }
  }];
}

- (void)promptForZarConversionAfterAddingPath:(const std::filesystem::path&)path
                                        games:
                                            (const std::vector<xe::ui::IOSDiscoveredGame>&)games
                              externalLibrary:(BOOL)externalLibrary
                                   completion:(void (^)(BOOL conversionChosen))completion {
  if ([_host respondsToSelector:@selector(
                                    documentImportCoordinatorPromptForZarConversionAfterAddingPath:
                                    scannedGames:externalLibrary:completion:)]) {
    [_host documentImportCoordinatorPromptForZarConversionAfterAddingPath:path
                                                             scannedGames:games
                                                          externalLibrary:externalLibrary
                                                               completion:completion];
    return;
  }

  if (completion) {
    completion(NO);
  }
}

- (void)presentGameImportPicker {
  _mode = IOSDocumentImportMode::kGameImport;
  if ([_host documentImportCoordinatorGameStopInProgress]) {
    XELOGI("iOS library action: game import picker deferred because stop is in progress");
    [_host documentImportCoordinatorSetStatusText:NSLocalizedString(
                                                      @"Stopping game... Please wait.",
                                                      @"document import coordinator: label text")];
    return;
  }

  NSMutableArray<UTType*>* content_types = [NSMutableArray array];
  for (NSString* extension in @[ @"iso", @"xex", @"zar" ]) {
    AddFilenameExtensionContentType(content_types, extension);
  }
  for (NSString* identifier in @[ @"public.iso-image", @"public.disk-image" ]) {
    UTType* type = [UTType typeWithIdentifier:identifier];
    if (type) {
      [content_types addObject:type];
    }
  }
  [content_types addObject:UTTypeData];
  [content_types addObject:UTTypeItem];
  XELOGI("iOS library action: presenting game import picker");

  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:content_types];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  picker.shouldShowFileExtensions = YES;
  [[_host documentImportCoordinatorPresenter] presentViewController:picker
                                                           animated:YES
                                                         completion:nil];
  [picker release];
}

- (void)presentExternalLibraryFolderPicker {
  _mode = IOSDocumentImportMode::kExternalLibraryFolder;
  if ([_host documentImportCoordinatorGameStopInProgress]) {
    XELOGI("iOS library action: external folder picker deferred because stop is in progress");
    [_host documentImportCoordinatorSetStatusText:NSLocalizedString(
                                                      @"Stopping game... Please wait.",
                                                      @"document import coordinator: label text")];
    return;
  }
  XELOGI("iOS library action: presenting external folder picker");

  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  picker.shouldShowFileExtensions = YES;
  [[_host documentImportCoordinatorPresenter] presentViewController:picker
                                                           animated:YES
                                                         completion:nil];
  [picker release];
}

- (void)presentTouchLayoutImportPickerFromViewController:(UIViewController*)presenter {
  _mode = IOSDocumentImportMode::kTouchLayoutImport;
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeData ]];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  picker.shouldShowFileExtensions = YES;
  [presenter presentViewController:picker animated:YES completion:nil];
  [picker release];
}

- (void)importGameAtURL:(NSURL*)url {
  [self importGameAtURL:url removeSourceWhenDone:NO];
}

// Imports a game, DLC or title-update file. The copy runs on a background queue
// behind a progress alert with Cancel; the security scope stays open until it
// has finished. `removeSource` deletes the source afterwards (an Inbox copy of
// a file opened from Files or AirDrop).
- (void)importGameAtURL:(NSURL*)url removeSourceWhenDone:(BOOL)removeSource {
  if (_importInProgress) {
    [_host documentImportCoordinatorPresentAlertWithTitle:NSLocalizedString(
                                                              @"Import In Progress",
                                                              @"document import coordinator: title")
                                                  message:
                                                      NSLocalizedString(
                                                          @"Wait for the current import to "
                                                          @"finish, or cancel it first.",
                                                          @"document import coordinator: message")];
    return;
  }
  _importInProgress = YES;
  BOOL access_granted = [url startAccessingSecurityScopedResource];
  XELOGI("iOS: User selected game file: {} (security-scoped: {})",
         xe::strip_pii(url.path ? [url.path UTF8String] : ""), access_granted ? "yes" : "no");

  const BOOL can_install_title_updates = [_host documentImportCoordinatorCanInstallTitleUpdates];
  XeniaIOSFileTransfer* transfer = [[XeniaIOSFileTransfer alloc] init];
  XeniaIOSTransferProgressAlert* progress_alert = [[XeniaIOSTransferProgressAlert
        presentFrom:[_host documentImportCoordinatorPresenter]
              title:NSLocalizedString(@"Importing", @"document import coordinator: title")
            message:[NSString
                        stringWithFormat:
                            NSLocalizedString(@"%@\nFor very large files, Link Folder plays them "
                                              @"in place without copying.",
                                              @"document import coordinator: message"),
                            url.lastPathComponent
                                ?: NSLocalizedString(
                                       @"Selected file",
                                       @"document import coordinator: user-visible text")]
      cancelHandler:^{
        [transfer cancel];
      }] retain];

  enum class Outcome { kImported, kUpdateInstalled, kFailed, kCancelled };
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    Outcome outcome = Outcome::kFailed;
    std::filesystem::path imported_path;
    NSString* message = nil;
    @autoreleasepool {
      const std::filesystem::path selected_path([url.path UTF8String]);
      const BOOL likely_direct_game = xe::ui::IsISOPath(selected_path) ||
                                      xe::ui::IsZarPath(selected_path) ||
                                      xe::ui::IsDefaultXexPath(selected_path);
      IOSSelectedContentPackage package_info;
      const BOOL has_content_package_info =
          !likely_direct_game &&
          xe_read_selected_content_package(selected_path, &package_info, nullptr);
      const BOOL is_launchable_package =
          has_content_package_info && xe::ui::IsIOSLaunchableContentType(package_info.content_type);
      BOOL handled = NO;
      if (can_install_title_updates && !likely_direct_game && !is_launchable_package) {
        // The emulator copies and installs the update itself: check space up
        // front, and the Cancel button no longer applies.
        NSError* space_error = nil;
        if (!XeniaIOSCheckFreeSpace(ToNSString((xe_get_ios_documents_path() / "content").string()),
                                    XeniaIOSItemByteSize(url.path), &space_error)) {
          outcome = Outcome::kFailed;
          message = space_error.localizedDescription;
          handled = YES;
        } else {
          dispatch_async(dispatch_get_main_queue(), ^{
            [progress_alert setCancelAvailable:NO];
            [progress_alert updateCopiedBytes:0 totalBytes:0];
          });
          std::string status;
          bool not_title_update = false;
          const bool success =
              [_host documentImportCoordinatorInstallTitleUpdateAtPath:selected_path
                                                                status:&status
                                                        notTitleUpdate:&not_title_update];
          if (success) {
            outcome = Outcome::kUpdateInstalled;
            message = status.empty() ? NSLocalizedString(@"Installed title update.",
                                                         @"document import coordinator: message")
                                     : ToNSString(status);
            handled = YES;
          } else if (!not_title_update) {
            outcome = Outcome::kFailed;
            message = status.empty() ? NSLocalizedString(@"Title update installation failed.",
                                                         @"document import coordinator: message")
                                     : ToNSString(status);
            handled = YES;
          }
        }
      }
      if (!handled) {
        NSError* import_error = nil;
        imported_path = xe::ui::ImportGameIntoIOSLibrary(
            url, transfer,
            ^(uint64_t copied, uint64_t total) {
              dispatch_async(dispatch_get_main_queue(), ^{
                [progress_alert updateCopiedBytes:copied totalBytes:total];
              });
            },
            &import_error);
        if (!imported_path.empty()) {
          outcome = Outcome::kImported;
          if (transfer.skippedItemCount > 0) {
            message = [NSString
                stringWithFormat:NSLocalizedString(
                                     @"%lu item(s) in the folder (links or special files) were not "
                                     @"copied. The game may be incomplete.",
                                     @"document import coordinator: message"),
                                 static_cast<unsigned long>(transfer.skippedItemCount)];
          }
        } else if (transfer.cancelled) {
          outcome = Outcome::kCancelled;
        } else {
          outcome = Outcome::kFailed;
          message = import_error.localizedDescription
                        ?: NSLocalizedString(@"Failed to import selected game.",
                                             @"document import coordinator: message");
        }
      }
      if (removeSource && (outcome == Outcome::kImported || outcome == Outcome::kUpdateInstalled)) {
        // Only after success: on a failure or cancel the Inbox copy may be the only one.
        // Gone already if the import was a rename.
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
      }
      [message retain];
    }

    dispatch_async(dispatch_get_main_queue(), ^{
      if (access_granted) {
        [url stopAccessingSecurityScopedResource];
      }
      _importInProgress = NO;
      [progress_alert dismissWithCompletion:^{
        switch (outcome) {
          case Outcome::kCancelled:
            [_host documentImportCoordinatorSetStatusText:
                       NSLocalizedString(@"Import cancelled.",
                                         @"document import coordinator: label text")];
            break;
          case Outcome::kFailed:
            [_host
                documentImportCoordinatorPresentAlertWithTitle:
                    NSLocalizedString(@"Import Failed", @"document import coordinator: title")
                                                       message:message
                                                                   ?: NSLocalizedString(
                                                                          @"Import failed.",
                                                                          @"document import "
                                                                          @"coordinator: message")];
            break;
          case Outcome::kUpdateInstalled:
            [_host documentImportCoordinatorSetStatusText:message];
            [_host documentImportCoordinatorRefreshImportedGames];
            [_host documentImportCoordinatorPresentAlertWithTitle:
                       NSLocalizedString(@"Title Update Installed",
                                         @"document import coordinator: title")
                                                          message:message];
            break;
          case Outcome::kImported: {
            if (message) {
              [_host documentImportCoordinatorPresentAlertWithTitle:
                         NSLocalizedString(@"Imported With Warnings",
                                           @"document import coordinator: title")
                                                            message:message];
            }
            NSString* imported_name = ToNSString(imported_path.filename().string());
            const std::filesystem::path launch_path = imported_path;
            [self refreshImportedGamesWithScannedGamesCompletion:^(
                      const std::vector<xe::ui::IOSDiscoveredGame>& scanned_games) {
              [self promptForZarConversionAfterAddingPath:launch_path
                                                    games:scanned_games
                                          externalLibrary:NO
                                               completion:^(BOOL conversionChosen) {
                                                 [self launchOrWaitForImportedPath:launch_path
                                                                       displayName:imported_name
                                                                  conversionChosen:conversionChosen];
                                               }];
            }];
          } break;
        }
        [message release];
        [progress_alert release];
        [transfer release];
      }];
    });
  });
}

- (void)launchOrWaitForImportedPath:(const std::filesystem::path&)path
                        displayName:(NSString*)displayName
                   conversionChosen:(BOOL)conversionChosen {
  if (conversionChosen) {
    return;
  }
  if ([_host documentImportCoordinatorGameRunning] ||
      [_host documentImportCoordinatorGameStopInProgress]) {
    // Opening a file must not replace a running game (unsaved progress).
    [_host documentImportCoordinatorSetStatusText:
               [NSString stringWithFormat:NSLocalizedString(@"Imported %@.",
                                                            @"document import coordinator: label text"),
                                          displayName]];
    return;
  }
  if ([_host documentImportCoordinatorJITAcquired]) {
    [_host documentImportCoordinatorLaunchGameAtPath:path displayName:displayName];
  } else {
    [_host documentImportCoordinatorSetStatusText:
               [NSString
                   stringWithFormat:NSLocalizedString(@"Imported %@. Waiting for JIT.",
                                                      @"document import coordinator: label text"),
                                    displayName]];
  }
}

// A file opened from Files, AirDrop or another app. With open-in-place the URL
// points at the original (security scoped); otherwise it is a private copy in
// Documents/Inbox that nothing else cleans up.
- (void)handleOpenedDocumentURL:(NSURL*)url openInPlace:(BOOL)openInPlace {
  if (!url.fileURL) {
    return;
  }
  const BOOL is_inbox_copy = xe::ui::IsPathInIOSDocumentsInbox(
      std::filesystem::path([url.path UTF8String]));
  XELOGI("iOS: Opened document {} (in place: {}, inbox copy: {})",
         xe::strip_pii(url.path ? [url.path UTF8String] : ""), openInPlace ? "yes" : "no",
         is_inbox_copy ? "yes" : "no");
  if ([_host documentImportCoordinatorGameStopInProgress]) {
    [_host documentImportCoordinatorSetStatusText:NSLocalizedString(
                                                      @"Stopping game... Please wait.",
                                                      @"document import coordinator: label text")];
    if (is_inbox_copy) {
      [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    }
    return;
  }

  // Hold the scope while the sheet is up; the import takes its own.
  BOOL access_granted = [url startAccessingSecurityScopedResource];
  NSString* name = url.lastPathComponent ?: @"file";
  UIAlertController* sheet = [UIAlertController
      alertControllerWithTitle:[NSString
                                   stringWithFormat:NSLocalizedString(
                                                        @"Open \"%@\"",
                                                        @"document import coordinator: title"),
                                                    name]
                       message:openInPlace
                                   ? NSLocalizedString(
                                         @"Import copies it into XeniOS (needs the file's size in "
                                         @"free storage). Link Folder plays it from its current "
                                         @"folder without copying.",
                                         @"document import coordinator: message")
                                   : NSLocalizedString(
                                         @"Import it into the XeniOS library.",
                                         @"document import coordinator: user-visible text")
                preferredStyle:UIAlertControllerStyleAlert];
  [url retain];
  void (^finish)(void) = ^{
    if (access_granted) {
      [url stopAccessingSecurityScopedResource];
    }
  };
  [sheet addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Import",
                                                         @"document import coordinator: title")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 // The import takes its own scope; drop ours after.
                                 [self importGameAtURL:url removeSourceWhenDone:is_inbox_copy];
                                 finish();
                                 [url release];
                               }]];
  if (openInPlace && !is_inbox_copy) {
    [sheet addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(@"Link Folder...",
                                                           @"document import coordinator: title")
                                   style:UIAlertActionStyleDefault
                                 handler:^(__unused UIAlertAction* action) {
                                   finish();
                                   [url release];
                                   [self presentExternalLibraryFolderPicker];
                                 }]];
  }
  [sheet addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel",
                                                         @"document import coordinator: title")
                                 style:UIAlertActionStyleCancel
                               handler:^(__unused UIAlertAction* action) {
                                 finish();
                                 if (is_inbox_copy) {
                                   [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
                                 }
                                 [url release];
                               }]];
  // Dropping the scope and the URL is the same on every path that doesn't reach a handler. The
  // Inbox copy, if any, is left for the next launch's cleanup.
  void (^abandon)(void) = [[^{
    finish();
    [url release];
  } copy] autorelease];
  [self presentOpenSheet:sheet attemptsLeft:20 abandon:abandon];
}

// A cold start from Files can get here before the main view is on screen, and a stacked alert
// can be mid-transition. Present only on a presenter that is in a window and idle; otherwise try
// again shortly, then give up through `abandon` so nothing stays held.
- (void)presentOpenSheet:(UIAlertController*)sheet
            attemptsLeft:(int)attempts
                 abandon:(void (^)(void))abandon {
  UIViewController* top = [_host documentImportCoordinatorPresenter];
  while (top.presentedViewController && !top.presentedViewController.isBeingDismissed) {
    top = top.presentedViewController;
  }
  const BOOL ready = top && top.viewIfLoaded.window && !top.isBeingPresented &&
                     !top.isBeingDismissed && !top.presentedViewController;
  if (ready) {
    [top presentViewController:sheet animated:YES completion:nil];
    return;
  }
  if (attempts <= 0) {
    XELOGW("iOS: Could not present the open-in sheet; dropping it");
    abandon();
    return;
  }
  [sheet retain];
  [abandon retain];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.5 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   [self presentOpenSheet:sheet attemptsLeft:attempts - 1 abandon:abandon];
                   [sheet release];
                   [abandon release];
                 });
}

- (void)linkExternalLibraryAtURL:(NSURL*)url {
  XELOGI("iOS library action: linking external folder path='{}'",
         xe::strip_pii(url.path ? [url.path UTF8String] : ""));
  NSError* error = nil;
  if (![_host documentImportCoordinatorLinkExternalLibraryAtURL:url error:&error]) {
    XELOGI("iOS library action: external folder link failed path='{}' error='{}'",
           xe::strip_pii(url.path ? [url.path UTF8String] : ""),
           error.localizedDescription ? xe::strip_pii([error.localizedDescription UTF8String])
                                      : "unknown");
    NSString* message = error.localizedDescription
                            ?: NSLocalizedString(@"Failed to link selected folder.",
                                                 @"document import coordinator: message");
    [_host documentImportCoordinatorPresentAlertWithTitle:NSLocalizedString(
                                                              @"Link Failed",
                                                              @"document import coordinator: title")
                                                  message:message];
    return;
  }
  XELOGI("iOS library action: external folder linked path='{}'",
         xe::strip_pii(url.path ? [url.path UTF8String] : ""));

  NSString* folder_name = url.lastPathComponent.length > 0 ? url.lastPathComponent : url.path;
  [_host
      documentImportCoordinatorSetStatusText:
          [NSString stringWithFormat:NSLocalizedString(@"Linked external library: %@",
                                                       @"document import coordinator: label text"),
                                     folder_name]];
  const std::filesystem::path linked_path(url.path ? [url.path UTF8String] : "");
  [self refreshImportedGamesWithScannedGamesCompletion:^(
            const std::vector<xe::ui::IOSDiscoveredGame>& scanned_games) {
    [self promptForZarConversionAfterAddingPath:linked_path
                                          games:scanned_games
                                externalLibrary:YES
                                     completion:nil];
  }];
}

#pragma mark - UIDocumentPickerDelegate

- (void)documentPicker:(UIDocumentPickerViewController* __unused)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  if (urls.count == 0) {
    return;
  }

  NSURL* url = urls[0];
  if (_mode == IOSDocumentImportMode::kTouchLayoutImport) {
    _mode = IOSDocumentImportMode::kGameImport;
    [_host documentImportCoordinatorImportTouchLayoutAtURL:url];
    return;
  }
  if (_mode == IOSDocumentImportMode::kExternalLibraryFolder) {
    _mode = IOSDocumentImportMode::kGameImport;
    [self linkExternalLibraryAtURL:url];
    return;
  }

  [self importGameAtURL:url];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController* __unused)controller {
  XELOGI("iOS: Document picker cancelled");
  if (_mode == IOSDocumentImportMode::kTouchLayoutImport) {
    _mode = IOSDocumentImportMode::kGameImport;
    [_host documentImportCoordinatorTouchLayoutImportCancelled];
  } else if (_mode == IOSDocumentImportMode::kExternalLibraryFolder) {
    _mode = IOSDocumentImportMode::kGameImport;
  }
}

@end
