/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_SYSTEM_VERSIONS_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_SYSTEM_VERSIONS_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "xenia/ui/system_files_installer_wx.h"

namespace xe {
namespace ui {

// Several versions of the Xbox 360 system software can be installed. The one
// games use is in the system folder of the storage root, where the only one
// always was, and the emulator mounts it on every launch. Each other one is a
// folder like it in system-versions beside it, named after its version, which
// only a launch of its own dashboard mounts.
//
// The version games use is changed by exchanging the two folders, so the
// system folder is never without a complete version. What a launch that died
// half-way left is settled before anything here is used.

// The build of the update DownloadSystemUpdate fetches: Microsoft's last one.
constexpr uint32_t kIOSDownloadedSystemBuild = 17559;

struct IOSSystemVersion {
  // The folder with its flash, sysext and sysaux.
  std::filesystem::path path;
  // The dashboard's, like "2.0.17559.0". Empty when it can't be read.
  std::string version;
  // The build in the version, like 17559. 0 when it can't be read.
  uint32_t build = 0;
  // The one in the system folder, which every game and the home dashboard use.
  bool is_default = false;
  // Whether it has a dashboard to start.
  bool has_dashboard = false;
  // 0 until measured (ReadIOSSystemSoftwareInfo).
  uint64_t size = 0;

  std::filesystem::path dashboard_path() const {
    return path / "flash" / "dash.xex";
  }
};

// Where the versions games don't use are.
std::filesystem::path IOSSystemVersionsPath();

// Every installed version: the one games use first, then the newest first.
// Reads the head of each dashboard, which is quick enough for the main thread.
std::vector<IOSSystemVersion> ListIOSSystemVersions();

// The installed version |path| is the dashboard of.
std::optional<IOSSystemVersion> FindIOSSystemVersionForDashboard(
    const std::filesystem::path& path);
// The installed version whose folder the file |path| is in: its dashboard, or
// another of its executables, which the dashboard starts.
std::optional<IOSSystemVersion> FindIOSSystemVersionForFile(
    const std::filesystem::path& path);

// The size of a version's folder. Walks it, so keep it off the main thread.
uint64_t MeasureIOSSystemVersion(const std::filesystem::path& version_path);

// Makes the version in |version_path| the one games use; the one they used
// takes a folder in system-versions. Not while a title runs.
bool UseIOSSystemVersionForGames(const std::filesystem::path& version_path);

// Removes the version in |version_path|, one of ListIOSSystemVersions's. The
// one games use goes with everything in the system folder. Not while a title
// runs.
bool RemoveIOSSystemVersion(const std::filesystem::path& version_path);

// An update unpacked beside the installed versions, yet to be put among them.
struct IOSStagedSystemSoftware {
  // False when the install is already done: with no version for games yet, the
  // update is installed in the system folder directly, as it always was.
  bool staged = false;
  // Its dashboard's version. Empty when it can't be read.
  std::string version;
  // That version is installed, and committing replaces it.
  bool already_installed = false;
};

// Unpacks the update in |source_path|, its zip or the folder Files expands it
// to. Blocks, so keep it off the main thread. Nothing installed changes until
// CommitIOSStagedSystemSoftware, unless |staged| says the install is done.
SystemFilesResult StageIOSSystemSoftware(
    const std::filesystem::path& source_path,
    const std::function<bool(size_t done, size_t total)>& progress,
    IOSStagedSystemSoftware* staged);
// Puts the staged update among the installed versions: in place of the
// installed one of its version, or as one more.
SystemFilesResult CommitIOSStagedSystemSoftware();
void DiscardIOSStagedSystemSoftware();

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_SYSTEM_VERSIONS_H_
