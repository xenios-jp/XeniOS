/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_ZAR_CONVERSION_COORDINATOR_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_ZAR_CONVERSION_COORDINATOR_H_

#import <UIKit/UIKit.h>

#include <cstddef>
#include <filesystem>
#include <vector>

#include "xenia/ui/ios/launcher/ios_game_library.h"
#import "xenia/ui/ios/shared/ios_status_toast.h"

@protocol XeniaIOSZarConversionCoordinatorHost <NSObject>
- (UIViewController*)zarConversionCoordinatorPresenter;
- (void)zarConversionCoordinatorShowStatusToast:(NSString*)message
                                          style:(XeniaIOSStatusToastStyle)style;
- (void)zarConversionCoordinatorRefreshImportedGames;
- (void)zarConversionCoordinatorRefreshImportedGamesWithCompletion:(void (^)(void))completion;
@end

// One way to convert a game, as the game page lists it.
@interface XeniaIOSZarConversionChoice : NSObject
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* detail;
// It deletes or unlinks the original afterwards.
@property(nonatomic, assign) BOOL destructive;
// Starts the conversion; its progress shows in a sheet.
@property(nonatomic, copy) void (^handler)(void);
@end

// Deletes a game's files in the library: its folder for an extracted game, or the file with its
// sidecar. Refuses paths holding anything else. Blocking: call it off the main thread.
bool XeniaDeleteGameFiles(const std::filesystem::path& source_path, std::string* message_out);

@interface XeniaIOSZarConversionCoordinator : NSObject

- (instancetype)initWithHost:(id<XeniaIOSZarConversionCoordinatorHost>)host;
- (void)checkPendingConversionOnLaunch;
- (void)presentConversionOptionsForGames:(const std::vector<xe::ui::IOSDiscoveredGame>&)games
                                   index:(size_t)gameIndex;
- (void)presentBulkConversionOptionsForGames:(const std::vector<xe::ui::IOSDiscoveredGame>&)games;
- (void)presentPostImportConversionPromptForGames:
            (const std::vector<xe::ui::IOSDiscoveredGame>&)games
                                        addedPath:(const std::filesystem::path&)addedPath
                                  externalLibrary:(BOOL)externalLibrary
                                       completion:(void (^)(BOOL conversionChosen))completion;
// The ways to convert a game, for the game page to list in place: |update| comes at once with
// |estimating| set, and again with the archive's estimated size. |summary| is nil for a game that
// is already a ZAR archive.
- (void)loadConversionChoicesForGames:(const std::vector<xe::ui::IOSDiscoveredGame>&)games
                                index:(size_t)gameIndex
                               update:(void (^)(NSString* summary, BOOL estimating,
                                                NSArray<XeniaIOSZarConversionChoice*>* choices))
                                          update;
// What converting the library would save, worked out in the background and kept per disc image:
// |completion| comes on the main queue with the bytes saved and how many games would shrink. A
// newer call supersedes an older one, whose completion never comes.
- (void)estimateLibrarySavingsForGames:(const std::vector<xe::ui::IOSDiscoveredGame>&)games
                            completion:(void (^)(uint64_t savedBytes, NSUInteger games))completion;
- (void)convertGameToZarForGames:(const std::vector<xe::ui::IOSDiscoveredGame>&)games
                           index:(size_t)gameIndex;

@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_ZAR_CONVERSION_COORDINATOR_H_
