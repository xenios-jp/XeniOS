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

// Settings > Xbox 360 System Software: what is installed, downloading the
// system update from Microsoft, installing one from Files (any version), and
// removing it; and the Xbox emulator original Xbox games run through, brought
// from Files, with the build to start games with. Installing and removing wait
// for the running game to end.
@interface XeniaSystemSoftwareViewController : XESheetTableViewController
- (instancetype)initWithGameRunning:(BOOL)game_running;
// Shown on its own, from the launcher, rather than inside Settings.
@property(nonatomic) BOOL showsDoneButton;
// Scrolls to the Original Xbox Games section when the page appears.
@property(nonatomic) BOOL showsOriginalXboxGames;
@end

#endif  // XENIA_UI_IOS_SETTINGS_IOS_SYSTEM_SOFTWARE_VIEW_CONTROLLER_H_
