/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/shared/ios_edge_adapter.h"

#include "xenia/ui/ios/launcher/ios_game_library.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <utility>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/ui/ios/launcher/ios_content_management.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"
#include "xenia/vfs/iso_metadata.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/vfs/xex_metadata.h"
#include "xenia/vfs/zar_metadata.h"
#include "xenia/xbox.h"

namespace xe {
namespace ui {

std::string ToLowerAsciiCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return value;
}

bool LooksLikeHexIdentifier(const std::string& value, size_t min_length = 8,
                            size_t max_length = 32) {
  if (value.size() < min_length || value.size() > max_length) {
    return false;
  }
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool IsISOPath(const std::filesystem::path& path) {
  return ToLowerAsciiCopy(path.extension().string()) == ".iso";
}

bool IsZarPath(const std::filesystem::path& path) {
  return ToLowerAsciiCopy(path.extension().string()) == ".zar";
}

bool IsDefaultXexPath(const std::filesystem::path& path) {
  return ToLowerAsciiCopy(path.filename().string()) == "default.xex";
}

bool IsDefaultXbePath(const std::filesystem::path& path) {
  return ToLowerAsciiCopy(path.filename().string()) == "default.xbe";
}

bool IsLaunchableXContentDirectoryName(const std::string& name_lower) {
  return name_lower == "00004000" ||  // Installed Game / GOD
         name_lower == "00007000" ||  // Xbox 360 Title
         name_lower == "00080000" ||  // Game Demo
         name_lower == "000d0000" ||  // XBLA
         name_lower == "02000000";    // Community Game
}

bool IsLikelyLaunchableXContentPath(const std::filesystem::path& path) {
  if (!path.has_filename()) {
    return false;
  }
  std::filesystem::path parent = path.parent_path();
  while (!parent.empty()) {
    std::string name_lower = ToLowerAsciiCopy(parent.filename().string());
    if (IsLaunchableXContentDirectoryName(name_lower)) {
      return true;
    }
    std::filesystem::path next = parent.parent_path();
    if (next == parent) {
      break;
    }
    parent = next;
  }
  return false;
}

bool LooksLikeHexContentFilename(const std::filesystem::path& path) {
  const std::string filename = path.filename().string();
  return LooksLikeHexIdentifier(filename, 8, 40);
}

bool IsLikelyGodContainerFile(const std::filesystem::path& path) {
  if (IsLikelyLaunchableXContentPath(path)) {
    return true;
  }
  if (path.has_extension()) {
    return false;
  }
  return LooksLikeHexContentFilename(path);
}

bool IsIOSLaunchableContentType(XContentType content_type) {
  switch (content_type) {
    case XContentType::kInstalledGame:
    case XContentType::kXbox360Title:
    case XContentType::kArcadeTitle:
    case XContentType::kGameDemo:
    case XContentType::kCommunityGame:
      return true;
    default:
      return false;
  }
}

std::string IOSContentTypeDisplayName(XContentType content_type) {
  switch (content_type) {
    case XContentType::kArcadeTitle:
      return "XBLA";
    case XContentType::kGameDemo:
      return "Demo";
    case XContentType::kCommunityGame:
      return "Community Game";
    case XContentType::kInstalledGame:
      return "Installed Game";
    case XContentType::kXbox360Title:
      return "Xbox 360 Title";
    default:
      return std::string();
  }
}

std::string LibraryFallbackTitleFromPath(const std::filesystem::path& path) {
  if (IsDefaultXexPath(path) || IsDefaultXbePath(path)) {
    std::filesystem::path parent = path.parent_path();
    while (!parent.empty()) {
      std::string candidate = parent.filename().string();
      std::string candidate_lower = ToLowerAsciiCopy(candidate);
      if (!candidate.empty() && !LooksLikeHexIdentifier(candidate) &&
          candidate_lower != "content" && candidate_lower != "games" &&
          candidate_lower != "files" && candidate_lower != "default") {
        return candidate;
      }
      std::filesystem::path next = parent.parent_path();
      if (next == parent) {
        break;
      }
      parent = next;
    }
  }

  std::string stem = path.stem().string();
  if (!stem.empty()) {
    return stem;
  }
  return path.filename().string();
}

NSString* NormalizeGameTitleForUI(NSString* title) {
  if (!title || title.length == 0) {
    return title;
  }
  if ([title rangeOfCharacterFromSet:[NSCharacterSet whitespaceCharacterSet]].location !=
      NSNotFound) {
    return title;
  }
  NSRange letter_range = [title rangeOfCharacterFromSet:[NSCharacterSet letterCharacterSet]];
  if (letter_range.location == NSNotFound) {
    return title;
  }
  NSRange lower_range =
      [title rangeOfCharacterFromSet:[NSCharacterSet lowercaseLetterCharacterSet]];
  if (lower_range.location != NSNotFound) {
    return title;
  }
  NSCharacterSet* roman_set = [NSCharacterSet characterSetWithCharactersInString:@"IVXLCDM"];
  NSCharacterSet* non_roman_set = [roman_set invertedSet];
  if ([title rangeOfCharacterFromSet:non_roman_set].location == NSNotFound) {
    return title;
  }
  return [title localizedCapitalizedString];
}

bool IsUnhelpfulMetadataTitle(const std::string& title) {
  const std::string title_lower = ToLowerAsciiCopy(title);
  if (title_lower.empty() || title_lower == "default" || title_lower == "unknown" ||
      title_lower == "unknown title") {
    return true;
  }
  // XEX/PE metadata "module_name" is usually the raw executable filename
  // (e.g. "default.xex", "default.pe", "nhlzf.exe") rather than a real title, so
  // reject any executable/module-style name and fall back to the file name.
  static const std::string kExecutableSuffixes[] = {".xex", ".xbe", ".exe", ".pe", ".elf", ".dll"};
  for (const std::string& suffix : kExecutableSuffixes) {
    if (title_lower.size() >= suffix.size() &&
        title_lower.compare(title_lower.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return true;
    }
  }
  return false;
}

std::string DisplayNameFromXexMetadata(const std::filesystem::path& path,
                                       const std::optional<xe::vfs::XexMetadata>& metadata) {
  if (metadata.has_value() && !IsDefaultXexPath(path) &&
      !IsUnhelpfulMetadataTitle(metadata->module_name)) {
    return metadata->module_name;
  }
  return LibraryFallbackTitleFromPath(path);
}

void SortDiscoveredGames(std::vector<IOSDiscoveredGame>* games) {
  if (!games) {
    return;
  }
  std::sort(games->begin(), games->end(),
            [](const IOSDiscoveredGame& a, const IOSDiscoveredGame& b) {
              if (a.title == b.title) {
                return a.path.filename().string() < b.path.filename().string();
              }
              return a.title < b.title;
            });
}

std::string FormatTitleID(uint32_t title_id) {
  if (!title_id) {
    return std::string();
  }
  char buffer[9] = {};
  std::snprintf(buffer, sizeof(buffer), "%08X", title_id);
  return std::string(buffer);
}

std::string DiscFormatTag(const std::filesystem::path& path) {
  if (IsZarPath(path)) {
    return "ZAR";
  }
  if (IsISOPath(path)) {
    return "ISO";
  }
  if (IsLikelyGodContainerFile(path)) {
    return "GOD";
  }
  if (IsDefaultXexPath(path) || IsDefaultXbePath(path)) {
    return "Folder";
  }
  return "";
}

std::string DiscLabelForGame(const IOSDiscoveredGame& game) {
  // Surface the format (ISO/ZAR/GOD/Folder) so that when several copies of the
  // same title are listed under "Discs" the user can tell them apart and pick
  // which one to boot.
  const std::string format = DiscFormatTag(game.path);
  if (game.disc_number > 0) {
    std::string label = game.disc_count > 1 ? "Disc " + std::to_string(game.disc_number) + " of " +
                                                  std::to_string(game.disc_count)
                                            : "Disc " + std::to_string(game.disc_number);
    if (!format.empty()) {
      label += " - " + format;
    }
    return label;
  }
  if (!format.empty()) {
    return format;
  }
  std::string filename = game.path.filename().string();
  return filename.empty() ? "Launch Item" : filename;
}

std::string DiscSourceLabel(bool has_imported_source, bool has_external_source) {
  if (has_imported_source && has_external_source) {
    return "Imported + External";
  }
  if (has_external_source) {
    return "External";
  }
  return "Imported";
}

int IOSDiscFormatPriority(const std::filesystem::path& path) {
  if (IsZarPath(path)) {
    return 0;
  }
  if (IsLikelyGodContainerFile(path)) {
    return 1;
  }
  if (IsISOPath(path)) {
    return 2;
  }
  return 3;
}

IOSDiscoveredGame::Disc DiscFromGame(const IOSDiscoveredGame& game) {
  IOSDiscoveredGame::Disc disc;
  disc.path = game.path;
  disc.label = DiscLabelForGame(game);
  disc.is_external = game.is_external;
  disc.has_imported_source = !game.is_external;
  disc.has_external_source = game.is_external;
  disc.source_label = DiscSourceLabel(disc.has_imported_source, disc.has_external_source);
  disc.media_id = game.media_id;
  disc.disc_number = game.disc_number;
  disc.disc_count = game.disc_count;
  return disc;
}

bool DiscSortLess(const IOSDiscoveredGame::Disc& a, const IOSDiscoveredGame::Disc& b) {
  const bool a_numbered = a.disc_number > 0;
  const bool b_numbered = b.disc_number > 0;
  if (a_numbered != b_numbered) {
    return a_numbered;
  }
  if (a.disc_number != b.disc_number) {
    return a.disc_number < b.disc_number;
  }
  if (a.media_id != b.media_id) {
    return a.media_id < b.media_id;
  }
  if (a.is_external != b.is_external) {
    return !a.is_external;
  }
  return a.path.filename().string() < b.path.filename().string();
}

bool DiscLaunchLess(const IOSDiscoveredGame::Disc& a, const IOSDiscoveredGame::Disc& b) {
  if (a.disc_number && b.disc_number && a.disc_number != b.disc_number) {
    return a.disc_number < b.disc_number;
  }
  const int a_format_priority = IOSDiscFormatPriority(a.path);
  const int b_format_priority = IOSDiscFormatPriority(b.path);
  if (a_format_priority != b_format_priority) {
    return a_format_priority < b_format_priority;
  }
  if (a.is_external != b.is_external) {
    return !a.is_external;
  }
  return DiscSortLess(a, b);
}

void ApplyPreferredLaunchDiscToGame(IOSDiscoveredGame* game) {
  if (!game || game->discs.empty()) {
    return;
  }
  auto preferred = std::min_element(game->discs.begin(), game->discs.end(), DiscLaunchLess);
  if (preferred == game->discs.end()) {
    return;
  }
  game->path = preferred->path;
  game->media_id = preferred->media_id;
  game->disc_number = preferred->disc_number;
  game->disc_count = preferred->disc_count;
  game->is_external = preferred->is_external;
}

void MergeDiscSourceAndPreferredLaunch(IOSDiscoveredGame::Disc* target,
                                       const IOSDiscoveredGame::Disc& incoming) {
  if (!target) {
    return;
  }
  const bool has_imported_source = target->has_imported_source || incoming.has_imported_source ||
                                   (!target->is_external && target->source_label.empty()) ||
                                   (!incoming.is_external && incoming.source_label.empty());
  const bool has_external_source = target->has_external_source || incoming.has_external_source ||
                                   (target->is_external && target->source_label.empty()) ||
                                   (incoming.is_external && incoming.source_label.empty());
  if (DiscLaunchLess(incoming, *target)) {
    const bool preserved_imported_source = has_imported_source;
    const bool preserved_external_source = has_external_source;
    *target = incoming;
    target->has_imported_source = preserved_imported_source;
    target->has_external_source = preserved_external_source;
  } else {
    target->has_imported_source = has_imported_source;
    target->has_external_source = has_external_source;
  }
  target->source_label = DiscSourceLabel(target->has_imported_source, target->has_external_source);
}

void EnsureDiscoveredGameDiscList(IOSDiscoveredGame* game) {
  if (!game || game->path.empty()) {
    return;
  }
  if (game->discs.empty()) {
    game->discs.push_back(DiscFromGame(*game));
  }
  std::sort(game->discs.begin(), game->discs.end(), DiscSortLess);
  ApplyPreferredLaunchDiscToGame(game);
}

void MergeDiscoveredGameDisc(IOSDiscoveredGame* game, const IOSDiscoveredGame& disc_game) {
  if (!game || disc_game.path.empty()) {
    return;
  }
  EnsureDiscoveredGameDiscList(game);
  std::vector<IOSDiscoveredGame::Disc> incoming = disc_game.discs;
  if (incoming.empty()) {
    incoming.push_back(DiscFromGame(disc_game));
  }
  for (IOSDiscoveredGame::Disc& disc : incoming) {
    // Keep every distinct copy (a different format or a genuinely different
    // disc) as its own selectable entry; only fold in exact-path duplicates so
    // the user can boot whichever ISO/ZAR/disc they want.
    auto existing = std::find_if(
        game->discs.begin(), game->discs.end(),
        [&](const IOSDiscoveredGame::Disc& candidate) { return candidate.path == disc.path; });
    if (existing == game->discs.end()) {
      game->discs.push_back(std::move(disc));
    } else {
      MergeDiscSourceAndPreferredLaunch(&*existing, disc);
    }
  }
  std::sort(game->discs.begin(), game->discs.end(), DiscSortLess);
  ApplyPreferredLaunchDiscToGame(game);
}

std::string NormalizeGameTitleForUI(const std::string& title) {
  NSString* normalized = NormalizeGameTitleForUI(ToNSString(title));
  return normalized ? std::string([normalized UTF8String]) : title;
}

bool BuildDiscoveredGameFromPath(const std::filesystem::path& path, IOSDiscoveredGame* game_out) {
  if (!game_out) {
    return false;
  }

  IOSDiscoveredGame game;
  game.path = path;
  if (IsISOPath(path)) {
    auto metadata = xe::vfs::ExtractIsoMetadata(path);
    if (metadata.has_value()) {
      game.system = IOSGameSystem::kXbox360;
      game.title_id = metadata->title_id;
      game.media_id = metadata->media_id;
      game.disc_number = metadata->disc_number;
      game.disc_count = metadata->disc_count;
      game.title = NormalizeGameTitleForUI(DisplayNameFromXexMetadata(path, metadata));
      EnsureDiscoveredGameDiscList(&game);
      *game_out = std::move(game);
      return true;
    }

    game.system = IOSGameSystem::kXbox360;
    game.title = NormalizeGameTitleForUI(LibraryFallbackTitleFromPath(path));
    EnsureDiscoveredGameDiscList(&game);
    *game_out = std::move(game);
    return true;
  }

  if (IsZarPath(path)) {
    auto metadata = xe::vfs::ExtractZarMetadata(path);
    if (metadata.has_value()) {
      game.system = IOSGameSystem::kXbox360;
      game.title_id = metadata->title_id;
      game.media_id = metadata->media_id;
      game.disc_number = metadata->disc_number;
      game.disc_count = metadata->disc_count;
      game.title = NormalizeGameTitleForUI(DisplayNameFromXexMetadata(path, metadata));
      EnsureDiscoveredGameDiscList(&game);
      *game_out = std::move(game);
      return true;
    }

    game.system = IOSGameSystem::kXbox360;
    game.title = NormalizeGameTitleForUI(LibraryFallbackTitleFromPath(path));
    EnsureDiscoveredGameDiscList(&game);
    *game_out = std::move(game);
    return true;
  }

  if (IsDefaultXexPath(path)) {
    game.system = IOSGameSystem::kXbox360;
    auto metadata = xe::vfs::ExtractXexMetadata(path);
    if (metadata.has_value()) {
      game.title_id = metadata->title_id;
      game.media_id = metadata->media_id;
      game.disc_number = metadata->disc_number;
      game.disc_count = metadata->disc_count;
    }
    game.title = NormalizeGameTitleForUI(DisplayNameFromXexMetadata(path, metadata));
    EnsureDiscoveredGameDiscList(&game);
    *game_out = std::move(game);
    return true;
  }

  if (!IsLikelyGodContainerFile(path)) {
    return false;
  }

  auto metadata = xe::vfs::ExtractStfsMetadata(path);
  if (!metadata.has_value()) {
    return false;
  }

  if (xe::ui::IOSContentDataFileCount(path) > 0 && !HasContentSidecarDataDirectory(path)) {
    XELOGW("iOS: Skipping XContent package missing .data sidecar: {}", xe::strip_pii(xe::path_to_utf8(path)));
    return false;
  }

  xe::XContentType content_type = static_cast<xe::XContentType>(metadata->content_type);
  if (!IsIOSLaunchableContentType(content_type)) {
    return false;
  }

  game.system = IOSGameSystem::kXbox360;
  game.content_type = content_type;
  game.content_type_name = IOSContentTypeDisplayName(content_type);
  game.title_id = metadata->title_id;
  game.media_id = metadata->media_id;
  game.disc_number = metadata->disc_number;
  game.disc_count = metadata->disc_count;
  std::string display_name = metadata->display_name;
  if (display_name.empty()) {
    display_name = metadata->title_name;
  }
  if (display_name.empty()) {
    game.title = LibraryFallbackTitleFromPath(path);
  } else {
    game.title = display_name;
  }
  game.title = NormalizeGameTitleForUI(game.title);

  game.icon_data = metadata->icon_data;

  EnsureDiscoveredGameDiscList(&game);
  *game_out = std::move(game);
  return true;
}

}  // namespace ui
}  // namespace xe
