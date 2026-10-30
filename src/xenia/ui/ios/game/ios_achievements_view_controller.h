/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_IOS_ACHIEVEMENTS_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_GAME_IOS_ACHIEVEMENTS_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include <cstdint>

#include "xenia/ui/ios/shared/ios_view_helpers.h"

namespace xe {
namespace ui {
class IOSWindowedAppContext;
}  // namespace ui
}  // namespace xe

// Native iOS achievements sheet backed by XAM's existing GPD achievement
// data. It is intentionally read-only: games still unlock and persist
// achievements through the shared AchievementManager path.
@interface XeniaAchievementsViewController : XESheetTableViewController

@property(nonatomic, copy) void (^dismissalHandler)(void);

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)appContext
                         userIndex:(uint32_t)userIndex
                           titleID:(uint32_t)titleID;

@end

#endif  // XENIA_UI_IOS_GAME_IOS_ACHIEVEMENTS_VIEW_CONTROLLER_H_
