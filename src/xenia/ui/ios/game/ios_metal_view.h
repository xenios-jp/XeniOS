/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_METAL_VIEW_H_
#define XENIA_UI_IOS_METAL_VIEW_H_

#import <UIKit/UIKit.h>

// UIView whose backing CALayer is a CAMetalLayer. Hosts the emulator's Metal
// surface and is owned by the root XeniaViewController.
@interface XeniaMetalView : UIView
@property(nonatomic, assign) CGFloat xeniaDrawableAspectRatio;
@end

#endif  // XENIA_UI_IOS_METAL_VIEW_H_
