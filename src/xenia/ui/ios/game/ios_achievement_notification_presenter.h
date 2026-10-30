/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_ACHIEVEMENT_NOTIFICATION_PRESENTER_H_
#define XENIA_UI_IOS_ACHIEVEMENT_NOTIFICATION_PRESENTER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/game/achievement_notification_payload.h"

@interface XeniaIOSAchievementNotificationPresenter : NSObject

- (void)presentPayload:(const xe::ui::AchievementNotificationPayload&)payload inView:(UIView*)view;
- (void)dismissAll;

@end

#endif  // XENIA_UI_IOS_ACHIEVEMENT_NOTIFICATION_PRESENTER_H_
