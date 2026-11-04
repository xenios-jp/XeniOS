/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_JIT_SHEET_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_JIT_SHEET_VIEW_CONTROLLER_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/shared/ios_view_helpers.h"

// The sheet shown when a game is started without JIT: what JIT is for in one sentence, the
// steps to turn it on, and one button that opens StikDebug.
@interface XeniaIOSJITSheetViewController : XESheetViewController

// |gameTitle| names the game that starts once JIT is on; nil for none.
- (instancetype)initWithGameTitle:(NSString*)gameTitle
                  openStikDebug:(void (^)(void))openStikDebug;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_JIT_SHEET_VIEW_CONTROLLER_H_
