/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LOG_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LOG_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/shared/apple_ui_navigation.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

// Live log viewer presented as a sheet from the launcher and from the
// in-game overlay. Tails xenia.log every 0.5s, supports refresh / share /
// gamepad navigation (LB/RB to scroll, X share, Y refresh).
@interface XeniaLogViewController : XESheetViewController
- (BOOL)handleControllerActions:(const xe::ui::apple::ControllerActionSet&)actions;
@end

#endif  // XENIA_UI_IOS_LOG_VIEW_CONTROLLER_H_
