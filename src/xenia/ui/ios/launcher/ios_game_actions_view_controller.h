/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_GAME_ACTIONS_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_GAME_ACTIONS_VIEW_CONTROLLER_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include <cstdint>

#import "xenia/ui/ios/shared/ios_view_helpers.h"

typedef NS_ENUM(NSInteger, XeniaIOSGameAction) {
  XeniaIOSGameActionPlay = 0,
  XeniaIOSGameActionGameSettings,
  XeniaIOSGameActionResetGameSettings,
  XeniaIOSGameActionTouchLayout,
  XeniaIOSGameActionCompatibility,
  XeniaIOSGameActionManageContent,
  XeniaIOSGameActionLaunchDisc,
  XeniaIOSGameActionPatches,
  XeniaIOSGameActionConvertToZar,
  XeniaIOSGameActionCopyLaunchURL,
};

@interface XeniaIOSGameActionsViewController : XESheetTableViewController

@property(nonatomic, copy) void (^actionHandler)(XeniaIOSGameAction action);

- (instancetype)initWithGameTitle:(NSString*)gameTitle
                          titleID:(uint32_t)titleID
            supportsCompatibility:(BOOL)supportsCompatibility
            supportsManageContent:(BOOL)supportsManageContent
            supportsDiscSelection:(BOOL)supportsDiscSelection
                  supportsPatches:(BOOL)supportsPatches
            supportsZarConversion:(BOOL)supportsZarConversion;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_GAME_ACTIONS_VIEW_CONTROLLER_H_
