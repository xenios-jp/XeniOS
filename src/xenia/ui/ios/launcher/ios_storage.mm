/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_storage.h"

#import <Foundation/Foundation.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <unordered_map>

#include "xenia/app/game_title_db.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/ui/ios/launcher/ios_game_library_store.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"

DECLARE_path(cache_root);

NSNotificationName const XeniaStorageDidChangeNotification = @"XeniaStorageDidChangeNotification";
NSNotificationName const XeniaStorageDeleteGameRequestNotification =
    @"XeniaStorageDeleteGameRequestNotification";
NSString* const XeniaStorageTitleIdKey = @"titleId";
NSString* const XeniaStorageGamePathKey = @"gamePath";

namespace {

namespace fs = std::filesystem;

// Temporary files and archiver copies older than this are left over.
constexpr double kLeftoverAgeSeconds = 24.0 * 3600.0;
// At launch, archiver copies this new may belong to a game that has started.
constexpr double kLaunchArchiverAgeSeconds = 10.0 * 60.0;
// A CPU instruction cache no game has written to for this long is removed.
constexpr double kModuleUnusedSeconds = 60.0 * 24.0 * 3600.0;

constexpr char kSharedOwner[] = "0000000000000000";
constexpr uint32_t kProfileTitleId = 0xFFFE07D1;
constexpr char kSavedGameType[] = "00000001";
// The emulated cache partitions (emulator.cc mounts them in Documents).
constexpr const char* kGameDataDrives[] = {"cache", "cache0", "cache1"};

// Every path here comes from Foundation, so they share one spelling of the
// container (/var or /private/var) and compare as prefixes of each other.
fs::path HomePath() { return NSHomeDirectory().UTF8String; }

// The library saves canonical paths (/private/var/...) where Foundation says
// /var/...: one spelling, so a path can be compared with the walk's.
fs::path Spelled(const fs::path& path) {
  const std::string text = path.lexically_normal().string();
  constexpr std::string_view kPrivate = "/private";
  return text.rfind("/private/var/", 0) == 0 ? fs::path(text.substr(kPrivate.size()))
                                             : fs::path(text);
}

fs::path DocumentsPath() { return xe_get_ios_documents_path(); }

fs::path CachesPath() {
  NSArray* paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
  return paths.count ? fs::path([paths[0] UTF8String]) : HomePath() / "Library" / "Caches";
}

std::string BundleId() {
  NSString* bundle = NSBundle.mainBundle.bundleIdentifier;
  return bundle ? bundle.UTF8String : "";
}

uint64_t EntryBytes(const fs::path& path, struct stat* st_out = nullptr) {
  struct stat st;
  if (::lstat(path.c_str(), &st)) {
    return 0;
  }
  if (st_out) {
    *st_out = st;
  }
  return uint64_t(st.st_blocks) * 512;
}

uint64_t TreeBytes(const fs::path& path) {
  struct stat st;
  uint64_t total = EntryBytes(path, &st);
  if (!S_ISDIR(st.st_mode)) {
    return total;
  }
  std::error_code ec;
  for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    total += EntryBytes(it->path());
  }
  return total;
}

// The newest change in a tree, in seconds since the epoch.
double NewestChange(const fs::path& path) {
  struct stat st;
  if (::lstat(path.c_str(), &st)) {
    return 0;
  }
  double newest = double(st.st_mtimespec.tv_sec);
  if (S_ISDIR(st.st_mode)) {
    std::error_code ec;
    for (fs::recursive_directory_iterator
             it(path, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
      if (!::lstat(it->path().c_str(), &st)) {
        newest = std::max(newest, double(st.st_mtimespec.tv_sec));
      }
    }
  }
  return newest;
}

bool OlderThan(const fs::path& path, double seconds) {
  return NewestChange(path) < double(std::time(nullptr)) - seconds;
}

// A title ID at the start of a cache name: "4D5307E6", "4D5307E6.xsh",
// "4d5307e6.jpg", "4D5307E6-0DD4CC6A.png".
bool TitleIdOfName(std::string_view name, uint32_t* title_id_out) {
  if (name.size() < 8 || (name.size() > 8 && name[8] != '.' && name[8] != '-')) {
    return false;
  }
  uint32_t value = 0;
  for (char c : name.substr(0, 8)) {
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else {
      return false;
    }
    value = value << 4 | uint32_t(digit);
  }
  *title_id_out = value;
  return true;
}

std::vector<fs::path> ArchiverStagingDirs() {
  std::vector<fs::path> dirs;
  const std::string bundle = BundleId();
  if (!bundle.empty()) {
    dirs.push_back(CachesPath() / bundle / "com.apple.gpuarchiver");
  }
  dirs.push_back(CachesPath() / "com.apple.gpuarchiver");
  return dirs;
}

// Removes the entries of `dir` older than `age_seconds`; returns the bytes.
uint64_t RemoveOldEntries(const fs::path& dir, double age_seconds) {
  uint64_t freed = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!OlderThan(entry.path(), age_seconds)) {
      continue;
    }
    const uint64_t bytes = TreeBytes(entry.path());
    std::error_code remove_ec;
    fs::remove_all(entry.path(), remove_ec);
    if (!remove_ec) {
      freed += bytes;
    }
  }
  return freed;
}

// Where the Files app keeps what was deleted from the app's folder there, until
// Recently Deleted is emptied. It still takes the app's space.
fs::path FilesTrashPath() { return DocumentsPath() / ".Trash"; }

// `files_trash` also empties the Files app's Recently Deleted for this app:
// only when the user asked for the cleanup.
uint64_t RemoveLeftovers(double archiver_age_seconds, bool files_trash) {
  uint64_t freed = 0;
  for (const fs::path& dir : ArchiverStagingDirs()) {
    freed += RemoveOldEntries(dir, archiver_age_seconds);
  }
  freed += RemoveOldEntries(HomePath() / "tmp", kLeftoverAgeSeconds);
  if (files_trash) {
    freed += RemoveOldEntries(FilesTrashPath(), 0.0);
  }
  return freed;
}

bool IsUnder(const fs::path& path, const fs::path& root) {
  auto mismatch = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
  return mismatch.first == root.end();
}

class StorageWalk {
 public:
  StorageWalk()
      : home_(Spelled(HomePath())),
        documents_(Spelled(DocumentsPath())),
        caches_(Spelled(CachesPath())),
        cache_root_(Spelled(xe_ios_cache_root())),
        shaders_(xe::gpu::GetShaderStorageRoot(cache_root_)),
        bundle_(BundleId()) {
    for (const fs::path& dir : ArchiverStagingDirs()) {
      leftover_dirs_.push_back(Spelled(dir));
    }
    leftover_dirs_.push_back(Spelled(FilesTrashPath()));
    uint64_t untitled_key = kUntitledKeyBase;
    for (const xe::ui::IOSDiscoveredGame& game : xe::ui::LoadCachedIOSGameLibrary()) {
      // A game whose title ID couldn't be read is still in the library: list it
      // under its own key, found again by its path.
      const uint64_t key = game.title_id ? game.title_id : untitled_key++;
      IOSStorageTitle& title = Title(key, game.title_id);
      title.in_library = true;
      if (title.game_path.empty()) {
        title.game_path = game.path.string();
      }
      if (!game.title.empty()) {
        title.name = game.title;
      }
      auto add_copy = [&](const fs::path& path) {
        if (path.empty()) {
          return;
        }
        const fs::path normal = Spelled(path);
        // Decided by where the file is, not the entry's linked flag: a folder
        // linked from inside the app's own storage still takes the app's space.
        if (!IsUnder(normal, home_)) {
          title.has_linked_copy = true;
          linked_paths_.emplace_back(normal, key);
          return;
        }
        title.has_imported_copy = true;
        game_paths_[normal.string()] = key;
        fs::path sidecar = normal;
        sidecar += ".data";
        game_paths_[sidecar.string()] = key;
      };
      if (game.discs.empty()) {
        add_copy(game.path);
      }
      for (const auto& disc : game.discs) {
        add_copy(disc.path);
      }
    }
  }

  IOSStorageReport Run() {
    WalkTemporary();
    Walk(documents_);
    Walk(home_ / "Library");
    MeasureLinked();
    AdoptUntitledGames();
    IOSStorageReport report;
    report.bytes = bytes_;
    for (auto& [key, title] : titles_) {
      if (key == kProfileTitleId || (!title.Total() && !title.in_library)) {
        continue;
      }
      if (title.name.empty() && title.title_id) {
        if (const xe::app::GameTitleInfo* info = xe::app::GetGameTitleInfo(title.title_id)) {
          title.name = info->name;
        }
      }
      report.titles.push_back(std::move(title));
    }
    std::sort(
        report.titles.begin(), report.titles.end(),
        [](const IOSStorageTitle& a, const IOSStorageTitle& b) {
          // With what a game takes on another drive.
          auto size = [](const IOSStorageTitle& title) {
            return title.Total() + title.linked_bytes -
                   title.bytes[size_t(IOSStorageKind::kLinked)];
          };
          return size(a) > size(b);
        });
    return report;
  }

 private:
  // Titles are keyed by title ID; games without one by kUntitledKeyBase + n.
  static constexpr uint64_t kUntitledKeyBase = uint64_t(1) << 32;

  // A library game whose title ID the scan couldn't read (an original Xbox
  // disc) still saves under its title ID. Data whose title has the game's name
  // is the game's: one entry, under the title ID.
  void AdoptUntitledGames() {
    auto lower = [](std::string text) {
      std::transform(text.begin(), text.end(), text.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      return text;
    };
    std::vector<uint64_t> untitled;
    for (const auto& [key, title] : titles_) {
      if (key >= kUntitledKeyBase && title.in_library && !title.name.empty()) {
        untitled.push_back(key);
      }
    }
    for (uint64_t game_key : untitled) {
      const IOSStorageTitle game = titles_[game_key];
      const std::string name = lower(game.name);
      IOSStorageTitle* data = nullptr;
      for (auto& [key, title] : titles_) {
        if (key >= kUntitledKeyBase || key == kProfileTitleId || title.in_library) {
          continue;
        }
        std::string data_name = title.name;
        if (data_name.empty()) {
          if (const xe::app::GameTitleInfo* info = xe::app::GetGameTitleInfo(title.title_id)) {
            data_name = info->name;
          }
        }
        if (lower(data_name) == name) {
          data = &title;
          break;
        }
      }
      if (!data) {
        continue;
      }
      data->name = game.name;
      data->in_library = true;
      data->has_imported_copy = game.has_imported_copy;
      data->has_linked_copy = game.has_linked_copy;
      data->linked_bytes = game.linked_bytes;
      data->game_path = game.game_path;
      for (size_t kind = 0; kind < data->bytes.size(); ++kind) {
        data->bytes[kind] += game.bytes[kind];
      }
      titles_.erase(game_key);
    }
  }

  IOSStorageTitle& Title(uint64_t key, uint32_t title_id) {
    IOSStorageTitle& title = titles_[key];
    title.title_id = title_id;
    return title;
  }

  // `key` is a title ID or an untitled game's key.
  void Add(IOSStorageKind kind, uint64_t bytes, uint64_t key = 0) {
    bytes_[size_t(kind)] += bytes;
    if (key && key != kProfileTitleId) {
      Title(key, key < kUntitledKeyBase ? uint32_t(key) : 0).bytes[size_t(kind)] += bytes;
    }
  }

  // Sizes the linked copies that are single files (ISO, ZAR, a GOD package's
  // header with the files directly in its ".data" folder), with the linked
  // folders' security scopes held. A folder game is left unsized: on a network
  // share, walking it could take minutes.
  void MeasureLinked() {
    if (linked_paths_.empty()) {
      return;
    }
    NSArray* accesses = xe::ui::StartIOSExternalLibraryAccessForAllLocations();
    struct stat home_info;
    const bool home_known = stat(home_.c_str(), &home_info) == 0;
    auto file_bytes = [](const fs::path& path, dev_t* device_out = nullptr) -> uint64_t {
      struct stat info;
      if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return 0;
      }
      if (device_out) {
        *device_out = info.st_dev;
      }
      return uint64_t(info.st_blocks) * 512;
    };
    for (const auto& [path, key] : linked_paths_) {
      dev_t device = 0;
      uint64_t bytes = file_bytes(path, &device);
      if (!bytes) {
        continue;
      }
      fs::path sidecar = path;
      sidecar += ".data";
      std::error_code ec;
      for (const auto& entry : fs::directory_iterator(sidecar, ec)) {
        bytes += file_bytes(entry.path());
      }
      titles_[key].linked_bytes += bytes;
      // A linked folder on the device's own storage (On My iPhone, say) takes
      // its space as the app's folder does; one on another drive doesn't.
      if (home_known && device == home_info.st_dev) {
        Add(IOSStorageKind::kLinked, bytes, key);
      }
    }
    (void)accesses;  // Released with the caller's autorelease pool.
  }

  void WalkTemporary() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(home_ / "tmp", ec)) {
      Add(OlderThan(entry.path(), kLeftoverAgeSeconds) ? IOSStorageKind::kLeftovers
                                                       : IOSStorageKind::kOther,
          TreeBytes(entry.path()));
    }
  }

  void Walk(const fs::path& root) {
    std::error_code ec;
    for (fs::recursive_directory_iterator
             it(root, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
      const fs::path& path = it->path();
      if (std::find(leftover_dirs_.begin(), leftover_dirs_.end(), path) != leftover_dirs_.end()) {
        Add(IOSStorageKind::kLeftovers, TreeBytes(path));
        it.disable_recursion_pending();
        continue;
      }
      if (auto game = game_paths_.find(path.lexically_normal().string());
          game != game_paths_.end()) {
        Add(IOSStorageKind::kGames, TreeBytes(path), game->second);
        it.disable_recursion_pending();
        continue;
      }
      uint32_t title_id = 0;
      Add(Classify(path, &title_id), EntryBytes(path), title_id);
    }
  }

  IOSStorageKind Classify(const fs::path& path, uint32_t* title_id_out) {
    if (IsUnder(path, documents_)) {
      std::vector<std::string> parts;
      for (const auto& part : path.lexically_relative(documents_)) {
        parts.push_back(part.string());
      }
      const std::string& top = parts[0];
      if (top == "games") {
        return IOSStorageKind::kGames;
      }
      if (std::find(std::begin(kGameDataDrives), std::end(kGameDataDrives), top) !=
          std::end(kGameDataDrives)) {
        return IOSStorageKind::kGameData;
      }
      if (top == "content") {
        // content/{owner}/{title ID}/{content type}/...: the shared owner holds
        // updates and add-ons, each profile its saves.
        const bool shared = parts.size() > 1 && parts[1] == kSharedOwner;
        if (parts.size() < 3 || parts[2].size() != 8 || !TitleIdOfName(parts[2], title_id_out)) {
          *title_id_out = 0;
          return shared ? IOSStorageKind::kContent : IOSStorageKind::kSaves;
        }
        if (shared) {
          return IOSStorageKind::kContent;
        }
        if (*title_id_out == kProfileTitleId || parts.size() < 4 || parts[3] == kSavedGameType) {
          return IOSStorageKind::kSaves;
        }
        return IOSStorageKind::kContent;
      }
    }
    if (IsUnder(path, shaders_)) {
      for (const auto& part : path.lexically_relative(shaders_)) {
        if (TitleIdOfName(part.string(), title_id_out)) {
          break;
        }
      }
      return IOSStorageKind::kShaders;
    }
    if (IsUnder(path, caches_)) {
      std::vector<std::string> parts;
      for (const auto& part : path.lexically_relative(caches_)) {
        parts.push_back(part.string());
      }
      if (parts.size() >= 2 && parts[0] == bundle_ && parts[1].rfind("com.apple.metal", 0) == 0) {
        return IOSStorageKind::kShaders;
      }
      if (parts.size() == 2 && (parts[0] == "game-boxart" || parts[0] == "game-background-art")) {
        TitleIdOfName(parts[1], title_id_out);
      }
    }
    if (IsUnder(path, cache_root_)) {
      const fs::path relative = path.lexically_relative(cache_root_);
      auto part = relative.begin();
      if (part != relative.end() && (*part == "title_art" || *part == "library-icons") &&
          ++part != relative.end()) {
        TitleIdOfName(part->string(), title_id_out);
      }
    }
    return IOSStorageKind::kOther;
  }

  fs::path home_, documents_, caches_, cache_root_, shaders_;
  std::string bundle_;
  std::vector<fs::path> leftover_dirs_;
  std::unordered_map<std::string, uint64_t> game_paths_;
  std::vector<std::pair<fs::path, uint64_t>> linked_paths_;
  std::unordered_map<uint64_t, IOSStorageTitle> titles_;
  IOSStorageBytes bytes_ = {};
};

}  // namespace

uint64_t IOSStorageTitle::Total() const {
  uint64_t total = 0;
  for (uint64_t bytes : this->bytes) {
    total += bytes;
  }
  return total;
}

uint64_t IOSStorageReport::Total() const {
  uint64_t total = 0;
  for (uint64_t bytes : this->bytes) {
    total += bytes;
  }
  return total;
}

IOSStorageReport xe_ios_measure_storage() {
  @autoreleasepool {
    return StorageWalk().Run();
  }
}

fs::path xe_ios_cache_root() {
  fs::path root = cvars::cache_root;
  if (root.empty()) {
    root = xe_get_ios_default_cache_root();
  } else if (!root.is_absolute()) {
    root = DocumentsPath() / root;
  }
  return fs::absolute(root);
}

uint64_t xe_ios_remove_storage_leftovers() {
  const uint64_t freed = RemoveLeftovers(0.0, true);
  XELOGI("Storage: removed {} MB of leftovers", freed >> 20);
  return freed;
}

bool xe_ios_clear_game_data_drives(std::string* error_message_out) {
  for (const char* drive : kGameDataDrives) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(DocumentsPath() / drive, ec)) {
      fs::remove_all(entry.path(), ec);
      if (ec) {
        if (error_message_out) {
          *error_message_out = ec.message();
        }
        return false;
      }
    }
  }
  XELOGI("Storage: cleared the game data drives");
  return true;
}

bool xe_ios_clear_title_shaders(uint32_t title_id, uintmax_t* removed_out,
                                std::error_code* error_out) {
  const fs::path shaders = xe::gpu::GetShaderStorageRoot(xe_ios_cache_root());
  // Every file or folder named after the title, at any depth.
  std::vector<fs::path> matches;
  std::error_code ec;
  for (fs::recursive_directory_iterator
           it(shaders, fs::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    uint32_t entry_title_id = 0;
    if (TitleIdOfName(it->path().filename().string(), &entry_title_id) &&
        entry_title_id == title_id) {
      matches.push_back(it->path());
      it.disable_recursion_pending();
    }
  }
  uintmax_t removed = 0;
  ec.clear();
  for (const fs::path& match : matches) {
    const uintmax_t count = fs::remove_all(match, ec);
    if (ec) {
      break;
    }
    removed += count;
  }
  if (removed_out) {
    *removed_out = removed;
  }
  if (error_out) {
    *error_out = ec;
  }
  return !ec;
}

bool xe_ios_clear_all_shaders(uintmax_t* removed_out, std::error_code* error_out,
                              bool system_cache) {
  std::error_code ec;
  const fs::path shaders = xe::gpu::GetShaderStorageRoot(xe_ios_cache_root());
  uintmax_t removed = fs::exists(shaders, ec) ? fs::remove_all(shaders, ec) : 0;
  if (!ec && system_cache) {
    const std::string bundle = BundleId();
    for (const auto& entry : fs::directory_iterator(CachesPath() / bundle, ec)) {
      if (!bundle.empty() && entry.path().filename().string().rfind("com.apple.metal", 0) == 0) {
        std::error_code remove_ec;
        removed += fs::remove_all(entry.path(), remove_ec);
      }
    }
    ec.clear();
  }
  if (removed_out) {
    *removed_out = ec ? 0 : removed;
  }
  if (error_out) {
    *error_out = ec;
  }
  return !ec;
}

void xe_ios_remove_title_art(uint32_t title_id) {
  char upper[9], lower[9];
  std::snprintf(upper, sizeof(upper), "%08X", title_id);
  std::snprintf(lower, sizeof(lower), "%08x", title_id);
  std::error_code ec;
  for (const char* dir : {"game-boxart", "game-background-art"}) {
    fs::remove(CachesPath() / dir / (std::string(lower) + ".jpg"), ec);
  }
  fs::remove(CachesPath() / (std::string("discussion-") + upper + ".json"), ec);
  const fs::path cache_root = xe_ios_cache_root();
  fs::remove_all(cache_root / "title_art" / upper, ec);
  for (const auto& entry : fs::directory_iterator(cache_root / "library-icons", ec)) {
    uint32_t entry_title_id = 0;
    if (TitleIdOfName(entry.path().filename().string(), &entry_title_id) &&
        entry_title_id == title_id) {
      std::error_code remove_ec;
      fs::remove(entry.path(), remove_ec);
    }
  }
}

bool xe_ios_remove_title_data(uint32_t title_id, std::string* error_message_out) {
  char title[9];
  std::snprintf(title, sizeof(title), "%08X", title_id);
  std::error_code ec;
  for (const auto& owner : fs::directory_iterator(DocumentsPath() / "content", ec)) {
    std::error_code remove_ec;
    fs::remove_all(owner.path() / title, remove_ec);
    if (remove_ec) {
      if (error_message_out) {
        *error_message_out = remove_ec.message();
      }
      return false;
    }
  }
  if (!xe_ios_clear_title_shaders(title_id, nullptr, &ec)) {
    if (error_message_out) {
      *error_message_out = ec.message();
    }
    return false;
  }
  xe_ios_remove_title_art(title_id);
  return true;
}

void xe_ios_storage_launch_maintenance() {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    @autoreleasepool {
      const uint64_t freed = RemoveLeftovers(kLaunchArchiverAgeSeconds, false);
      // What games install to the cache drives comes back by itself, and can
      // be gigabytes: not worth a backup.
      for (const char* drive : kGameDataDrives) {
        NSURL* url = [NSURL fileURLWithPath:@((DocumentsPath() / drive).c_str()) isDirectory:YES];
        if ([url checkResourceIsReachableAndReturnError:nil]) {
          [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        }
      }
      uint64_t modules_freed = 0;
      modules_freed += RemoveOldEntries(xe_ios_cache_root() / "modules", kModuleUnusedSeconds);
      if (freed || modules_freed) {
        XELOGI("Storage: removed {} MB of leftovers and {} MB of unused CPU caches at launch",
               freed >> 20, modules_freed >> 20);
      }
    }
  });
}
