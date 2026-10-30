/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_TOUCH_LAYOUT_EDITOR_VIEW_CONTROLLER_IOS_H_
#define XENIA_UI_IOS_TOUCH_TOUCH_LAYOUT_EDITOR_VIEW_CONTROLLER_IOS_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include <cstdint>

#import "xenia/ui/ios/shared/ios_view_helpers.h"
#import "xenia/ui/ios/touch/touch_layout_ui_coordinator_ios.h"

@interface XeniaIOSTouchLayoutEditorViewController
    : XESheetViewController <XeniaIOSTouchLayoutUICoordinatorHost, UIDocumentPickerDelegate>

- (instancetype)initWithTitleID:(uint32_t)titleID title:(NSString*)title;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_TOUCH_TOUCH_LAYOUT_EDITOR_VIEW_CONTROLLER_IOS_H_
