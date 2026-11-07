/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/gamer_pictures.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/xam/xui_package.h"

namespace xe {
namespace kernel {
namespace xam {

std::vector<GamerPicture> ReadBuiltInGamerPictures(
    std::span<const uint8_t> xam_xex) {
  const auto image = cpu::ReadXexHostImage(xam_xex);
  if (!image) {
    return {};
  }
  const auto files = ReadXuiPackage(image->Resource("shrdres"));
  if (!files) {
    return {};
  }
  // 64_fffe07d10002000500010005.png and its 32_ twin.
  constexpr std::string_view kDashboardPrefix = "fffe07d1";
  std::map<std::string, GamerPicture> pictures;
  for (const auto& file : *files) {
    std::string lower(file.name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char c) { return char(std::tolower(uint8_t(c))); });
    const std::string_view name = lower;
    if (name.size() != 3 + 24 + 4 || name.substr(27) != ".png" ||
        name[2] != '_' || name.substr(3, 8) != kDashboardPrefix) {
      continue;
    }
    const std::string_view size = name.substr(0, 2);
    if (size != "64" && size != "32") {
      continue;
    }
    std::string key(name.substr(3, 24));
    if (!std::all_of(key.begin(), key.end(),
                     [](char c) { return std::isxdigit(uint8_t(c)); })) {
      continue;
    }
    std::transform(key.begin(), key.end(), key.begin(),
                   [](char c) { return char(std::toupper(uint8_t(c))); });
    GamerPicture& picture = pictures[key];
    picture.key = key;
    (size == "64" ? picture.large : picture.small)
        .assign(file.data.begin(), file.data.end());
  }
  std::vector<GamerPicture> result;
  for (auto& [key, picture] : pictures) {
    if (!picture.large.empty() && !picture.small.empty()) {
      result.push_back(std::move(picture));
    }
  }
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
