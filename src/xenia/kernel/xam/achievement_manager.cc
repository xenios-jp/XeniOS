/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/base/platform.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/achievement_backends/gpd_achievement_backend.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
#if XE_PLATFORM_IOS
#include "xenia/ui/windowed_app_context_ios.h"
#endif
#if !XE_PLATFORM_IOS
#include "xenia/ui/audio_helper.h"
#endif
#include "xenia/ui/imgui_guest_notification.h"

DEFINE_bool(show_achievement_notification, true,
            "Show achievement notification on screen.", "UI");

DEFINE_bool(achievement_notification_position_by_game, false,
            "Use game-specified notification position for achievements. "
            "When disabled, achievements always appear at center-bottom.",
            "UI");

DEFINE_string(
    default_achievements_backend, "GPD",
    "Defines which achievements backend should be used as an default. "
    "Possible options: GPD.",
    "Kernel");

DECLARE_int32(user_language);

namespace xe {
namespace kernel {
namespace xam {

AchievementManager::AchievementManager() {
  default_achievements_backend_ = std::make_unique<GpdAchievementBackend>();

  // Add any optional backend here.
};
void AchievementManager::EarnAchievement(const uint32_t user_index,
                                         const uint32_t title_id,
                                         const uint32_t achievement_id) const {
  const auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return;
  }

  EarnAchievement(user->xuid(), title_id, achievement_id);
};

void AchievementManager::EarnAchievement(const uint64_t xuid,
                                         const uint32_t title_id,
                                         const uint32_t achievement_id) const {
  if (!DoesAchievementExist(achievement_id)) {
    XELOGW(
        "{}: Achievement with ID: {} for title: {:08X} doesn't exist in "
        "database!",
        __func__, achievement_id, title_id);
    return;
  }
  // Always send request to unlock in 3PP backends. It's up to them to check if
  // achievement was unlocked
  for (auto& backend : achievement_backends_) {
    backend->EarnAchievement(xuid, title_id, achievement_id);
  }

  if (default_achievements_backend_->IsAchievementUnlocked(xuid, title_id,
                                                           achievement_id)) {
    return;
  }

  default_achievements_backend_->EarnAchievement(xuid, title_id,
                                                 achievement_id);

  if (!cvars::show_achievement_notification) {
    return;
  }

  const auto achievement = default_achievements_backend_->GetAchievementInfo(
      xuid, title_id, achievement_id);

  if (!achievement) {
    // Something went really wrong!
    return;
  }
  ShowAchievementEarnedNotification(&achievement.value(), xuid, title_id);
}

void AchievementManager::LoadTitleAchievements(const uint64_t xuid) const {
  default_achievements_backend_->LoadAchievementsData(xuid);
}

const std::optional<Achievement> AchievementManager::GetAchievementInfo(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  return default_achievements_backend_->GetAchievementInfo(xuid, title_id,
                                                           achievement_id);
}

const std::vector<Achievement> AchievementManager::GetTitleAchievements(
    const uint64_t xuid, const uint32_t title_id) const {
  return default_achievements_backend_->GetTitleAchievements(xuid, title_id);
}

const std::span<const uint8_t> AchievementManager::GetAchievementIcon(
    const uint64_t xuid, const uint32_t title_id,
    const uint32_t achievement_id) const {
  return default_achievements_backend_->GetAchievementIcon(xuid, title_id,
                                                           achievement_id);
}

const std::optional<TitleAchievementsProfileInfo>
AchievementManager::GetTitleAchievementsInfo(const uint64_t xuid,
                                             const uint32_t title_id) const {
  TitleAchievementsProfileInfo info = {};

  const auto achievements = GetTitleAchievements(xuid, title_id);

  if (achievements.empty()) {
    return std::nullopt;
  }

  info.achievements_count = static_cast<uint32_t>(achievements.size());

  for (const auto& entry : achievements) {
    if (!entry.IsUnlocked()) {
      continue;
    }

    info.unlocked_achievements_count++;
    info.gamerscore += entry.gamerscore;
  }

  return info;
}

bool AchievementManager::DoesAchievementExist(
    const uint32_t achievement_id) const {
  return kernel_state()->xam_state()->spa_info()->GetAchievement(
      achievement_id);
}

void AchievementManager::ShowAchievementEarnedNotification(
    const Achievement* achievement, uint64_t xuid, uint32_t title_id) const {
  auto sanitize_utf16 = [](const std::u16string& value) {
    std::string utf8 = xe::to_utf8(value);
    if (!utf8.empty() && utf8.back() == '\0') {
      utf8.pop_back();
    }
    return utf8;
  };

  const std::string achievement_name = sanitize_utf16(achievement->achievement_name);
  const std::string achievement_detail = sanitize_utf16(achievement->unlocked_description);
  const std::string description =
      fmt::format("{}G - {}", achievement->gamerscore, achievement_name);

  const Emulator* emulator = kernel_state()->emulator();
  ui::WindowedAppContext& app_context =
      emulator->display_window()->app_context();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

  if (!imgui_drawer) {
#if XE_PLATFORM_IOS
    auto& ios_app_context =
        static_cast<ui::IOSWindowedAppContext&>(app_context);
    ui::IOSAchievementNotificationData notification;
    notification.title = "Achievement unlocked";
    notification.subtitle = achievement_name;
    notification.description = achievement_detail;
    notification.gamerscore = achievement->gamerscore;
    auto icon = default_achievements_backend_->GetAchievementIcon(
        xuid, title_id, achievement->achievement_id);
    notification.icon_data.assign(icon.begin(), icon.end());
    ios_app_context.CallInUIThread([&ios_app_context, notification]() {
      ios_app_context.NotifyAchievementUnlocked(notification);
    });
#else
    // No UI drawer available; just log.
    XELOGI("Achievement unlocked (no UI): {}", description);
#endif
    return;
  }

  // Use game-specified position if enabled, otherwise default to center-bottom
  const uint8_t position = cvars::achievement_notification_position_by_game
                               ? kernel_state()->notification_position_
                               : 2;

  app_context.CallInUIThread([imgui_drawer, description, position]() {
  // TODO(wmarti): Implement iOS audio helper (AVAudioPlayer) to restore this.
#if !XE_PLATFORM_IOS
    // Play achievement sound
    ui::AudioHelper::Instance().PlayAchievementSound();
#endif

    // Show notification
    new ui::AchievementNotificationWindow(imgui_drawer, "Achievement unlocked",
                                          description, 0, position);
  });
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
