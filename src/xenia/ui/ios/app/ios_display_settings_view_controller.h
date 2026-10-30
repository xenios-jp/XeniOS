/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_DISPLAY_SETTINGS_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_APP_IOS_DISPLAY_SETTINGS_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/app/ios_window_layout.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

// Host accessors backing the Display page. XeniaViewController already
// implements all of these (it owns the window scaling/letterbox/refresh state),
// so it conforms with no extra glue.
@protocol XeniaDisplaySettingsHost <NSObject>
- (XeniaIOSWindowScalingMode)currentWindowScalingMode;
- (void)setCurrentWindowScalingMode:(XeniaIOSWindowScalingMode)mode;
- (BOOL)isPresentLetterboxEnabled;
- (void)setPresentLetterboxEnabled:(BOOL)enabled;
- (BOOL)isGuestDisplayUncapped;
- (void)setGuestDisplayUncapped:(BOOL)uncapped;
@end

// Small pushable Display page used by the pause dashboard in place of the older
// context UIMenu: a plain-language screen-mode picker (Fit / Fill / Stretch)
// plus the letterbox and uncapped-refresh toggles. Changes apply live through
// the host.
@interface XeniaDisplaySettingsViewController : XESheetTableViewController
- (instancetype)initWithHost:(id<XeniaDisplaySettingsHost>)host;
@end

#endif  // XENIA_UI_IOS_APP_IOS_DISPLAY_SETTINGS_VIEW_CONTROLLER_H_
