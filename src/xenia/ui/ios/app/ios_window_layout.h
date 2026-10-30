/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_WINDOW_LAYOUT_H_
#define XENIA_UI_IOS_WINDOW_LAYOUT_H_

#import <UIKit/UIKit.h>

// User-selectable mode for how the aspect-aware presenter surface is fit into
// the device screen. Persisted in NSUserDefaults by the helper functions below.
typedef NS_ENUM(NSInteger, XeniaIOSWindowScalingMode) {
  XeniaIOSWindowScalingModeFit = 0,
  XeniaIOSWindowScalingModeStretch = 1,
  XeniaIOSWindowScalingModeZoom = 2,
};

XeniaIOSWindowScalingMode XeniaIOSCurrentWindowScalingMode(void);
void XeniaIOSSetCurrentWindowScalingMode(XeniaIOSWindowScalingMode mode);

CGPoint XeniaIOSCurrentPortraitWindowOffset(void);
void XeniaIOSSetPortraitWindowOffset(CGPoint offset);
void XeniaIOSResetPortraitWindowOffset(void);

CGRect XeniaIOSMetalViewFrameForParent(CGRect parent, CGFloat guest_aspect,
                                       XeniaIOSWindowScalingMode mode, CGPoint portrait_offset);
CGPoint XeniaIOSPortraitWindowOffsetByApplyingDrag(CGPoint current_offset, CGSize slack,
                                                   CGPoint translation);

#endif  // XENIA_UI_IOS_WINDOW_LAYOUT_H_
