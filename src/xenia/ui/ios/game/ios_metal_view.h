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
// Drawable height in pixels, or 0 for the default 720. Set while the view is
// on an external display, so the presenter scales to the screen's resolution.
@property(nonatomic, assign) CGFloat xeniaDrawableHeight;
@end

#endif  // XENIA_UI_IOS_METAL_VIEW_H_
