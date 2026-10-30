/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_LIBRARY_H_
#define XENIA_UI_IOS_GAME_LIBRARY_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/ui/ios/app/windowed_app_context_ios.h"
#include "xenia/xbox.h"

namespace xe {
namespace ui {

struct IOSDiscoveredGame {
  std::filesystem::path path;
  std::string title;
  IOSGameSystem system = IOSGameSystem::kXbox360;
  XContentType content_type = XContentType::kInvalid;
  std::string content_type_name;
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;
  std::vector<uint8_t> icon_data;
  bool has_compat_info = false;
  std::string compat_status;
  std::string compat_perf;
  std::string compat_notes;
  bool has_installed_content = false;
  bool is_external = false;
  std::string external_location_name;
  struct Disc {
    std::filesystem::path path;
    std::string label;
    bool is_external = false;
    bool has_imported_source = false;
    bool has_external_source = false;
    std::string source_label;
    uint32_t media_id = 0;
    uint8_t disc_number = 0;
    uint8_t disc_count = 0;
  };
  std::vector<Disc> discs;
};

std::string ToLowerAsciiCopy(std::string value);
bool IsISOPath(const std::filesystem::path& path);
bool IsZarPath(const std::filesystem::path& path);
bool IsDefaultXexPath(const std::filesystem::path& path);
bool IsDefaultXbePath(const std::filesystem::path& path);
bool IsLikelyGodContainerFile(const std::filesystem::path& path);
// Ranks a game container by preferred format (lower = preferred): ZAR, GOD,
// ISO, then folder/loose. Single source of truth for both the library merge
// (pick the best copy to keep) and the disc list sort.
int IOSDiscFormatPriority(const std::filesystem::path& path);
bool IsIOSLaunchableContentType(XContentType content_type);
std::string IOSContentTypeDisplayName(XContentType content_type);
void SortDiscoveredGames(std::vector<IOSDiscoveredGame>* games);
std::string FormatTitleID(uint32_t title_id);
std::string NormalizeGameTitleForUI(const std::string& title);
void EnsureDiscoveredGameDiscList(IOSDiscoveredGame* game);
void MergeDiscoveredGameDisc(IOSDiscoveredGame* game,
                             const IOSDiscoveredGame& disc_game);
bool BuildDiscoveredGameFromPath(const std::filesystem::path& path,
                                 IOSDiscoveredGame* game_out);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_GAME_LIBRARY_H_
