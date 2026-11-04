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
  XeniaIOSGameActionSystemLink,
};

// The game page: the game's art, its cover and status, Play with touch layout and
// settings beside it, then the rest of what can be done with the game.
@interface XeniaIOSGameActionsViewController : XESheetViewController

@property(nonatomic, copy) void (^actionHandler)(XeniaIOSGameAction action);
// The compatibility status under the title ("● Playable"); hidden when nil.
@property(nonatomic, copy) NSString* statusText;
@property(nonatomic, retain) UIColor* statusColor;

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
