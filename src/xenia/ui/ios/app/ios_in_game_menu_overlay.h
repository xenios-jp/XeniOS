/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_IN_GAME_MENU_OVERLAY_H_
#define XENIA_UI_IOS_APP_IOS_IN_GAME_MENU_OVERLAY_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger, XeniaIOSInGameMenuAction) {
  XeniaIOSInGameMenuActionNone = -1,
  XeniaIOSInGameMenuActionResume = 0,
  XeniaIOSInGameMenuActionEditControls,
  XeniaIOSInGameMenuActionAchievements,
  XeniaIOSInGameMenuActionDisplay = 4,
  XeniaIOSInGameMenuActionSettings,
  XeniaIOSInGameMenuActionLiveLog,
  XeniaIOSInGameMenuActionExit,
  XeniaIOSInGameMenuActionGraphics = 8,
};

@interface XeniaIOSInGameMenuOverlay : UIView

@property(nonatomic, copy) void (^resumeHandler)(void);
@property(nonatomic, copy) void (^editControlsHandler)(void);
@property(nonatomic, copy) void (^achievementsHandler)(void);
@property(nonatomic, copy) void (^settingsHandler)(void);
@property(nonatomic, copy) void (^liveLogHandler)(void);
@property(nonatomic, copy) void (^exitHandler)(void);
@property(nonatomic, copy) void (^graphicsHandler)(void);
@property(nonatomic, strong) UIMenu* displayMenu;

- (BOOL)isOverlayVisible;
- (BOOL)isActionEnabled:(XeniaIOSInGameMenuAction)action;
- (void)performAction:(XeniaIOSInGameMenuAction)action;
- (void)setControllerNavigationEnabled:(BOOL)enabled
                         focusedAction:(XeniaIOSInGameMenuAction)focusedAction;
- (void)setOverlayVisible:(BOOL)visible
                 animated:(BOOL)animated
               completion:(void (^)(BOOL finished))completion;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_APP_IOS_IN_GAME_MENU_OVERLAY_H_
