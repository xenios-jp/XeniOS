/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SETTINGS_IOS_SYSTEM_SOFTWARE_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_SETTINGS_IOS_SYSTEM_SOFTWARE_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/shared/ios_view_helpers.h"

// Settings > Xbox 360 System Software: what is installed, Microsoft's page for
// the USB system update, installing it from Files, and removing it. Installing
// and removing wait for the running game to end.
@interface XeniaSystemSoftwareViewController : XESheetTableViewController
- (instancetype)initWithGameRunning:(BOOL)game_running;
// Shown on its own, from the launcher, rather than inside Settings.
@property(nonatomic) BOOL showsDoneButton;
@end

#endif  // XENIA_UI_IOS_SETTINGS_IOS_SYSTEM_SOFTWARE_VIEW_CONTROLLER_H_
