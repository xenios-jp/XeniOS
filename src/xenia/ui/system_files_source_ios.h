/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SYSTEM_FILES_SOURCE_IOS_H_
#define XENIA_UI_SYSTEM_FILES_SOURCE_IOS_H_

#include <cstddef>
#include <filesystem>
#include <functional>

#include "xenia/ui/system_files_installer_wx.h"

namespace xe {
namespace ui {

// The system files installer's unzip step on iOS, which has no wx. Writes the
// files of a USB system update into |update_path| from the update's zip, or
// from a folder of its files: its $SystemUpdate folder or the folder holding
// that one, which is what the Files app expands the zip to. |progress| gets the
// files done out of a total and cancels by returning false.
SystemFilesResult ExtractSystemUpdate(
    const std::filesystem::path& source_path,
    const std::filesystem::path& update_path,
    const std::function<bool(size_t done, size_t total)>& progress);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SYSTEM_FILES_SOURCE_IOS_H_
