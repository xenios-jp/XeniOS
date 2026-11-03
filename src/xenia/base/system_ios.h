/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_SYSTEM_IOS_H_
#define XENIA_BASE_SYSTEM_IOS_H_

#include <filesystem>
#include <string_view>

#include "xenia/base/system.h"

namespace xe {

// xenia-base has no UIKit; the iOS UI layer (ios_crash_reporting.mm) installs
// these at startup. Any pointer may be null, which falls back to logging only.
struct IOSSystemHooks {
  // Blocks the caller until dismissed. Callable from any thread.
  void (*show_message_box)(SimpleMessageBoxType type, std::string_view message);
  // Called by FatalError before the message box, so the session is recorded as
  // crashed even if the box cannot be shown.
  void (*note_fatal_error)(std::string_view message);
  // Both may be called from any thread and must not block.
  void (*open_url)(std::string_view url);
  void (*open_file_explorer)(const std::filesystem::path& path);
};

void SetIOSSystemHooks(const IOSSystemHooks& hooks);
void IOSNoteFatalError(std::string_view message);

}  // namespace xe

#endif  // XENIA_BASE_SYSTEM_IOS_H_
