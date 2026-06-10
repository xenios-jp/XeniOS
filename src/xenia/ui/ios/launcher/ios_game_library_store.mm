/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_library_store.h"

#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/ui/ios/launcher/ios_compat_data.h"
#include "xenia/ui/ios/launcher/ios_content_management.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

@implementation XeniaIOSExternalLibraryAccess {
  NSURL* _url;
  BOOL _accessGranted;
}

@synthesize url = _url;
@synthesize accessGranted = _accessGranted;

- (instancetype)initWithURL:(NSURL*)url {
  if (!(self = [super init])) {
    return nil;
  }
  _url = [url retain];
  _accessGranted = [_url startAccessingSecurityScopedResource];
  XELOGI("iOS: External library scope started for {} (security-scoped: {})",
         _url.path ? [_url.path UTF8String] : "", _accessGranted ? "yes" : "no");
  return self;
}

- (void)dealloc {
  if (_accessGranted) {
    [_url stopAccessingSecurityScopedResource];
  }
  [_url release];
  [super dealloc];
}

@end

namespace xe {
namespace ui {

namespace {

NSString* const kExternalLibraryLocationsDefaultsKey = @"XeniaIOSExternalLibraryLocations";
NSString* const kExternalLibraryBookmarkKey = @"bookmark";
NSString* const kExternalLibraryPathKey = @"path";
NSString* const kExternalLibraryNameKey = @"name";
NSString* const kExternalLibraryHiddenGamesDefaultsKey = @"XeniaIOSExternalLibraryHiddenGames";
NSString* const kExternalLibraryHiddenGamePathKey = @"path";
NSString* const kExternalLibraryHiddenGameNameKey = @"name";
NSString* const kExternalLibraryHiddenGameTitleIDKey = @"titleID";
NSString* const kExternalLibraryHiddenGameMediaIDKey = @"mediaID";
NSString* const kExternalLibraryHiddenGameDiscNumberKey = @"discNumber";
NSString* const kExternalLibraryHiddenGameDiscCountKey = @"discCount";

struct IOSGameLibraryScanRoot {
  std::filesystem::path path;
  bool is_external = false;
  std::string external_location_name;
};

struct IOSHiddenExternalGame {
  std::filesystem::path path;
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;
};

bool PathIsInDirectory(const std::filesystem::path& path, const std::filesystem::path& directory) {
  auto path_it = path.begin();
  auto directory_it = directory.begin();
  for (; directory_it != directory.end(); ++directory_it, ++path_it) {
    if (path_it == path.end() || *path_it != *directory_it) {
      return false;
    }
  }
  return true;
}

std::filesystem::path WeaklyCanonicalOrAbsolute(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    return canonical;
  }
  ec.clear();
  std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  return ec ? path : absolute;
}

const char* LibraryCandidateKindForLog(const std::filesystem::path& path) {
  if (IsZarPath(path)) {
    return "zar";
  }
  if (IsISOPath(path)) {
    return "iso";
  }
  if (IsDefaultXexPath(path) || IsDefaultXbePath(path)) {
    return "xex";
  }
  if (IsLikelyGodContainerFile(path)) {
    return "god";
  }
  return "unknown";
}

const char* BoolForLog(bool value) { return value ? "true" : "false"; }

NSArray<NSDictionary*>* ExternalLibraryLocationRecords() {
  id records =
      [[NSUserDefaults standardUserDefaults] objectForKey:kExternalLibraryLocationsDefaultsKey];
  if (![records isKindOfClass:[NSArray class]]) {
    return @[];
  }
  return (NSArray<NSDictionary*>*)records;
}

NSArray* ExternalLibraryHiddenGameRecords() {
  id records =
      [[NSUserDefaults standardUserDefaults] objectForKey:kExternalLibraryHiddenGamesDefaultsKey];
  if (![records isKindOfClass:[NSArray class]]) {
    return @[];
  }
  return (NSArray*)records;
}

NSMutableSet<NSString*>* ExternalLibraryHiddenGamePathSet() {
  NSMutableSet<NSString*>* hidden_paths = [NSMutableSet set];
  for (id record in ExternalLibraryHiddenGameRecords()) {
    if ([record isKindOfClass:[NSString class]] && [record length] > 0) {
      [hidden_paths addObject:(NSString*)record];
      continue;
    }
    if ([record isKindOfClass:[NSDictionary class]]) {
      NSString* path = [(NSDictionary*)record objectForKey:kExternalLibraryHiddenGamePathKey];
      if ([path isKindOfClass:[NSString class]] && path.length > 0) {
        [hidden_paths addObject:path];
      }
    }
  }
  return hidden_paths;
}

NSString* ExternalLibraryHiddenGamePathKey(const std::filesystem::path& path) {
  return ToNSString(WeaklyCanonicalOrAbsolute(path).string());
}

uint32_t UInt32FromObject(id value) {
  if (![value respondsToSelector:@selector(unsignedLongLongValue)]) {
    return 0;
  }
  unsigned long long raw = [value unsignedLongLongValue];
  return raw <= UINT32_MAX ? static_cast<uint32_t>(raw) : 0;
}

uint8_t UInt8FromObject(id value) {
  if (![value respondsToSelector:@selector(unsignedLongLongValue)]) {
    return 0;
  }
  unsigned long long raw = [value unsignedLongLongValue];
  return raw <= UINT8_MAX ? static_cast<uint8_t>(raw) : 0;
}

std::vector<IOSHiddenExternalGame> HiddenExternalGameRecordsForScan() {
  std::vector<IOSHiddenExternalGame> hidden_games;
  for (id record in ExternalLibraryHiddenGameRecords()) {
    IOSHiddenExternalGame hidden_game;
    if ([record isKindOfClass:[NSString class]]) {
      NSString* path = (NSString*)record;
      if (path.length == 0) {
        continue;
      }
      hidden_game.path =
          WeaklyCanonicalOrAbsolute(std::filesystem::path(std::string([path UTF8String])));
      hidden_games.push_back(std::move(hidden_game));
      continue;
    }
    if (![record isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    NSDictionary* dictionary = (NSDictionary*)record;
    NSString* path = [dictionary objectForKey:kExternalLibraryHiddenGamePathKey];
    if (![path isKindOfClass:[NSString class]] || path.length == 0) {
      continue;
    }
    hidden_game.path =
        WeaklyCanonicalOrAbsolute(std::filesystem::path(std::string([path UTF8String])));
    hidden_game.title_id =
        UInt32FromObject([dictionary objectForKey:kExternalLibraryHiddenGameTitleIDKey]);
    hidden_game.media_id =
        UInt32FromObject([dictionary objectForKey:kExternalLibraryHiddenGameMediaIDKey]);
    hidden_game.disc_number =
        UInt8FromObject([dictionary objectForKey:kExternalLibraryHiddenGameDiscNumberKey]);
    hidden_game.disc_count =
        UInt8FromObject([dictionary objectForKey:kExternalLibraryHiddenGameDiscCountKey]);
    hidden_games.push_back(std::move(hidden_game));
  }
  return hidden_games;
}

NSDictionary* HiddenExternalGameRecordForPath(const std::filesystem::path& path,
                                              NSString** hiddenName) {
  IOSDiscoveredGame game;
  const bool has_game_identity = BuildDiscoveredGameFromPath(path, &game);
  NSString* path_key = ExternalLibraryHiddenGamePathKey(path);
  NSMutableDictionary* record =
      [NSMutableDictionary dictionaryWithObject:path_key forKey:kExternalLibraryHiddenGamePathKey];
  if (has_game_identity) {
    if (!game.title.empty()) {
      [record setObject:ToNSString(game.title) forKey:kExternalLibraryHiddenGameNameKey];
    }
    if (game.title_id) {
      [record setObject:@(game.title_id) forKey:kExternalLibraryHiddenGameTitleIDKey];
    }
    if (game.media_id) {
      [record setObject:@(game.media_id) forKey:kExternalLibraryHiddenGameMediaIDKey];
    }
    if (game.disc_number) {
      [record setObject:@(game.disc_number) forKey:kExternalLibraryHiddenGameDiscNumberKey];
    }
    if (game.disc_count) {
      [record setObject:@(game.disc_count) forKey:kExternalLibraryHiddenGameDiscCountKey];
    }
  }

  if (hiddenName) {
    NSString* display_name = [record objectForKey:kExternalLibraryHiddenGameNameKey];
    if (![display_name isKindOfClass:[NSString class]] || display_name.length == 0) {
      std::string filename = path.filename().string();
      display_name = ToNSString(filename.empty() ? path.string() : filename);
    }
    *hiddenName = display_name;
  }
  return record;
}

NSURL* ResolveExternalLibraryRecord(NSDictionary* record, BOOL* stale, NSError** error) {
  if (stale) {
    *stale = NO;
  }
  if (![record isKindOfClass:[NSDictionary class]]) {
    return nil;
  }
  NSData* bookmark = [record objectForKey:kExternalLibraryBookmarkKey];
  if (![bookmark isKindOfClass:[NSData class]] || bookmark.length == 0) {
    return nil;
  }
  BOOL bookmark_stale = NO;
  NSURLBookmarkResolutionOptions options =
      NSURLBookmarkResolutionWithoutUI | NSURLBookmarkResolutionWithoutImplicitStartAccessing;
  NSURL* url = [NSURL URLByResolvingBookmarkData:bookmark
                                         options:options
                                   relativeToURL:nil
                             bookmarkDataIsStale:&bookmark_stale
                                           error:error];
  if (stale) {
    *stale = bookmark_stale;
  }
  return url;
}

NSString* ExternalLibraryRecordName(NSDictionary* record, NSURL* resolved_url) {
  NSString* name = [record objectForKey:kExternalLibraryNameKey];
  if ([name isKindOfClass:[NSString class]] && name.length > 0) {
    return name;
  }
  return resolved_url.lastPathComponent.length > 0 ? resolved_url.lastPathComponent
                                                   : resolved_url.path;
}

BOOL RecordPathContainsGamePath(NSDictionary* record,
                                const std::filesystem::path& normalized_game_path) {
  NSString* stored_path = [record objectForKey:kExternalLibraryPathKey];
  if (![stored_path isKindOfClass:[NSString class]] || stored_path.length == 0) {
    return NO;
  }
  std::filesystem::path root_path =
      WeaklyCanonicalOrAbsolute(std::filesystem::path(std::string([stored_path UTF8String])));
  return PathIsInDirectory(normalized_game_path, root_path);
}

BOOL ExternalLibraryRecordContainsPath(NSDictionary* record,
                                       const std::filesystem::path& normalized_game_path,
                                       NSURL** resolvedURL, NSError** resolveError) {
  BOOL record_matches_path = RecordPathContainsGamePath(record, normalized_game_path);
  BOOL stale = NO;
  NSError* local_resolve_error = nil;
  NSURL* url = ResolveExternalLibraryRecord(record, &stale, &local_resolve_error);
  if (url.path.length > 0) {
    std::filesystem::path root_path =
        WeaklyCanonicalOrAbsolute(std::filesystem::path(std::string([url.path UTF8String])));
    record_matches_path = record_matches_path || PathIsInDirectory(normalized_game_path, root_path);
  }
  if (resolvedURL) {
    *resolvedURL = url;
  }
  if (resolveError) {
    *resolveError = local_resolve_error;
  }
  return record_matches_path;
}

bool IsHiddenExternalGamePath(const std::filesystem::path& path, NSSet<NSString*>* hidden_paths) {
  if (!hidden_paths.count) {
    return false;
  }
  return [hidden_paths containsObject:ExternalLibraryHiddenGamePathKey(path)];
}

bool IsHiddenExternalGameIdentity(const IOSDiscoveredGame& game,
                                  const std::vector<IOSHiddenExternalGame>& hidden_games) {
  if (!game.title_id) {
    return false;
  }
  for (const IOSHiddenExternalGame& hidden_game : hidden_games) {
    if (!hidden_game.title_id || hidden_game.title_id != game.title_id) {
      continue;
    }
    // A mismatch only rules out this hidden record - other hidden records may
    // still match (e.g. several hidden discs or copies of the same title).
    if (hidden_game.media_id && game.media_id) {
      if (hidden_game.media_id == game.media_id) {
        return true;
      }
      continue;
    }
    if (hidden_game.disc_number && game.disc_number &&
        hidden_game.disc_number == game.disc_number) {
      if (hidden_game.disc_count == game.disc_count || !hidden_game.disc_count ||
          !game.disc_count) {
        return true;
      }
      continue;
    }
    if (!hidden_game.media_id && !game.media_id && !hidden_game.disc_number && !game.disc_number) {
      return true;
    }
  }
  return false;
}

std::string LibrarySourceLabel(bool is_external) { return is_external ? "External" : "Imported"; }

void ApplyScanRootSourceToGame(IOSDiscoveredGame* game, const IOSGameLibraryScanRoot& root) {
  if (!game) {
    return;
  }
  game->is_external = root.is_external;
  game->external_location_name = root.external_location_name;
  const std::string source_label = LibrarySourceLabel(root.is_external);
  for (IOSDiscoveredGame::Disc& disc : game->discs) {
    disc.is_external = root.is_external;
    disc.has_imported_source = !root.is_external;
    disc.has_external_source = root.is_external;
    disc.source_label = source_label;
  }
}

}  // namespace

std::filesystem::path IOSImportedGamesDirectory() {
  return xe_get_ios_documents_path() / "games";
}

bool IsPathInIOSImportedGamesDirectory(const std::filesystem::path& path) {
  return PathIsInDirectory(WeaklyCanonicalOrAbsolute(path),
                           WeaklyCanonicalOrAbsolute(IOSImportedGamesDirectory()));
}

std::filesystem::path ImportGameIntoIOSLibrary(NSURL* source_url, NSError** error) {
  std::filesystem::path source_path([source_url.path UTF8String]);
  std::filesystem::path documents_path = xe_get_ios_documents_path();
  std::filesystem::path library_path = IOSImportedGamesDirectory();

  std::error_code ec;
  auto weak_source = std::filesystem::weakly_canonical(source_path, ec);
  if (ec) {
    weak_source = std::filesystem::absolute(source_path, ec);
    ec.clear();
  }
  std::filesystem::create_directories(library_path, ec);
  if (ec) {
    if (error) {
      *error = [NSError
          errorWithDomain:@"XeniaIOSImport"
                     code:1001
                 userInfo:@{
                   NSLocalizedDescriptionKey : [NSString
                       stringWithFormat:@"Failed creating library folder: %s", ec.message().c_str()]
                 }];
    }
    return {};
  }

  auto weak_library = std::filesystem::weakly_canonical(library_path, ec);
  if (ec) {
    weak_library = std::filesystem::absolute(library_path, ec);
  }
  ec.clear();
  if (PathIsInDirectory(weak_source, weak_library)) {
    return weak_source;
  }

  auto weak_documents = std::filesystem::weakly_canonical(documents_path, ec);
  if (ec) {
    weak_documents = std::filesystem::absolute(documents_path, ec);
  }
  ec.clear();
  const bool source_inside_documents = PathIsInDirectory(weak_source, weak_documents);
  const bool source_has_sidecar = HasContentSidecarDataDirectory(source_path);
  const bool should_move_source = source_inside_documents &&
                                  std::filesystem::is_regular_file(source_path, ec) &&
                                  !source_has_sidecar;
  ec.clear();

  // A folder-based game (an extracted default.xex / default.xbe) is a whole
  // directory of companion files. Relocating only the .xex would orphan it from
  // its data and it would not boot, so move/copy the entire container folder and
  // return the launchable inside the relocated copy.
  if (IsDefaultXexPath(source_path) || IsDefaultXbePath(source_path)) {
    const std::filesystem::path container = source_path.parent_path();
    std::error_code container_ec;
    std::filesystem::path weak_container =
        std::filesystem::weakly_canonical(container, container_ec);
    if (container_ec) {
      weak_container = std::filesystem::absolute(container, container_ec);
    }
    container_ec.clear();
    const bool container_is_relocatable =
        !container.empty() && std::filesystem::is_directory(container, container_ec) &&
        weak_container != weak_documents && weak_container != weak_library &&
        !PathIsInDirectory(weak_library, weak_container);
    container_ec.clear();
    if (container_is_relocatable) {
      std::filesystem::path destination_dir = library_path / container.filename();
      const std::string base_name = destination_dir.filename().string();
      for (int attempt = 2; std::filesystem::exists(destination_dir); ++attempt) {
        destination_dir = library_path / (base_name + " (" + std::to_string(attempt) + ")");
      }
      NSString* container_ns = ToNSString(container.string());
      NSString* destination_dir_ns = ToNSString(destination_dir.string());
      NSFileManager* file_manager = [NSFileManager defaultManager];
      const BOOL relocated =
          source_inside_documents
              ? [file_manager moveItemAtPath:container_ns toPath:destination_dir_ns error:error]
              : [file_manager copyItemAtPath:container_ns toPath:destination_dir_ns error:error];
      if (!relocated) {
        return {};
      }
      XELOGI("iOS: Relocated folder game container {} -> {}", container.string(),
             destination_dir.string());
      return destination_dir / source_path.filename();
    }
  }

  std::filesystem::path destination = library_path / source_path.filename();
  std::filesystem::path stem = destination.stem();
  std::filesystem::path extension = destination.extension();
  for (int attempt = 2; std::filesystem::exists(destination); ++attempt) {
    destination =
        library_path / std::filesystem::path(stem.string() + " (" + std::to_string(attempt) + ")" +
                                             extension.string());
  }

  NSString* source_ns = source_url.path;
  NSString* destination_ns = ToNSString(destination.string());
  NSFileManager* file_manager = [NSFileManager defaultManager];
  BOOL transferred = should_move_source
                         ? [file_manager moveItemAtPath:source_ns toPath:destination_ns error:error]
                         : [file_manager copyItemAtPath:source_ns
                                                 toPath:destination_ns
                                                  error:error];
  if (!transferred) {
    return {};
  }

  if (source_has_sidecar) {
    std::filesystem::path source_sidecar = source_path;
    source_sidecar += ".data";
    std::filesystem::path destination_sidecar = destination;
    destination_sidecar += ".data";

    std::string error_message;
    if (!xe_copy_directory_recursive(source_sidecar, destination_sidecar, &error_message)) {
      std::error_code cleanup_error;
      std::filesystem::remove(destination, cleanup_error);
      std::filesystem::remove_all(destination_sidecar, cleanup_error);
      if (error) {
        *error = [NSError
            errorWithDomain:@"XeniaIOSImport"
                       code:1002
                   userInfo:@{
                     NSLocalizedDescriptionKey : ToNSString(
                         error_message.empty() ? "Failed copying package sidecar." : error_message)
                   }];
      }
      return {};
    }
  }

  return destination;
}

BOOL SaveIOSExternalLibraryLocation(NSURL* folder_url, NSError** error) {
  if (!folder_url.fileURL) {
    if (error) {
      *error = [NSError
          errorWithDomain:@"XeniaIOSExternalLibrary"
                     code:2001
                 userInfo:@{NSLocalizedDescriptionKey : @"Selected location is not a folder."}];
    }
    return NO;
  }

  BOOL access_granted = [folder_url startAccessingSecurityScopedResource];
  NSNumber* is_directory = nil;
  if (![folder_url getResourceValue:&is_directory forKey:NSURLIsDirectoryKey error:error] ||
      !is_directory.boolValue) {
    if (access_granted) {
      [folder_url stopAccessingSecurityScopedResource];
    }
    if (error && !*error) {
      *error = [NSError
          errorWithDomain:@"XeniaIOSExternalLibrary"
                     code:2002
                 userInfo:@{NSLocalizedDescriptionKey : @"Selected location is not a folder."}];
    }
    return NO;
  }

  NSData* bookmark = [folder_url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark
                          includingResourceValuesForKeys:nil
                                           relativeToURL:nil
                                                   error:error];
  if (access_granted) {
    [folder_url stopAccessingSecurityScopedResource];
  }
  if (!bookmark) {
    return NO;
  }

  NSString* path = folder_url.path ?: @"";
  NSString* name = folder_url.lastPathComponent.length > 0 ? folder_url.lastPathComponent : path;
  NSMutableArray<NSDictionary*>* records =
      [[ExternalLibraryLocationRecords() mutableCopy] autorelease];
  for (NSInteger i = static_cast<NSInteger>(records.count) - 1; i >= 0; --i) {
    NSDictionary* record = records[static_cast<NSUInteger>(i)];
    NSString* existing_path = [record objectForKey:kExternalLibraryPathKey];
    if ([existing_path isKindOfClass:[NSString class]] && [existing_path isEqualToString:path]) {
      [records removeObjectAtIndex:static_cast<NSUInteger>(i)];
    }
  }
  [records addObject:@{
    kExternalLibraryBookmarkKey : bookmark,
    kExternalLibraryPathKey : path,
    kExternalLibraryNameKey : name ?: @"External Library",
  }];
  [[NSUserDefaults standardUserDefaults] setObject:records
                                            forKey:kExternalLibraryLocationsDefaultsKey];
  XELOGI("iOS: Linked external library location: {}", [path UTF8String]);
  return YES;
}

std::vector<IOSExternalLibraryLocation> ListIOSExternalLibraryLocations() {
  std::vector<IOSExternalLibraryLocation> locations;
  for (NSDictionary* record in ExternalLibraryLocationRecords()) {
    if (![record isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    BOOL stale = NO;
    NSError* error = nil;
    NSURL* url = ResolveExternalLibraryRecord(record, &stale, &error);
    IOSExternalLibraryLocation location;
    NSString* name = ExternalLibraryRecordName(record, url);
    location.name =
        name.length > 0 ? std::string([name UTF8String]) : std::string("External Library");
    NSString* path_string =
        url.path.length > 0 ? url.path : [record objectForKey:kExternalLibraryPathKey];
    if ([path_string isKindOfClass:[NSString class]] && path_string.length > 0) {
      location.path = std::filesystem::path(std::string([path_string UTF8String]));
    }
    location.available = url != nil && !stale && url.path.length > 0;
    locations.push_back(std::move(location));
  }
  return locations;
}

BOOL HideIOSExternalLibraryGameAtPath(const std::filesystem::path& path, NSString** hiddenName,
                                      NSError** error) {
  if (hiddenName) {
    *hiddenName = nil;
  }

  const std::filesystem::path normalized_game_path = WeaklyCanonicalOrAbsolute(path);
  BOOL matched = NO;
  for (NSDictionary* record in ExternalLibraryLocationRecords()) {
    if (ExternalLibraryRecordContainsPath(record, normalized_game_path, nullptr, nullptr)) {
      matched = YES;
      break;
    }
  }

  if (!matched) {
    if (error) {
      *error = [NSError
          errorWithDomain:@"XeniaIOSExternalLibrary"
                     code:2005
                 userInfo:@{NSLocalizedDescriptionKey : @"Linked external game was not found."}];
    }
    return NO;
  }

  NSString* hidden_path_key = ExternalLibraryHiddenGamePathKey(normalized_game_path);
  NSMutableArray* records = [[ExternalLibraryHiddenGameRecords() mutableCopy] autorelease];
  for (NSInteger i = static_cast<NSInteger>(records.count) - 1; i >= 0; --i) {
    id record = records[static_cast<NSUInteger>(i)];
    NSString* record_path = nil;
    if ([record isKindOfClass:[NSString class]]) {
      record_path = (NSString*)record;
    } else if ([record isKindOfClass:[NSDictionary class]]) {
      record_path = [(NSDictionary*)record objectForKey:kExternalLibraryHiddenGamePathKey];
    }
    if ([record_path isKindOfClass:[NSString class]] &&
        [record_path isEqualToString:hidden_path_key]) {
      [records removeObjectAtIndex:static_cast<NSUInteger>(i)];
    }
  }
  [records addObject:HiddenExternalGameRecordForPath(normalized_game_path, hiddenName)];
  [[NSUserDefaults standardUserDefaults] setObject:records
                                            forKey:kExternalLibraryHiddenGamesDefaultsKey];
  [[NSUserDefaults standardUserDefaults] synchronize];
  XELOGI("iOS: Hid external library game {}", normalized_game_path.string());
  return YES;
}

BOOL RemoveIOSExternalLibraryLocationForPath(const std::filesystem::path& path,
                                             NSString** removedName, NSError** error) {
  if (removedName) {
    *removedName = nil;
  }

  const std::filesystem::path normalized_game_path = WeaklyCanonicalOrAbsolute(path);
  NSMutableArray<NSDictionary*>* records =
      [[ExternalLibraryLocationRecords() mutableCopy] autorelease];
  BOOL removed = NO;
  for (NSInteger i = static_cast<NSInteger>(records.count) - 1; i >= 0; --i) {
    NSDictionary* record = records[static_cast<NSUInteger>(i)];
    NSURL* url = nil;
    BOOL record_matches_path =
        ExternalLibraryRecordContainsPath(record, normalized_game_path, &url, nullptr);
    if (!record_matches_path) {
      continue;
    }

    if (removedName && !*removedName) {
      NSString* name = ExternalLibraryRecordName(record, url);
      *removedName = [[name copy] autorelease];
    }
    [records removeObjectAtIndex:static_cast<NSUInteger>(i)];
    removed = YES;
  }

  if (!removed) {
    if (error) {
      *error = [NSError
          errorWithDomain:@"XeniaIOSExternalLibrary"
                     code:2004
                 userInfo:@{NSLocalizedDescriptionKey : @"Linked external folder was not found."}];
    }
    return NO;
  }

  [[NSUserDefaults standardUserDefaults] setObject:records
                                            forKey:kExternalLibraryLocationsDefaultsKey];
  XELOGI("iOS: Removed external library location for {}", path.string());
  return YES;
}

BOOL RemoveIOSExternalLibraryLocationAtRoot(const std::filesystem::path& path,
                                            NSString** removedName, NSError** error) {
  if (removedName) {
    *removedName = nil;
  }

  // Unlike RemoveIOSExternalLibraryLocationForPath, which matches by path
  // containment (intended for "remove the location providing this game"),
  // this matches the record root exactly so that unlinking a folder nested
  // inside another linked folder doesn't also unlink the parent.
  const std::filesystem::path normalized_root = WeaklyCanonicalOrAbsolute(path);
  NSMutableArray<NSDictionary*>* records =
      [[ExternalLibraryLocationRecords() mutableCopy] autorelease];
  BOOL removed = NO;
  for (NSInteger i = static_cast<NSInteger>(records.count) - 1; i >= 0; --i) {
    NSDictionary* record = records[static_cast<NSUInteger>(i)];
    BOOL record_matches_root = NO;
    NSString* stored_path = [record objectForKey:kExternalLibraryPathKey];
    if ([stored_path isKindOfClass:[NSString class]] && stored_path.length > 0) {
      record_matches_root =
          WeaklyCanonicalOrAbsolute(std::filesystem::path(
              std::string([stored_path UTF8String]))) == normalized_root;
    }
    BOOL stale = NO;
    NSURL* url = ResolveExternalLibraryRecord(record, &stale, nullptr);
    if (!record_matches_root && url.path.length > 0) {
      record_matches_root =
          WeaklyCanonicalOrAbsolute(std::filesystem::path(
              std::string([url.path UTF8String]))) == normalized_root;
    }
    if (!record_matches_root) {
      continue;
    }

    if (removedName && !*removedName) {
      NSString* name = ExternalLibraryRecordName(record, url);
      *removedName = [[name copy] autorelease];
    }
    [records removeObjectAtIndex:static_cast<NSUInteger>(i)];
    removed = YES;
  }

  if (!removed) {
    if (error) {
      *error = [NSError
          errorWithDomain:@"XeniaIOSExternalLibrary"
                     code:2004
                 userInfo:@{NSLocalizedDescriptionKey : @"Linked external folder was not found."}];
    }
    return NO;
  }

  [[NSUserDefaults standardUserDefaults] setObject:records
                                            forKey:kExternalLibraryLocationsDefaultsKey];
  XELOGI("iOS: Removed external library location rooted at {}", path.string());
  return YES;
}

XeniaIOSExternalLibraryAccess* StartIOSExternalLibraryAccessForPath(
    const std::filesystem::path& path, BOOL* matchedExternalLocation, NSError** error) {
  if (matchedExternalLocation) {
    *matchedExternalLocation = NO;
  }
  const std::filesystem::path normalized_game_path = WeaklyCanonicalOrAbsolute(path);
  for (NSDictionary* record in ExternalLibraryLocationRecords()) {
    BOOL stale = NO;
    NSError* resolve_error = nil;
    NSURL* url = ResolveExternalLibraryRecord(record, &stale, &resolve_error);
    BOOL record_matches_path = RecordPathContainsGamePath(record, normalized_game_path);
    if (url.path.length > 0) {
      std::filesystem::path root_path =
          WeaklyCanonicalOrAbsolute(std::filesystem::path(std::string([url.path UTF8String])));
      record_matches_path =
          record_matches_path || PathIsInDirectory(normalized_game_path, root_path);
    }
    if (!record_matches_path) {
      continue;
    }
    if (matchedExternalLocation) {
      *matchedExternalLocation = YES;
    }
    if (!url || stale) {
      if (error) {
        *error = resolve_error
                     ?: [NSError errorWithDomain:@"XeniaIOSExternalLibrary"
                                            code:2003
                                        userInfo:@{
                                          NSLocalizedDescriptionKey :
                                              @"External library folder is no longer available."
                                        }];
      }
      return nil;
    }
    XeniaIOSExternalLibraryAccess* access =
        [[[XeniaIOSExternalLibraryAccess alloc] initWithURL:url] autorelease];
    return access;
  }
  return nil;
}

std::vector<IOSDiscoveredGame> ScanIOSGameLibrary(NSDictionary* title_name_cache) {
  std::vector<IOSDiscoveredGame> games;

  NSMutableArray<XeniaIOSExternalLibraryAccess*>* external_accesses = [NSMutableArray array];
  std::vector<IOSGameLibraryScanRoot> scan_roots;
  NSSet<NSString*>* hidden_external_game_paths = ExternalLibraryHiddenGamePathSet();
  const std::vector<IOSHiddenExternalGame> hidden_external_games =
      HiddenExternalGameRecordsForScan();
  const std::filesystem::path documents_root = xe_get_ios_documents_path();
  const std::filesystem::path library_root = IOSImportedGamesDirectory();
  const std::filesystem::path normalized_documents_root = WeaklyCanonicalOrAbsolute(documents_root);
  const std::filesystem::path normalized_library_root = WeaklyCanonicalOrAbsolute(library_root);
  NSArray<NSDictionary*>* external_records = ExternalLibraryLocationRecords();
  XELOGI("iOS library scan: starting documents='{}' imported='{}' "
         "external_records={} hidden_paths={} hidden_identities={}",
         documents_root.string(), library_root.string(),
         static_cast<unsigned long>(external_records.count),
         static_cast<unsigned long>(hidden_external_game_paths.count),
         hidden_external_games.size());
  scan_roots.push_back({library_root});
  if (documents_root != library_root) {
    scan_roots.push_back({documents_root});
  }

  for (NSDictionary* record in external_records) {
    BOOL stale = NO;
    NSError* resolve_error = nil;
    NSURL* url = ResolveExternalLibraryRecord(record, &stale, &resolve_error);
    if (!url || stale || url.path.length == 0) {
      XELOGW("iOS: Skipping unavailable external library bookmark: {}",
             resolve_error.localizedDescription ? [resolve_error.localizedDescription UTF8String]
                                                : "stale or invalid bookmark");
      continue;
    }
    XeniaIOSExternalLibraryAccess* access =
        [[[XeniaIOSExternalLibraryAccess alloc] initWithURL:url] autorelease];
    [external_accesses addObject:access];
    NSString* name = ExternalLibraryRecordName(record, url);
    XELOGI("iOS library scan: linked external root name='{}' path='{}' "
           "stale={}",
           name ? [name UTF8String] : "", url.path ? [url.path UTF8String] : "", BoolForLog(stale));
    scan_roots.push_back({std::filesystem::path(std::string([url.path UTF8String])), true,
                          name ? std::string([name UTF8String]) : std::string()});
  }

  std::set<std::filesystem::path> seen_paths;
  std::map<uint32_t, size_t> title_id_to_index;
  for (const auto& root : scan_roots) {
    std::error_code ec;
    if (!std::filesystem::exists(root.path, ec)) {
      XELOGI("iOS library scan: skipping missing root path='{}' external={} "
             "error='{}'",
             root.path.string(), BoolForLog(root.is_external), ec ? ec.message() : "none");
      continue;
    }

    size_t pruned_directories = 0;
    size_t candidate_files = 0;
    size_t added_games = 0;
    size_t merged_games = 0;
    size_t duplicate_paths = 0;
    size_t hidden_paths = 0;
    size_t hidden_identities = 0;
    size_t build_failures = 0;
    const bool is_documents_fallback_root =
        !root.is_external && WeaklyCanonicalOrAbsolute(root.path) == normalized_documents_root &&
        normalized_documents_root != normalized_library_root;
    XELOGI("iOS library scan: scanning root path='{}' external={} "
           "documents_fallback={} name='{}'",
           root.path.string(), BoolForLog(root.is_external), BoolForLog(is_documents_fallback_root),
           root.external_location_name);
    std::filesystem::recursive_directory_iterator it(
        root.path, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
      XELOGI("iOS library scan: failed opening root path='{}' error='{}'", root.path.string(),
             ec.message());
      continue;
    }
    std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
      const auto& entry = *it;
      const auto filename = entry.path().filename().string();
      const auto filename_lower = ToLowerAsciiCopy(filename);
      if (entry.is_directory(ec)) {
        if (ToLowerAsciiCopy(entry.path().extension().string()) == ".data") {
          ++pruned_directories;
          XELOGI("iOS library scan: pruning .data directory path='{}'", entry.path().string());
          it.disable_recursion_pending();
        }
        if (is_documents_fallback_root &&
            PathIsInDirectory(WeaklyCanonicalOrAbsolute(entry.path()), normalized_library_root)) {
          ++pruned_directories;
          XELOGI("iOS library scan: pruning imported games mirror from "
                 "Documents fallback path='{}'",
                 entry.path().string());
          it.disable_recursion_pending();
        }
        if (filename_lower == "cache" || filename_lower == "cache_host") {
          ++pruned_directories;
          XELOGI("iOS library scan: pruning cache directory path='{}'", entry.path().string());
          it.disable_recursion_pending();
        }
      } else if (entry.is_regular_file(ec) &&
                 (IsISOPath(entry.path()) || IsZarPath(entry.path()) ||
                  IsDefaultXexPath(entry.path()) || IsLikelyGodContainerFile(entry.path()))) {
        ++candidate_files;
        const std::filesystem::path canonical_path =
            std::filesystem::weakly_canonical(entry.path(), ec);
        const std::filesystem::path unique_path =
            ec ? std::filesystem::absolute(entry.path(), ec) : canonical_path;
        ec.clear();
        XELOGI("iOS library scan: candidate kind={} external={} path='{}'",
               LibraryCandidateKindForLog(unique_path), BoolForLog(root.is_external),
               unique_path.string());

        if (root.is_external && IsHiddenExternalGamePath(unique_path, hidden_external_game_paths)) {
          ++hidden_paths;
          XELOGI("iOS library scan: skipping hidden external path='{}'", unique_path.string());
          ++it;
          continue;
        }

        if (seen_paths.insert(unique_path).second) {
          IOSDiscoveredGame game;
          if (!BuildDiscoveredGameFromPath(unique_path, &game)) {
            ++build_failures;
            XELOGI("iOS library scan: failed building game metadata kind={} "
                   "path='{}'",
                   LibraryCandidateKindForLog(unique_path), unique_path.string());
            ++it;
            continue;
          }
          if (root.is_external && IsHiddenExternalGameIdentity(game, hidden_external_games)) {
            ++hidden_identities;
            XELOGI("iOS library scan: skipping hidden external identity "
                   "title_id={:08X} media_id={:08X} disc={}/{} path='{}'",
                   game.title_id, game.media_id, game.disc_number, game.disc_count,
                   unique_path.string());
            ++it;
            continue;
          }
          ApplyScanRootSourceToGame(&game, root);
          if (game.title_id && title_name_cache) {
            NSString* key = XEFormatTitleIDHexLower(game.title_id);
            NSString* cached = [title_name_cache objectForKey:key];
            if (cached.length > 0) {
              game.title = NormalizeGameTitleForUI(std::string([cached UTF8String]));
            }
          }
          XELOGI("iOS library scan: discovered title='{}' title_id={:08X} "
                 "media_id={:08X} disc={}/{} content_type='{}' kind={} "
                 "external={} path='{}'",
                 game.title, game.title_id, game.media_id, game.disc_number, game.disc_count,
                 game.content_type_name, LibraryCandidateKindForLog(unique_path),
                 BoolForLog(game.is_external), unique_path.string());
          if (game.title_id) {
            auto existing = title_id_to_index.find(game.title_id);
            if (existing != title_id_to_index.end()) {
              IOSDiscoveredGame& existing_game = games[existing->second];
              int old_priority = IOSDiscFormatPriority(existing_game.path);
              int new_priority = IOSDiscFormatPriority(unique_path);
              ++merged_games;
              XELOGI("iOS library scan: merging duplicate title_id={:08X} "
                     "old_priority={} new_priority={} old_path='{}' "
                     "new_path='{}'",
                     game.title_id, old_priority, new_priority, existing_game.path.string(),
                     unique_path.string());
              if (new_priority < old_priority) {
                IOSDiscoveredGame previous_game = std::move(existing_game);
                existing_game = std::move(game);
                MergeDiscoveredGameDisc(&existing_game, previous_game);
              } else {
                MergeDiscoveredGameDisc(&existing_game, game);
              }
              ++it;
              continue;
            }
            title_id_to_index[game.title_id] = games.size();
          }
          ++added_games;
          games.push_back(std::move(game));
        } else {
          ++duplicate_paths;
          XELOGI("iOS library scan: skipping duplicate path='{}'", unique_path.string());
        }
      }

      ++it;
    }
    XELOGI("iOS library scan: finished root path='{}' external={} "
           "candidates={} added={} merged={} duplicate_paths={} "
           "hidden_paths={} hidden_identities={} build_failures={} "
           "pruned_directories={} scan_error='{}'",
           root.path.string(), BoolForLog(root.is_external), candidate_files, added_games,
           merged_games, duplicate_paths, hidden_paths, hidden_identities, build_failures,
           pruned_directories, ec ? ec.message() : "none");
  }

  for (auto& game : games) {
    if (!game.title_id) {
      continue;
    }
    std::error_code ec;
    if (std::filesystem::exists(xe_title_content_root(game.title_id), ec)) {
      game.has_installed_content = true;
    }
  }

  SortDiscoveredGames(&games);
  XELOGI("iOS library scan: complete games={}", games.size());
  return games;
}

void ApplyIOSCompatibilityDataToDiscoveredGames(
    NSDictionary* compat_data, std::vector<IOSDiscoveredGame>* games) {
  if (!games) {
    return;
  }
  for (auto& game : *games) {
    game.has_compat_info = false;
    game.compat_status.clear();
    game.compat_perf.clear();
    game.compat_notes.clear();
    if (!compat_data || !game.title_id) {
      continue;
    }
    NSString* key = XEFormatTitleIDHexUpper(game.title_id);
    NSDictionary* info = [compat_data objectForKey:key];
    if (!info) {
      continue;
    }
    NSString* title = xe_string_from_object(info[@"title"]);
    if (title.length > 0) {
      game.title = NormalizeGameTitleForUI(std::string([title UTF8String]));
    }
    NSDictionary* summary = xe_preferred_summary_from_compat_info(info);
    NSDictionary* source = summary ?: info;
    NSString* status = xe_string_from_object(source[@"status"]);
    NSString* perf = xe_string_from_object(source[@"perf"]);
    NSString* notes = xe_string_from_object(source[@"notes"]);
    if ([status isKindOfClass:[NSString class]] && status.length > 0) {
      game.has_compat_info = true;
      game.compat_status = std::string([status UTF8String]);
      game.compat_perf =
          [perf isKindOfClass:[NSString class]] ? std::string([perf UTF8String]) : "";
      game.compat_notes =
          [notes isKindOfClass:[NSString class]] ? std::string([notes UTF8String]) : "";
    }
  }
}

}  // namespace ui
}  // namespace xe
