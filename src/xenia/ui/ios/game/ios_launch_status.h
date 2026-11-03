/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_IOS_LAUNCH_STATUS_H_
#define XENIA_UI_IOS_GAME_IOS_LAUNCH_STATUS_H_

#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include <cstdint>

namespace xe {
namespace ui {
namespace ios {

// What a game launch is doing, from the launch request to the game's first frame.
enum class LaunchStage : uint32_t {
  kNone,
  // The emulator is being set up for the game.
  kPreparing,
  // Waiting for the JIT tool (StikDebug) to prepare the code region. The tool stops
  // the whole app while it works, so the screen can't change until it's done.
  kJit,
  // The game's files are mounted and its executable loaded.
  kLoading,
  // The game runs and hasn't shown a frame yet.
  kStarting,
};

// The emulator thread's side. Begin starts a launch at kPreparing; End closes it
// when the game exits or fails to start. Each change is logged with the time since
// the launch began.
void BeginLaunchStatus();
void SetLaunchStage(LaunchStage stage);
// Publishes |stage| and waits up to |timeout_ms| for the launch screen to show it.
// For a stage that may freeze the app, so the frozen screen says what it waits for.
void SetLaunchStageAndWaitUntilShown(LaunchStage stage, uint32_t timeout_ms);
void EndLaunchStatus();

}  // namespace ios
}  // namespace ui
}  // namespace xe
#endif  // __cplusplus

// Covers the game from the launch request until its first frame: the game's name,
// what the launch is doing and, when a step runs long, how long it has taken and a
// way out.
@interface XeniaLaunchStatusView : UIView

// Polls the launch only while active (a game is running). Turning it off hides the
// screen at once.
@property(nonatomic, assign, getter=isActive) BOOL active;
// Called when the user cancels a launch that runs long.
@property(nonatomic, copy) void (^cancelHandler)(void);

// Shows the screen at once for a launch the user just started, named |title|.
- (void)beginLaunchWithTitle:(NSString*)title;

@end

#endif  // XENIA_UI_IOS_GAME_IOS_LAUNCH_STATUS_H_
