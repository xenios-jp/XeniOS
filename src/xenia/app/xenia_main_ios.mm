/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Foundation/Foundation.h>

#include <pthread.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/backend/jit_memory_ios.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/ui/ios/app/ios_lifecycle_audio_session.h"
#include "xenia/ui/ios/app/ios_window_layout.h"
#include "xenia/ui/ios/app/windowed_app_context_ios.h"
#include "xenia/ui/ios/game/ios_launch_status.h"
#include "xenia/ui/ios/game/ios_shader_compile_indicator.h"
#include "xenia/ui/ios/game/window_ios.h"
#include "xenia/ui/ios/launcher/ios_dashboard.h"
#include "xenia/ui/ios/launcher/ios_game_library_store.h"
#include "xenia/ui/ios/shared/ios_crash_reporting.h"
#include "xenia/ui/ios/shared/ios_diagnostics.h"
#include "xenia/ui/ios/shared/ios_idle_timer.h"
#include "xenia/ui/ios/shared/ios_session_stats.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app.h"

// Graphics system.
#include "xenia/gpu/metal/metal_graphics_system.h"
#if defined(XE_IOS_MOLTENVK_ENABLED)
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#endif

// Audio systems.
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/apu/phase/phase_audio_system.h"
#include "xenia/apu/sdl/sdl_audio_system.h"

// Input drivers.
#include "xenia/hid/input_system.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/hid/sdl/sdl_hid.h"
#include "xenia/hid/touch/touch_hid_ios.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/xthread.h"
#include "xenia/patcher/patch_db.h"

DECLARE_bool(present_letterbox);

// CVars normally defined in xenia_main.cc (excluded on iOS).
DEFINE_path(storage_root, "",
            "Root path for persistent internal data storage (config, etc.), or empty "
            "to use the path preferred for the OS.",
            "Storage");
DEFINE_path(content_root, "",
            "Root path for guest content storage (saves, etc.), or empty to use the "
            "content folder under the storage root.",
            "Storage");
DEFINE_path(cache_root, "",
            "Root path for cache files. If empty, the cache folder under the storage "
            "root will be used.",
            "Storage");
DECLARE_string(apu);
DECLARE_string(gpu);
DEFINE_bool(ios_emulator_thread_user_initiated_qos, false,
            "Run the iOS emulator host thread at user-initiated QoS. This is an "
            "iOS scheduler experiment for devices where guest CPU work falls behind.",
            "iOS");

// CVar normally defined in windowed_app_main_qt.cc (excluded on iOS).
DEFINE_transient_path(target, "", "Specifies the target file to run.", "General");
DECLARE_string(launch_module);
DECLARE_uint32(launch_flags);
DECLARE_string(launch_data);

DECLARE_string(cpu);

namespace xe {
namespace app {

namespace {

std::filesystem::path IOSPatchesDirectory(const std::filesystem::path& storage_root) {
  return storage_root / "patches";
}

std::filesystem::path IOSPatchStoragePath(const std::filesystem::path& storage_root,
                                          const patcher::PatchSourceFile& patch_file) {
  return IOSPatchesDirectory(storage_root) / patch_file.filename;
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }
  std::stringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

std::string DisplayNameForPatchFile(const patcher::PatchSourceFile& bundled) {
  std::string display_name = bundled.filename;
  constexpr std::string_view kPatchSuffix = ".patch.toml";
  if (display_name.ends_with(kPatchSuffix)) {
    display_name.resize(display_name.size() - kPatchSuffix.size());
  }
  size_t separator = display_name.find(" - ");
  if (separator != std::string::npos) {
    display_name = display_name.substr(separator + 3);
  }
  return display_name;
}

bool IsPatchFileName(const std::string& filename) {
  static const std::regex patch_filename_regex("^[A-Fa-f0-9]{8}.*\\.patch\\.toml$");
  return std::regex_match(filename, patch_filename_regex);
}

void EnsureIOSPatchesDirectoryExposed(const std::filesystem::path& storage_root,
                                      uint32_t title_id = 0) {
  const std::filesystem::path patches_dir = IOSPatchesDirectory(storage_root);
  std::error_code ec;
  std::filesystem::create_directories(patches_dir, ec);
  if (ec) {
    XELOGW("iOS patches: failed creating Documents patches folder {}: {}", xe::strip_pii(xe::path_to_utf8(patches_dir)),
           ec.message());
    return;
  }

  size_t copied_files = 0;
  size_t existing_files = 0;
  size_t failed_files = 0;
  for (const patcher::PatchSourceFile& bundled :
       patcher::EnumerateBundledPatchesForTitle(title_id)) {
    const std::filesystem::path storage_path = IOSPatchStoragePath(storage_root, bundled);
    ec.clear();
    if (std::filesystem::exists(storage_path, ec)) {
      ++existing_files;
      continue;
    }
    if (ec) {
      ++failed_files;
      XELOGW("iOS patches: failed checking {}: {}", xe::strip_pii(xe::path_to_utf8(storage_path)), ec.message());
      continue;
    }

    std::ofstream file(storage_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      ++failed_files;
      XELOGW("iOS patches: failed opening {} for write", xe::strip_pii(xe::path_to_utf8(storage_path)));
      continue;
    }
    file.write(bundled.toml_content.data(),
               static_cast<std::streamsize>(bundled.toml_content.size()));
    if (!file.good()) {
      ++failed_files;
      XELOGW("iOS patches: failed writing {}", xe::strip_pii(xe::path_to_utf8(storage_path)));
      continue;
    }
    ++copied_files;
  }

  XELOGI("iOS patches: exposed Documents patches folder {} (copied={}, "
         "existing={}, failed={})",
         xe::strip_pii(xe::path_to_utf8(patches_dir)), copied_files, existing_files, failed_files);
}

std::vector<patcher::PatchSourceFile> EnumerateIOSPatchFilesForTitle(
    const std::filesystem::path& storage_root, uint32_t title_id,
    ui::IOSPatchDiscoverySummary* summary_out = nullptr) {
  EnsureIOSPatchesDirectoryExposed(storage_root, title_id);
  std::vector<patcher::PatchSourceFile> patch_files =
      patcher::EnumerateBundledPatchesForTitle(title_id);
  if (summary_out) {
    summary_out->directory_path =
        xe::path_to_utf8(IOSPatchesDirectory(storage_root).lexically_normal());
    summary_out->bundled_files = patch_files.size();
  }
  if (!title_id) {
    return patch_files;
  }

  const std::filesystem::path patches_dir = IOSPatchesDirectory(storage_root);
  if (!std::filesystem::exists(patches_dir) || !std::filesystem::is_directory(patches_dir)) {
    return patch_files;
  }
  if (summary_out) {
    summary_out->directory_exists = true;
  }

  patcher::PatchDB scratch{std::filesystem::path()};
  for (const auto& file_info : xe::filesystem::ListFiles(patches_dir)) {
    if (file_info.type != xe::filesystem::FileInfo::Type::kFile) {
      continue;
    }
    if (summary_out) {
      ++summary_out->scanned_files;
    }

    const std::string filename = xe::path_to_utf8(file_info.name);
    if (!IsPatchFileName(filename)) {
      continue;
    }
    if (summary_out) {
      ++summary_out->candidate_files;
    }

    const std::filesystem::path path = file_info.path / file_info.name;
    const std::string toml_content = ReadTextFile(path);
    patcher::PatchFileEntry entry = scratch.ReadPatchFromString(filename, toml_content);
    if (entry.title_id == static_cast<uint32_t>(-1)) {
      if (summary_out) {
        ++summary_out->parse_failures;
      }
      continue;
    }
    if (entry.title_id != title_id) {
      if (summary_out) {
        ++summary_out->title_mismatches;
      }
      continue;
    }
    entry.filename = filename;
    if (summary_out) {
      ++summary_out->matching_files;
    }

    patcher::PatchSourceFile local_patch_file;
    local_patch_file.filename = filename;
    local_patch_file.toml_content = toml_content;
    local_patch_file.entry = std::move(entry);

    auto existing = std::find_if(patch_files.begin(), patch_files.end(),
                                 [&](const patcher::PatchSourceFile& candidate) {
                                   return candidate.filename == local_patch_file.filename;
                                 });
    if (existing != patch_files.end()) {
      *existing = std::move(local_patch_file);
    } else {
      patch_files.push_back(std::move(local_patch_file));
    }
  }

  return patch_files;
}

std::vector<std::string> SplitPatchLines(const std::string& source_text) {
  std::vector<std::string> lines;
  std::stringstream stream(source_text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(std::move(line));
  }
  if (!source_text.empty() && source_text.back() == '\n') {
    lines.emplace_back();
  }
  return lines;
}

bool UpdatePatchEnabledLine(std::vector<std::string>* lines, size_t patch_index, bool new_value) {
  if (!lines) {
    return false;
  }

  static const std::regex patch_header(R"(^\s*\[\[patch\]\]\s*$)");
  static const std::regex section_header(R"(^\s*\[([^\[\]]+)\]\s*$)");
  static const std::regex is_enabled(R"(^\s*is_enabled\s*=\s*(.+))");
  static const std::regex comment_line(R"(^\s*#.*)");
  static const std::regex value_regex(R"((^\s*is_enabled\s*=\s*)(true|false)(.*)$)");

  std::vector<size_t> enabled_lines;
  bool in_patch = false;
  for (size_t i = 0; i < lines->size(); ++i) {
    const std::string& line = (*lines)[i];
    if (std::regex_match(line, comment_line)) {
      continue;
    }
    if (std::regex_match(line, patch_header)) {
      in_patch = true;
      continue;
    }
    if (std::regex_match(line, section_header)) {
      in_patch = false;
      continue;
    }
    if (in_patch && std::regex_search(line, is_enabled)) {
      enabled_lines.push_back(i);
    }
  }
  if (patch_index >= enabled_lines.size()) {
    return false;
  }

  std::smatch match;
  std::string& line = (*lines)[enabled_lines[patch_index]];
  if (!std::regex_match(line, match, value_regex)) {
    return false;
  }
  line = match[1].str() + (new_value ? "true" : "false") + match[3].str();
  return true;
}

std::string JoinPatchLines(const std::vector<std::string>& lines) {
  std::stringstream stream;
  for (size_t i = 0; i < lines.size(); ++i) {
    stream << lines[i];
    if (i + 1 < lines.size()) {
      stream << '\n';
    }
  }
  return stream.str();
}

bool WritePatchTextAtomically(const std::filesystem::path& path, const std::string& text,
                              std::string* status) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    if (status) {
      *status = "Failed creating patch directory: " + ec.message();
    }
    return false;
  }

  std::filesystem::path temp_path = path;
  temp_path += ".tmp";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      if (status) {
        *status = "Failed opening patch file for writing.";
      }
      return false;
    }
    out << text;
    out.close();
    if (!out) {
      std::filesystem::remove(temp_path, ec);
      if (status) {
        *status = "Failed writing patch file.";
      }
      return false;
    }
  }

  std::filesystem::rename(temp_path, path, ec);
  if (ec) {
    std::filesystem::remove(temp_path, ec);
    if (status) {
      *status = "Failed replacing patch file: " + ec.message();
    }
    return false;
  }
  if (status) {
    *status = "Saved. Takes effect on next launch.";
  }
  return true;
}

ui::IOSPatchFileSummary BuildIOSPatchFileSummary(const patcher::PatchSourceFile& bundled,
                                                 const std::filesystem::path& storage_root) {
  ui::IOSPatchFileSummary summary;
  summary.title_id = bundled.entry.title_id;
  summary.filename = bundled.filename;
  summary.display_name = DisplayNameForPatchFile(bundled);
  summary.title_name = bundled.entry.title_name;

  const std::filesystem::path storage_path = IOSPatchStoragePath(storage_root, bundled);
  std::string source_text =
      std::filesystem::exists(storage_path) ? ReadTextFile(storage_path) : bundled.toml_content;
  toml::table root;
  try {
    root = toml::parse(source_text);
  } catch (const std::exception& e) {
    XELOGE("iOS patches: failed to parse {}: {}", xe::strip_pii(bundled.filename),
           xe::strip_pii(e.what()));
    return summary;
  }

  auto* patches = root["patch"].as_array();
  if (!patches) {
    return summary;
  }

  size_t patch_index = 0;
  for (const auto& node : *patches) {
    auto* table = node.as_table();
    if (!table) {
      ++patch_index;
      continue;
    }
    ui::IOSPatchEntrySummary patch;
    patch.patch_index = patch_index++;
    if (auto value = (*table)["name"].value<std::string>()) {
      patch.name = *value;
    }
    if (auto value = (*table)["desc"].value<std::string>()) {
      patch.description = *value;
    }
    if (auto value = (*table)["author"].value<std::string>()) {
      patch.author = *value;
    }
    if (auto value = (*table)["is_enabled"].value<bool>()) {
      patch.is_enabled = *value;
    }
    summary.patches.push_back(std::move(patch));
  }
  return summary;
}

}  // namespace

namespace {

// What a title-to-title launch carries beside its path, as the desktop
// RelaunchTitle does: the package inside a disc and the original Xbox game
// XeFu runs.
struct LaunchExtras {
  std::string package;
  std::filesystem::path xbox_disc;
};

// Internal pause reason, beside ui::IOSWindowedAppContext::EmulationPauseReason:
// a live settings change is being applied.
constexpr uint32_t kPauseReasonLiveSettings = 1u << 8;

// How long a pause waits for the GPU worker to commit and drain its Metal work.
// The worker reaches a pause point between ring bursts, so this is normally a
// few milliseconds; the wait is bounded because a worker blocked on the guest
// (a memory wait that only a guest thread can satisfy) would never get there.
constexpr std::chrono::milliseconds kGpuPauseTimeout{3000};
// How long a pause waits for the six guest dispatch threads to settle. A fiber
// normally yields within one scheduler quantum; the long tail is a fiber inside a
// global-lock or DISPATCH_LEVEL section.
constexpr std::chrono::milliseconds kSchedulerPauseTimeout{1500};
// A live settings change keeps the game paused this long after the last change, so a slider
// drag pauses once instead of once per tick.
constexpr int64_t kLiveSettingsHoldNs = 250 * 1000 * 1000;
// How long the emulator thread waits for the pause queue to drain before tearing down. It has to
// outlast the slowest pause still in flight (the GPU wait, the audio and XMA pauses of about 2 s
// each, the scheduler wait), or teardown frees the audio system and the scheduler under it.
constexpr std::chrono::milliseconds kResumeForTeardownTimeout =
    kGpuPauseTimeout + std::chrono::milliseconds{4000} + kSchedulerPauseTimeout +
    std::chrono::milliseconds{3000};
// How long the main thread waits for a pause before applying live settings.
constexpr std::chrono::milliseconds kLiveSettingsPauseTimeout{3000};
// How long stopping a game may take before the user is told it is stuck. A normal
// stop (guest threads terminated, GPU drained, Metal torn down) is a few seconds;
// 10 s is well past that without keeping the user staring at "Stopping game...".
constexpr std::chrono::seconds kStopWatchdogDelay{10};

// Set by CreateGraphicsSystem: whether the command processor is Metal's.
std::atomic<bool> g_graphics_is_metal{true};

// Holds the GPU worker at a pause point (see PauseEmulationNow).
struct GpuPauseHold {
  std::mutex mutex;
  std::condition_variable cv;
  bool reached = false;
  bool release = false;

  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
    cv.notify_all();
  }
};

// One-shot completion for a block queued on a dispatch queue.
struct QueueWaiter {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
};

// User-facing text for a LaunchPath failure. No host paths.
std::string LaunchFailureMessage(uint32_t status) {
  char code[16];
  std::snprintf(code, sizeof(code), "%08X", status);
  return std::string(
             "The game couldn't be started. Its files may be missing, damaged or not supported. "
             "(error ") +
         code + ")";
}

constexpr const char kEmulatorSetupFailedMessage[] =
    "Xenia couldn't start the emulator. Close the app and open it again. If this keeps "
    "happening, share the log.";
constexpr const char kSubsystemSetupFailedMessage[] =
    "Xenia couldn't set up the game's graphics, audio or input. Close the app and open it "
    "again. If this keeps happening, share the log.";
constexpr const char kPresenterAttachFailedMessage[] =
    "Xenia couldn't attach the game to the screen. Close the app and open it again. If this "
    "keeps happening, share the log.";
constexpr const char kTitleUpdateInstallBusyMessage[] =
    "A title update is still installing. Wait for it to finish, then start the game again.";

}  // namespace

class EmulatorAppIOS final : public xe::ui::WindowedApp {
 public:
  static std::unique_ptr<xe::ui::WindowedApp> Create(xe::ui::WindowedAppContext& app_context) {
    return std::unique_ptr<xe::ui::WindowedApp>(new EmulatorAppIOS(app_context));
  }

  bool OnInitialize() override;
  void OnDestroy() override;

 private:
  static constexpr size_t kZOrderHidInput = 128;

  explicit EmulatorAppIOS(xe::ui::WindowedAppContext& app_context)
      : xe::ui::WindowedApp(app_context, "xenia") {}

  std::filesystem::path GetIOSStorageRoot() const;
  std::filesystem::path GetStorageRootForCallbacks() const;
  bool RequestGameStop(const char* reason);
  void ClearGameplayInputBlockerIfApplied();
  void ClearPresenterForTitleExit();
  void StartEmulatorThread(std::filesystem::path game_path, bool require_cpu_backend,
                           LaunchExtras extras = {});
  void StartQueuedLaunchIfIdle();
  void EmulatorThread(const std::filesystem::path& game_path, bool require_cpu_backend,
                      const LaunchExtras& extras);
  void ApplyGuestDisplayRefreshCapToWindowFromUIThread(bool capped);
  bool EnsureProfileServicesReady();

  // Emulation pause. Reasons are set from the main thread; the pause itself runs
  // on pause_queue_, so the main thread never waits for the GPU or audio
  // workers (except ApplyLiveSettings, bounded).
  void SetEmulationPaused(ui::IOSWindowedAppContext::EmulationPauseReason reason, bool paused);
  void ApplyLiveSettings(const std::function<void()>& apply);
  void ScheduleEmulationPauseReconcile();
  // Waits for everything queued on pause_queue_ so far. Returns false on timeout.
  bool WaitForEmulationPauseQueue(std::chrono::milliseconds timeout);
  // Runs on pause_queue_.
  void ReconcileEmulationPause();
  void PauseEmulationNow();
  void ResumeEmulationNow();
  // Called on the emulator thread: the title is running / is about to be torn
  // down (resumes first, so the GPU worker and guest threads can exit).
  void BeginEmulationPauseSession();
  void EndEmulationPauseSession();
  void ArmStopWatchdog();
  // Gives the Metal command processor the shader-wait compile boost the sheet
  // asked for, unless the device is hot. Safe to repeat; needs a live title.
  void ApplyCompileThreadBoost();

  static std::unique_ptr<apu::AudioSystem> CreateAudioSystem(cpu::Processor* processor);
  static std::unique_ptr<gpu::GraphicsSystem> CreateGraphicsSystem();
  static std::vector<std::unique_ptr<hid::InputDriver>> CreateInputDrivers(ui::Window* window);

  std::unique_ptr<Emulator> emulator_;
  std::filesystem::path storage_root_;
  std::unique_ptr<ui::Window> window_;
  std::thread emulator_thread_;
  std::atomic<bool> worker_running_{false};
  std::condition_variable launch_condition_;
  std::optional<std::tuple<std::filesystem::path, bool, LaunchExtras>> pending_worker_launch_;
  bool subsystems_ready_ = false;  // Accessed only by the emulator worker.
  std::atomic<bool> emulator_thread_running_{false};
  std::atomic<bool> game_stop_requested_{false};
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> gameplay_input_blocked_{false};
  std::atomic<bool> gameplay_input_blocker_applied_{false};
  std::mutex launch_mutex_;
  std::optional<std::filesystem::path> queued_launch_path_;
  LaunchExtras queued_launch_extras_;  // Of queued_launch_path_.
  // Set by each launch from the library: whether it is the dashboard, which
  // boots with the console's animation and which a game it starts goes back to.
  std::atomic<bool> dashboard_session_{false};
  std::atomic<bool> boot_animation_pending_{false};
  bool title_update_install_in_progress_ = false;
  std::atomic<bool> emulator_initialized_{false};
  std::atomic<bool> emulator_cpu_initialized_{false};

  // Pause reasons currently wanted (the context's EmulationPauseReason bits plus
  // kPauseReasonLiveSettings). Written on the main thread only.
  std::atomic<uint32_t> pause_desired_{0};
  // The shader wait sheet is up, so the compile threads may use every core.
  std::atomic<bool> shader_wait_boost_requested_{false};
  id thermal_observer_ = nil;
  dispatch_queue_t pause_queue_ = nullptr;
  std::mutex pause_state_mutex_;
  bool pause_game_running_ = false;                     // pause_state_mutex_
  std::shared_ptr<GpuPauseHold> gpu_pause_hold_;       // pause_state_mutex_
  bool pause_applied_ = false;                         // pause_queue_ only
  std::atomic<bool> pause_applied_visible_{false};     // mirrors pause_applied_
  uint64_t live_settings_generation_ = 0;              // main thread only
  std::thread pause_stress_thread_;                    // XENIA_PAUSE_STRESS_MS, debug
  std::atomic<bool> pause_stress_stop_{false};

  // Stop watchdog (S17). The state outlives the app object for a delayed block.
  struct StopWatchdog {
    std::mutex mutex;
    bool alive = true;
  };
  std::shared_ptr<StopWatchdog> stop_watchdog_ = std::make_shared<StopWatchdog>();
  std::atomic<uint64_t> stop_epoch_{0};
};

bool EmulatorAppIOS::EnsureProfileServicesReady() {
  auto& ios_context = static_cast<ui::IOSWindowedAppContext&>(app_context());
  if (emulator_ && emulator_->kernel_state() && emulator_->kernel_state()->xam_state()) {
    ios_context.SetProfileServicesReady(true);
    return true;
  }

  ios_context.SetProfileServicesReady(false);
  if (!shutting_down_.load(std::memory_order_acquire) &&
      !emulator_thread_running_.load(std::memory_order_acquire) &&
      !emulator_initialized_.load(std::memory_order_acquire)) {
    StartEmulatorThread({}, false);
  }

  // Do not block here; callers may execute on UI-sensitive paths.
  return emulator_ && emulator_->kernel_state() && emulator_->kernel_state()->xam_state();
}

void EmulatorAppIOS::ApplyGuestDisplayRefreshCapToWindowFromUIThread(bool capped) {
  if (!window_) {
    return;
  }
  static_cast<ui::iOSWindow*>(window_.get())->SetGuestDisplayRefreshCapFromUIThread(capped);
}

std::filesystem::path EmulatorAppIOS::GetIOSStorageRoot() const {
#if XE_PLATFORM_IOS
  // On iOS, use the app's Documents directory as the storage root.
  // This is accessible via Files.app and iTunes file sharing.
  return xe_get_ios_documents_path();
#else
  return xe::filesystem::GetUserFolder() / "Xenia";
#endif
}

std::filesystem::path EmulatorAppIOS::GetStorageRootForCallbacks() const {
  if (!storage_root_.empty()) {
    return storage_root_;
  }
  if (emulator_) {
    return emulator_->storage_root();
  }
  return std::filesystem::absolute(GetIOSStorageRoot());
}

bool EmulatorAppIOS::OnInitialize() {
  ui::IOSAudioSessionStart();

  // Set up storage paths.
  std::filesystem::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = GetIOSStorageRoot();
  }
  storage_root = std::filesystem::absolute(storage_root);
  storage_root_ = storage_root;

  // Create storage directories if they don't exist.
  std::filesystem::create_directories(storage_root);
  EnsureIOSPatchesDirectoryExposed(storage_root);
  XELOGI("iOS: Storage root: {}", xe::strip_pii(xe::path_to_utf8(storage_root)));

  config::SetupConfig(storage_root);

  std::filesystem::path content_root = cvars::content_root;
  if (content_root.empty()) {
    content_root = storage_root / "content";
  } else if (!content_root.is_absolute()) {
    content_root = storage_root / content_root;
  }
  content_root = std::filesystem::absolute(content_root);
  std::filesystem::create_directories(content_root);
  XELOGI("iOS: Content root: {}", xe::strip_pii(xe::path_to_utf8(content_root)));

  std::filesystem::path cache_root = cvars::cache_root;
  if (cache_root.empty()) {
#if XE_PLATFORM_IOS
    @autoreleasepool {
      NSArray* cache_paths =
          NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
      if (cache_paths.count > 0) {
        cache_root = std::filesystem::path([cache_paths[0] UTF8String]) / "xenia";
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
  XELOGI("iOS: Cache root: {}", xe::strip_pii(xe::path_to_utf8(cache_root)));

  // Create the emulator instance.
  emulator_ = std::make_unique<Emulator>("", storage_root, content_root, cache_root);

  emulator_->set_on_exit_to_dashboard([this]() {
    // A game the dashboard started goes back to it, as on a console, unless the
    // app is already stopping it.
    if (dashboard_session_.load(std::memory_order_acquire) &&
        !game_stop_requested_.load(std::memory_order_acquire) &&
        xe::ui::IsIOSDashboardInstalled()) {
      std::lock_guard<std::mutex> lock(launch_mutex_);
      if (!queued_launch_path_.has_value()) {
        queued_launch_path_ = xe::ui::IOSInstalledDashboardPath();
        queued_launch_extras_ = {};
      }
    }
    return RequestGameStop("Exit to dashboard");
  });

  // A disc swap offers the library's copies of the title's discs. It reads the
  // last scan's cache: a scan here would run on the guest thread, open and close
  // the folders' security scopes and rewrite the cache.
  emulator_->set_disc_provider([](uint32_t title_id) {
    std::vector<Emulator::TitleDisc> discs;
    @autoreleasepool {
      for (auto& game : xe::ui::LoadCachedIOSGameLibrary()) {
        if (game.title_id != title_id) {
          continue;
        }
        xe::ui::EnsureDiscoveredGameDiscList(&game);
        for (const auto& disc : game.discs) {
          discs.push_back({disc.label, disc.path, disc.disc_number});
        }
      }
    }
    return discs;
  });

  // The dashboard's Open Tray offers the library's discs.
  emulator_->set_disc_picker(xe::ui::PickIOSDashboardDisc);

  // iOS title-to-title relaunch flow:
  // 1. XamLoaderLaunchTitle invokes this callback with relaunch parameters.
  // 2. We queue the resolved target and preserve launch metadata in cvars.
  // 3. XamLoaderLaunchTitle then terminates the current title.
  // 4. Emulator thread exit triggers StartQueuedLaunchIfIdle().
  emulator_->set_on_launch_new_title([this](const std::string& host_path,
                                            const std::string& launch_module, uint32_t launch_flags,
                                            const std::string& launch_data_hex,
                                            const std::string& xbox_disc,
                                            const std::string& package) {
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
      const std::string relaunch_extension = relaunch_target.extension().string();
      const bool relaunch_target_is_xex = xe_strcasecmp(relaunch_extension.c_str(), ".xex") == 0;
      const bool relaunch_target_is_disc_container =
          xe_strcasecmp(relaunch_extension.c_str(), ".iso") == 0 ||
          xe_strcasecmp(relaunch_extension.c_str(), ".zar") == 0;
      if (relaunch_target.empty()) {
        relaunch_target = launch_module_path;
      } else if (relaunch_target_is_xex) {
        relaunch_target = relaunch_target.parent_path() / launch_module_path;
      } else if (!relaunch_target_is_disc_container) {
        relaunch_target = relaunch_target / launch_module_path;
      }
    }
    relaunch_target = relaunch_target.lexically_normal();
    if (relaunch_target.empty()) {
      XELOGW("iOS: Ignoring title relaunch request with empty target "
             "(host_path='{}', launch_module='{}')",
             xe::strip_pii(host_path), xe::strip_pii(launch_module));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(launch_mutex_);
      queued_launch_path_ = relaunch_target;
      queued_launch_extras_ = {package, xbox_disc.empty() ? std::filesystem::path()
                                                          : std::filesystem::path(xbox_disc)};
    }

    cvars::launch_module = launch_module;
    cvars::launch_flags = launch_flags;
    cvars::launch_data = launch_data_hex;

    RequestGameStop("Title relaunch");

    XELOGI("iOS: Queued title relaunch target='{}' module='{}' flags={} "
           "data_len={} package='{}' xbox_disc={}",
           xe::strip_pii(xe::path_to_utf8(relaunch_target)), xe::strip_pii(launch_module),
           launch_flags, launch_data_hex.size(), package, !xbox_disc.empty());
  });

  emulator_->on_launch.AddListener([](uint32_t title_id, const std::string_view title_name) {
    if (title_id == 0 || title_name.empty()) {
      return;
    }
    @autoreleasepool {
      NSString* caches =
          NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
      if (!caches) {
        return;
      }
      NSString* path = [caches stringByAppendingPathComponent:@"title-names.plist"];
      NSMutableDictionary* dict = [NSMutableDictionary dictionaryWithContentsOfFile:path];
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
  // Diagnostics: the game a crash or a session belongs to, and the JIT's code
  // range, where crash frames in guest code fall.
  emulator_->on_launch.AddListener([this](uint32_t title_id, const std::string_view) {
    uint64_t jit_base = 0;
    uint64_t jit_size = 0;
    cpu::Processor* processor = emulator_->processor();
    if (processor && processor->backend()) {
      if (cpu::backend::CodeCache* code_cache = processor->backend()->code_cache()) {
        jit_base = code_cache->execute_base_address();
        jit_size = code_cache->total_size();
      }
    }
    xe_ios_diagnostics_note_title(title_id, jit_base, jit_size);
  });

  // Create the display window from the Metal view in the app context.
  window_ = ui::Window::Create(app_context(), "Xenia", 1280, 720);
  if (!window_ || !window_->Open()) {
    XELOGE("iOS: Failed to create or open display window");
    return false;
  }

  XELOGI("iOS: Display window created ({}x{})", window_->GetActualPhysicalWidth(),
         window_->GetActualPhysicalHeight());
  ApplyGuestDisplayRefreshCapToWindowFromUIThread(cvars::guest_display_refresh_cap);

  // Register callbacks with the app context.
  auto& ios_context = static_cast<ui::IOSWindowedAppContext&>(app_context());

  ios_context.set_guest_display_refresh_cap_getter(
      []() { return cvars::guest_display_refresh_cap; });
  ios_context.set_guest_display_refresh_cap_setter([this](bool capped) {
    SetGuestDisplayRefreshCap(capped);
    app_context().CallInUIThread(
        [this, capped]() { ApplyGuestDisplayRefreshCapToWindowFromUIThread(capped); });
    std::lock_guard<std::mutex> lock(launch_mutex_);
    config::SaveGameConfigSetting(emulator_.get(), "GPU", "guest_display_refresh_cap", capped);
  });

  ios_context.set_present_letterbox_getter([]() {
    return cvars::present_letterbox &&
           XeniaIOSCurrentWindowScalingMode() != XeniaIOSWindowScalingModeStretch;
  });
  ios_context.set_present_letterbox_setter([this](bool enabled) {
    if (enabled && XeniaIOSCurrentWindowScalingMode() == XeniaIOSWindowScalingModeStretch) {
      enabled = false;
    }
    // At the game config's priority, which the next title's config load drops.
    // The saved game setting below takes over for later launches of this one.
    if (cvar::ConfigVars) {
      auto it = cvar::ConfigVars->find("present_letterbox");
      if (it != cvar::ConfigVars->end()) {
        if (auto* config_var = dynamic_cast<cvar::ConfigVar<bool>*>(it->second)) {
          config_var->SetGameConfigValue(enabled);
        }
      }
    }
    std::lock_guard<std::mutex> lock(launch_mutex_);
    config::SaveGameConfigSetting(emulator_.get(), "Display", "present_letterbox", enabled);
  });

  ios_context.set_guest_display_aspect_ratio_callback([this]() {
    auto* graphics_system = emulator_ ? emulator_->graphics_system() : nullptr;
    return graphics_system ? graphics_system->GetScaledAspectRatio()
                           : std::pair<uint32_t, uint32_t>{16, 9};
  });

  // Forward layout changes (rotation, resize) to the window.
  ios_context.set_layout_changed_callback([this]() {
    if (window_) {
      static_cast<ui::iOSWindow*>(window_.get())->HandleSizeChange();
    }
  });

  ios_context.set_game_launch_callback([this](const std::string& path) {
    if (shutting_down_.load(std::memory_order_acquire)) {
      XELOGW("iOS: Ignoring game launch request while shutting down");
      return;
    }

    if (path.empty()) {
      XELOGI("iOS: Profile services init requested");
    } else {
      XELOGI("iOS: Game launch requested: {}", xe::strip_pii(path));
    }
    auto game_path = std::filesystem::path(path);
    if (!game_path.empty()) {
      // User-driven launches should not reuse prior relaunch metadata.
      cvars::launch_module = "";
      cvars::launch_flags = 0;
      cvars::launch_data = "";
      const bool is_dashboard = xe::ui::IsIOSInstalledDashboardPath(game_path);
      // Going back to the dashboard from one of its games skips the boot
      // animation, as on a console.
      const bool returns_to_dashboard = is_dashboard &&
                                        dashboard_session_.load(std::memory_order_acquire) &&
                                        emulator_thread_running_.load(std::memory_order_acquire);
      dashboard_session_.store(is_dashboard, std::memory_order_release);
      boot_animation_pending_.store(is_dashboard && !returns_to_dashboard,
                                    std::memory_order_release);
    }

    bool should_queue_launch = false;
    {
      std::lock_guard<std::mutex> lock(launch_mutex_);
      if (title_update_install_in_progress_) {
        XELOGW("iOS: Ignoring game launch while title update installation "
               "is in progress");
        // The UI is waiting for this launch: tell it, as for a launch that
        // failed. A game already running keeps its screen.
        if (!game_path.empty() && !emulator_thread_running_.load(std::memory_order_acquire)) {
          app_context().CallInUIThread([this]() {
            auto& context = static_cast<ui::IOSWindowedAppContext&>(app_context());
            context.NotifyGameExited();
            context.NotifyGameLaunchFailed(kTitleUpdateInstallBusyMessage);
          });
        }
        return;
      }
      if (emulator_thread_running_.load(std::memory_order_acquire)) {
        queued_launch_path_ = game_path;
        queued_launch_extras_ = {};
        should_queue_launch = true;
      }
    }
    if (should_queue_launch) {
      XELOGI("iOS: Emulator thread is running; queued launch '{}'",
             xe::strip_pii(xe::path_to_utf8(game_path)));
      if (game_stop_requested_.load(std::memory_order_acquire)) {
        XELOGI("iOS: Current game stop already in progress; queued launch will "
               "start after teardown");
        return;
      }
      RequestGameStop("Queued launch");
      return;
    }

    const bool require_cpu_backend = !game_path.empty();
    StartEmulatorThread(std::move(game_path), require_cpu_backend);
  });

  ios_context.set_game_terminate_callback(
      [this]() { return RequestGameStop("TerminateCurrentGame"); });

  pause_queue_ = dispatch_queue_create("xenia.emulation-pause", DISPATCH_QUEUE_SERIAL);
  // The JIT tool stops the app while it prepares the code region: put that on the launch screen
  // first, so a tool that never answers leaves the screen saying what it waits for.
  cpu::backend::IOSJitMemory::SetExternalPrepareHook([](bool preparing) {
    if (preparing) {
      ui::ios::SetLaunchStageAndWaitUntilShown(ui::ios::LaunchStage::kJit, 400);
    } else {
      ui::ios::SetLaunchStage(ui::ios::LaunchStage::kPreparing);
    }
  });
  ios_context.set_emulation_pause_callback(
      [this](ui::IOSWindowedAppContext::EmulationPauseReason reason, bool paused) {
        SetEmulationPaused(reason, paused);
      });
  // The shader wait sheet holds the guest with its own reason (main thread).
  xe_ios_set_shader_wait_handler(^(BOOL waiting) {
    SetEmulationPaused(ui::IOSWindowedAppContext::EmulationPauseReason::kShaderPreparation,
                       waiting ? true : false);
    // The held game leaves the CPU to the shader compile threads: let them use every core.
    // Kept as a request, which a title that has no GPU yet gets when it launches.
    shader_wait_boost_requested_.store(waiting ? true : false, std::memory_order_release);
    dispatch_async(pause_queue_, ^{
      {
        std::lock_guard<std::mutex> lock(pause_state_mutex_);
        if (!pause_game_running_) {
          return;
        }
      }
      ApplyCompileThreadBoost();
    });
  });
  // A hot device gets no extra compile threads; a change applies at once.
  thermal_observer_ = [[[NSNotificationCenter defaultCenter]
      addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification*) {
                if (!pause_queue_) {
                  return;
                }
                dispatch_async(pause_queue_, ^{
                  {
                    std::lock_guard<std::mutex> lock(pause_state_mutex_);
                    if (!pause_game_running_) {
                      return;
                    }
                  }
                  ApplyCompileThreadBoost();
                });
              }] retain];
  ios_context.set_live_settings_apply_callback(
      [this](const std::function<void()>& apply) { ApplyLiveSettings(apply); });
  ios_context.set_emulation_pause_settled_callback([this](std::function<void()> done) {
    if (!pause_queue_) {
      done();
      return;
    }
    auto shared_done = std::make_shared<std::function<void()>>(std::move(done));
    dispatch_async(pause_queue_, ^{
      app_context().CallInUIThread([shared_done]() { (*shared_done)(); });
    });
  });

  // Kick off profile/content services init in the background ahead of time so a
  // title-update install does not fail its first attempt with "still
  // initializing". Non-blocking: it just starts the headless emulator thread.
  ios_context.set_profile_services_prepare_callback([this]() { EnsureProfileServicesReady(); });

  ios_context.set_title_update_install_callback([this](const std::string& package_path,
                                                       std::string* status_out,
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
      set_status("Profile services are still initializing. Please try again.");
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
      parse_status = emulator_->ProcessContentPackageHeader(entry.path_, entry);
    }
    if (XFAILED(parse_status) || entry.installation_state_ == Emulator::InstallState::failed) {
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
    if (XFAILED(install_status) || entry.installation_state_ != Emulator::InstallState::installed) {
      std::string reason = entry.installation_error_message_.empty()
                               ? "Title update installation failed."
                               : entry.installation_error_message_;
      set_status(reason);
      return false;
    }

    const std::string installed_name =
        entry.name_.empty() ? std::filesystem::path(package_path).filename().string() : entry.name_;
    set_status("Installed title update: " + installed_name);
    return true;
  });

  ios_context.set_controller_state_callback([this](uint32_t user_index,
                                                   hid::X_INPUT_STATE* out_state) {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    if (!out_state || !emulator_ || !emulator_->input_system()) {
      return false;
    }
    return emulator_->input_system()->GetStateForUI(user_index, 1, out_state) == X_ERROR_SUCCESS;
  });

  ios_context.set_gameplay_input_blocked_callback([this](bool blocked) {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    gameplay_input_blocked_.store(blocked, std::memory_order_release);
    if (!emulator_ || !emulator_->input_system()) {
      gameplay_input_blocker_applied_.store(false, std::memory_order_release);
      return;
    }
    if (blocked) {
      if (!gameplay_input_blocker_applied_.exchange(true, std::memory_order_acq_rel)) {
        emulator_->input_system()->AddUIInputBlocker();
      }
    } else if (gameplay_input_blocker_applied_.exchange(false, std::memory_order_acq_rel)) {
      emulator_->input_system()->RemoveUIInputBlocker();
    }
  });

  ios_context.set_profiles_list_callback([this]() {
    std::vector<ui::IOSProfileSummary> profiles;
    if (!EnsureProfileServicesReady() || !emulator_ || !emulator_->kernel_state() ||
        !emulator_->kernel_state()->xam_state()) {
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
    if (!EnsureProfileServicesReady() || !emulator_ || !emulator_->kernel_state() ||
        !emulator_->kernel_state()->xam_state()) {
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

  ios_context.set_achievements_snapshot_callback([this](uint32_t user_index, uint32_t title_id) {
    ui::IOSAchievementsSnapshot snapshot;
    std::lock_guard<std::mutex> lock(launch_mutex_);
    if (!emulator_ || !emulator_->kernel_state() || !emulator_->kernel_state()->xam_state()) {
      return snapshot;
    }

    auto* xam_state = emulator_->kernel_state()->xam_state();
    auto* profile = xam_state->GetUserProfile(user_index);
    if (!profile) {
      return snapshot;
    }

    uint32_t resolved_title_id = title_id;
    if (!resolved_title_id && xam_state->spa_info()) {
      resolved_title_id = xam_state->spa_info()->title_id();
    }
    if (!resolved_title_id) {
      return snapshot;
    }

    snapshot.title_id = resolved_title_id;
    auto title_info =
        xam_state->user_tracker()->GetUserTitleInfo(profile->xuid(), resolved_title_id);
    if (title_info) {
      snapshot.title_name = xe::to_utf8(title_info->title_name);
      snapshot.achievements_total = title_info->achievements_count;
      snapshot.achievements_unlocked = title_info->unlocked_achievements_count;
      snapshot.gamerscore_total = title_info->gamerscore_amount;
      snapshot.gamerscore_earned = title_info->title_earned_gamerscore;
    }

    const auto achievements =
        xam_state->achievement_manager()->GetTitleAchievements(profile->xuid(), resolved_title_id);
    snapshot.achievements.reserve(achievements.size());
    uint32_t computed_unlocked = 0;
    uint32_t computed_total_score = 0;
    uint32_t computed_earned_score = 0;
    for (const auto& achievement : achievements) {
      ui::IOSAchievementEntry entry;
      entry.achievement_id = achievement.achievement_id;
      entry.gamerscore = achievement.gamerscore;
      entry.flags = achievement.flags;
      entry.unlocked = achievement.IsUnlocked();
      entry.unlocked_online = achievement.IsUnlockedOnline();
      entry.show_unachieved =
          (achievement.flags &
           static_cast<uint32_t>(kernel::xam::AchievementFlags::kShowUnachieved)) != 0;
      entry.title = xe::to_utf8(achievement.achievement_name);
      entry.unlocked_description = xe::to_utf8(achievement.unlocked_description);
      entry.locked_description = xe::to_utf8(achievement.locked_description);

      const auto icon = xam_state->achievement_manager()->GetAchievementIcon(
          profile->xuid(), resolved_title_id, achievement.achievement_id);
      entry.icon_data.assign(icon.begin(), icon.end());

      computed_total_score += achievement.gamerscore;
      if (entry.unlocked) {
        ++computed_unlocked;
        computed_earned_score += achievement.gamerscore;
      }
      snapshot.achievements.push_back(std::move(entry));
    }

    if (!snapshot.achievements_total) {
      snapshot.achievements_total = static_cast<uint32_t>(snapshot.achievements.size());
    }
    if (!snapshot.achievements_unlocked) {
      snapshot.achievements_unlocked = computed_unlocked;
    }
    if (!snapshot.gamerscore_total) {
      snapshot.gamerscore_total = computed_total_score;
    }
    if (!snapshot.gamerscore_earned) {
      snapshot.gamerscore_earned = computed_earned_score;
    }
    return snapshot;
  });

  ios_context.set_patch_files_list_callback([this](uint32_t title_id) {
    std::filesystem::path storage_root;
    {
      std::lock_guard<std::mutex> lock(launch_mutex_);
      if (!title_id) {
        return std::vector<ui::IOSPatchFileSummary>();
      }
      storage_root = GetStorageRootForCallbacks();
    }

    std::vector<ui::IOSPatchFileSummary> summaries;
    auto patch_files = EnumerateIOSPatchFilesForTitle(storage_root, title_id);
    summaries.reserve(patch_files.size());
    for (const auto& patch_file : patch_files) {
      summaries.push_back(BuildIOSPatchFileSummary(patch_file, storage_root));
    }
    std::sort(summaries.begin(), summaries.end(),
              [](const ui::IOSPatchFileSummary& a, const ui::IOSPatchFileSummary& b) {
                return a.display_name < b.display_name;
              });
    return summaries;
  });

  ios_context.set_patch_discovery_summary_callback([this](uint32_t title_id) {
    std::filesystem::path storage_root;
    {
      std::lock_guard<std::mutex> lock(launch_mutex_);
      storage_root = GetStorageRootForCallbacks();
    }
    ui::IOSPatchDiscoverySummary summary;
    (void)EnumerateIOSPatchFilesForTitle(storage_root, title_id, &summary);
    return summary;
  });

  ios_context.set_patch_set_enabled_callback([this](uint32_t title_id, const std::string& filename,
                                                    size_t patch_index, bool enabled,
                                                    std::string* status_out) {
    auto set_status = [&](const std::string& status) {
      if (status_out) {
        *status_out = status;
      }
    };
    if (!title_id || filename.empty()) {
      set_status("No patch file was selected.");
      return false;
    }

    std::filesystem::path storage_root;
    {
      std::lock_guard<std::mutex> lock(launch_mutex_);
      storage_root = GetStorageRootForCallbacks();
    }

    auto patch_files = EnumerateIOSPatchFilesForTitle(storage_root, title_id);
    auto patch_file_it = std::find_if(patch_files.begin(), patch_files.end(),
                                      [&](const patcher::PatchSourceFile& patch_file) {
                                        return patch_file.filename == filename;
                                      });
    if (patch_file_it == patch_files.end()) {
      set_status("Patch file was not found.");
      return false;
    }

    const std::filesystem::path storage_path = IOSPatchStoragePath(storage_root, *patch_file_it);
    std::string source_text = std::filesystem::exists(storage_path) ? ReadTextFile(storage_path)
                                                                    : patch_file_it->toml_content;
    std::vector<std::string> lines = SplitPatchLines(source_text);
    if (!UpdatePatchEnabledLine(&lines, patch_index, enabled)) {
      set_status("Failed updating patch state.");
      return false;
    }
    return WritePatchTextAtomically(storage_path, JoinPatchLines(lines), status_out);
  });

  XELOGI("iOS: EmulatorAppIOS initialized successfully");
  return true;
}

void EmulatorAppIOS::SetEmulationPaused(ui::IOSWindowedAppContext::EmulationPauseReason reason,
                                        bool paused) {
  const uint32_t bit = static_cast<uint32_t>(reason);
  const uint32_t before = paused ? pause_desired_.fetch_or(bit, std::memory_order_acq_rel)
                                 : pause_desired_.fetch_and(~bit, std::memory_order_acq_rel);
  if (((before & bit) != 0) == paused) {
    return;
  }
  ScheduleEmulationPauseReconcile();
}

void EmulatorAppIOS::ApplyLiveSettings(const std::function<void()>& apply) {
  bool game_running;
  {
    std::lock_guard<std::mutex> lock(pause_state_mutex_);
    game_running = pause_game_running_;
  }
  if (!game_running || !pause_queue_) {
    apply();
    return;
  }
  // The caller needs |apply| to have run on return, so the first change of a burst waits
  // (bounded) for the pause. The pause is then held until kLiveSettingsHoldNs after the last
  // change, so the following changes find the game already paused and do not wait.
  const uint64_t generation = ++live_settings_generation_;
  pause_desired_.fetch_or(kPauseReasonLiveSettings, std::memory_order_acq_rel);
  ScheduleEmulationPauseReconcile();
  if (!pause_applied_visible_.load(std::memory_order_acquire) &&
      !WaitForEmulationPauseQueue(kLiveSettingsPauseTimeout)) {
    XELOGW("iOS: the game did not pause in time; applying live settings anyway");
  }
  apply();
  // The GPU worker caches the occlusion query mode when the game starts. Queued behind the
  // pause, the switch runs on the worker as the game resumes, with the GPU idle.
  dispatch_async(pause_queue_, ^{
    {
      std::lock_guard<std::mutex> lock(pause_state_mutex_);
      if (!pause_game_running_) {
        return;
      }
    }
    Emulator* emulator = emulator_.get();
    gpu::GraphicsSystem* graphics = emulator ? emulator->graphics_system() : nullptr;
    if (gpu::CommandProcessor* processor = graphics ? graphics->command_processor() : nullptr) {
      processor->ReloadZPDMode();
    }
  });
  auto watchdog = stop_watchdog_;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kLiveSettingsHoldNs), dispatch_get_main_queue(),
                 ^{
                   std::lock_guard<std::mutex> lock(watchdog->mutex);
                   // |this| is valid while |alive|: OnDestroy clears it on the main thread.
                   if (!watchdog->alive || generation != live_settings_generation_) {
                     return;
                   }
                   pause_desired_.fetch_and(~kPauseReasonLiveSettings, std::memory_order_acq_rel);
                   ScheduleEmulationPauseReconcile();
                 });
}

void EmulatorAppIOS::ScheduleEmulationPauseReconcile() {
  if (!pause_queue_) {
    return;
  }
  dispatch_async(pause_queue_, ^{
    ReconcileEmulationPause();
  });
}

bool EmulatorAppIOS::WaitForEmulationPauseQueue(std::chrono::milliseconds timeout) {
  if (!pause_queue_) {
    return true;
  }
  auto waiter = std::make_shared<QueueWaiter>();
  dispatch_async(pause_queue_, ^{
    std::lock_guard<std::mutex> lock(waiter->mutex);
    waiter->done = true;
    waiter->cv.notify_all();
  });
  std::unique_lock<std::mutex> lock(waiter->mutex);
  return waiter->cv.wait_for(lock, timeout, [&waiter]() { return waiter->done; });
}

void EmulatorAppIOS::ReconcileEmulationPause() {
  while (true) {
    bool want;
    {
      std::lock_guard<std::mutex> lock(pause_state_mutex_);
      want = pause_game_running_ && pause_desired_.load(std::memory_order_acquire) != 0;
    }
    if (want == pause_applied_) {
      return;
    }
    if (want) {
      PauseEmulationNow();
    } else {
      ResumeEmulationNow();
    }
    pause_applied_ = want;
    pause_applied_visible_.store(want, std::memory_order_release);
  }
}

void EmulatorAppIOS::PauseEmulationNow() {
  Emulator* emulator = emulator_.get();
  if (!emulator || !emulator->kernel_state()) {
    return;
  }
  // Teardown ends the session and waits for this to return, so each stage first checks that the
  // title is still live. The resume that follows an abandoned pause undoes the stages done.
  auto session_live = [this]() {
    std::lock_guard<std::mutex> lock(pause_state_mutex_);
    return pause_game_running_;
  };
  if (!session_live()) {
    return;
  }
  XELOGI("iOS: pausing emulation");

  // GPU first, then audio, then guest threads, like Emulator::Pause. The GPU
  // worker can be waiting for a value only a guest thread writes (a memory wait
  // in the ring), and it only reaches the pause point once that is written.
  // Emulator::Pause itself is not used: it host-suspends every XThread's host
  // thread, and fiber threads have none (thread() is null), so it would crash.
  // CommandProcessor::Pause is not used either: it waits without a timeout.
  if (auto* graphics = emulator->graphics_system()) {
    if (auto* processor = graphics->command_processor()) {
      auto hold = std::make_shared<GpuPauseHold>();
      const bool metal = g_graphics_is_metal.load(std::memory_order_acquire);
      {
        std::lock_guard<std::mutex> lock(pause_state_mutex_);
        gpu_pause_hold_ = hold;
      }
      // Runs on the GPU worker between ring bursts. Commits the open command
      // buffer and waits for the GPU to finish everything submitted, because
      // iOS does not allow GPU work to be in flight after the app backgrounds.
      processor->CallInThread([hold, metal, processor]() {
        if (metal) {
          gpu::metal::MetalGraphicsSystem::AwaitGpuIdleOnWorkerThread(processor);
        }
        std::unique_lock<std::mutex> lock(hold->mutex);
        hold->reached = true;
        hold->cv.notify_all();
        hold->cv.wait(lock, [&hold]() { return hold->release; });
      });
      std::unique_lock<std::mutex> lock(hold->mutex);
      if (!hold->cv.wait_for(lock, kGpuPauseTimeout, [&hold]() { return hold->reached; })) {
        XELOGW("iOS: the GPU worker did not reach a pause point in {} ms",
               static_cast<int>(kGpuPauseTimeout.count()));
      }
    }
  }

  if (!session_live()) {
    return;
  }
  if (auto* audio = emulator->audio_system()) {
    audio->Pause();
  }
  if (!session_live()) {
    return;
  }

  // Guest threads run as fibers on six dispatch threads. Stop the dispatch
  // threads: running fibers yield at their next safepoint and nothing new starts,
  // including threads created while launching (the scheduler may not have started
  // yet, its dispatch threads then start held). No guest suspend counts are
  // touched and the object table (global critical region) is not walked, so a
  // fiber parked holding the global lock cannot wedge this queue. Host threads
  // (GPU, audio, vblank, kernel dispatch) are not touched: suspending a host
  // thread that holds a lock can deadlock the process.
  if (auto* scheduler = emulator->kernel_state()->guest_scheduler()) {
    uint32_t pending_cpus = 0;
    if (!scheduler->PauseDispatch(kSchedulerPauseTimeout, &pending_cpus)) {
      XELOGW("iOS: guest CPUs {:#x} did not settle in {} ms; they hold at their next slice",
             pending_cpus, static_cast<int>(kSchedulerPauseTimeout.count()));
    }
  }
  XELOGI("iOS: emulation paused");
}

void EmulatorAppIOS::ResumeEmulationNow() {
  Emulator* emulator = emulator_.get();
  XELOGI("iOS: resuming emulation");
  if (emulator) {
    if (auto* audio = emulator->audio_system()) {
      audio->Resume();
    }
  }
  if (emulator && emulator->kernel_state()) {
    if (auto* scheduler = emulator->kernel_state()->guest_scheduler()) {
      scheduler->ResumeDispatch();
    }
  }
  std::shared_ptr<GpuPauseHold> hold;
  {
    std::lock_guard<std::mutex> lock(pause_state_mutex_);
    hold = std::move(gpu_pause_hold_);
  }
  if (hold) {
    hold->Release();
  }
}

void EmulatorAppIOS::BeginEmulationPauseSession() {
  {
    std::lock_guard<std::mutex> lock(pause_state_mutex_);
    pause_game_running_ = true;
  }
  ScheduleEmulationPauseReconcile();
  // Debug: XENIA_PAUSE_STRESS_MS=N toggles the user pause reason at random
  // 20..N ms intervals until the session ends, logging each cycle.
  if (const char* stress = std::getenv("XENIA_PAUSE_STRESS_MS")) {
    const int max_ms = std::max(40, std::atoi(stress));
    pause_stress_stop_.store(false);
    pause_stress_thread_ = std::thread([this, max_ms]() {
      uint32_t rng = 2463534242u;
      auto next_ms = [&]() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return 20 + int(rng % uint32_t(max_ms - 19));
      };
      uint64_t cycle = 0;
      while (!pause_stress_stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(next_ms()));
        SetEmulationPaused(ui::IOSWindowedAppContext::EmulationPauseReason::kUser, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(next_ms()));
        SetEmulationPaused(ui::IOSWindowedAppContext::EmulationPauseReason::kUser, false);
        XELOGI("iOS: pause stress cycle {}", cycle++);
      }
    });
  }
}

void EmulatorAppIOS::EndEmulationPauseSession() {
  pause_stress_stop_.store(true);
  if (pause_stress_thread_.joinable()) {
    pause_stress_thread_.join();
  }
  std::shared_ptr<GpuPauseHold> hold;
  {
    std::lock_guard<std::mutex> lock(pause_state_mutex_);
    pause_game_running_ = false;
    hold = gpu_pause_hold_;
  }
  // A shader wait must not outlive its title: the next one would start paused.
  pause_desired_.fetch_and(
      ~static_cast<uint32_t>(ui::IOSWindowedAppContext::EmulationPauseReason::kShaderPreparation),
      std::memory_order_acq_rel);
  shader_wait_boost_requested_.store(false, std::memory_order_release);
  // Free the GPU worker even if the queue is wedged: teardown joins it.
  if (hold) {
    hold->Release();
  }
  ScheduleEmulationPauseReconcile();
  if (!WaitForEmulationPauseQueue(kResumeForTeardownTimeout)) {
    XELOGW("iOS: the pause queue did not drain before title teardown");
  }
}

void EmulatorAppIOS::ApplyCompileThreadBoost() {
  Emulator* emulator = emulator_.get();
  gpu::GraphicsSystem* graphics = emulator ? emulator->graphics_system() : nullptr;
  if (!graphics || !graphics->command_processor() ||
      !g_graphics_is_metal.load(std::memory_order_acquire)) {
    return;
  }
  const bool hot = [NSProcessInfo processInfo].thermalState >= NSProcessInfoThermalStateSerious;
  gpu::metal::MetalGraphicsSystem::SetCompileBoostAllowed(graphics->command_processor(), !hot);
  gpu::metal::MetalGraphicsSystem::SetShaderWaitCompileBoost(
      graphics->command_processor(), shader_wait_boost_requested_.load(std::memory_order_acquire));
}

void EmulatorAppIOS::ArmStopWatchdog() {
  const uint64_t epoch = stop_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
  auto watchdog = stop_watchdog_;
  const int64_t delay_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(kStopWatchdogDelay).count();
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, delay_ns), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        std::lock_guard<std::mutex> lock(watchdog->mutex);
        // |this| is valid while |alive|: OnDestroy clears it under this mutex.
        if (!watchdog->alive || stop_epoch_.load(std::memory_order_acquire) != epoch ||
            !game_stop_requested_.load(std::memory_order_acquire) ||
            !emulator_thread_running_.load(std::memory_order_acquire)) {
          return;
        }
        XELOGW("iOS: stopping the game has taken more than {} s", kStopWatchdogDelay.count());
        app_context().CallInUIThread([this, watchdog, epoch]() {
          std::lock_guard<std::mutex> lock(watchdog->mutex);
          if (!watchdog->alive || stop_epoch_.load(std::memory_order_acquire) != epoch ||
              !game_stop_requested_.load(std::memory_order_acquire)) {
            return;
          }
          static_cast<ui::IOSWindowedAppContext&>(app_context()).NotifyGameStopStuck();
        });
      });
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
  ArmStopWatchdog();
  // The emulator thread owns reset and teardown. Never terminate guest
  // threads or destroy subsystems from a UIKit or guest callback.
  return true;
}

void EmulatorAppIOS::ClearGameplayInputBlockerIfApplied() {
  std::lock_guard<std::mutex> lock(launch_mutex_);
  if (!gameplay_input_blocker_applied_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (emulator_ && emulator_->input_system()) {
    emulator_->input_system()->RemoveUIInputBlocker();
  }
}

void EmulatorAppIOS::ClearPresenterForTitleExit() {
  if (!emulator_ || !emulator_->graphics_system()) {
    return;
  }

  auto* presenter = emulator_->graphics_system()->presenter();
  if (!presenter) {
    return;
  }

  presenter->RefreshGuestOutput(0, 0, 0, 0,
                                [](ui::Presenter::GuestOutputRefreshContext&) { return true; });

  if (!app_context().CallInUIThreadSynchronous(
          [presenter]() { presenter->PaintFromUIThread(true); })) {
    XELOGW("iOS: failed to clear presenter output before title teardown");
    return;
  }
}

void EmulatorAppIOS::StartEmulatorThread(std::filesystem::path game_path,
                                         bool require_cpu_backend, LaunchExtras extras) {
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    pending_worker_launch_.emplace(std::move(game_path), require_cpu_backend, std::move(extras));
    game_stop_requested_.store(false, std::memory_order_release);
    emulator_thread_running_.store(true, std::memory_order_release);
  }
  if (!emulator_thread_.joinable()) {
    worker_running_.store(true, std::memory_order_release);
    emulator_thread_ = std::thread([this]() {
      // SDL setup, title reset and final destruction stay on one host thread.
      while (true) {
        std::tuple<std::filesystem::path, bool, LaunchExtras> launch;
        {
          std::unique_lock<std::mutex> lock(launch_mutex_);
          launch_condition_.wait(lock, [this]() {
            return shutting_down_.load(std::memory_order_acquire) ||
                   pending_worker_launch_.has_value();
          });
          if (shutting_down_.load(std::memory_order_acquire)) {
            break;
          }
          launch = std::move(*pending_worker_launch_);
          pending_worker_launch_.reset();
        }
        EmulatorThread(std::get<0>(launch), std::get<1>(launch), std::get<2>(launch));
        emulator_thread_running_.store(false, std::memory_order_release);
        game_stop_requested_.store(false, std::memory_order_release);
        app_context().CallInUIThread([this]() { StartQueuedLaunchIfIdle(); });
      }
      emulator_.reset();
      worker_running_.store(false, std::memory_order_release);
    });
  }
  launch_condition_.notify_one();
}

void EmulatorAppIOS::StartQueuedLaunchIfIdle() {
  if (shutting_down_.load(std::memory_order_acquire) ||
      emulator_thread_running_.load(std::memory_order_acquire)) {
    return;
  }

  std::optional<std::filesystem::path> queued_path;
  LaunchExtras queued_extras;
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
    queued_extras = std::exchange(queued_launch_extras_, {});
  }

  if (!queued_path.has_value() || queued_path->empty()) {
    return;
  }

  XELOGI("iOS: Starting queued game launch: {}", xe::strip_pii(xe::path_to_utf8(*queued_path)));
  @autoreleasepool {
    [[NSNotificationCenter defaultCenter]
        postNotificationName:XeniaIOSTitleWillStartNotification
                      object:nil
                    userInfo:@{
                      XeniaIOSTitlePathKey : [NSString
                          stringWithUTF8String:xe::path_to_utf8(*queued_path).c_str()]
                          ?: @""
                    }];
  }
  StartEmulatorThread(std::move(*queued_path), true, std::move(queued_extras));
}

void EmulatorAppIOS::OnDestroy() {
  XELOGI("iOS: EmulatorAppIOS::OnDestroy invoked");
  shutting_down_.store(true, std::memory_order_release);
  {
    // Delayed stop-watchdog blocks stop touching |this| from here on.
    std::lock_guard<std::mutex> lock(stop_watchdog_->mutex);
    stop_watchdog_->alive = false;
  }
  auto& ios_context = static_cast<ui::IOSWindowedAppContext&>(app_context());
  ios_context.set_emulation_pause_callback({});
  xe_ios_set_shader_wait_handler(nil);
  if (thermal_observer_) {
    [[NSNotificationCenter defaultCenter] removeObserver:thermal_observer_];
    [thermal_observer_ release];
    thermal_observer_ = nil;
  }
  ios_context.set_live_settings_apply_callback({});
  ios_context.set_emulation_pause_settled_callback({});
  ios_context.set_game_launch_failed_callback({});
  ios_context.set_game_stop_stuck_callback({});
  ios_context.set_guest_display_refresh_cap_getter({});
  ios_context.set_guest_display_refresh_cap_setter({});
  ios_context.set_present_letterbox_getter({});
  ios_context.set_present_letterbox_setter({});
  ios_context.set_guest_display_aspect_ratio_callback({});
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    queued_launch_path_.reset();
  }
  if (emulator_thread_.joinable()) {
    RequestGameStop("OnDestroy");
    launch_condition_.notify_one();
    // Presenter destruction must run on UIKit's thread. Service Edge's UI
    // queue while the worker finishes instead of joining with it blocked.
    while (worker_running_.load(std::memory_order_acquire)) {
      app_context().ExecutePendingFunctionsFromUIThread();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    emulator_thread_.join();
    app_context().ExecutePendingFunctionsFromUIThread();
  }
  if (pause_queue_) {
    // The game is gone, so this returns at once; the bound is for safety.
    WaitForEmulationPauseQueue(std::chrono::milliseconds(2000));
    dispatch_release(pause_queue_);
    pause_queue_ = nullptr;
  }
  xe_ios_idle_timer_release(@"game");
  if (emulator_) {
    xe_ios_session_stats_note_halted_faults(emulator_->halted_fault_count());
  }
  xe_ios_session_note_game_running(NO);
  emulator_.reset();
  window_.reset();
}

void EmulatorAppIOS::EmulatorThread(const std::filesystem::path& game_path,
                                    bool require_cpu_backend, const LaunchExtras& extras) {
  xe::threading::set_name("Emulator Thread");
  const qos_class_t qos =
      cvars::ios_emulator_thread_user_initiated_qos ? QOS_CLASS_USER_INITIATED : QOS_CLASS_DEFAULT;
  pthread_set_qos_class_self_np(qos, 0);
  const bool launched_with_game = !game_path.empty();

  // The launch screen follows the launch from here to the game's first frame.
  struct LaunchStatusScope {
    bool open = false;
    ~LaunchStatusScope() {
      if (open) {
        ui::ios::EndLaunchStatus();
      }
    }
  } launch_status;
  if (launched_with_game) {
    ui::ios::BeginLaunchStatus();
    launch_status.open = true;
    XELOGI("iOS: Loading game config for: {}", xe::strip_pii(xe::path_to_utf8(game_path)));
    config::LoadGameConfigForFile(game_path);
    app_context().CallInUIThread([this]() {
      ApplyGuestDisplayRefreshCapToWindowFromUIThread(cvars::guest_display_refresh_cap);
    });
  }

  // Keeps the screen awake while a game runs: controller input doesn't reset
  // the idle timer. Released on every way out of this function.
  struct IdleTimerHold {
    bool held = false;
    ~IdleTimerHold() {
      if (held) {
        xe_ios_idle_timer_release(@"game");
      }
    }
  } idle_timer_hold;
  if (launched_with_game) {
    xe_ios_idle_timer_hold(@"game");
    idle_timer_hold.held = true;
  }

  bool game_exit_notified = false;
  // |failure_reason|, when given, is a short user-facing sentence (no host paths)
  // shown by the UI before it returns to the library.
  auto notify_game_exited = [this, &game_exit_notified](const char* failure_reason = nullptr) {
    if (emulator_) {
      xe_ios_session_stats_note_halted_faults(emulator_->halted_fault_count());
    }
    xe_ios_session_note_game_running(NO);
    if (game_exit_notified) {
      return;
    }
    game_exit_notified = true;
    std::string reason = failure_reason ? failure_reason : "";
    app_context().CallInUIThread([this, reason]() {
      auto& ios_context = static_cast<ui::IOSWindowedAppContext&>(app_context());
      // A pause left on by the game that just ended must not pause the next one.
      SetEmulationPaused(ui::IOSWindowedAppContext::EmulationPauseReason::kUser, false);
      // The exit first: it shows the library and dismisses sheets, which would take the alert
      // with them.
      ios_context.NotifyGameExited();
      if (!reason.empty()) {
        ios_context.NotifyGameLaunchFailed(reason);
      }
    });
  };

  struct ScopeExit {
    std::function<void()> fn;
    ~ScopeExit() {
      if (fn) {
        fn();
      }
    }
  } notify_exit{[this, launched_with_game, &notify_game_exited]() {
    if (!launched_with_game) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(launch_mutex_);
      if (queued_launch_path_.has_value()) {
        XELOGI("iOS: keeping game UI active for queued launch");
        return;
      }
    }
    notify_game_exited();
  }};

  const bool need_profile_mode_transition =
      !emulator_initialized_.load(std::memory_order_acquire) ||
      (require_cpu_backend && !emulator_cpu_initialized_.load(std::memory_order_acquire));

  if (need_profile_mode_transition) {
    if (emulator_initialized_.load(std::memory_order_acquire) &&
        !emulator_cpu_initialized_.load(std::memory_order_acquire) && require_cpu_backend) {
      XELOGI("iOS: Reinitializing emulator for game mode");
      static_cast<ui::IOSWindowedAppContext&>(app_context()).SetProfileServicesReady(false);
      emulator_->Shutdown();
    }

    // Edge selects NullBackend for an unknown CPU when a backend isn't
    // required. Profile browsing must not allocate JIT memory.
    const std::string saved_cpu = cvars::cpu;
    if (!require_cpu_backend) {
      cvars::cpu = "none";
    }
    X_STATUS setup_result =
        emulator_->Setup(window_.get(),
                         nullptr,  // No ImGui drawer on iOS for now.
                         require_cpu_backend, require_cpu_backend ? CreateAudioSystem : nullptr,
                         require_cpu_backend ? CreateGraphicsSystem : nullptr,
                         require_cpu_backend ? CreateInputDrivers : nullptr);
    cvars::cpu = saved_cpu;

    if (XFAILED(setup_result)) {
      XELOGE("iOS: Emulator::Setup failed with status {:08X}", setup_result);
      emulator_initialized_.store(false, std::memory_order_release);
      emulator_cpu_initialized_.store(false, std::memory_order_release);
      static_cast<ui::IOSWindowedAppContext&>(app_context()).SetProfileServicesReady(false);
      if (launched_with_game) {
        notify_game_exited(kEmulatorSetupFailedMessage);
      }
      return;
    }

    emulator_->MountStandardDrives();

    emulator_initialized_.store(true, std::memory_order_release);
    emulator_cpu_initialized_.store(require_cpu_backend, std::memory_order_release);
    static_cast<ui::IOSWindowedAppContext&>(app_context()).SetProfileServicesReady(true);
    XELOGI("iOS: Emulator setup complete");

    if (!launched_with_game) {
      app_context().CallInUIThread([this]() {
        auto& ios_context = static_cast<ui::IOSWindowedAppContext&>(app_context());
        ios_context.NotifyProfileServicesReady();
      });
    }
  }

  if (require_cpu_backend && !subsystems_ready_) {
    X_STATUS subsystem_result = emulator_->SetupSubsystems();
    if (XFAILED(subsystem_result)) {
      XELOGE("iOS: Emulator::SetupSubsystems failed with status {:08X}", subsystem_result);
      emulator_initialized_.store(false, std::memory_order_release);
      emulator_cpu_initialized_.store(false, std::memory_order_release);
      static_cast<ui::IOSWindowedAppContext&>(app_context()).SetProfileServicesReady(false);
      if (launched_with_game) {
        notify_game_exited(kSubsystemSetupFailedMessage);
      }
      return;
    }
    emulator_->MountStandardDrives();
    if (emulator_->input_system()) {
      if (gameplay_input_blocked_.load(std::memory_order_acquire)) {
        if (!gameplay_input_blocker_applied_.exchange(true, std::memory_order_acq_rel)) {
          emulator_->input_system()->AddUIInputBlocker();
        }
      } else if (gameplay_input_blocker_applied_.exchange(false, std::memory_order_acq_rel)) {
        emulator_->input_system()->RemoveUIInputBlocker();
      }
    }
    if (auto* fs = emulator_->file_system()) {
      std::string cache_target;
      if (fs->FindSymbolicLink("cache:", cache_target)) {
        XELOGI("iOS: cache: mounted to {}", xe::strip_pii(cache_target));
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
      static_cast<ui::IOSWindowedAppContext&>(app_context()).SetProfileServicesReady(false);
      if (launched_with_game) {
        notify_game_exited(kPresenterAttachFailedMessage);
      }
      return;
    }

    subsystems_ready_ = true;
  }

  if (!game_path.empty()) {
    auto abs_path = std::filesystem::absolute(game_path);
    XELOGI("iOS: Launching game: {}", xe::strip_pii(xe::path_to_utf8(abs_path)));

    {
      auto xam = emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
      if (xam) {
        auto& loader_data = xam->loader_data();
        loader_data.host_path = xe::path_to_utf8(abs_path);
        loader_data.package_path = extras.package;
        loader_data.launch_data_present = false;
        loader_data.launch_flags = 0;
        loader_data.launch_data.clear();
        if (cvars::launch_flags != 0 || !cvars::launch_data.empty()) {
          loader_data.launch_data_present = true;
          loader_data.launch_flags = cvars::launch_flags;
          const std::string& hex = cvars::launch_data;
          for (size_t i = 0; i + 1 < hex.length(); i += 2) {
            std::string byte_str = hex.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            loader_data.launch_data.push_back(byte);
          }
        }
      }
    }

    auto teardown_title_session = [this]() {
      // Faults that halted a guest thread leave the game running, so the
      // session would not otherwise show them.
      xe_ios_session_stats_note_halted_faults(emulator_->halted_fault_count());
      xe_ios_session_note_game_running(NO);
      // Resume first: ResetTitle joins the GPU worker and terminates guest
      // threads, and neither can finish while held at a pause point.
      EndEmulationPauseSession();
      ClearGameplayInputBlockerIfApplied();
      ClearPresenterForTitleExit();

      static_cast<ui::IOSWindowedAppContext&>(app_context()).SetProfileServicesReady(false);
      // Edge stops the GPU worker, scheduler and guest threads in order.
      // Retain its freshly reset kernel and input drivers for the next title.
      emulator_->ResetTitle();
      emulator_initialized_.store(true, std::memory_order_release);
      emulator_cpu_initialized_.store(true, std::memory_order_release);
      subsystems_ready_ = false;
      static_cast<ui::IOSWindowedAppContext&>(app_context()).SetProfileServicesReady(true);
    };

    xe_ios_session_note_game_running(YES);
    // Pauses (a covered or backgrounded scene) apply from here on, not only once the title is up:
    // LaunchPath starts the guest threads and the GPU.
    BeginEmulationPauseSession();
    // The dashboard lists the library as its games.
    emulator_->SetLibraryTitles(xe::ui::IOSDashboardLibraryTitles());
    if (boot_animation_pending_.exchange(false, std::memory_order_acq_rel)) {
      emulator_->RequestBootAnimation();
    }
    ui::ios::SetLaunchStage(ui::ios::LaunchStage::kLoading);
    // ResetTitle emptied the drive, which XeFu's original Xbox game needs kept.
    if (!extras.xbox_disc.empty()) {
      emulator_->InsertXboxGame(extras.xbox_disc);
    }
    X_STATUS launch_result = emulator_->LaunchPath(abs_path, extras.package);
    cvars::launch_module = "";
    cvars::launch_flags = 0;
    cvars::launch_data = "";
    if (XFAILED(launch_result)) {
#if XE_PLATFORM_IOS
      const bool launch_interrupted_by_title_stop =
          game_stop_requested_.load(std::memory_order_acquire);
      if (launch_result == X_STATUS_PROCESS_IS_TERMINATING || launch_interrupted_by_title_stop) {
        XELOGI("iOS: Game launch interrupted by title stop; running title-exit "
               "teardown");
        teardown_title_session();
        return;
      }
#endif  // XE_PLATFORM_IOS
      XELOGE("iOS: Failed to launch game: {:08X}", launch_result);
      EndEmulationPauseSession();
      notify_game_exited(LaunchFailureMessage(launch_result).c_str());
      return;
    }

    XELOGI("iOS: Game launched successfully");
    // A shader wait that began before the GPU existed.
    ApplyCompileThreadBoost();
    if (g_graphics_is_metal.load(std::memory_order_acquire)) {
      // Until the first frame, which only the Metal backend reports.
      ui::ios::SetLaunchStage(ui::ios::LaunchStage::kStarting);
    } else {
      ui::ios::EndLaunchStatus();
    }
    // Hold the main thread object while waiting. Both user stop requests and
    // guest dashboard requests are serviced here, on a non-guest thread.
    auto main_thread = emulator_->kernel_state()->GetThreadByID(emulator_->main_thread_id());
    while (main_thread && !game_stop_requested_.load(std::memory_order_acquire)) {
      const auto result =
          xe::threading::Wait(main_thread->wait_handle(), false, std::chrono::milliseconds(50));
      if (result == xe::threading::WaitResult::kSuccess ||
          result == xe::threading::WaitResult::kFailed) {
        break;
      }
    }
    main_thread.reset();
    XELOGI("iOS: Game execution finished (exit wait completed)");

    teardown_title_session();
  }
}

std::unique_ptr<apu::AudioSystem> EmulatorAppIOS::CreateAudioSystem(cpu::Processor* processor) {
  const std::string apu_name = cvars::apu;
  if (apu_name == "phase") {
    XELOGI("Audio backend: PHASE (spatial) selected via --apu/apu cvar");
    return std::make_unique<apu::phase::PHASEAudioSystem>(processor);
  }
  if (apu_name == "nop") {
    XELOGI("Audio backend: NOP selected via --apu/apu cvar");
    return std::make_unique<apu::nop::NopAudioSystem>(processor);
  }
  if (apu_name != "sdl") {
    XELOGW("Unknown iOS audio backend '{}'; falling back to SDL", apu_name);
  } else {
    XELOGI("Audio backend: SDL selected via --apu/apu cvar");
  }
  return std::make_unique<apu::sdl::SDLAudioSystem>(processor);
}

std::unique_ptr<gpu::GraphicsSystem> EmulatorAppIOS::CreateGraphicsSystem() {
  const std::string gpu_implementation_name = cvars::gpu;
#if defined(XE_IOS_MOLTENVK_ENABLED)
  if (gpu_implementation_name == "vulkan") {
    g_graphics_is_metal.store(false, std::memory_order_release);
    return std::make_unique<gpu::vulkan::VulkanGraphicsSystem>();
  }
#endif
  g_graphics_is_metal.store(true, std::memory_order_release);
  if (gpu_implementation_name != "metal") {
    XELOGW("iOS: unsupported GPU backend '{}', falling back to Metal", gpu_implementation_name);
  }
  return std::make_unique<gpu::metal::MetalGraphicsSystem>();
}

std::vector<std::unique_ptr<hid::InputDriver>> EmulatorAppIOS::CreateInputDrivers(
    ui::Window* window) {
  std::vector<std::unique_ptr<hid::InputDriver>> drivers;

  auto touch_driver = xe::hid::touch::Create(window, kZOrderHidInput);
  if (touch_driver && XSUCCEEDED(touch_driver->Setup())) {
    drivers.emplace_back(std::move(touch_driver));
  } else {
    XELOGW("iOS: Touch input driver setup failed");
  }

  if (drivers.empty()) {
    drivers.emplace_back(xe::hid::nop::Create(window, kZOrderHidInput));
  }
  return drivers;
}

}  // namespace app
}  // namespace xe

XE_DEFINE_WINDOWED_APP(xenia, xe::app::EmulatorAppIOS::Create);
