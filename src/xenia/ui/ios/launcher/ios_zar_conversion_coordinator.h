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
- (void)zarConversionCoordinatorShowPersistentStatusToast:(NSString*)message
                                                    style:(XeniaIOSStatusToastStyle)style;
- (void)zarConversionCoordinatorUpdateStatusToast:(NSString*)message;
- (void)zarConversionCoordinatorDismissStatusToast;
- (void)zarConversionCoordinatorRefreshImportedGames;
- (void)zarConversionCoordinatorRefreshImportedGamesWithCompletion:(void (^)(void))completion;
@end

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
- (void)convertGameToZarForGames:(const std::vector<xe::ui::IOSDiscoveredGame>&)games
                           index:(size_t)gameIndex;

@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_ZAR_CONVERSION_COORDINATOR_H_
