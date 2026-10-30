/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_DELEGATE_H_
#define XENIA_UI_IOS_APP_DELEGATE_H_

#import <UIKit/UIKit.h>

@class XeniaViewController;

// Application + scene delegate pair that own the iOS lifecycle: create the
// XeniaWindowedAppContext, mount the XeniaViewController as the window's
// root, route external xenios:// URLs and rebroadcast scene-becomes-active
// into the StikDebug auto-handoff probe.
@interface XeniaAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
- (BOOL)bootstrapApplicationWithWindow:(UIWindow*)window
                             launchURL:(NSURL*)launch_url
                             sourceTag:(const char*)source_tag;
- (XeniaViewController*)xeniaViewController;
- (BOOL)handleExternalLaunchURL:(NSURL*)url sourceTag:(const char*)source_tag;
- (void)evaluateAutomaticStikDebugJITHandoffIfNeeded:(const char*)source_tag;
@end

@interface XeniaSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

#endif  // XENIA_UI_IOS_APP_DELEGATE_H_
