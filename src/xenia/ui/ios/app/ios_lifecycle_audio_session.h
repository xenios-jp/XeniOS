/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_LIFECYCLE_AUDIO_SESSION_H_
#define XENIA_UI_IOS_APP_IOS_LIFECYCLE_AUDIO_SESSION_H_

namespace xe {
namespace ui {

// Configures and activates the shared AVAudioSession on a background queue
// (its setters can block) and starts watching for interruptions, route changes
// and media services resets. Safe to call more than once.
void IOSAudioSessionStart();

// Reactivates the session after it was lost, retrying for a few seconds while
// another app (a call) still holds audio, then tells the audio backend to
// restart its engine. Returns immediately. |reason| is for the log.
void IOSAudioSessionRestore(const char* reason);

// For a return from the background, or the end of a pause where the session
// was interrupted and not yet restored: restores only if it is needed (the
// session was interrupted, reset, or the app was in the background).
void IOSAudioSessionRestoreIfNeeded(const char* reason);

// Marks that the app moved to the background, so the next
// IOSAudioSessionRestoreIfNeeded restores the session and engine.
void IOSAudioSessionNoteBackgrounded();

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_APP_IOS_LIFECYCLE_AUDIO_SESSION_H_
