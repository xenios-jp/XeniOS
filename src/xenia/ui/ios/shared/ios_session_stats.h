/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_SESSION_STATS_H_
#define XENIA_UI_IOS_SHARED_IOS_SESSION_STATS_H_

#import <Foundation/Foundation.h>

#include <cstdint>

// Statistics for one game session, sent as a "session" diagnostics record:
// time to the first frame, guest frame pacing, long frames during shader
// compiles, the shader cache, GPU errors, thermal state and memory. Sampled
// every 5 s and checkpointed 10 s in, then every minute, so a session cut short
// by a crash or a kill is still reported, at the next launch. Only started
// while diagnostics can be sent.

// A game starts. Ignored if a session is already running.
void xe_ios_session_stats_begin(void);
// The running game's title ID, from the emulator's launch event.
void xe_ios_session_stats_note_title(uint32_t title_id);
// The game ended normally: returns the session's record body, or nil if no
// session was running.
NSDictionary* xe_ios_session_stats_end(void);
BOOL xe_ios_session_stats_running(void);
// Diagnostics were turned off: stops the running session, if any, and deletes
// its checkpoint.
void xe_ios_session_stats_discard(void);

// One guest frame was presented (Presenter's refresh observer). Lock-free.
void xe_ios_session_stats_note_guest_frame(void);

// The last checkpoint of a session the previous process didn't end, or nil.
// Deletes it.
NSMutableDictionary* xe_ios_session_stats_take_unfinished(void);

#endif  // XENIA_UI_IOS_SHARED_IOS_SESSION_STATS_H_
