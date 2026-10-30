/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_DEBUG_SETTINGS_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_DEBUG_SETTINGS_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/settings/ios_config_view_controller.h"

// Searchable config controller for curated Advanced Debug and All Config pages.
@interface XeniaIOSDebugSettingsViewController : XeniaConfigViewController <UISearchResultsUpdating>
- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind
                       liveOverride:(BOOL)liveOverride;
- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind
                       liveOverride:(BOOL)liveOverride
                        gameTitleID:(uint32_t)gameTitleID
                          gameTitle:(NSString*)gameTitle;
@end

#endif  // XENIA_UI_IOS_DEBUG_SETTINGS_VIEW_CONTROLLER_H_
