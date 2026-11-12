/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_system_versions.h"

#include <stdio.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <system_error>
#include <utility>

#include "xenia/base/logging.h"
#include "xenia/ui/ios/launcher/ios_dashboard.h"
#include "xenia/vfs/xex_metadata.h"

namespace xe {
namespace ui {

namespace {

// Folders of system-versions that aren't versions. The dot keeps them out of
// the list, and out of Files.
// An update unpacked and not yet among the versions, or the version it took
// the place of.
constexpr char kIncomingName[] = ".incoming";
// The system folder, set aside by an exchange that took renames.
constexpr char kOutgoingName[] = ".outgoing";
// A version's folder, set aside likewise.
constexpr char kReplacedName[] = ".replaced";
// A version being deleted.
constexpr char kRemovingName[] = ".removing";
// What the folder of a version that can't be read is called.
constexpr char kUnknownName[] = "unknown";

// Held for every change to the folders, and to list them.
std::mutex versions_mutex;

struct DashboardVersion {
  bool has_dashboard = false;
  std::string version;
  uint32_t build = 0;
};

DashboardVersion ReadDashboardVersion(const std::filesystem::path& version_path) {
  DashboardVersion dashboard;
  const auto dashboard_path = version_path / "flash" / "dash.xex";
  std::error_code ec;
  dashboard.has_dashboard = std::filesystem::is_regular_file(dashboard_path, ec);
  if (dashboard.has_dashboard) {
    if (auto metadata = vfs::ExtractXexMetadata(dashboard_path)) {
      dashboard.version = metadata->version.ToString();
      dashboard.build = metadata->version.build;
    }
  }
  return dashboard;
}

bool IsHiddenName(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  return name.empty() || name.front() == '.';
}

// Whether |path| is a folder of system-versions that holds a version.
bool IsVersionFolder(const std::filesystem::path& path) {
  std::error_code ec;
  return !IsHiddenName(path) && std::filesystem::is_directory(path, ec) &&
         std::filesystem::equivalent(path.parent_path(), IOSSystemVersionsPath(), ec);
}

// Whether a folder called |name| is named after |version|: a second folder of
// a version has a number after it.
bool IsNameForVersion(const std::string& name, const std::string& version) {
  return !version.empty() && name.compare(0, version.size(), version) == 0 &&
         (name.size() == version.size() || name[version.size()] == '-');
}

// A folder of system-versions for |version| that isn't there yet.
std::filesystem::path FreeVersionPath(const std::string& version) {
  const auto versions_path = IOSSystemVersionsPath();
  const std::string name = version.empty() ? kUnknownName : version;
  std::error_code ec;
  auto path = versions_path / name;
  for (int number = 2; std::filesystem::exists(path, ec); ++number) {
    path = versions_path / (name + "-" + std::to_string(number));
  }
  return path;
}

// The folder of the versions isn't left behind empty. remove() only takes an
// empty folder.
void RemoveVersionsFolderIfEmpty() {
  std::error_code ec;
  std::filesystem::remove(IOSSystemVersionsPath(), ec);
}

// The update a download left in |system_path| is kept for the next install,
// which looks for it in the system folder.
void KeepDownload(const std::filesystem::path& former_system_path,
                  const std::filesystem::path& system_path) {
  const auto download_path = GetSystemUpdateDownloadPath(former_system_path).parent_path();
  const auto kept_path = GetSystemUpdateDownloadPath(system_path).parent_path();
  std::error_code ec;
  if (std::filesystem::exists(download_path, ec) && !std::filesystem::exists(kept_path, ec)) {
    std::filesystem::rename(download_path, kept_path, ec);
  }
}

// Puts the folder |from| where the folder |to| is, and |to| where |from| was.
// Returns where |to|'s former content is: |from|, or |aside| if it couldn't be
// moved there. Nothing if nothing changed.
std::optional<std::filesystem::path> ExchangeFolders(const std::filesystem::path& from,
                                                     const std::filesystem::path& to,
                                                     const std::filesystem::path& aside) {
  // In one step, so there is no moment without a folder at |to|.
  if (::renamex_np(from.c_str(), to.c_str(), RENAME_SWAP) == 0) {
    return from;
  }
  const int swap_error = errno;
  if (swap_error != ENOTSUP) {
    XELOGE("iOS: exchanging system software folders failed: {}", std::strerror(swap_error));
    return std::nullopt;
  }
  // The volume can't do that (APFS can). By renames, then: a launch that finds
  // |aside| undoes or finishes them (SettleInterruptedChanges).
  std::error_code ec;
  if (std::filesystem::exists(aside, ec)) {
    XELOGE("iOS: a system software folder set aside earlier is still there");
    return std::nullopt;
  }
  std::filesystem::rename(to, aside, ec);
  if (ec) {
    XELOGE("iOS: setting a system software folder aside failed: {}", ec.message());
    return std::nullopt;
  }
  std::filesystem::rename(from, to, ec);
  if (ec) {
    XELOGE("iOS: putting a system software folder in place failed: {}", ec.message());
    std::filesystem::rename(aside, to, ec);
    if (ec) {
      XELOGE("iOS: putting the folder set aside back failed: {}", ec.message());
    }
    return std::nullopt;
  }
  std::filesystem::rename(aside, from, ec);
  return ec ? aside : from;
}

// |former_path| was the system folder until an exchange. It stays as one of
// the versions, under its version's name, unless there is nothing in it.
void SettleFormerSystemFolder(const std::filesystem::path& former_path,
                              const std::filesystem::path& system_path) {
  KeepDownload(former_path, system_path);
  std::error_code ec;
  // What an install that died left.
  std::filesystem::remove_all(former_path / "staging", ec);
  // No version was in it if it's empty now. remove() only takes an empty
  // folder.
  if (std::filesystem::remove(former_path, ec)) {
    return;
  }
  const auto version = ReadDashboardVersion(former_path).version;
  if (!IsHiddenName(former_path) && IsNameForVersion(former_path.filename().string(), version)) {
    return;
  }
  const auto version_path = FreeVersionPath(version);
  std::filesystem::rename(former_path, version_path, ec);
  if (ec) {
    XELOGE("iOS: naming a system software folder after its version failed: {}", ec.message());
  }
}

// Undoes or finishes what a launch that died in the middle of a change left.
void SettleInterruptedChanges() {
  const auto system_path = IOSSystemFilesPath();
  const auto versions_path = IOSSystemVersionsPath();
  std::error_code ec;
  if (!std::filesystem::is_directory(versions_path, ec)) {
    // Only ever one version: nothing was changed here.
    return;
  }
  // An update that was never put among the versions, or the version one
  // replaced, and a version that was being removed.
  for (const char* name : {kIncomingName, kRemovingName}) {
    std::filesystem::remove_all(versions_path / name, ec);
  }
  // The system folder an exchange by renames set aside goes back if nothing
  // took its place.
  const auto outgoing_path = versions_path / kOutgoingName;
  if (std::filesystem::exists(outgoing_path, ec) && !HasSystemFiles(system_path)) {
    std::filesystem::rename(outgoing_path, system_path, ec);
    if (ec) {
      XELOGE("iOS: putting the system folder back failed: {}", ec.message());
    } else {
      XELOGW("iOS: put back the system folder a change of version left set aside");
    }
  }
  // Otherwise the exchange went through, and what was set aside is one of the
  // versions.
  for (const char* name : {kOutgoingName, kReplacedName}) {
    const auto aside_path = versions_path / name;
    if (std::filesystem::exists(aside_path, ec)) {
      SettleFormerSystemFolder(aside_path, system_path);
    }
  }
  // A folder an exchange left under the name of the version it was exchanged
  // for.
  std::vector<std::filesystem::path> folders;
  for (std::filesystem::directory_iterator it(versions_path, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code type_ec;
    if (it->is_directory(type_ec) && !IsHiddenName(it->path())) {
      folders.push_back(it->path());
    }
  }
  for (const auto& folder : folders) {
    const auto version = ReadDashboardVersion(folder).version;
    if (version.empty() || IsNameForVersion(folder.filename().string(), version)) {
      continue;
    }
    std::filesystem::rename(folder, FreeVersionPath(version), ec);
  }
  RemoveVersionsFolderIfEmpty();
}

void EnsureSettled() {
  static std::once_flag settled;
  std::call_once(settled, []() {
    std::lock_guard<std::mutex> lock(versions_mutex);
    SettleInterruptedChanges();
  });
}

std::vector<IOSSystemVersion> ListVersionsLocked() {
  std::vector<IOSSystemVersion> versions;
  const auto add = [&versions](const std::filesystem::path& path, bool is_default) {
    const auto dashboard = ReadDashboardVersion(path);
    IOSSystemVersion version;
    version.path = path;
    version.version = dashboard.version;
    version.build = dashboard.build;
    version.is_default = is_default;
    version.has_dashboard = dashboard.has_dashboard;
    versions.push_back(std::move(version));
  };
  if (IsIOSDashboardInstalled()) {
    add(IOSSystemFilesPath(), true);
  }
  const auto first_other = versions.size();
  std::error_code ec;
  for (std::filesystem::directory_iterator it(IOSSystemVersionsPath(), ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code type_ec;
    if (it->is_directory(type_ec) && !IsHiddenName(it->path())) {
      add(it->path(), false);
    }
  }
  std::sort(versions.begin() + first_other, versions.end(),
            [](const IOSSystemVersion& a, const IOSSystemVersion& b) {
              return a.build != b.build ? a.build > b.build : a.path.filename() < b.path.filename();
            });
  return versions;
}

}  // namespace

std::filesystem::path IOSSystemVersionsPath() {
  return IOSSystemFilesPath().parent_path() / "system-versions";
}

std::vector<IOSSystemVersion> ListIOSSystemVersions() {
  EnsureSettled();
  std::lock_guard<std::mutex> lock(versions_mutex);
  return ListVersionsLocked();
}

std::optional<IOSSystemVersion> FindIOSSystemVersionForDashboard(
    const std::filesystem::path& path) {
  if (path.empty()) {
    return std::nullopt;
  }
  for (auto& version : ListIOSSystemVersions()) {
    std::error_code ec;
    if (version.has_dashboard && std::filesystem::equivalent(path, version.dashboard_path(), ec)) {
      return std::move(version);
    }
  }
  return std::nullopt;
}

std::optional<IOSSystemVersion> FindIOSSystemVersionForFile(const std::filesystem::path& path) {
  if (path.empty()) {
    return std::nullopt;
  }
  const auto versions = ListIOSSystemVersions();
  for (auto folder = path.parent_path(); !folder.empty() && folder != folder.parent_path();
       folder = folder.parent_path()) {
    for (const auto& version : versions) {
      std::error_code ec;
      if (std::filesystem::equivalent(folder, version.path, ec)) {
        return version;
      }
    }
  }
  return std::nullopt;
}

uint64_t MeasureIOSSystemVersion(const std::filesystem::path& version_path) {
  uint64_t size = 0;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           version_path, std::filesystem::directory_options::skip_permission_denied, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code size_ec;
    if (it->is_regular_file(size_ec)) {
      const auto file_size = it->file_size(size_ec);
      if (!size_ec) {
        size += file_size;
      }
    }
  }
  return size;
}

bool UseIOSSystemVersionForGames(const std::filesystem::path& version_path) {
  EnsureSettled();
  std::lock_guard<std::mutex> lock(versions_mutex);
  const auto system_path = IOSSystemFilesPath();
  if (!IsVersionFolder(version_path) || !HasSystemFiles(version_path)) {
    XELOGE("iOS: no system software version to use for games in the folder given");
    return false;
  }
  std::error_code ec;
  // The copy a failed install left in the system folder is dealt with first,
  // as before an install.
  if (std::filesystem::exists(GetUnrestoredSystemFilesPath(system_path), ec)) {
    return false;
  }
  // With no version for games yet, it's exchanged for an empty folder.
  std::filesystem::create_directories(system_path, ec);
  if (ec) {
    return false;
  }
  const auto former_path =
      ExchangeFolders(version_path, system_path, IOSSystemVersionsPath() / kOutgoingName);
  if (!former_path) {
    return false;
  }
  SettleFormerSystemFolder(*former_path, system_path);
  RemoveVersionsFolderIfEmpty();
  XELOGI("iOS: games use another system software version now");
  return true;
}

bool RemoveIOSSystemVersion(const std::filesystem::path& version_path) {
  EnsureSettled();
  std::error_code ec;
  if (std::filesystem::equivalent(version_path, IOSSystemFilesPath(), ec)) {
    return RemoveIOSSystemSoftware();
  }
  const auto versions_path = IOSSystemVersionsPath();
  const auto removing_path = versions_path / kRemovingName;
  {
    std::lock_guard<std::mutex> lock(versions_mutex);
    if (!IsVersionFolder(version_path)) {
      return false;
    }
    // Out of the list in one step, so a version is never left half there.
    std::filesystem::remove_all(removing_path, ec);
    std::filesystem::rename(version_path, removing_path, ec);
    if (ec) {
      XELOGE("iOS: removing a system software version failed: {}", ec.message());
      return false;
    }
  }
  std::filesystem::remove_all(removing_path, ec);
  if (ec) {
    // The next launch removes the rest.
    XELOGW("iOS: a removed system software version isn't deleted yet: {}", ec.message());
  }
  RemoveVersionsFolderIfEmpty();
  return true;
}

SystemFilesResult StageIOSSystemSoftware(
    const std::filesystem::path& source_path,
    const std::function<bool(size_t done, size_t total)>& progress,
    IOSStagedSystemSoftware* staged) {
  EnsureSettled();
  *staged = {};
  const auto system_path = IOSSystemFilesPath();
  if (!IsIOSDashboardInstalled()) {
    // No version for games yet: this is it, installed where the only version
    // always was.
    return InstallSystemFiles(source_path, system_path, progress);
  }
  std::error_code ec;
  // A previous install that couldn't be put back stays until it is dealt with.
  if (std::filesystem::exists(GetUnrestoredSystemFilesPath(system_path), ec)) {
    return SystemFilesResult::kRestoreFailed;
  }
  const auto incoming_path = IOSSystemVersionsPath() / kIncomingName;
  std::filesystem::remove_all(incoming_path, ec);
  if (ec) {
    return SystemFilesResult::kWriteFailed;
  }
  const auto result = InstallSystemFiles(source_path, incoming_path, progress);
  if (result != SystemFilesResult::kSuccess) {
    DiscardIOSStagedSystemSoftware();
    return result;
  }
  staged->staged = true;
  staged->version = ReadDashboardVersion(incoming_path).version;
  if (!staged->version.empty()) {
    for (const auto& version : ListIOSSystemVersions()) {
      staged->already_installed |= version.version == staged->version;
    }
  }
  return SystemFilesResult::kSuccess;
}

SystemFilesResult CommitIOSStagedSystemSoftware() {
  EnsureSettled();
  const auto versions_path = IOSSystemVersionsPath();
  const auto incoming_path = versions_path / kIncomingName;
  std::error_code ec;
  // The version the update took the place of, to delete.
  std::filesystem::path replaced_path;
  {
    std::lock_guard<std::mutex> lock(versions_mutex);
    if (!HasSystemFiles(incoming_path)) {
      return SystemFilesResult::kWriteFailed;
    }
    const auto version = ReadDashboardVersion(incoming_path).version;
    // The installed version it is, the one games use before any other.
    std::optional<IOSSystemVersion> installed;
    if (!version.empty()) {
      for (auto& candidate : ListVersionsLocked()) {
        if (candidate.version == version) {
          installed = std::move(candidate);
          break;
        }
      }
    }
    if (!installed) {
      // One more version.
      std::filesystem::rename(incoming_path, FreeVersionPath(version), ec);
      if (ec) {
        XELOGE("iOS: adding the system software version failed: {}", ec.message());
        std::filesystem::remove_all(incoming_path, ec);
        RemoveVersionsFolderIfEmpty();
        return SystemFilesResult::kWriteFailed;
      }
      return SystemFilesResult::kSuccess;
    }
    const auto former_path =
        ExchangeFolders(incoming_path, installed->path,
                        versions_path / (installed->is_default ? kOutgoingName : kReplacedName));
    if (!former_path) {
      // Nothing changed: the installed version stays.
      std::filesystem::remove_all(incoming_path, ec);
      RemoveVersionsFolderIfEmpty();
      return SystemFilesResult::kWriteFailed;
    }
    replaced_path = *former_path;
    if (installed->is_default) {
      KeepDownload(replaced_path, installed->path);
    }
  }
  // Under one of the names the next launch clears, should this not finish.
  std::filesystem::remove_all(replaced_path, ec);
  RemoveVersionsFolderIfEmpty();
  return SystemFilesResult::kSuccess;
}

void DiscardIOSStagedSystemSoftware() {
  std::error_code ec;
  std::filesystem::remove_all(IOSSystemVersionsPath() / kIncomingName, ec);
  RemoveVersionsFolderIfEmpty();
}

}  // namespace ui
}  // namespace xe
