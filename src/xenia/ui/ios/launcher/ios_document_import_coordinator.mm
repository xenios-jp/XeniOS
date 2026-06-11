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
#import "xenia/ui/ios/launcher/ios_game_library.h"
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
    [_host documentImportCoordinatorSetStatusText:@"Stopping game... Please wait."];
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
    [_host documentImportCoordinatorSetStatusText:@"Stopping game... Please wait."];
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
  BOOL access_granted = [url startAccessingSecurityScopedResource];
  XELOGI("iOS: User selected game file: {} (security-scoped: {})", [url.path UTF8String],
         access_granted ? "yes" : "no");

  void (^import_selected_game)(void) = ^{
    NSError* import_error = nil;
    std::filesystem::path imported_path =
        [_host documentImportCoordinatorImportGameAtURL:url error:&import_error];
    if (access_granted) {
      [url stopAccessingSecurityScopedResource];
    }

    if (imported_path.empty()) {
      NSString* message = import_error.localizedDescription ?: @"Failed to import selected game.";
      [_host documentImportCoordinatorPresentAlertWithTitle:@"Import Failed" message:message];
      return;
    }

    NSString* imported_name = ToNSString(imported_path.filename().string());
    [self refreshImportedGamesWithScannedGamesCompletion:^(
              const std::vector<xe::ui::IOSDiscoveredGame>& scanned_games) {
      [self
          promptForZarConversionAfterAddingPath:imported_path
                                          games:scanned_games
                                externalLibrary:NO
                                     completion:^(BOOL conversionChosen) {
                                       if (conversionChosen) {
                                         return;
                                       }
                                       if ([_host documentImportCoordinatorJITAcquired]) {
                                         [_host
                                             documentImportCoordinatorLaunchGameAtPath:imported_path
                                                                           displayName:
                                                                               imported_name];
                                       } else {
                                         [_host documentImportCoordinatorSetStatusText:
                                                    [NSString stringWithFormat:
                                                                  @"Imported %@. Waiting for JIT.",
                                                                  imported_name]];
                                       }
                                     }];
    }];
  };

  const std::filesystem::path selected_path([url.path UTF8String]);
  const BOOL likely_direct_game = xe::ui::IsISOPath(selected_path) ||
                                  xe::ui::IsZarPath(selected_path) ||
                                  xe::ui::IsDefaultXexPath(selected_path);
  IOSSelectedContentPackage package_info;
  const BOOL has_content_package_info =
      xe_read_selected_content_package(selected_path, &package_info, nullptr);
  const BOOL is_launchable_package =
      has_content_package_info && xe::ui::IsIOSLaunchableContentType(package_info.content_type);
  const BOOL should_try_title_update_install =
      [_host documentImportCoordinatorCanInstallTitleUpdates] && !likely_direct_game &&
      !is_launchable_package;
  if (should_try_title_update_install) {
    [_host documentImportCoordinatorSetStatusText:@"Checking content package..."];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      std::string status;
      bool not_title_update = false;
      bool success = [_host documentImportCoordinatorInstallTitleUpdateAtPath:selected_path
                                                                       status:&status
                                                               notTitleUpdate:&not_title_update];

      dispatch_async(dispatch_get_main_queue(), ^{
        if (success) {
          if (access_granted) {
            [url stopAccessingSecurityScopedResource];
          }
          NSString* message = status.empty() ? @"Installed title update." : ToNSString(status);
          [_host documentImportCoordinatorSetStatusText:message];
          [_host documentImportCoordinatorRefreshImportedGames];
          [_host documentImportCoordinatorPresentAlertWithTitle:@"Title Update Installed"
                                                        message:message];
          return;
        }

        if (!not_title_update) {
          if (access_granted) {
            [url stopAccessingSecurityScopedResource];
          }
          NSString* message =
              status.empty() ? @"Title update installation failed." : ToNSString(status);
          [_host documentImportCoordinatorSetStatusText:message];
          [_host documentImportCoordinatorPresentAlertWithTitle:@"Installation Failed"
                                                        message:message];
          return;
        }

        import_selected_game();
      });
    });
    return;
  }

  import_selected_game();
}

- (void)linkExternalLibraryAtURL:(NSURL*)url {
  XELOGI("iOS library action: linking external folder path='{}'",
         url.path ? [url.path UTF8String] : "");
  NSError* error = nil;
  if (![_host documentImportCoordinatorLinkExternalLibraryAtURL:url error:&error]) {
    XELOGI("iOS library action: external folder link failed path='{}' error='{}'",
           url.path ? [url.path UTF8String] : "",
           error.localizedDescription ? [error.localizedDescription UTF8String] : "unknown");
    NSString* message = error.localizedDescription ?: @"Failed to link selected folder.";
    [_host documentImportCoordinatorPresentAlertWithTitle:@"Link Failed" message:message];
    return;
  }
  XELOGI("iOS library action: external folder linked path='{}'",
         url.path ? [url.path UTF8String] : "");

  NSString* folder_name = url.lastPathComponent.length > 0 ? url.lastPathComponent : url.path;
  [_host documentImportCoordinatorSetStatusText:[NSString
                                                    stringWithFormat:@"Linked external library: %@",
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
