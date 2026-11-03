/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_dashboard.h"

#import <UIKit/UIKit.h>

#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/title_id_utils.h"
#import "xenia/ui/ios/app/ios_landscape_navigation_controller.h"
#import "xenia/ui/ios/launcher/ios_dashboard_disc_tray_view_controller.h"
#include "xenia/ui/ios/launcher/ios_game_library_store.h"
#import "xenia/ui/ios/shared/ios_alert_actions.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"
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
  std::string title;
  // Which disc, its format and the linked folder it is in.
  std::string detail;
  std::filesystem::path path;
  std::filesystem::path icon_path;
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

// The disc's format, as the library names it.
NSString* DiscFormat(const std::filesystem::path& path) {
  if (IsZarPath(path)) {
    return @"ZAR";
  }
  if (IsISOPath(path)) {
    return @"ISO";
  }
  if (IsLikelyGodContainerFile(path)) {
    return @"GOD";
  }
  if (IsDefaultXexPath(path)) {
    return NSLocalizedString(@"Folder", @"dashboard disc tray: a game kept as a folder of files");
  }
  return nil;
}

// Like "Disc 1 of 2 · ZAR · Backup".
std::string DiscDetail(const IOSDiscoveredGame& game, const IOSDiscoveredGame::Disc& disc) {
  NSMutableArray<NSString*>* parts = [NSMutableArray arrayWithCapacity:3];
  if (disc.disc_count > 1 && disc.disc_number > 0) {
    [parts
        addObject:[NSString
                      localizedStringWithFormat:NSLocalizedString(
                                                    @"Disc %u of %u",
                                                    @"dashboard disc tray: which disc of a game"),
                                                unsigned(disc.disc_number),
                                                unsigned(disc.disc_count)]];
  }
  if (NSString* format = DiscFormat(disc.path)) {
    [parts addObject:format];
  }
  if (disc.is_external && !game.external_location_name.empty()) {
    [parts addObject:[NSString stringWithUTF8String:game.external_location_name.c_str()]];
  }
  return [[parts componentsJoinedByString:@" · "] UTF8String];
}

// The app's root view controller, which the tray is presented over.
UIViewController* AppRootViewController() {
  for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
    if ([scene isKindOfClass:[UIWindowScene class]]) {
      if (UIViewController* root = ((UIWindowScene*)scene).keyWindow.rootViewController) {
        return root;
      }
    }
  }
  return nil;
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
      // Every copy of every disc can go in the tray.
      EnsureDiscoveredGameDiscList(&game);
      for (const auto& disc : game.discs) {
        discs.push_back({game.title, DiscDetail(game, disc), disc.path, title.icon_path});
      }
      titles.push_back(std::move(title));
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
  // The answer comes from a sheet on the main thread, so it can't be waited
  // for there.
  if ([NSThread isMainThread]) {
    XELOGW("iOS: the dashboard's disc tray can't wait for a disc on the main thread");
    return {};
  }
  std::vector<LibraryDisc> library;
  {
    std::lock_guard<std::mutex> lock(library_titles_mutex);
    library = library_discs;
  }
  __block NSString* picked = nil;
  dispatch_semaphore_t answered = dispatch_semaphore_create(0);
  @autoreleasepool {
    NSMutableArray<XeniaDashboardDisc*>* discs = [NSMutableArray arrayWithCapacity:library.size()];
    for (const auto& entry : library) {
      XeniaDashboardDisc* disc = [[[XeniaDashboardDisc alloc] init] autorelease];
      disc.title = [NSString stringWithUTF8String:entry.title.c_str()];
      disc.detail = [NSString stringWithUTF8String:entry.detail.c_str()];
      disc.path = [NSString stringWithUTF8String:entry.path.c_str()];
      if (!entry.icon_path.empty()) {
        disc.iconPath = [NSString stringWithUTF8String:entry.icon_path.c_str()];
      }
      [discs addObject:disc];
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      void (^answer)(NSString*) = ^(NSString* path) {
        picked = [path copy];
        dispatch_semaphore_signal(answered);
      };
      UIViewController* root = AppRootViewController();
      if (!root) {
        XELOGW("iOS: no window to show the dashboard's disc tray in");
        answer(nil);
        return;
      }
      XeniaDashboardDiscTrayViewController* tray =
          [[[XeniaDashboardDiscTrayViewController alloc] initWithDiscs:discs
                                                            completion:answer] autorelease];
      XeniaLandscapeNavigationController* sheet = [[[XeniaLandscapeNavigationController alloc]
          initWithRootViewController:tray] autorelease];
      XEConfigureTaskSheet(sheet, root.view, CGSizeMake(540.0, 620.0), NO);
      XeniaPresentGuestPrompt(root, sheet, ^{
        [tray answerEmpty];
      });
    });
  }
  dispatch_semaphore_wait(answered, DISPATCH_TIME_FOREVER);
  dispatch_release(answered);
  std::filesystem::path path;
  if (picked) {
    path = picked.fileSystemRepresentation;
    [picked release];
  }
  return path;
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
