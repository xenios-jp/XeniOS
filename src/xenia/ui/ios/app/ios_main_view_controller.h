/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_MAIN_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_MAIN_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include <cstdint>

#include "xenia/ui/ios/game/achievement_notification_payload.h"

@class XeniaMetalView;

namespace xe {
namespace hid {
namespace touch {
class IOSTouchRuntimeModel;
}  // namespace touch
}  // namespace hid
namespace ui {
class IOSWindowedAppContext;
}  // namespace ui
}  // namespace xe

// Root iOS UI: hosts the XeniaMetalView (the running emulator surface) and
// the launcher overlay (game library + import + settings + profile + JIT
// status). Owns the in-game menu overlay, controller-driven focus
// navigation, JIT polling, automatic StikDebug handoff and the external
// xenios:// launch flow.
@interface XeniaViewController : UIViewController

// App-delegate bridge surface.
@property(nonatomic, strong) XeniaMetalView* metalView;
@property(nonatomic, strong) UILabel* statusLabel;
@property(nonatomic, assign) xe::ui::IOSWindowedAppContext* appContext;

- (void)refreshSignedInProfileUI;
- (void)refreshImportedGames;
- (void)showLauncherOverlay;
- (void)presentSystemSigninPromptForUserIndex:(uint32_t)user_index
                                  usersNeeded:(uint32_t)users_needed
                                   completion:(void (^)(BOOL success))completion;
- (void)presentAchievementsForUserIndex:(uint32_t)user_index
                                titleID:(uint32_t)title_id
                             completion:(void (^)(BOOL success))completion;
- (void)presentAchievementNotification:(const xe::ui::AchievementNotificationPayload&)payload;
- (void)presentSystemKeyboardPromptWithTitle:(NSString*)title
                                 description:(NSString*)description
                                 defaultText:(NSString*)default_text
                                  completion:(void (^)(BOOL cancelled, NSString* text))completion;
// The text is at most maxLength UTF-16 units, 0 for no limit.
- (void)presentSystemKeyboardPromptWithTitle:(NSString*)title
                                 description:(NSString*)description
                                 defaultText:(NSString*)default_text
                                   maxLength:(NSUInteger)max_length
                                  completion:(void (^)(BOOL cancelled, NSString* text))completion;
- (BOOL)handleExternalLaunchURL:(NSURL*)url;
- (void)evaluateAutomaticStikDebugJITHandoffIfNeeded;

// Returns the IOSTouchRuntimeModel owned by this controller. Used by the app
// delegate to publish the runtime model into the iOS app context so the touch
// HID driver can pull state.
- (xe::hid::touch::IOSTouchRuntimeModel*)touchRuntimeModel;
@end

#endif  // XENIA_UI_IOS_MAIN_VIEW_CONTROLLER_H_
