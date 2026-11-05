/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_CRASH_REPORTING_H_
#define XENIA_UI_IOS_CRASH_REPORTING_H_

#import <UIKit/UIKit.h>

#include <filesystem>

// Session tracking, crash/fatal-error reporting and log sharing for iOS.
//
// A small state file in Library/Application Support records whether the
// session is in the foreground (running), in the background (iOS may kill it
// silently, which is not a crash), ended normally, or hit a FatalError. A
// session that was last seen in the foreground or fatal counts as having died
// unexpectedly, and the next launch offers to share its log.

// Call once, on the main thread, just before xe::InitializeLogging. Rotates
// log_path to xenia.previous.log (kept if an earlier crash has not been
// offered yet), reads the previous session's state, starts tracking this one
// and installs the iOS message box, URL and Files hooks (xenia/base/system_ios.h)
// and the MetricKit subscriber.
void xe_ios_begin_session_log(const std::filesystem::path& log_path);

// Records whether a game is running, so a report says what the app was doing.
// Any thread; cheap, and only writes when the value changes.
void xe_ios_session_note_game_running(BOOL running);

// Main thread. If the previous session died unexpectedly and has not been
// offered yet, presents "XeniOS closed unexpectedly last time" with Share Log
// and Dismiss. Does nothing when the presenter is not on screen, is already
// presenting something, or a game is running; call it again later in that case.
void xe_ios_offer_previous_session_report_if_needed(UIViewController* presenter);

// The previous session's crash was in JIT code (its fault record's "inJit"):
// the report says so. Call before the report is first offered.
void xe_ios_note_previous_crash_in_jit(void);

// Kept with this launch's state for a crash record at the next launch:
// {"settings", "gameVersion", "jit"} (xe_ios_diagnostics_note_game_details).
void xe_ios_session_note_crash_details(NSDictionary* details);

// Main thread. Shares the redacted current or previous session log (plus, for
// the previous one, a session summary and any MetricKit diagnostics).
// source_view anchors the iPad popover; nil anchors it to the presenter's view.
void xe_ios_present_log_share(UIViewController* presenter, UIView* source_view,
                              BOOL previous_session);
void xe_ios_present_log_share_from_bar_button(UIViewController* presenter,
                                              UIBarButtonItem* bar_button,
                                              BOOL previous_session);

// Main thread. Non-blocking "could not start" alert with Share Log and OK.
// presenter may be nil, then the topmost foreground view controller is used.
void xe_ios_present_init_failure_alert(UIViewController* presenter, NSString* message);

#endif  // XENIA_UI_IOS_CRASH_REPORTING_H_
