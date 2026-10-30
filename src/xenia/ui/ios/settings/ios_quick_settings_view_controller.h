/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_QUICK_SETTINGS_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_QUICK_SETTINGS_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/settings/ios_config_view_controller.h"

// Presented modally from the in-game overlay. Displays Graphics Compatibility
// settings with live override. When a title ID is provided, changes are also
// persisted as title-specific overrides. Includes a footer row that pushes
// curated Advanced Debug settings.
@interface XeniaIOSQuickSettingsViewController : XeniaConfigViewController
- (instancetype)initWithGameTitleID:(uint32_t)gameTitleID gameTitle:(NSString*)gameTitle;
@end

#endif  // XENIA_UI_IOS_QUICK_SETTINGS_VIEW_CONTROLLER_H_
