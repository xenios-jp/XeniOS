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

// A small capsule at the top of the game view while the Metal backend has a real
// shader backlog: loading the title's shader cache (with progress), or compiling
// shaders the game asked for. On a title's first run it opens once into a short
// explanation. The view covers the host's bounds (frame and autoresizing set by the
// host) and never takes touches; it places the capsule itself.
@interface XeniaShaderCompileIndicator : UIView

// Polls the compile counters only while active (a game is running). Turning it off
// hides the capsule at once.
@property(nonatomic, assign, getter=isActive) BOOL active;

@end

// Settings > Display > Shader Compile Status. On by default.
BOOL xe_ios_shader_compile_status_enabled(void);
void xe_ios_set_shader_compile_status_enabled(BOOL enabled);

#endif  // XENIA_UI_IOS_GAME_IOS_SHADER_COMPILE_INDICATOR_H_
