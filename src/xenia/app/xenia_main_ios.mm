/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_ios.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_ios.h"

// Graphics system.
#include "xenia/gpu/metal/metal_graphics_system.h"

// Audio systems.
#include "xenia/apu/sdl/sdl_audio_system.h"

// Input drivers.
#include "xenia/hid/input_system.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/hid/sdl/sdl_hid.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_state.h"

// CVars normally defined in xenia_main.cc (excluded on iOS).
DEFINE_path(
    storage_root, "",
    "Root path for persistent internal data storage (config, etc.), or empty "
    "to use the path preferred for the OS.",
    "Storage");
DEFINE_path(
    content_root, "",
    "Root path for guest content storage (saves, etc.), or empty to use the "
    "content folder under the storage root.",
    "Storage");
DEFINE_path(
    cache_root, "",
    "Root path for cache files. If empty, the cache folder under the storage "
    "root will be used.",
    "Storage");
DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");
DEFINE_bool(mount_cache, true, "Enable cache mount", "Storage");

// CVar normally defined in windowed_app_main_qt.cc (excluded on iOS).
DEFINE_transient_path(target, "", "Specifies the target file to run.",
                      "General");
DECLARE_string(launch_module);
DECLARE_uint32(launch_flags);
DECLARE_string(launch_data);

namespace xe {
namespace app {

class EmulatorAppIOS final : public xe::ui::WindowedApp {
 public:
  static std::unique_ptr<xe::ui::WindowedApp> Create(
      xe::ui::WindowedAppContext& app_context) {
    return std::unique_ptr<xe::ui::WindowedApp>(
        new EmulatorAppIOS(app_context));
  }

  bool OnInitialize() override;
  void OnDestroy() override;

 private:
  static constexpr size_t kZOrderHidInput = 128;

  explicit EmulatorAppIOS(xe::ui::WindowedAppContext& app_context)
      : xe::ui::WindowedApp(app_context, "xenia") {}

  std::filesystem::path GetIOSStorageRoot() const;
  bool RequestGameStop(const char* reason);
  void StartEmulatorThread(std::filesystem::path game_path, bool require_cpu_backend);
  void StartQueuedLaunchIfIdle();
  void EmulatorThread(const std::filesystem::path& game_path, bool require_cpu_backend);
  bool EnsureProfileServicesReady();

  static std::unique_ptr<apu::AudioSystem> CreateAudioSystem(
      cpu::Processor* processor);
  static std::unique_ptr<gpu::GraphicsSystem> CreateGraphicsSystem();
  static std::vector<std::unique_ptr<hid::InputDriver>> CreateInputDrivers(
      ui::Window* window);

  std::unique_ptr<Emulator> emulator_;
  std::unique_ptr<ui::Window> window_;
  std::thread emulator_thread_;
  std::atomic<bool> emulator_thread_running_{false};
  std::atomic<bool> game_stop_requested_{false};
  std::atomic<bool> shutting_down_{false};
  std::mutex launch_mutex_;
  std::optional<std::filesystem::path> queued_launch_path_;
  bool title_update_install_in_progress_ = false;
  std::atomic<bool> emulator_initialized_{false};
  std::atomic<bool> emulator_cpu_initialized_{false};
};

bool EmulatorAppIOS::EnsureProfileServicesReady() {
  if (emulator_ && emulator_->kernel_state() && emulator_->kernel_state()->xam_state()) {
    return true;
  }

  if (!shutting_down_.load(std::memory_order_acquire) &&
      !emulator_thread_running_.load(std::memory_order_acquire) &&
      !emulator_initialized_.load(std::memory_order_acquire)) {
    StartEmulatorThread({}, false);
  }

  // Do not block here; callers may execute on UI-sensitive paths.
  return emulator_ && emulator_->kernel_state() &&
         emulator_->kernel_state()->xam_state();
}

std::filesystem::path EmulatorAppIOS::GetIOSStorageRoot() const {
#if XE_PLATFORM_IOS
  // On iOS, use the app's Documents directory as the storage root.
  // This is accessible via Files.app and iTunes file sharing.
  @autoreleasepool {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    if (paths.count > 0) {
      NSString* documents_path = paths[0];
      return std::filesystem::path([documents_path UTF8String]);
    }
    return std::filesystem::path([NSHomeDirectory() UTF8String]) / "Documents";
  }
#else
  return xe::filesystem::GetUserFolder() / "Xenia";
#endif
}

bool EmulatorAppIOS::OnInitialize() {
  // Set up storage paths.
  std::filesystem::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = GetIOSStorageRoot();
  }
  storage_root = std::filesystem::absolute(storage_root);

  // Create storage directories if they don't exist.
  std::filesystem::create_directories(storage_root);
  XELOGI("iOS: Storage root: {}", storage_root);

  config::SetupConfig(storage_root);

  std::filesystem::path content_root = cvars::content_root;
  if (content_root.empty()) {
    content_root = storage_root / "content";
  } else if (!content_root.is_absolute()) {
    content_root = storage_root / content_root;
  }
  content_root = std::filesystem::absolute(content_root);
  std::filesystem::create_directories(content_root);
  XELOGI("iOS: Content root: {}", content_root);

  std::filesystem::path cache_root = cvars::cache_root;
  if (cache_root.empty()) {
#if XE_PLATFORM_IOS
    @autoreleasepool {
      NSArray* cache_paths = NSSearchPathForDirectoriesInDomains(
          NSCachesDirectory, NSUserDomainMask, YES);
      if (cache_paths.count > 0) {
        cache_root = std::filesystem::path(
            [cache_paths[0] UTF8String]) / "xenia";
      } else {
        cache_root = storage_root / "cache_host";
      }
    }
#else
    cache_root = storage_root / "cache_host";
#endif
  } else if (!cache_root.is_absolute()) {
    cache_root = storage_root / cache_root;
  }
  cache_root = std::filesystem::absolute(cache_root);
  std::filesystem::create_directories(cache_root);
  XELOGI("iOS: Cache root: {}", cache_root);

  // Create the emulator instance.
  emulator_ =
      std::make_unique<Emulator>("", storage_root, content_root, cache_root);

  // iOS title-to-title relaunch flow:
  // 1. XamLoaderLaunchTitle invokes this callback with relaunch parameters.
  // 2. We queue the resolved target and preserve launch metadata in cvars.
  // 3. XamLoaderLaunchTitle then terminates the current title.
  // 4. Emulator thread exit triggers StartQueuedLaunchIfIdle().
  emulator_->set_on_launch_new_title(
      [this](const std::string& host_path, const std::string& launch_module,
             uint32_t launch_flags, const std::string& launch_data_hex) {
        if (shutting_down_.load(std::memory_order_acquire)) {
          XELOGW("iOS: Ignoring title relaunch request while shutting down");
          return;
        }

        std::filesystem::path relaunch_target;
        if (!host_path.empty()) {
          relaunch_target = std::filesystem::path(host_path);
        }
        if (!launch_module.empty()) {
          std::filesystem::path launch_module_path(launch_module);
          if (relaunch_target.empty()) {
            relaunch_target = launch_module_path;
          } else if (relaunch_target.extension() == ".xex" ||
                     relaunch_target.extension() == ".XEX") {
            relaunch_target = relaunch_target.parent_path() / launch_module_path;
          } else {
            relaunch_target = relaunch_target / launch_module_path;
          }
        }
        relaunch_target = relaunch_target.lexically_normal();
        if (relaunch_target.empty()) {
          XELOGW("iOS: Ignoring title relaunch request with empty target "
                 "(host_path='{}', launch_module='{}')",
                 host_path, launch_module);
          return;
        }

        {
          std::lock_guard<std::mutex> lock(launch_mutex_);
          queued_launch_path_ = relaunch_target;
        }

        cvars::launch_module = launch_module;
        cvars::launch_flags = launch_flags;
        cvars::launch_data = launch_data_hex;

        XELOGI("iOS: Queued title relaunch target='{}' module='{}' flags={} "
               "data_len={}",
               relaunch_target.string(), launch_module, launch_flags,
               launch_data_hex.size());
      });

  emulator_->on_launch.AddListener(
      [](uint32_t title_id, const std::string_view title_name) {
        if (title_id == 0 || title_name.empty()) {
          return;
        }
        @autoreleasepool {
          NSString* caches = NSSearchPathForDirectoriesInDomains(
              NSCachesDirectory, NSUserDomainMask, YES)
                                 .firstObject;
          if (!caches) {
            return;
          }
          NSString* path =
              [caches stringByAppendingPathComponent:@"title-names.plist"];
          NSMutableDictionary* dict =
              [NSMutableDictionary dictionaryWithContentsOfFile:path];
          if (!dict) {
            dict = [NSMutableDictionary dictionary];
          }
          NSString* key = [NSString stringWithFormat:@"%08x", title_id];
          NSString* value = [[NSString alloc] initWithBytes:title_name.data()
                                                     length:title_name.size()
                                                   encoding:NSUTF8StringEncoding];
          if (value && key) {
            [dict setObject:value forKey:key];
            [dict writeToFile:path atomically:YES];
          }
          [value release];
        }
      });

  // Create the display window from the Metal view in the app context.
  window_ = ui::Window::Create(app_context(), "Xenia", 1280, 720, true);
  if (!window_ || !window_->Open()) {
    XELOGE("iOS: Failed to create or open display window");
    return false;
  }

  XELOGI("iOS: Display window created ({}x{})",
         window_->GetActualPhysicalWidth(),
         window_->GetActualPhysicalHeight());

  // Register callbacks with the app context.
  auto& ios_context =
      static_cast<ui::IOSWindowedAppContext&>(app_context());

  // Forward layout changes (rotation, resize) to the window.
  ios_context.set_layout_changed_callback([this]() {
    if (window_) {
      static_cast<ui::iOSWindow*>(window_.get())->HandleSizeChange();
    }
  });

  ios_context.set_game_launch_callback(
      [this](const std::string& path) {
        if (shutting_down_.load(std::memory_order_acquire)) {
          XELOGW("iOS: Ignoring game launch request while shutting down");
          return;
        }

        if (path.empty()) {
          XELOGI("iOS: Profile services init requested");
        } else {
          XELOGI("iOS: Game launch requested: {}", path);
        }
        auto game_path = std::filesystem::path(path);
        if (!game_path.empty()) {
          // User-driven launches should not reuse prior relaunch metadata.
          cvars::launch_module = "";
          cvars::launch_flags = 0;
          cvars::launch_data = "";
        }

        bool should_queue_launch = false;
        {
          std::lock_guard<std::mutex> lock(launch_mutex_);
          if (title_update_install_in_progress_) {
            XELOGW("iOS: Ignoring game launch while title update installation "
                   "is in progress");
            return;
          }
          if (emulator_thread_running_.load(std::memory_order_acquire)) {
            queued_launch_path_ = game_path;
            should_queue_launch = true;
          }
        }
        if (should_queue_launch) {
          XELOGI("iOS: Emulator thread is running; queued launch '{}'", game_path.string());
          RequestGameStop("Queued launch");
          return;
        }

        const bool require_cpu_backend = !game_path.empty();
        StartEmulatorThread(std::move(game_path), require_cpu_backend);
      });

  ios_context.set_game_terminate_callback([this]() {
    return RequestGameStop("TerminateCurrentGame");
  });

  ios_context.set_title_update_install_callback(
      [this](const std::string& package_path, std::string* status_out,
             bool* not_title_update_out) {
        auto set_status = [&](const std::string& status) {
          if (status_out) {
            *status_out = status;
          }
        };
        auto set_not_title_update = [&](bool not_title_update) {
          if (not_title_update_out) {
            *not_title_update_out = not_title_update;
          }
        };
        set_not_title_update(false);

        if (package_path.empty()) {
          set_status("No title update package selected.");
          return false;
        }
        if (shutting_down_.load(std::memory_order_acquire)) {
          set_status("App is shutting down.");
          return false;
        }

        {
          std::lock_guard<std::mutex> lock(launch_mutex_);
          if (!emulator_) {
            set_status("Emulator is not initialized.");
            return false;
          }
          if (title_update_install_in_progress_) {
            set_status("Another title update installation is already running.");
            return false;
          }
          if (emulator_thread_running_.load(std::memory_order_acquire)) {
            set_status("Stop the current game before installing a title update.");
            return false;
          }
          title_update_install_in_progress_ = true;
        }

        struct TitleUpdateInstallScope {
          EmulatorAppIOS* app = nullptr;
          ~TitleUpdateInstallScope() {
            if (!app) {
              return;
            }
            std::lock_guard<std::mutex> lock(app->launch_mutex_);
            app->title_update_install_in_progress_ = false;
          }
        } install_scope{this};

        if (!EnsureProfileServicesReady()) {
          set_status(
              "Profile services are still initializing. Please try again.");
          return false;
        }

        Emulator::ContentInstallEntry entry{std::filesystem::path(package_path)};
        X_STATUS parse_status = X_STATUS_UNSUCCESSFUL;
        X_STATUS install_status = X_STATUS_UNSUCCESSFUL;
        {
          std::lock_guard<std::mutex> lock(launch_mutex_);
          if (shutting_down_.load(std::memory_order_acquire)) {
            set_status("App is shutting down.");
            return false;
          }
          if (!emulator_) {
            set_status("Emulator is unavailable.");
            return false;
          }
          parse_status =
              emulator_->ProcessContentPackageHeader(entry.path_, entry);
        }
        if (XFAILED(parse_status) ||
            entry.installation_state_ == Emulator::InstallState::failed) {
          set_not_title_update(true);
          set_status("Selected file is not a title update package.");
          return false;
        }

        if (entry.content_type_ != XContentType::kInstaller) {
          set_not_title_update(true);
          set_status("Selected file is not a title update package.");
          return false;
        }

        {
          std::lock_guard<std::mutex> lock(launch_mutex_);
          if (shutting_down_.load(std::memory_order_acquire)) {
            set_status("App is shutting down.");
            return false;
          }
          if (!emulator_) {
            set_status("Emulator is unavailable.");
            return false;
          }
          install_status = emulator_->InstallContentPackage(entry.path_, entry);
        }
        if (XFAILED(install_status) ||
            entry.installation_state_ != Emulator::InstallState::installed) {
          std::string reason = entry.installation_error_message_.empty()
                                   ? "Title update installation failed."
                                   : entry.installation_error_message_;
          set_status(reason);
          return false;
        }

        const std::string installed_name =
            entry.name_.empty()
                ? std::filesystem::path(package_path).filename().string()
                : entry.name_;
        set_status("Installed title update: " + installed_name);
        return true;
      });

  ios_context.set_controller_state_callback(
      [this](uint32_t user_index, hid::X_INPUT_STATE* out_state) {
        std::lock_guard<std::mutex> lock(launch_mutex_);
        if (!out_state || !emulator_ || !emulator_->input_system()) {
          return false;
        }
        return emulator_->input_system()->GetStateForUI(user_index, 1,
                                                        out_state) ==
               X_ERROR_SUCCESS;
      });

  ios_context.set_profiles_list_callback([this]() {
    std::vector<ui::IOSProfileSummary> profiles;
    if (!emulator_ || !emulator_->kernel_state() || !emulator_->kernel_state()->xam_state()) {
      return profiles;
    }
    auto* profile_manager = emulator_->kernel_state()->xam_state()->profile_manager();
    if (!profile_manager) {
      return profiles;
    }
    const auto* accounts = profile_manager->GetAccounts();
    profiles.reserve(accounts->size());
    for (const auto& [xuid, account] : *accounts) {
      ui::IOSProfileSummary summary;
      summary.xuid = xuid;
      summary.gamertag = account.GetGamertagString();
      uint8_t slot = profile_manager->GetUserIndexAssignedToProfile(xuid);
      summary.signed_in = slot < XUserMaxUserCount;
      summary.signed_in_slot = slot;
      auto titles = profile_manager->GetProfile(xuid);
      if (titles) {
        for (const auto* title_info : titles->dashboard_gpd().GetTitlesInfo()) {
          if (!title_info) {
            continue;
          }
          summary.gamerscore += title_info->gamerscore_earned;
        }
      }
      profiles.push_back(std::move(summary));
    }
    std::sort(profiles.begin(), profiles.end(),
              [](const ui::IOSProfileSummary& a, const ui::IOSProfileSummary& b) {
                return a.gamertag < b.gamertag;
              });
    return profiles;
  });

  ios_context.set_profile_create_callback([this](const std::string& gamertag) -> uint64_t {
    if (!emulator_ || !EnsureProfileServicesReady()) {
      return uint64_t(0);
    }
    auto* profile_manager = emulator_->kernel_state()->xam_state()->profile_manager();
    if (!profile_manager) {
      return uint64_t(0);
    }
    if (!xe::kernel::xam::ProfileManager::IsGamertagValid(gamertag)) {
      return uint64_t(0);
    }

    std::set<uint64_t> before_ids;
    for (const auto& [xuid, account] : *profile_manager->GetAccounts()) {
      before_ids.insert(xuid);
    }

    if (!profile_manager->CreateProfile(gamertag, false)) {
      return uint64_t(0);
    }

    for (const auto& [xuid, account] : *profile_manager->GetAccounts()) {
      if (!before_ids.contains(xuid)) {
        return xuid;
      }
    }
    return uint64_t(0);
  });

  ios_context.set_profile_sign_in_callback([this](uint64_t xuid) {
    if (!emulator_ || !emulator_->kernel_state() || !emulator_->kernel_state()->xam_state()) {
      return false;
    }
    auto* profile_manager = emulator_->kernel_state()->xam_state()->profile_manager();
    if (!profile_manager || !profile_manager->GetAccount(xuid)) {
      return false;
    }

    for (uint8_t slot = 0; slot < XUserMaxUserCount; ++slot) {
      if (profile_manager->GetProfile(slot)) {
        profile_manager->Logout(slot, false);
      }
    }
    profile_manager->Login(xuid, 0, true);
    return profile_manager->GetProfile(xuid) != nullptr;
  });

  ios_context.set_achievements_list_callback(
      [this](uint32_t requested_title_id) -> std::optional<ui::IOSAchievementsData> {
        std::lock_guard<std::mutex> lock(launch_mutex_);
        if (!emulator_ || !emulator_->kernel_state() ||
            !emulator_->kernel_state()->xam_state()) {
          return std::nullopt;
        }

        auto* kernel_state = emulator_->kernel_state();
        auto* xam_state = kernel_state->xam_state();
        auto* profile_manager = xam_state->profile_manager();
        auto* achievement_manager = xam_state->achievement_manager();
        if (!profile_manager || !achievement_manager) {
          return std::nullopt;
        }

        xe::kernel::xam::UserProfile* profile = nullptr;
        for (uint8_t slot = 0; slot < XUserMaxUserCount; ++slot) {
          profile = profile_manager->GetProfile(slot);
          if (profile) {
            break;
          }
        }
        if (!profile) {
          return std::nullopt;
        }

        const uint32_t resolved_title_id =
            requested_title_id ? requested_title_id : emulator_->title_id();
        if (!resolved_title_id) {
          return std::nullopt;
        }

        ui::IOSAchievementsData data;
        data.xuid = profile->xuid();
        data.title_id = resolved_title_id;

        if (auto title_info = xam_state->user_tracker()->GetUserTitleInfo(
                profile->xuid(), resolved_title_id)) {
          data.title_name = xe::to_utf8(title_info->title_name);
          if (!data.title_name.empty() && data.title_name.back() == '\0') {
            data.title_name.pop_back();
          }
          data.achievements_total = title_info->achievements_count;
          data.achievements_unlocked = title_info->unlocked_achievements_count;
          data.gamerscore_total = title_info->gamerscore_amount;
          data.gamerscore_earned = title_info->title_earned_gamerscore;
        }

        if (data.title_name.empty() && resolved_title_id == emulator_->title_id()) {
          data.title_name = emulator_->title_name();
        }

        auto stats = profile->GetTitleAchievementStats(resolved_title_id);
        if (!data.achievements_total) {
          data.achievements_total = stats.achievements_total;
          data.achievements_unlocked = stats.achievements_unlocked;
          data.gamerscore_total = stats.gamerscore_total;
          data.gamerscore_earned = stats.gamerscore_earned;
        }

        auto achievements =
            achievement_manager->GetTitleAchievements(profile->xuid(), resolved_title_id);
        data.achievements.reserve(achievements.size());

        auto sanitize_utf16 = [](const std::u16string& value) {
          std::string utf8 = xe::to_utf8(value);
          if (!utf8.empty() && utf8.back() == '\0') {
            utf8.pop_back();
          }
          return utf8;
        };

        for (const auto& achievement : achievements) {
          ui::IOSAchievementEntry entry;
          entry.achievement_id = achievement.achievement_id;
          entry.gamerscore = achievement.gamerscore;
          entry.unlocked = achievement.IsUnlocked();

          const bool can_show_locked =
              (achievement.flags &
               static_cast<uint32_t>(xe::kernel::xam::AchievementFlags::kShowUnachieved)) != 0;
          if (entry.unlocked || can_show_locked) {
            entry.title = sanitize_utf16(achievement.achievement_name);
          } else {
            entry.title = "Secret Achievement";
          }
          if (entry.unlocked) {
            entry.description = sanitize_utf16(achievement.unlocked_description);
          } else if (can_show_locked) {
            entry.description = sanitize_utf16(achievement.locked_description);
          }

          if (entry.unlocked) {
            auto icon_data = achievement_manager->GetAchievementIcon(
                profile->xuid(), resolved_title_id, achievement.achievement_id);
            entry.icon_data.assign(icon_data.begin(), icon_data.end());
          }
          data.achievements.push_back(std::move(entry));
        }

        std::stable_sort(data.achievements.begin(), data.achievements.end(),
                         [](const ui::IOSAchievementEntry& a,
                            const ui::IOSAchievementEntry& b) {
                           if (a.unlocked != b.unlocked) {
                             return a.unlocked && !b.unlocked;
                           }
                           return a.achievement_id < b.achievement_id;
                         });

        return data;
      });

  ios_context.set_achievements_reset_callback(
      [this](uint32_t title_id, std::string* status_out) {
        std::lock_guard<std::mutex> lock(launch_mutex_);
        if (!emulator_ || !emulator_->kernel_state() ||
            !emulator_->kernel_state()->xam_state()) {
          if (status_out) {
            *status_out = "Profile services are unavailable.";
          }
          return false;
        }

        auto* xam_state = emulator_->kernel_state()->xam_state();
        auto* profile_manager = xam_state->profile_manager();
        auto* user_tracker = xam_state->user_tracker();
        if (!profile_manager || !user_tracker || !title_id) {
          if (status_out) {
            *status_out = "Achievement reset is unavailable.";
          }
          return false;
        }

        xe::kernel::xam::UserProfile* profile = nullptr;
        for (uint8_t slot = 0; slot < XUserMaxUserCount; ++slot) {
          profile = profile_manager->GetProfile(slot);
          if (profile) {
            break;
          }
        }
        if (!profile) {
          if (status_out) {
            *status_out = "Sign in to a profile before resetting achievements.";
          }
          return false;
        }

        if (!user_tracker->ResetTitleAchievements(profile->xuid(), title_id)) {
          if (status_out) {
            *status_out = "No saved achievements were found for this game.";
          }
          return false;
        }

        profile_manager->ReloadProfiles();
        if (status_out) {
          *status_out = "Achievements reset.";
        }
        return true;
      });

  XELOGI("iOS: EmulatorAppIOS initialized successfully");
  return true;
}

bool EmulatorAppIOS::RequestGameStop(const char* reason) {
  if (!emulator_ || !emulator_thread_running_.load(std::memory_order_acquire)) {
    game_stop_requested_.store(false, std::memory_order_release);
    return false;
  }
  if (game_stop_requested_.exchange(true, std::memory_order_acq_rel)) {
    XELOGI("iOS: {} already in progress", reason ? reason : "Game stop");
    return true;
  }

  XELOGI("iOS: {} requested", reason ? reason : "Game stop");
  const X_STATUS terminate_status = emulator_->TerminateTitle();
  if (XFAILED(terminate_status)) {
    game_stop_requested_.store(false, std::memory_order_release);
    XELOGW("iOS: TerminateTitle failed with status {:08X}", terminate_status);
    return false;
  }
  return true;
}

void EmulatorAppIOS::StartEmulatorThread(std::filesystem::path game_path,
                                         bool require_cpu_backend) {
  if (emulator_thread_.joinable()) {
    emulator_thread_.join();
  }

  game_stop_requested_.store(false, std::memory_order_release);
  emulator_thread_ = std::thread([this, game_path = std::move(game_path), require_cpu_backend]() {
    emulator_thread_running_.store(true, std::memory_order_release);
    EmulatorThread(game_path, require_cpu_backend);
    emulator_thread_running_.store(false, std::memory_order_release);
    game_stop_requested_.store(false, std::memory_order_release);
    app_context().CallInUIThread([this]() { StartQueuedLaunchIfIdle(); });
  });
}

void EmulatorAppIOS::StartQueuedLaunchIfIdle() {
  if (shutting_down_.load(std::memory_order_acquire) ||
      emulator_thread_running_.load(std::memory_order_acquire)) {
    return;
  }

  std::optional<std::filesystem::path> queued_path;
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    if (title_update_install_in_progress_) {
      return;
    }
    if (!queued_launch_path_.has_value()) {
      return;
    }
    queued_path = std::move(queued_launch_path_);
    queued_launch_path_.reset();
  }

  if (!queued_path.has_value() || queued_path->empty()) {
    return;
  }

  XELOGI("iOS: Starting queued game launch: {}", queued_path->string());
  StartEmulatorThread(std::move(*queued_path), true);
}

void EmulatorAppIOS::OnDestroy() {
  XELOGI("iOS: EmulatorAppIOS::OnDestroy invoked");
  shutting_down_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    queued_launch_path_.reset();
  }
  if (emulator_thread_.joinable()) {
    if (emulator_thread_running_.load(std::memory_order_acquire)) {
      RequestGameStop("OnDestroy");
    }
    emulator_thread_.join();
    emulator_thread_running_.store(false, std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    emulator_.reset();
  }
  window_.reset();
}

void EmulatorAppIOS::EmulatorThread(const std::filesystem::path& game_path,
                                    bool require_cpu_backend) {
  xe::threading::set_name("Emulator Thread");
  const bool launched_with_game = !game_path.empty();

  struct ScopeExit {
    std::function<void()> fn;
    ~ScopeExit() {
      if (fn) {
        fn();
      }
    }
  } notify_exit{[this, launched_with_game]() {
    if (!launched_with_game) {
      return;
    }
    app_context().CallInUIThread([this]() {
      auto& ios_context = static_cast<ui::IOSWindowedAppContext&>(app_context());
      ios_context.NotifyGameExited();
    });
  }};

  const bool need_profile_mode_transition =
      !emulator_initialized_.load(std::memory_order_acquire) ||
      (require_cpu_backend && !emulator_cpu_initialized_.load(std::memory_order_acquire));

  if (need_profile_mode_transition) {
    if (emulator_initialized_.load(std::memory_order_acquire) &&
        !emulator_cpu_initialized_.load(std::memory_order_acquire) && require_cpu_backend) {
      XELOGI("iOS: Reinitializing emulator for game mode");
      // Detach the presenter on the UI thread before tearing down the graphics
      // system, otherwise the window may present using a freed presenter.
      if (!app_context().CallInUIThreadSynchronous([this]() {
            if (window_) {
              window_->SetPresenter(nullptr);
            }
          })) {
        XELOGW("iOS: Failed to detach presenter prior to shutdown");
      }
      emulator_->Shutdown();
    }

    X_STATUS setup_result =
        emulator_->Setup(window_.get(),
                         nullptr,  // No ImGui drawer on iOS for now.
                         require_cpu_backend, require_cpu_backend ? CreateAudioSystem : nullptr,
                         require_cpu_backend ? CreateGraphicsSystem : nullptr,
                         require_cpu_backend ? CreateInputDrivers : nullptr);

    if (XFAILED(setup_result)) {
      XELOGE("iOS: Emulator::Setup failed with status {:08X}", setup_result);
      emulator_initialized_.store(false, std::memory_order_release);
      emulator_cpu_initialized_.store(false, std::memory_order_release);
      if (launched_with_game) {
        app_context().CallInUIThread([this]() {
          auto& ios_context =
              static_cast<ui::IOSWindowedAppContext&>(app_context());
          ios_context.NotifyGameExited();
        });
      }
      return;
    }

    emulator_->MountStandardDrives();
    if (auto* fs = emulator_->file_system()) {
      std::string cache_target;
      if (fs->FindSymbolicLink("cache:", cache_target)) {
        XELOGI("iOS: cache: mounted to {}", cache_target);
      } else {
        XELOGW("iOS: cache: mount missing after MountStandardDrives");
      }
    }

    auto* graphics_system = emulator_->graphics_system();
    auto* presenter = graphics_system ? graphics_system->presenter() : nullptr;
    if (presenter && !app_context().CallInUIThreadSynchronous([this, presenter]() {
          if (window_) {
            window_->SetPresenter(presenter);
          }
        })) {
      XELOGE("iOS: Failed to attach presenter to display window");
      emulator_initialized_.store(false, std::memory_order_release);
      emulator_cpu_initialized_.store(false, std::memory_order_release);
      return;
    }

    emulator_initialized_.store(true, std::memory_order_release);
    emulator_cpu_initialized_.store(require_cpu_backend, std::memory_order_release);
    XELOGI("iOS: Emulator setup complete");

    if (!launched_with_game) {
      app_context().CallInUIThread([this]() {
        auto& ios_context = static_cast<ui::IOSWindowedAppContext&>(app_context());
        ios_context.NotifyProfileServicesReady();
      });
    }
  }

  // Load game-specific config if available.
  if (!game_path.empty()) {
    XELOGI("iOS: Loading game config for: {}", game_path.string());
    config::LoadGameConfigForFile(game_path);
  }

  if (!game_path.empty()) {
    auto abs_path = std::filesystem::absolute(game_path);
    XELOGI("iOS: Launching game: {}", abs_path);

    auto xam = emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
    if (xam) {
      auto& loader_data = xam->loader_data();
      loader_data.host_path = xe::path_to_utf8(abs_path);
      loader_data.launch_data_present = false;
      loader_data.launch_flags = 0;
      loader_data.launch_data.clear();
      if (cvars::launch_flags != 0 || !cvars::launch_data.empty()) {
        loader_data.launch_data_present = true;
        loader_data.launch_flags = cvars::launch_flags;
        const std::string& hex = cvars::launch_data;
        for (size_t i = 0; i + 1 < hex.length(); i += 2) {
          std::string byte_str = hex.substr(i, 2);
          uint8_t byte =
              static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
          loader_data.launch_data.push_back(byte);
        }
      }
    }

    X_STATUS launch_result = emulator_->LaunchPath(abs_path);
    cvars::launch_module = "";
    cvars::launch_flags = 0;
    cvars::launch_data = "";
    if (XFAILED(launch_result)) {
      XELOGE("iOS: Failed to launch game: {:08X}", launch_result);
      return;
    }

    XELOGI("iOS: Game launched successfully");
    emulator_->WaitUntilExit();
    XELOGI("iOS: Game execution finished (exit wait completed)");
  }
}

std::unique_ptr<apu::AudioSystem> EmulatorAppIOS::CreateAudioSystem(
    cpu::Processor* processor) {
  // SDL2 uses CoreAudio on iOS for audio output.
  return std::make_unique<apu::sdl::SDLAudioSystem>(processor);
}

std::unique_ptr<gpu::GraphicsSystem> EmulatorAppIOS::CreateGraphicsSystem() {
  return std::make_unique<gpu::metal::MetalGraphicsSystem>();
}

std::vector<std::unique_ptr<hid::InputDriver>>
EmulatorAppIOS::CreateInputDrivers(ui::Window* window) {
  std::vector<std::unique_ptr<hid::InputDriver>> drivers;
  // SDL2 uses the GameController framework on iOS for MFi/Bluetooth gamepads.
  auto sdl_driver = xe::hid::sdl::Create(window, kZOrderHidInput);
  if (sdl_driver && XSUCCEEDED(sdl_driver->Setup())) {
    drivers.emplace_back(std::move(sdl_driver));
  } else {
    XELOGW("iOS: SDL input driver setup failed, falling back to nop input");
    drivers.emplace_back(xe::hid::nop::Create(window, kZOrderHidInput));
  }
  return drivers;
}

}  // namespace app
}  // namespace xe

XE_DEFINE_WINDOWED_APP(xenia, xe::app::EmulatorAppIOS::Create);
