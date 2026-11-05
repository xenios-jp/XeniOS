/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_HOST_WARNING_H_
#define XENIA_UI_IOS_SHARED_IOS_HOST_WARNING_H_

#include <string>

// Shows a warning toast over the app's window. For shared code that would
// raise an ImGui host notification elsewhere: iOS has no ImGui drawer. Safe
// from any thread.
void xe_ios_show_host_warning(const std::string& message);

#endif  // XENIA_UI_IOS_SHARED_IOS_HOST_WARNING_H_
