/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_CHOICE_LIST_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_CHOICE_LIST_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include <cstdint>
#include <vector>

#include "xenia/ui/ios/settings/ios_config_models.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

typedef void (^IOSChoiceSelectionHandler)(int64_t value);

// Pushed onto the settings nav stack to pick a single value from a list of
// IOSConfigChoice rows. Confirms the new selection back to the caller via
// IOSChoiceSelectionHandler and pops itself.
@interface XeniaChoiceListViewController : XESheetTableViewController
- (instancetype)initWithTitle:(NSString*)title
                     subtitle:(NSString*)subtitle
                      choices:(const std::vector<IOSConfigChoice>&)choices
                selectedValue:(int64_t)selectedValue
                  onSelection:(IOSChoiceSelectionHandler)onSelection;
@end

#endif  // XENIA_UI_IOS_CHOICE_LIST_VIEW_CONTROLLER_H_
