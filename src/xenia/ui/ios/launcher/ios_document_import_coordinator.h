/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_DOCUMENT_IMPORT_COORDINATOR_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_DOCUMENT_IMPORT_COORDINATOR_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include <filesystem>
#include <string>
#include <vector>

#include "xenia/ui/ios/launcher/ios_game_library.h"

@protocol XeniaIOSDocumentImportCoordinatorHost <NSObject>

- (UIViewController*)documentImportCoordinatorPresenter;
- (BOOL)documentImportCoordinatorGameStopInProgress;
- (BOOL)documentImportCoordinatorJITAcquired;
- (void)documentImportCoordinatorSetStatusText:(NSString*)text;
- (std::filesystem::path)documentImportCoordinatorImportGameAtURL:(NSURL*)sourceURL
                                                            error:(NSError**)error;
- (BOOL)documentImportCoordinatorLinkExternalLibraryAtURL:(NSURL*)folderURL error:(NSError**)error;
- (void)documentImportCoordinatorRefreshImportedGames;
- (void)documentImportCoordinatorRefreshImportedGamesWithCompletion:(void (^)(void))completion;
- (void)documentImportCoordinatorRefreshImportedGamesWithScannedGamesCompletion:
    (void (^)(const std::vector<xe::ui::IOSDiscoveredGame>& games))completion;
- (void)documentImportCoordinatorPromptForZarConversionAfterAddingPath:
            (const std::filesystem::path&)path
                                                          scannedGames:
                                                              (const std::vector<
                                                                  xe::ui::IOSDiscoveredGame>&)games
                                                       externalLibrary:(BOOL)externalLibrary
                                                            completion:
                                                                (void (^)(BOOL conversionChosen))
                                                                    completion;
- (void)documentImportCoordinatorLaunchGameAtPath:(const std::filesystem::path&)gamePath
                                      displayName:(NSString*)displayName;
- (BOOL)documentImportCoordinatorCanInstallTitleUpdates;
- (BOOL)documentImportCoordinatorInstallTitleUpdateAtPath:(const std::filesystem::path&)path
                                                   status:(std::string*)statusOut
                                           notTitleUpdate:(bool*)notTitleUpdateOut;
- (void)documentImportCoordinatorPresentAlertWithTitle:(NSString*)title message:(NSString*)message;
- (void)documentImportCoordinatorImportTouchLayoutAtURL:(NSURL*)url;
- (void)documentImportCoordinatorTouchLayoutImportCancelled;

@end

@interface XeniaIOSDocumentImportCoordinator : NSObject <UIDocumentPickerDelegate>

- (instancetype)initWithHost:(id<XeniaIOSDocumentImportCoordinatorHost>)host;
- (void)presentGameImportPicker;
- (void)presentExternalLibraryFolderPicker;
- (void)presentTouchLayoutImportPickerFromViewController:(UIViewController*)presenter;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_DOCUMENT_IMPORT_COORDINATOR_H_
