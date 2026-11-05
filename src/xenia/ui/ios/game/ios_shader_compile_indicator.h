/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_IOS_SHADER_COMPILE_INDICATOR_H_
#define XENIA_UI_IOS_GAME_IOS_SHADER_COMPILE_INDICATOR_H_

#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include "xenia/ui/ios/game/ios_shader_activity.h"
#endif  // __cplusplus

// A small capsule at the top of the game view while the Metal backend has a real
// shader backlog: loading the title's shader cache (with progress), or compiling
// shaders the game asked for. On a title's first run it opens once into a short
// explanation. When the stored shaders are forecast to take a long while to rebuild it also
// offers a sheet to wait for them or play now. The view covers the host's bounds (frame and autoresizing set by the
// host) and never takes touches; it places the capsule itself.
@interface XeniaShaderCompileIndicator : UIView

// Polls the compile counters only while active (a game is running). Turning it off
// hides the capsule at once.
@property(nonatomic, assign, getter=isActive) BOOL active;

@end

// The game-side hold for the Wait choice: called on the main thread with YES to hold the guest
// (and GPU and audio) while the stored shaders rebuild, NO to let it play. The host installs it
// for the app's lifetime; pass nil to remove it.
void xe_ios_set_shader_wait_handler(void (^handler)(BOOL waiting));

// The launch screen's hold, while the stored shaders rebuild before the first frame (main
// thread). Combined with the Wait choice: the game is held while either asks.
void xe_ios_set_launch_shader_hold(BOOL hold);

// Posted on the main thread when the user picks Play now on the shader sheet.
extern NSNotificationName const XeniaShaderPlayNowNotification;

// Settings > Display > Shader Compile Status. On by default.
BOOL xe_ios_shader_compile_status_enabled(void);
void xe_ios_set_shader_compile_status_enabled(BOOL enabled);

#endif  // XENIA_UI_IOS_GAME_IOS_SHADER_COMPILE_INDICATOR_H_
