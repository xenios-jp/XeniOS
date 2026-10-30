/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_CONTROL_SHELL_VIEW_IOS_H_
#define XENIA_UI_IOS_TOUCH_CONTROL_SHELL_VIEW_IOS_H_

#import <UIKit/UIKit.h>

#include "xenia/hid/touch/touch_layout_ios.h"

@interface XeniaTouchControlShellView : UIView

- (instancetype)initWithControl:(const xe::hid::touch::IOSTouchControlDefinition&)control;
- (void)applyControlDefinition:(const xe::hid::touch::IOSTouchControlDefinition&)control;
- (void)setBehaviorAnnotationsVisible:(BOOL)visible;
- (void)setTouchActive:(BOOL)active;
- (void)setConflictHighlighted:(BOOL)highlighted;
// When YES, the shell suppresses all visible chrome (fill, border, label).
// Used for full-screen Look swipe zones during gameplay so the player's view
// of the game isn't obscured. The shell still receives touches.
- (void)setChromeSuppressed:(BOOL)suppressed;

@end

#endif  // XENIA_UI_IOS_TOUCH_CONTROL_SHELL_VIEW_IOS_H_
