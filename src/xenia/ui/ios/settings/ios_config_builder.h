/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_CONFIG_BUILDER_H_
#define XENIA_UI_IOS_CONFIG_BUILDER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/settings/ios_config_catalog.h"
#include "xenia/ui/ios/settings/ios_config_storage.h"

// Bridge between Xenia's cvar / NSUserDefaults state and the iOS settings
// sheet. BuildIOSConfigSections constructs the rows the sheet shows;
// ApplyIOSConfigSections writes them back into the cvar registry / user
// defaults and persists the config on disk.

#endif  // XENIA_UI_IOS_CONFIG_BUILDER_H_
