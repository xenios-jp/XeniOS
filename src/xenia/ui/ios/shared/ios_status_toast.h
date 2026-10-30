/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_STATUS_TOAST_H_
#define XENIA_UI_IOS_SHARED_IOS_STATUS_TOAST_H_

#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger, XeniaIOSStatusToastStyle) {
  XeniaIOSStatusToastStyleInfo = 0,
  XeniaIOSStatusToastStyleSuccess,
  XeniaIOSStatusToastStyleWarning,
  XeniaIOSStatusToastStyleError,
};

@interface XeniaIOSStatusToastPresenter : NSObject

- (void)presentMessage:(NSString*)message
                 style:(XeniaIOSStatusToastStyle)style
                inView:(UIView*)view;
- (void)presentMessage:(NSString*)message
                 style:(XeniaIOSStatusToastStyle)style
                inView:(UIView*)view
              duration:(NSTimeInterval)duration;
- (void)updateMessage:(NSString*)message;
- (void)dismiss;

@end

#endif  // XENIA_UI_IOS_SHARED_IOS_STATUS_TOAST_H_
