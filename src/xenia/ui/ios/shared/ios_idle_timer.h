/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_IDLE_TIMER_H_
#define XENIA_UI_IOS_SHARED_IOS_IDLE_TIMER_H_

#import <Foundation/Foundation.h>

// Keeps the screen from dimming and locking while any holder is active, such as
// a running game (controller input doesn't reset the idle timer) or a long
// conversion. Holders are named so one owner can't release another's hold.
// Callable from any thread; the change is applied on the main thread.
void xe_ios_idle_timer_hold(NSString* holder);
void xe_ios_idle_timer_release(NSString* holder);

#endif  // XENIA_UI_IOS_SHARED_IOS_IDLE_TIMER_H_
