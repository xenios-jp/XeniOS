/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_RESOURCE_PRESSURE_H_
#define XENIA_UI_IOS_SHARED_IOS_RESOURCE_PRESSURE_H_

#import <Foundation/Foundation.h>

// Reacts to system resource pressure without changing emulation:
//  - When the device reaches the Serious (or Critical) thermal state while a
//    game is running, shows one toast. It isn't shown again until the state
//    has dropped below Serious.
//  - On a memory warning, drops UI-side caches that are rebuilt on next use.
//
// Main thread only; call once. Both blocks are copied and invoked on the main
// thread. |is_game_running| reports whether a game is currently running;
// |present_toast| shows a message through the app's status toast presenter.
void xe_ios_resource_pressure_install(BOOL (^is_game_running)(void),
                                      void (^present_toast)(NSString* message));

// Call when a game starts: a device that is already hot gets the toast too.
void xe_ios_resource_pressure_game_started(void);

#endif  // XENIA_UI_IOS_SHARED_IOS_RESOURCE_PRESSURE_H_
