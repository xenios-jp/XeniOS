/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_dashboard.h"

#import <ImageIO/ImageIO.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/title_id_utils.h"
#import "xenia/ui/ios/app/ios_landscape_navigation_controller.h"
#import "xenia/ui/ios/launcher/ios_dashboard_disc_tray_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#include "xenia/ui/ios/launcher/ios_game_library_store.h"
#import "xenia/ui/ios/shared/ios_alert_actions.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/xex_metadata.h"

DECLARE_path(storage_root);

// Edge's setting (emulator_window.cc), for the iOS app.
DEFINE_bool(boot_to_dashboard, false,
            "Start the installed dashboard after the boot animation instead of "
            "showing the game list, and go back to the dashboard when a title "
            "exits.",
            "General");

NSNotificationName const XeniaIOSSystemSoftwareDidChangeNotification =
    @"XeniaIOSSystemSoftwareDidChangeNotification";
NSNotificationName const XeniaIOSTitleWillStartNotification = @"XeniaIOSTitleWillStartNotification";
NSString* const XeniaIOSTitlePathKey = @"path";

namespace xe {
namespace ui {

namespace {

struct LibraryDisc {
  uint32_t title_id = 0;
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

// A release's thumbnail keeps its path, so the dashboard reads the cover that
// replaces its icon from where it read the icon.
std::filesystem::path LibraryIconPath(const std::filesystem::path& icons_path, uint32_t title_id,
                                      uint32_t media_id) {
  return icons_path / fmt::format("{:08X}-{:08X}.png", title_id, media_id);
}

// The dashboard draws a game's thumbnail 64 pixels square.
constexpr size_t kThumbnailPixels = 64;

// Covers made into thumbnails, by title id. Only on LibraryQueue.
std::unordered_map<uint32_t, std::vector<uint8_t>>& CoverThumbnails() {
  static auto* thumbnails = new std::unordered_map<uint32_t, std::vector<uint8_t>>();
  return *thumbnails;
}

// The cover the library fetched for the title, whole, in the dashboard's
// square thumbnail, as a PNG that fits a content header. Nothing if the
// library has no cover for it yet.
std::vector<uint8_t> CoverThumbnail(uint32_t title_id) {
  auto& thumbnails = CoverThumbnails();
  if (auto it = thumbnails.find(title_id); it != thumbnails.end()) {
    return it->second;
  }
  std::vector<uint8_t> thumbnail;
  @autoreleasepool {
    CGImageRef cover = xe_cached_game_art(title_id).CGImage;
    if (!cover) {
      return {};
    }
    CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(nullptr, kThumbnailPixels, kThumbnailPixels, 8, 0,
                                                 color_space, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(color_space);
    if (!context) {
      return {};
    }
    // Box art is taller than it is wide. It keeps its shape, with clear sides.
    const CGFloat width = CGImageGetWidth(cover);
    const CGFloat height = CGImageGetHeight(cover);
    const CGFloat scale = CGFloat(kThumbnailPixels) / std::max(width, height);
    const CGFloat fitted_width = std::round(width * scale);
    const CGFloat fitted_height = std::round(height * scale);
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(
        context,
        CGRectMake((kThumbnailPixels - fitted_width) / 2, (kThumbnailPixels - fitted_height) / 2,
                   fitted_width, fitted_height),
        cover);
    CGImageRef fitted = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    NSMutableData* png = [NSMutableData data];
    CGImageDestinationRef destination = CGImageDestinationCreateWithData(
        (CFMutableDataRef)png, (CFStringRef)UTTypePNG.identifier, 1, nullptr);
    if (fitted && destination) {
      CGImageDestinationAddImage(destination, fitted, nullptr);
      if (CGImageDestinationFinalize(destination) &&
          png.length <= vfs::XContentMetadata::kThumbLengthV2) {
        const auto* bytes = static_cast<const uint8_t*>(png.bytes);
        thumbnail.assign(bytes, bytes + png.length);
      } else {
        XELOGW("iOS: the dashboard thumbnail for {:08X} is too large ({} bytes)", title_id,
               png.length);
      }
    }
    if (destination) {
      CFRelease(destination);
    }
    CGImageRelease(fitted);
  }
  if (!thumbnail.empty()) {
    thumbnails.emplace(title_id, thumbnail);
  }
  return thumbnail;
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

// Writes |data| unless the file already holds it, all at once, as the
// dashboard may be reading it.
bool WriteIcon(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  if (xe::filesystem::ReadAllBytes(path) == data) {
    return true;
  }
  NSData* bytes = [NSData dataWithBytes:data.data() length:data.size()];
  return [bytes writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES];
}

// Gives the dashboard a cover that arrived after the library was handed to it.
// Only on LibraryQueue.
void UseFetchedCover(uint32_t title_id) {
  const auto thumbnail = CoverThumbnail(title_id);
  const auto icons_path = LibraryIconsPath();
  if (thumbnail.empty() || icons_path.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(library_titles_mutex);
  for (auto& title : library_titles) {
    if (title.title_id != title_id) {
      continue;
    }
    auto icon_path = LibraryIconPath(icons_path, title.title_id, title.version);
    if (!WriteIcon(icon_path, thumbnail)) {
      continue;
    }
    for (auto& disc : library_discs) {
      if (disc.title_id == title_id) {
        disc.icon_path = icon_path;
      }
    }
    title.icon_path = std::move(icon_path);
  }
}

// Fetches the covers the library hasn't yet, as it does for the games it
// shows, and gives the dashboard each one that arrives.
void FetchCovers(std::vector<uint32_t> title_ids) {
  std::sort(title_ids.begin(), title_ids.end());
  title_ids.erase(std::unique(title_ids.begin(), title_ids.end()), title_ids.end());
  dispatch_async(dispatch_get_main_queue(), ^{
    for (const uint32_t title_id : title_ids) {
      xe_fetch_game_art(title_id, ^(UIImage* cover) {
        if (cover) {
          dispatch_async(LibraryQueue(), ^{
            UseFetchedCover(title_id);
          });
        }
      });
    }
  });
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

bool IsIOSDashboardHome() { return cvars::boot_to_dashboard && IsIOSDashboardInstalled(); }

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

NSString* IOSSystemSoftwareInstallFailureMessage(SystemFilesResult result) {
  switch (result) {
    case SystemFilesResult::kSuccess:
    case SystemFilesResult::kCancelled:
      return nil;
    case SystemFilesResult::kNotSystemUpdate:
      return NSLocalizedString(
          @"This isn't a USB system update. Pick the system update zip from Microsoft, or the "
          @"folder Files expands it to.",
          @"system software view controller: install error");
    case SystemFilesResult::kReadFailed:
      return NSLocalizedString(@"The system update couldn't be read.",
                               @"system software view controller: install error");
    case SystemFilesResult::kWriteFailed:
      return NSLocalizedString(
          @"The system software couldn't be written. Check that there is enough free space.",
          @"system software view controller: install error");
    case SystemFilesResult::kRestoreFailed:
      return NSLocalizedString(
          @"The previous system software couldn't be put back. Its folders are in the "
          @"\"unrestored\" folder in XeniOS's system folder in Files. Move them back or delete "
          @"them to install again.",
          @"system software view controller: install error");
  }
  return nil;
}

void InstallIOSSystemSoftwareAsync(NSURL* source, std::shared_ptr<std::atomic<bool>> cancel,
                                   void (^progress)(int percent),
                                   void (^completion)(SystemFilesResult result)) {
  const std::filesystem::path source_path([source fileSystemRepresentation]);
  [source retain];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    // The picker hands over a copy in tmp, or the original (a folder, say) with
    // a scope that has to be open to read it. Harmless for a copy.
    const BOOL access_granted = [source startAccessingSecurityScopedResource];
    int last_percent = 0;
    const auto result = InstallIOSSystemSoftware(
        source_path, [&last_percent, &cancel, progress](size_t done, size_t total) {
          const int percent = total ? static_cast<int>(done * 100 / total) : 0;
          if (percent != last_percent) {
            last_percent = percent;
            dispatch_async(dispatch_get_main_queue(), ^{
              progress(percent);
            });
          }
          return !cancel->load();
        });
    if (access_granted) {
      [source stopAccessingSecurityScopedResource];
    }
    [source release];
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  });
}

NSString* IOSSystemUpdateDownloadFailureMessage(SystemUpdateDownload result) {
  switch (result) {
    case SystemUpdateDownload::kDownloaded:
    case SystemUpdateDownload::kCancelled:
      return nil;
    case SystemUpdateDownload::kFailed:
      return NSLocalizedString(@"Download failed. Check your internet connection and try again.",
                               @"system software view controller: download error");
    case SystemUpdateDownload::kNotVerified:
      return NSLocalizedString(
          @"The downloaded file failed integrity verification and was discarded.",
          @"system software view controller: download error");
    case SystemUpdateDownload::kWriteFailed:
      return NSLocalizedString(@"Failed to write the system files.",
                               @"system software view controller: download error");
  }
  return nil;
}

void DownloadAndInstallIOSSystemUpdateAsync(std::shared_ptr<std::atomic<bool>> cancel,
                                            void (^progress)(int percent, BOOL installing),
                                            void (^completion)(SystemUpdateDownload download,
                                                               SystemFilesResult install)) {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const auto system_path = IOSSystemFilesPath();
    int last_percent = -1;
    const auto report = [&last_percent, progress](int percent, BOOL installing) {
      if (percent != last_percent) {
        last_percent = percent;
        dispatch_async(dispatch_get_main_queue(), ^{
          progress(percent, installing);
        });
      }
    };
    const auto download =
        DownloadSystemUpdate(system_path, [&report, &cancel](uint64_t received, uint64_t size) {
          report(size ? static_cast<int>(received * 100 / size) : 0, NO);
          return !cancel->load();
        });
    auto install = SystemFilesResult::kCancelled;
    if (download == SystemUpdateDownload::kDownloaded) {
      last_percent = -1;
      const auto zip_path = GetSystemUpdateDownloadPath(system_path);
      install = InstallIOSSystemSoftware(zip_path, [&report, &cancel](size_t done, size_t total) {
        report(total ? static_cast<int>(done * 100 / total) : 0, YES);
        return !cancel->load();
      });
      // Kept until it's installed, so a failed install doesn't download it
      // again.
      if (install == SystemFilesResult::kSuccess) {
        std::error_code ec;
        std::filesystem::remove(zip_path, ec);
      }
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(download, install);
    });
  });
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
    std::vector<uint32_t> missing_covers;
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
      // The cover the library fetched, as its games show, else the game's own
      // icon.
      std::vector<uint8_t> cover;
      if (game.system == IOSGameSystem::kXbox360) {
        cover = CoverThumbnail(game.title_id);
        if (cover.empty()) {
          missing_covers.push_back(game.title_id);
        }
      }
      const auto& icon = cover.empty() ? game.icon_data : cover;
      if (!icons_path.empty()) {
        auto icon_path = LibraryIconPath(icons_path, game.title_id, game.media_id);
        // Also for a game with no icon or cover yet: the emulator takes its copy of the
        // library when a title starts, and a cover fetched later (UseFetchedCover) reaches
        // the dashboard only through the path that copy already holds.
        if (icon.empty() || WriteIcon(icon_path, icon)) {
          title.icon_path = std::move(icon_path);
        }
      }
      // Every copy of every disc can go in the tray.
      EnsureDiscoveredGameDiscList(&game);
      for (const auto& disc : game.discs) {
        discs.push_back(
            {game.title_id, game.title, DiscDetail(game, disc), disc.path, title.icon_path});
      }
      titles.push_back(std::move(title));
    }
    {
      std::lock_guard<std::mutex> lock(library_titles_mutex);
      library_titles = std::move(titles);
      library_discs = std::move(discs);
    }
    if (!missing_covers.empty()) {
      FetchCovers(std::move(missing_covers));
    }
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
      disc.titleId = entry.title_id;
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
  return StartIOSExternalLibraryAccessForAllLocations();
}

}  // namespace ui
}  // namespace xe
