/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_LAYOUT_LIBRARY_IOS_H_
#define XENIA_UI_IOS_TOUCH_LAYOUT_LIBRARY_IOS_H_

#import <UIKit/UIKit.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "xenia/hid/touch/touch_layout_ios.h"

namespace xe {
namespace ui {

inline constexpr const char* kOfficialTouchLayoutFPSCompactLocalID = "fps_compact";
inline constexpr const char* kOfficialTouchLayoutFPSFullLocalID = "fps_full";
inline constexpr const char* kOfficialTouchLayoutActionAdventureLocalID = "action_adventure";
inline constexpr const char* kOfficialTouchLayoutArcadeDpadLocalID = "arcade_dpad";
inline constexpr const char* kOfficialTouchLayoutDrivingLocalID = "driving";
inline constexpr const char* kOfficialTouchLayoutMinimalStarterLocalID = "minimal_starter";
inline constexpr const char* kTouchLayoutAssignmentSection = "TouchLayoutAssignment";

struct IOSTouchLocalLayoutInfo {
  std::filesystem::path path;
  std::string local_id;
  std::string display_name;
  std::string author;
  bool official = false;
  // Cached layout content. Loaded by availableLocalTouchLayouts so the
  // library populator can render thumbnails without re-reading from disk.
  xe::hid::touch::IOSTouchLayoutModel layout;
};

bool IsOfficialTouchLayoutLocalID(const std::string& local_id);
size_t OfficialTouchLayoutPresetSortOrder(const std::string& local_id);
std::string NormalizeOfficialTouchLayoutBaseTemplate(std::string base_template);
std::string MakeTouchLayoutSlug(std::string value);
bool TryNormalizeConfiguredTouchLayoutLocalID(const std::string& configured_local_id,
                                              std::string* normalized_local_id_out);
std::string TouchLayoutBaseTemplateForTable(const toml::table& table);
std::string DefaultOfficialTouchLayoutLocalID();
xe::hid::touch::IOSTouchLayoutModel MakeTouchLayoutSeedModelForTable(const toml::table& table);
xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSTouchLayoutModelForLocalID(
    const std::string& local_id);
xe::hid::touch::IOSTouchLayoutModel MakeOfficialIOSTouchLayoutModel();
UIImage* RenderTouchLayoutThumbnail(const xe::hid::touch::IOSTouchLayoutModel& layout, CGSize size);
std::string ReadGlobalTouchLayoutAssignment();
void WriteGlobalTouchLayoutAssignment(const std::string& local_id);
std::string ReadTitleTouchLayoutAssignment(uint32_t title_id);
bool IsFavoriteTouchLayoutLocalID(const std::string& local_id);
void SetFavoriteTouchLayoutLocalID(const std::string& local_id, bool favorite);
bool TouchLayoutContentMatches(const xe::hid::touch::IOSTouchLayoutModel& a,
                               const xe::hid::touch::IOSTouchLayoutModel& b);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_TOUCH_LAYOUT_LIBRARY_IOS_H_
