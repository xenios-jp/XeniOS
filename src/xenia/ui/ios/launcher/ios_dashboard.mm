/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_dashboard.h"

#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/xam_ui_ios.h"
#include "xenia/ui/ios/launcher/ios_game_library_store.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"
#include "xenia/vfs/xex_metadata.h"

DECLARE_path(storage_root);

NSNotificationName const XeniaIOSSystemSoftwareDidChangeNotification =
    @"XeniaIOSSystemSoftwareDidChangeNotification";
NSNotificationName const XeniaIOSTitleWillStartNotification = @"XeniaIOSTitleWillStartNotification";
NSString* const XeniaIOSTitlePathKey = @"path";
NSString* const XeniaIOSSystemUpdateSupportURL =
    @"https://support.xbox.com/en-US/help/xbox-360/console/system-updates-info";

namespace xe {
namespace ui {

namespace {

struct LibraryDisc {
  std::string label;
  std::filesystem::path path;
};

std::mutex library_titles_mutex;
std::vector<Emulator::LibraryTitle> library_titles;
std::vector<LibraryDisc> library_discs;

dispatch_queue_t LibraryQueue() {
  static dispatch_queue_t queue =
      dispatch_queue_create("xenia.dashboard-library", DISPATCH_QUEUE_SERIAL);
  return queue;
}

std::filesystem::path LibraryIconsPath() {
  NSString* caches =
      NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
  if (!caches) {
    return {};
  }
  return std::filesystem::path([caches fileSystemRepresentation]) / "xenia" / "library-icons";
}

// Writes |data| unless the file already holds as many bytes, which an icon
// that changed is unlikely to.
bool WriteIcon(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (!ec && size == data.size()) {
    return true;
  }
  return xe::filesystem::WriteAllBytes(path, data);
}

}  // namespace

std::filesystem::path IOSSystemFilesPath() {
  // The emulator's storage root, as the app sets it up.
  std::filesystem::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = xe_get_ios_documents_path();
  }
  return std::filesystem::absolute(storage_root) / "system";
}

std::filesystem::path IOSInstalledDashboardPath() {
  return IOSSystemFilesPath() / "flash" / "dash.xex";
}

bool IsIOSDashboardInstalled() {
  std::error_code ec;
  return std::filesystem::is_regular_file(IOSInstalledDashboardPath(), ec);
}

bool IsIOSInstalledDashboardPath(const std::filesystem::path& path) {
  std::error_code ec;
  return !path.empty() && std::filesystem::equivalent(path, IOSInstalledDashboardPath(), ec);
}

IOSSystemSoftwareInfo ReadIOSSystemSoftwareInfo() {
  IOSSystemSoftwareInfo info;
  const auto system_path = IOSSystemFilesPath();
  std::error_code ec;
  info.has_unrestored_files =
      std::filesystem::exists(GetUnrestoredSystemFilesPath(system_path), ec);
  info.installed = IsIOSDashboardInstalled();
  if (!info.installed) {
    return info;
  }
  if (auto metadata = vfs::ExtractXexMetadata(IOSInstalledDashboardPath())) {
    info.version = metadata->version.ToString();
  }
  for (auto it = std::filesystem::recursive_directory_iterator(
           system_path, std::filesystem::directory_options::skip_permission_denied, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code size_ec;
    if (it->is_regular_file(size_ec)) {
      const auto size = it->file_size(size_ec);
      if (!size_ec) {
        info.size += size;
      }
    }
  }
  return info;
}

SystemFilesResult InstallIOSSystemSoftware(
    const std::filesystem::path& source_path,
    const std::function<bool(size_t done, size_t total)>& progress) {
  const auto result = InstallSystemFiles(source_path, IOSSystemFilesPath(), progress);
  XELOGI("iOS: system software install finished with result {}", static_cast<int>(result));
  return result;
}

bool RemoveIOSSystemSoftware() {
  std::error_code ec;
  std::filesystem::remove_all(IOSSystemFilesPath(), ec);
  if (ec) {
    XELOGE("iOS: removing the system software failed: {}", ec.message());
    return false;
  }
  return true;
}

void PublishIOSLibraryToDashboard(std::vector<IOSDiscoveredGame> games) {
  auto* owned = new std::vector<IOSDiscoveredGame>(std::move(games));
  dispatch_async(LibraryQueue(), ^{
    std::unique_ptr<std::vector<IOSDiscoveredGame>> games_owner(owned);
    const auto icons_path = LibraryIconsPath();
    std::error_code ec;
    if (!icons_path.empty()) {
      std::filesystem::create_directories(icons_path, ec);
    }
    std::vector<Emulator::LibraryTitle> titles;
    titles.reserve(games_owner->size());
    std::vector<LibraryDisc> discs;
    for (auto& game : *games_owner) {
      // The dashboard doesn't list itself, nor anything without a title.
      if (!game.title_id || kernel::IsSystemTitle(game.title_id) || game.title.empty()) {
        continue;
      }
      Emulator::LibraryTitle title;
      title.title_id = game.title_id;
      // Releases of a title are told apart by the version, which the iOS
      // library doesn't read. Its media id tells them apart as well.
      title.version = game.media_id;
      title.name = game.title;
      title.path = game.path;
      title.content_type = game.content_type;
      if (!game.icon_data.empty() && !icons_path.empty()) {
        auto icon_path =
            icons_path / fmt::format("{:08X}-{:08X}.png", game.title_id, game.media_id);
        if (WriteIcon(icon_path, game.icon_data)) {
          title.icon_path = std::move(icon_path);
        }
      }
      titles.push_back(std::move(title));
      // Every copy of every disc can go in the tray.
      EnsureDiscoveredGameDiscList(&game);
      for (const auto& disc : game.discs) {
        discs.push_back(
            {game.discs.size() > 1 ? fmt::format("{} ({})", game.title, disc.label) : game.title,
             disc.path});
      }
    }
    std::lock_guard<std::mutex> lock(library_titles_mutex);
    library_titles = std::move(titles);
    library_discs = std::move(discs);
  });
}

std::vector<Emulator::LibraryTitle> IOSDashboardLibraryTitles() {
  std::lock_guard<std::mutex> lock(library_titles_mutex);
  return library_titles;
}

std::filesystem::path PickIOSDashboardDisc() {
  std::vector<LibraryDisc> discs;
  {
    std::lock_guard<std::mutex> lock(library_titles_mutex);
    discs = library_discs;
  }
  std::vector<std::string> options;
  options.reserve(discs.size() + 1);
  for (const auto& disc : discs) {
    options.push_back(disc.label);
  }
  std::string title, text;
  @autoreleasepool {
    title = NSLocalizedString(@"Insert a Disc", @"dashboard disc tray: prompt title").UTF8String;
    text = (discs.empty()
                ? NSLocalizedString(@"There are no games in the library to put in the drive.",
                                    @"dashboard disc tray: prompt text, empty library")
                : NSLocalizedString(@"Choose a game from the library to put in the drive.",
                                    @"dashboard disc tray: prompt text"))
               .UTF8String;
    options.push_back(
        NSLocalizedString(@"Leave Empty", @"dashboard disc tray: option for no disc").UTF8String);
  }
  // ChooseOption gives -1 on the main thread, as for a cancel.
  const int choice = kernel::xam::ios::ChooseOption(title, text, options);
  if (choice < 0 || static_cast<size_t>(choice) >= discs.size()) {
    return {};
  }
  return discs[choice].path;
}

NSArray<XeniaIOSExternalLibraryAccess*>* StartIOSDashboardLibraryAccess() {
  NSMutableArray<XeniaIOSExternalLibraryAccess*>* accesses = [NSMutableArray array];
  for (const auto& location : ListIOSExternalLibraryLocations()) {
    if (!location.available) {
      continue;
    }
    if (XeniaIOSExternalLibraryAccess* access =
            StartIOSExternalLibraryAccessForPath(location.path, nullptr, nil)) {
      [accesses addObject:access];
    }
  }
  return accesses;
}

}  // namespace ui
}  // namespace xe
