/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_SETUP_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_SETUP_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/shared/ios_view_helpers.h"

// The Xbox 360 Dashboard's setup, from its tile in the library: gets
// Microsoft's system update, installs it, then offers to start the dashboard
// through |startHandler| once the sheet has closed.
@interface XeniaDashboardSetupViewController : XESheetViewController
- (instancetype)initWithStartHandler:(void (^)(void))start_handler;
@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_SETUP_VIEW_CONTROLLER_H_
