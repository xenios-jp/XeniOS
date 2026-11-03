/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_LIFECYCLE_OBSERVER_H_
#define XENIA_UI_IOS_APP_IOS_LIFECYCLE_OBSERVER_H_

namespace xe {
namespace ui {

class IOSWindowedAppContext;

// Watches the scene's activation state and pauses the running game while the
// scene is inactive or in the background (IOSWindowedAppContext::
// SetEmulationPaused with kInactive / kBackground). Moving to the background
// holds a background task until the GPU work in flight has drained, because iOS
// does not allow GPU submissions from the background. Also restores the audio
// session when the app comes back.
//
// Main thread only. |context| must outlive the observer: call Remove before it
// is destroyed.
void IOSLifecycleObserverInstall(IOSWindowedAppContext* context);
void IOSLifecycleObserverRemove();

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_APP_IOS_LIFECYCLE_OBSERVER_H_
