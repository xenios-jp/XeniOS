/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_COMPAT_REPORT_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_COMPAT_REPORT_VIEW_CONTROLLER_H_

#import <PhotosUI/PhotosUI.h>
#import <UIKit/UIKit.h>

#include <cstdint>

#include "xenia/ui/ios/shared/ios_view_helpers.h"

// "Submit a Report" sheet. Lets the user pick a status, perf rating, optional
// notes and an optional screenshot, signs the payload with the build's
// attestation if one is present and uploads it via the compat-submission API
// (falling back to GitHub discussion creation when the worker is offline).
@interface XeniaCompatReportViewController
    : XESheetTableViewController <PHPickerViewControllerDelegate, UITextViewDelegate>
- (instancetype)initWithTitleID:(uint32_t)title_id title:(NSString*)title;
@end

#endif  // XENIA_UI_IOS_COMPAT_REPORT_VIEW_CONTROLLER_H_
