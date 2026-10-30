/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_CONTROLLER_NAVIGATION_COORDINATOR_H_
#define XENIA_UI_IOS_APP_IOS_CONTROLLER_NAVIGATION_COORDINATOR_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include "xenia/hid/input.h"
#include "xenia/ui/ios/shared/apple_ui_navigation.h"

#import "xenia/ui/ios/app/ios_in_game_menu_overlay.h"

@protocol XeniaIOSControllerNavigationHost <NSObject>

- (BOOL)controllerNavigationLauncherVisible;
- (BOOL)controllerNavigationLauncherActionsEnabled;
- (NSInteger)controllerNavigationGameCount;
- (NSInteger)controllerNavigationLauncherColumnCount;
- (NSInteger)controllerNavigationLauncherPageStep;
- (BOOL)controllerNavigationGameRunning;
- (BOOL)controllerNavigationInGameMenuVisible;
- (BOOL)controllerNavigationInGameMenuActionEnabled:(XeniaIOSInGameMenuAction)action;
- (UIViewController*)controllerNavigationPresentedController;
- (BOOL)controllerNavigationHasConnectedController;
- (BOOL)controllerNavigationReadEmulatorControllerState:(xe::hid::X_INPUT_STATE*)outState;

- (void)controllerNavigationApplyFocusedGameIndex:(NSInteger)index scroll:(BOOL)scroll;
- (void)controllerNavigationApplyLauncherFocusEnabled:(BOOL)enabled
                                      settingsFocused:(BOOL)settingsFocused
                                       profileFocused:(BOOL)profileFocused
                                        importFocused:(BOOL)importFocused
                                   libraryFocusActive:(BOOL)libraryFocusActive;
- (void)controllerNavigationApplyInGameMenuFocusEnabled:(BOOL)enabled
                                          focusedAction:(XeniaIOSInGameMenuAction)focusedAction;

- (void)controllerNavigationOpenSettings;
- (void)controllerNavigationOpenProfile;
- (void)controllerNavigationImportGame;
- (void)controllerNavigationManageGameAtIndex:(NSInteger)index;
- (void)controllerNavigationLaunchGameAtIndex:(NSInteger)index;
- (void)controllerNavigationShowInGameMenu;
- (void)controllerNavigationHideInGameMenu;
- (void)controllerNavigationPerformInGameMenuAction:(XeniaIOSInGameMenuAction)action;

@end

@interface XeniaIOSControllerNavigationCoordinator : NSObject

@property(nonatomic, readonly) NSInteger focusedGameIndex;

- (instancetype)initWithHost:(id<XeniaIOSControllerNavigationHost>)host;
- (void)start;
- (void)invalidate;
- (void)setFocusedGameIndex:(NSInteger)index scroll:(BOOL)scroll;
- (void)refreshLauncherFocus;
- (void)refreshInGameFocus;
- (void)focusDefaultInGameAction;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_APP_IOS_CONTROLLER_NAVIGATION_COORDINATOR_H_
