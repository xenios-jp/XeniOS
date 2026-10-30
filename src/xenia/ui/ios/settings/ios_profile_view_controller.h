/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_PROFILE_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_PROFILE_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/app/windowed_app_context_ios.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

typedef void (^IOSProfileStatusHandler)(NSString* status_message);

// Local profile management sheet: create new gamertags and sign existing
// profiles into emulation slots. Status messages are forwarded back to the
// presenter via the IOSProfileStatusHandler block.
@interface XeniaProfileViewController : XESheetTableViewController
@property(nonatomic, assign) BOOL showsDismissButton;

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(IOSProfileStatusHandler)on_status;
@end

#endif  // XENIA_UI_IOS_PROFILE_VIEW_CONTROLLER_H_
