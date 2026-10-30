/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_ACHIEVEMENT_NOTIFICATION_PAYLOAD_H_
#define XENIA_UI_ACHIEVEMENT_NOTIFICATION_PAYLOAD_H_

#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace ui {

struct AchievementNotificationPayload {
  uint8_t user_index = 0;
  uint8_t position_id = 2;
  uint64_t xuid = 0;
  uint32_t title_id = 0;
  uint32_t achievement_id = 0;
  uint32_t gamerscore = 0;
  std::string title;
  std::string description;
  std::vector<uint8_t> icon_data;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_ACHIEVEMENT_NOTIFICATION_PAYLOAD_H_
