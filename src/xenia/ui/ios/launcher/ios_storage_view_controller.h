/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_STORAGE_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_STORAGE_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/shared/ios_view_helpers.h"

// What XeniOS keeps on the device, by kind and by game, with what can be freed.
// Self-contained: it measures and clears through ios_storage.h, and asks the
// launcher to delete a library game (XeniaStorageDeleteGameRequestNotification).
@interface XeniaStorageViewController : XESheetTableViewController
// While a game runs, nothing is cleared: its files are in use.
- (instancetype)initWithGameRunning:(BOOL)game_running;
@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_STORAGE_VIEW_CONTROLLER_H_
