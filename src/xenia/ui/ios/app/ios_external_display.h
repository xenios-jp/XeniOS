/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_EXTERNAL_DISPLAY_H_
#define XENIA_UI_IOS_EXTERNAL_DISPLAY_H_

#import <UIKit/UIKit.h>

// External display (USB-C, HDMI adapter or AirPlay) support.
//
// When a display connects and the system offers it to the app (it does unless
// the iPad extends its desktop onto it with Stage Manager), UIKit connects a
// UIWindowSceneSessionRoleExternalDisplayNonInteractive scene (from iOS 27 only
// for a registered scene accessory, see below), handled by
// XeniaExternalDisplaySceneDelegate. Its window shows a plain XeniOS screen
// until a game runs; then the main view controller moves the game's Metal view
// into XeniaExternalDisplayHostView() and keeps touch controls and menus on
// the device. With Stage Manager the app's own window can be on the external
// screen instead, and the game stays in it.

// Posted on the main thread when the external display's host view appears or
// goes away, and when its bounds change.
extern NSNotificationName const XeniaExternalDisplayHostDidChangeNotification;

@interface XeniaExternalDisplaySceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

// iOS 27 and later present external display scenes only for a view controller
// that registers a scene accessory for them; earlier versions use the
// Info.plist scene configuration alone. Returns the registration (retained),
// or nil before iOS 27.
NSObject* XeniaExternalDisplayRegisterSceneAccessory(UIViewController* view_controller)
    NS_RETURNS_RETAINED;

// The external display's full-screen view that hosts the game, or nil when no
// external display scene is connected.
UIView* XeniaExternalDisplayHostView(void);

// Hides the external display's idle screen while the game is on it.
void XeniaExternalDisplaySetHostingGame(BOOL hosting_game);

#endif  // XENIA_UI_IOS_EXTERNAL_DISPLAY_H_
