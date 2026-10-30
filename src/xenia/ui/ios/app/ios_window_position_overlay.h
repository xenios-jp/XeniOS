/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_WINDOW_POSITION_OVERLAY_H_
#define XENIA_UI_IOS_APP_IOS_WINDOW_POSITION_OVERLAY_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

@interface XeniaIOSWindowPositionOverlay : UIView

- (void)beginInView:(UIView*)view
          metalView:(UIView*)metalView
         completion:(void (^)(void))completion;
- (void)endAnimated:(BOOL)animated;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_APP_IOS_WINDOW_POSITION_OVERLAY_H_
