/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/system.h"

#include "xenia/base/logging.h"
#include "xenia/base/system_ios.h"

namespace xe {

namespace {
// Written once at startup, before any thread that could call these exists.
IOSSystemHooks ios_hooks_ = {};
}  // namespace

void SetIOSSystemHooks(const IOSSystemHooks& hooks) { ios_hooks_ = hooks; }

void IOSNoteFatalError(std::string_view message) {
  if (ios_hooks_.note_fatal_error) {
    ios_hooks_.note_fatal_error(message);
  }
}

void LaunchWebBrowser(const std::string_view url) {
  if (!ios_hooks_.open_url) {
    XELOGW("LaunchWebBrowser is not available on iOS: {}", strip_pii(url));
    return;
  }
  ios_hooks_.open_url(url);
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  if (!ios_hooks_.open_file_explorer) {
    XELOGW("LaunchFileExplorer is not available on iOS: {}",
           strip_pii(path.string()));
    return;
  }
  ios_hooks_.open_file_explorer(path);
}

bool SetProcessPriorityClass(const uint32_t priority_class) {
  static_cast<void>(priority_class);
  return true;
}

bool IsUseNexusForGameBarEnabled() { return false; }

// iOS has no readable hardware UUID.
std::string GetMachineId() { return {}; }

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  if (!ios_hooks_.show_message_box) {
    XELOGW("ShowSimpleMessageBox is not available on iOS: {}",
           strip_pii(message));
    return;
  }
  ios_hooks_.show_message_box(type, message);
}

}  // namespace xe
