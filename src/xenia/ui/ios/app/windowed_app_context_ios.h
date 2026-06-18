/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOWED_APP_CONTEXT_IOS_H_
#define XENIA_UI_WINDOWED_APP_CONTEXT_IOS_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "xenia/ui/achievement_notification_payload.h"
#include "xenia/ui/windowed_app_context.h"

#ifdef __OBJC__
@class UIView;
@class UIViewController;
#else
typedef struct objc_object UIView;
typedef struct objc_object UIViewController;
#endif

namespace xe {
namespace hid {
struct X_INPUT_STATE;
namespace touch {
class IOSTouchRuntimeModel;
}  // namespace touch
}  // namespace hid
namespace ui {

enum class IOSGameSystem : uint8_t {
  kXbox360 = 0,
};

struct IOSProfileSummary {
  uint64_t xuid = 0;
  std::string gamertag;
  bool signed_in = false;
  uint8_t signed_in_slot = 0xFF;
};

struct IOSAchievementEntry {
  uint32_t achievement_id = 0;
  uint32_t gamerscore = 0;
  uint32_t flags = 0;
  bool unlocked = false;
  bool unlocked_online = false;
  bool show_unachieved = false;
  std::string title;
  std::string unlocked_description;
  std::string locked_description;
  std::vector<uint8_t> icon_data;
};

struct IOSAchievementsSnapshot {
  uint32_t title_id = 0;
  std::string title_name;
  uint32_t achievements_total = 0;
  uint32_t achievements_unlocked = 0;
  uint32_t gamerscore_total = 0;
  uint32_t gamerscore_earned = 0;
  std::vector<IOSAchievementEntry> achievements;
};

struct IOSPatchEntrySummary {
  size_t patch_index = 0;
  std::string name;
  std::string description;
  std::string author;
  bool is_enabled = false;
};

struct IOSPatchFileSummary {
  uint32_t title_id = 0;
  std::string filename;
  std::string display_name;
  std::string title_name;
  std::vector<IOSPatchEntrySummary> patches;
};

struct IOSPatchDiscoverySummary {
  std::string directory_path;
  bool directory_exists = false;
  size_t bundled_files = 0;
  size_t scanned_files = 0;
  size_t candidate_files = 0;
  size_t matching_files = 0;
  size_t parse_failures = 0;
  size_t title_mismatches = 0;
};

class IOSWindowedAppContext final : public WindowedAppContext {
 public:
  IOSWindowedAppContext();
  ~IOSWindowedAppContext();

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  // The Metal-backed rendering view, set by the app delegate after UIKit
  // hierarchy creation.
  UIView* metal_view() const { return metal_view_; }
  void set_metal_view(UIView* view) { metal_view_ = view; }

  UIViewController* view_controller() const { return view_controller_; }
  void set_view_controller(UIViewController* vc) { view_controller_ = vc; }

  // Callback invoked when the user selects a game file to launch.
  using GameLaunchCallback = std::function<void(const std::string&)>;
  void set_game_launch_callback(GameLaunchCallback callback) {
    game_launch_callback_ = std::move(callback);
  }
  void LaunchGame(const std::string& path) {
    if (game_launch_callback_) {
      game_launch_callback_(path);
    }
  }

  using ProfilesListCallback = std::function<std::vector<IOSProfileSummary>()>;
  using ProfileCreateCallback = std::function<uint64_t(const std::string&)>;
  using ProfileSignInCallback = std::function<bool(uint64_t)>;
  using GameTerminateCallback = std::function<bool()>;
  using TitleUpdateInstallCallback = std::function<bool(const std::string&, std::string*, bool*)>;
  using GameExitedCallback = std::function<void()>;
  using ProfileServicesReadyCallback = std::function<void()>;
  using ProfileServicesPrepareCallback = std::function<void()>;
  using SignInUIPromptCallback = std::function<bool(uint32_t, uint32_t)>;
  using AchievementsSnapshotCallback = std::function<IOSAchievementsSnapshot(uint32_t, uint32_t)>;
  using AchievementsUIPromptCallback = std::function<bool(uint32_t, uint32_t)>;
  using AchievementNotificationCallback =
      std::function<bool(const AchievementNotificationPayload&)>;
  using PatchFilesListCallback = std::function<std::vector<IOSPatchFileSummary>(uint32_t)>;
  using PatchDiscoverySummaryCallback = std::function<IOSPatchDiscoverySummary(uint32_t)>;
  using PatchSetEnabledCallback =
      std::function<bool(uint32_t, const std::string&, size_t, bool, std::string*)>;
  using ControllerStateCallback = std::function<bool(uint32_t, hid::X_INPUT_STATE*)>;
  using MessageBoxPromptCallback =
      std::function<bool(const std::string&, const std::string&, const std::vector<std::string>&,
                         uint32_t, uint32_t*)>;
  using KeyboardPromptCallback = std::function<bool(const std::string&, const std::string&,
                                                    const std::string&, std::string*, bool*)>;
  using GameplayInputBlockedCallback = std::function<void(bool)>;
  using GuestDisplayAspectRatioCallback = std::function<std::pair<uint32_t, uint32_t>()>;
  using GuestDisplayRefreshCapGetter = std::function<bool()>;
  using GuestDisplayRefreshCapSetter = std::function<void(bool)>;
  using PresentLetterboxGetter = std::function<bool()>;
  using PresentLetterboxSetter = std::function<void(bool)>;

  void set_profiles_list_callback(ProfilesListCallback callback) {
    profiles_list_callback_ = std::move(callback);
  }
  std::vector<IOSProfileSummary> ListProfiles() const {
    if (!ProfileServicesReady()) {
      PrepareProfileServices();
    }
    if (!profiles_list_callback_) {
      return {};
    }
    return profiles_list_callback_();
  }

  void set_profile_create_callback(ProfileCreateCallback callback) {
    profile_create_callback_ = std::move(callback);
  }
  uint64_t CreateProfile(const std::string& gamertag) const {
    if (!profile_create_callback_) {
      return 0;
    }
    return profile_create_callback_(gamertag);
  }

  void set_profile_sign_in_callback(ProfileSignInCallback callback) {
    profile_sign_in_callback_ = std::move(callback);
  }
  bool SignInProfile(uint64_t xuid) const {
    if (!profile_sign_in_callback_) {
      return false;
    }
    return profile_sign_in_callback_(xuid);
  }

  void set_profile_services_ready_callback(ProfileServicesReadyCallback callback) {
    profile_services_ready_callback_ = std::move(callback);
  }
  bool ProfileServicesReady() const {
    return profile_services_ready_.load(std::memory_order_acquire);
  }
  void SetProfileServicesReady(bool ready) const {
    profile_services_ready_.store(ready, std::memory_order_release);
  }
  void NotifyProfileServicesReady() const {
    SetProfileServicesReady(true);
    if (profile_services_ready_callback_) {
      profile_services_ready_callback_();
    }
  }

  // Asks the app to begin bringing up profile/content services (a lightweight
  // headless emulator init) without blocking, so they are ready by the time the
  // user acts -- e.g. opening Manage Content well before picking a package.
  void set_profile_services_prepare_callback(ProfileServicesPrepareCallback callback) {
    profile_services_prepare_callback_ = std::move(callback);
  }
  void PrepareProfileServices() const {
    if (profile_services_prepare_callback_) {
      profile_services_prepare_callback_();
    }
  }

  void set_game_terminate_callback(GameTerminateCallback callback) {
    game_terminate_callback_ = std::move(callback);
  }
  bool TerminateCurrentGame() const {
    if (!game_terminate_callback_) {
      return false;
    }
    return game_terminate_callback_();
  }

  void set_title_update_install_callback(TitleUpdateInstallCallback callback) {
    title_update_install_callback_ = std::move(callback);
  }
  bool InstallTitleUpdate(const std::string& path, std::string* status_out,
                          bool* not_title_update_out) const {
    if (not_title_update_out) {
      *not_title_update_out = false;
    }
    if (!title_update_install_callback_) {
      if (status_out) {
        *status_out = "Title update installation is unavailable.";
      }
      return false;
    }
    return title_update_install_callback_(path, status_out, not_title_update_out);
  }

  void set_game_exited_callback(GameExitedCallback callback) {
    game_exited_callback_ = std::move(callback);
  }
  void NotifyGameExited() {
    if (game_exited_callback_) {
      game_exited_callback_();
    }
  }

  void set_signin_ui_prompt_callback(SignInUIPromptCallback callback) {
    signin_ui_prompt_callback_ = std::move(callback);
  }
  bool PromptSignInUI(uint32_t user_index, uint32_t users_needed) const {
    if (!signin_ui_prompt_callback_) {
      return false;
    }
    return signin_ui_prompt_callback_(user_index, users_needed);
  }

  void set_achievements_snapshot_callback(AchievementsSnapshotCallback callback) {
    achievements_snapshot_callback_ = std::move(callback);
  }
  IOSAchievementsSnapshot LoadAchievementsSnapshot(uint32_t user_index, uint32_t title_id) const {
    if (!achievements_snapshot_callback_) {
      return {};
    }
    return achievements_snapshot_callback_(user_index, title_id);
  }

  void set_achievements_ui_prompt_callback(AchievementsUIPromptCallback callback) {
    achievements_ui_prompt_callback_ = std::move(callback);
  }
  bool PromptAchievementsUI(uint32_t user_index, uint32_t title_id) const {
    if (!achievements_ui_prompt_callback_) {
      return false;
    }
    return achievements_ui_prompt_callback_(user_index, title_id);
  }

  void set_achievement_notification_callback(AchievementNotificationCallback callback) {
    achievement_notification_callback_ = std::move(callback);
  }
  bool PresentAchievementNotification(const AchievementNotificationPayload& payload) const {
    if (!achievement_notification_callback_) {
      return false;
    }
    return achievement_notification_callback_(payload);
  }

  void set_patch_files_list_callback(PatchFilesListCallback callback) {
    patch_files_list_callback_ = std::move(callback);
  }
  std::vector<IOSPatchFileSummary> ListPatchFiles(uint32_t title_id) const {
    if (!patch_files_list_callback_) {
      return {};
    }
    return patch_files_list_callback_(title_id);
  }

  void set_patch_discovery_summary_callback(PatchDiscoverySummaryCallback callback) {
    patch_discovery_summary_callback_ = std::move(callback);
  }
  IOSPatchDiscoverySummary GetPatchDiscoverySummary(uint32_t title_id) const {
    if (!patch_discovery_summary_callback_) {
      return {};
    }
    return patch_discovery_summary_callback_(title_id);
  }

  void set_patch_set_enabled_callback(PatchSetEnabledCallback callback) {
    patch_set_enabled_callback_ = std::move(callback);
  }
  bool SetPatchEnabled(uint32_t title_id, const std::string& filename, size_t patch_index,
                       bool enabled, std::string* status_out) const {
    if (!patch_set_enabled_callback_) {
      if (status_out) {
        *status_out = "Patch management is unavailable.";
      }
      return false;
    }
    return patch_set_enabled_callback_(title_id, filename, patch_index, enabled, status_out);
  }

  void set_controller_state_callback(ControllerStateCallback callback) {
    controller_state_callback_ = std::move(callback);
  }
  bool GetControllerState(uint32_t user_index, hid::X_INPUT_STATE* out_state) const {
    if (!controller_state_callback_) {
      return false;
    }
    return controller_state_callback_(user_index, out_state);
  }

  void set_message_box_prompt_callback(MessageBoxPromptCallback callback) {
    message_box_prompt_callback_ = std::move(callback);
  }
  bool PromptMessageBoxUI(const std::string& title, const std::string& text,
                          const std::vector<std::string>& buttons, uint32_t default_button,
                          uint32_t* selected_button_out) const;

  void set_keyboard_prompt_callback(KeyboardPromptCallback callback) {
    keyboard_prompt_callback_ = std::move(callback);
  }
  bool PromptKeyboardUI(const std::string& title, const std::string& description,
                        const std::string& default_text, std::string* text_out,
                        bool* cancelled_out) const {
    if (!keyboard_prompt_callback_) {
      return false;
    }
    return keyboard_prompt_callback_(title, description, default_text, text_out, cancelled_out);
  }

  void set_gameplay_input_blocked_callback(GameplayInputBlockedCallback callback) {
    gameplay_input_blocked_callback_ = std::move(callback);
  }
  void SetGameplayInputBlocked(bool blocked) const {
    if (gameplay_input_blocked_callback_) {
      gameplay_input_blocked_callback_(blocked);
    }
  }

  void set_guest_display_refresh_cap_getter(GuestDisplayRefreshCapGetter callback) {
    guest_display_refresh_cap_getter_ = std::move(callback);
  }
  bool GetGuestDisplayRefreshCap() const {
    if (!guest_display_refresh_cap_getter_) {
      return true;
    }
    return guest_display_refresh_cap_getter_();
  }
  void set_guest_display_refresh_cap_setter(GuestDisplayRefreshCapSetter callback) {
    guest_display_refresh_cap_setter_ = std::move(callback);
  }
  void SetGuestDisplayRefreshCap(bool capped) const {
    if (guest_display_refresh_cap_setter_) {
      guest_display_refresh_cap_setter_(capped);
    }
  }

  void set_present_letterbox_getter(PresentLetterboxGetter callback) {
    present_letterbox_getter_ = std::move(callback);
  }
  bool GetPresentLetterbox() const {
    if (!present_letterbox_getter_) {
      return true;
    }
    return present_letterbox_getter_();
  }
  void set_present_letterbox_setter(PresentLetterboxSetter callback) {
    present_letterbox_setter_ = std::move(callback);
  }
  void SetPresentLetterbox(bool enabled) const {
    if (present_letterbox_setter_) {
      present_letterbox_setter_(enabled);
    }
  }

  void set_guest_display_aspect_ratio_callback(GuestDisplayAspectRatioCallback callback) {
    guest_display_aspect_ratio_callback_ = std::move(callback);
  }
  std::pair<uint32_t, uint32_t> GetGuestDisplayAspectRatio() const {
    if (!guest_display_aspect_ratio_callback_) {
      return {16, 9};
    }
    return guest_display_aspect_ratio_callback_();
  }
  void NotifyGuestDisplayAspectRatioChanged() const;

  // Callback invoked when the view layout changes (rotation, resize, etc.).
  using LayoutChangedCallback = std::function<void()>;
  void set_layout_changed_callback(LayoutChangedCallback callback) {
    layout_changed_callback_ = std::move(callback);
  }
  void NotifyLayoutChanged() {
    if (layout_changed_callback_) {
      layout_changed_callback_();
    }
  }

  hid::touch::IOSTouchRuntimeModel* touch_runtime_model() const { return touch_runtime_model_; }
  void set_touch_runtime_model(hid::touch::IOSTouchRuntimeModel* runtime_model) {
    touch_runtime_model_ = runtime_model;
  }

 private:
  UIView* metal_view_ = nullptr;
  UIViewController* view_controller_ = nullptr;
  hid::touch::IOSTouchRuntimeModel* touch_runtime_model_ = nullptr;
  GameLaunchCallback game_launch_callback_;
  ProfilesListCallback profiles_list_callback_;
  ProfileCreateCallback profile_create_callback_;
  ProfileSignInCallback profile_sign_in_callback_;
  GameTerminateCallback game_terminate_callback_;
  TitleUpdateInstallCallback title_update_install_callback_;
  GameExitedCallback game_exited_callback_;
  SignInUIPromptCallback signin_ui_prompt_callback_;
  AchievementsSnapshotCallback achievements_snapshot_callback_;
  AchievementsUIPromptCallback achievements_ui_prompt_callback_;
  AchievementNotificationCallback achievement_notification_callback_;
  PatchFilesListCallback patch_files_list_callback_;
  PatchDiscoverySummaryCallback patch_discovery_summary_callback_;
  PatchSetEnabledCallback patch_set_enabled_callback_;
  ControllerStateCallback controller_state_callback_;
  mutable std::atomic<bool> profile_services_ready_{false};
  ProfileServicesReadyCallback profile_services_ready_callback_;
  ProfileServicesPrepareCallback profile_services_prepare_callback_;
  MessageBoxPromptCallback message_box_prompt_callback_;
  KeyboardPromptCallback keyboard_prompt_callback_;
  GameplayInputBlockedCallback gameplay_input_blocked_callback_;
  GuestDisplayRefreshCapGetter guest_display_refresh_cap_getter_;
  GuestDisplayRefreshCapSetter guest_display_refresh_cap_setter_;
  PresentLetterboxGetter present_letterbox_getter_;
  PresentLetterboxSetter present_letterbox_setter_;
  GuestDisplayAspectRatioCallback guest_display_aspect_ratio_callback_;
  LayoutChangedCallback layout_changed_callback_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOWED_APP_CONTEXT_IOS_H_
