/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LANDSCAPE_NAVIGATION_CONTROLLER_H_
#define XENIA_UI_IOS_LANDSCAPE_NAVIGATION_CONTROLLER_H_

#import <UIKit/UIKit.h>

// UINavigationController used as the host for sheets that can rotate freely
// alongside the emulator window — landscape, portrait or back. Exists solely
// to override the orientation gating that UIKit otherwise inherits from the
// presenting controller.
@interface XeniaLandscapeNavigationController : UINavigationController
@property(nonatomic, assign) BOOL landscapeOnly;
@end

#endif  // XENIA_UI_IOS_LANDSCAPE_NAVIGATION_CONTROLLER_H_
