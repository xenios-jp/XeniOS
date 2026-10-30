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

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
  XELOGW("LaunchWebBrowser is not supported on iOS: {}", url);
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  XELOGW("LaunchFileExplorer is not supported on iOS: {}", path.string());
}

bool SetProcessPriorityClass(const uint32_t priority_class) {
  static_cast<void>(priority_class);
  return true;
}

bool IsUseNexusForGameBarEnabled() { return false; }

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  static_cast<void>(type);
  XELOGW("ShowSimpleMessageBox is not supported on iOS: {}", message);
}

}  // namespace xe
