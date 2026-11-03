/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_DIAGNOSTICS_H_
#define XENIA_UI_IOS_SHARED_IOS_DIAGNOSTICS_H_

#import <Foundation/Foundation.h>

#include <cstdint>

// Anonymous crash reports and performance statistics ("diagnostics"), recorded
// and sent only while Online Features and Share Diagnostics are both on
// (ios_online_features.h). Records:
// - session: one per game session (ios_session_stats.h);
// - crash: a fault Xenia's exception handler didn't claim (ios_fault_record.h)
//   or a fatal error, sent at the next launch; on iOS 27 the crash reporter
//   extension's report of the same launch; and MetricKit's crash reports,
//   reduced to the crashing thread, with the launch and game they came from.
// They wait in the outbox (ios_diagnostics_outbox.h) until sent. A launch,
// crash or session from a time diagnostics were off is never sent.

// At launch, from the session log setup, with the previous launch's session
// marker (its "state", "version" and "message"): queues what that launch left
// behind, arms the fault record and the guest frame observer, and sends
// pending records a little later.
void xe_ios_diagnostics_start(NSDictionary* previous_marker);

// Random per launch: joins a crash with its session and with the crash
// reporter extension's report.
NSString* xe_ios_diagnostics_launch_id(void);

// A game starts or stops (the session marker's game-running note).
void xe_ios_diagnostics_note_game_running(BOOL running);
// The emulator launched a title: its title ID and the JIT's code range.
void xe_ios_diagnostics_note_title(uint32_t title_id, uint64_t jit_base, uint64_t jit_size);

// A MetricKit crash report as a crash record body (source metrickit), from a
// payload covering |begin| to |end|. Queued with the launch and game running
// then, or dropped when diagnostics were off then or the time is unclear.
void xe_ios_diagnostics_note_metrickit_crash(NSDictionary* body, NSDate* begin, NSDate* end);

// Online Features, Share Diagnostics or a notice answer changed. Turning
// either switch off stops the running session's statistics and deletes
// everything recorded or queued.
void xe_ios_diagnostics_settings_changed(void);

@class UIViewController;
// Settings > About > Pending Reports: the queued records, as they will be sent.
UIViewController* xe_ios_diagnostics_make_pending_reports_controller(void);

#endif  // XENIA_UI_IOS_SHARED_IOS_DIAGNOSTICS_H_
