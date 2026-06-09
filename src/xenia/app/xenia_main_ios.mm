/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
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
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/ui/ios/app/ios_window_layout.h"
#include "xenia/ui/ios/app/windowed_app_context_ios.h"
#include "xenia/ui/ios/game/window_ios.h"
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
#include "xenia/apu/phase/phase_audio_system.h"
#include "xenia/apu/sdl/sdl_audio_system.h"

// Input drivers.
#include "xenia/hid/input_system.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/hid/sdl/sdl_hid.h"
#include "xenia/hid/touch/touch_hid_ios.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_state.h"
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
DEFINE_string(apu, "phase", "Audio system. Use: [sdl, phase, nop]", "APU");
DEFINE_string(gpu, "metal", "Graphics system. Use: [metal, vulkan]", "GPU");
DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");
DEFINE_bool(mount_cache, true, "Enable cache mount", "Storage");
DEFINE_bool(mount_memory_unit, false, "Enable memory unit (MU) mount", "Storage");
DEFINE_bool(ios_emulator_thread_user_initiated_qos, false,
            "Run the iOS emulator host thread at user-initiated QoS. This is an "
            "iOS scheduler experiment for devices where guest CPU work falls behind.",
            "iOS");

// CVar normally defined in windowed_app_main_qt.cc (excluded on iOS).
DEFINE_transient_path(target, "", "Specifies the target file to run.", "General");
DECLARE_string(launch_module);
DECLARE_uint32(launch_flags);
DECLARE_string(launch_data);

namespace xe {
namespace app {

namespace {

std::string NSErrorDescription(NSError* error) {
  if (!error) {
    return "unknown error";
  }
  NSString* description = [error localizedDescription];
  return description ? std::string([description UTF8String]) : "unknown error";
}

void ConfigureAudioSession() {
  static std::once_flag once;
  std::call_once(once, []() {
    // AVAudioSession's setters — especially setActive: — are synchronous and can
    // block for a noticeable amount of time, so calling them on the main thread
    // risks UI unresponsiveness. Configuration here is fire-and-forget at init
    // time (audio playback only begins once a title is running), so hop onto a
    // background queue and let it complete asynchronously.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;

        if (![session setCategory:AVAudioSessionCategoryPlayback
                             mode:AVAudioSessionModeDefault
                          options:0
                            error:&error]) {
          XELOGW("iOS audio session: setCategory failed: {}", NSErrorDescription(error));
          error = nil;
        }

        if (![session setPreferredSampleRate:48000.0 error:&error]) {
          XELOGW("iOS audio session: setPreferredSampleRate failed: {}",
                 NSErrorDescription(error));
          error = nil;
        }

        constexpr NSTimeInterval kPreferredIOBufferDuration = 0.02;
        if (![session setPreferredIOBufferDuration:kPreferredIOBufferDuration error:&error]) {
          XELOGW("iOS audio session: setPreferredIOBufferDuration failed: {}",
                 NSErrorDescription(error));
          error = nil;
        }

        if (![session setActive:YES error:&error]) {
          XELOGW("iOS audio session: activation failed: {}", NSErrorDescription(error));
          error = nil;
        }

        XELOGI("iOS audio session: category={}, sample_rate={}, "
               "preferred_sample_rate={}, io_buffer={}, preferred_io_buffer={}",
               [[session category] UTF8String], [session sampleRate],
               [session preferredSampleRate], [session IOBufferDuration],
               [session preferredIOBufferDuration]);
      }
    });
  });
}

std::filesystem::path IOSPatchesDirectory(const std::filesystem::path& storage_root) {
  return storage_root / "patches";
}

std::filesystem::path IOSPatchStoragePath(const std::filesystem::path& storage_root,
                                          const patcher::BundledPatchFile& patch_file) {
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

void LogIOSProvisioningProfileHints() {
  @autoreleasepool {
    NSString* profile_path =
        [[NSBundle mainBundle] pathForResource:@"embedded"
                                        ofType:@"mobileprovision"];
    if (!profile_path.length) {
      XELOGW("iOS signing: embedded.mobileprovision not present in app bundle");
      return;
    }

    const std::filesystem::path profile_fs_path([profile_path UTF8String]);
    const std::string profile = ReadTextFile(profile_fs_path);
    auto contains_entitlement = [&profile](const char* entitlement) {
      return profile.find(entitlement) != std::string::npos;
    };
    XELOGI("iOS signing: embedded.mobileprovision bytes={} "
           "increased_memory_limit={} file_picker_read_write={}",
           profile.size(),
           contains_entitlement(
               "com.apple.developer.kernel.increased-memory-limit"),
           contains_entitlement(
               "com.apple.security.files.user-selected.read-write"));
  }
}

std::string DisplayNameForPatchFile(const patcher::BundledPatchFile& bundled) {
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

void EnsureIOSPatchesDirectoryExposed(const std::filesystem::path& storage_root) {
  const std::filesystem::path patches_dir = IOSPatchesDirectory(storage_root);
  std::error_code ec;
  std::filesystem::create_directories(patches_dir, ec);
  if (ec) {
    XELOGW("iOS patches: failed creating Documents patches folder {}: {}", patches_dir,
           ec.message());
    return;
  }

  size_t copied_files = 0;
  size_t existing_files = 0;
  size_t failed_files = 0;
  for (const patcher::BundledPatchFile& bundled : patcher::EnumerateBundledPatches()) {
    const std::filesystem::path storage_path = IOSPatchStoragePath(storage_root, bundled);
    ec.clear();
    if (std::filesystem::exists(storage_path, ec)) {
      ++existing_files;
      continue;
    }
    if (ec) {
      ++failed_files;
      XELOGW("iOS patches: failed checking {}: {}", storage_path, ec.message());
      continue;
    }

    std::ofstream file(storage_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      ++failed_files;
      XELOGW("iOS patches: failed opening {} for write", storage_path);
      continue;
    }
    file.write(bundled.toml_content.data(),
               static_cast<std::streamsize>(bundled.toml_content.size()));
    if (!file.good()) {
      ++failed_files;
      XELOGW("iOS patches: failed writing {}", storage_path);
      continue;
    }
    ++copied_files;
  }

  XELOGI("iOS patches: exposed Documents patches folder {} (copied={}, "
         "existing={}, failed={})",
         patches_dir, copied_files, existing_files, failed_files);
}

std::vector<patcher::BundledPatchFile> EnumerateIOSPatchFilesForTitle(
    const std::filesystem::path& storage_root, uint32_t title_id,
    ui::IOSPatchDiscoverySummary* summary_out = nullptr) {
  std::vector<patcher::BundledPatchFile> patch_files =
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

    patcher::BundledPatchFile local_patch_file;
    local_patch_file.filename = filename;
    local_patch_file.toml_content = toml_content;
    local_patch_file.entry = std::move(entry);

    auto existing = std::find_if(patch_files.begin(), patch_files.end(),
                                 [&](const patcher::BundledPatchFile& candidate) {
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

ui::IOSPatchFileSummary BuildIOSPatchFileSummary(const patcher::BundledPatchFile& bundled,
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
    XELOGE("iOS patches: failed to parse {}: {}", bundled.filename, e.what());
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
  void StartEmulatorThread(std::filesystem::path game_path, bool require_cpu_backend);
  void StartQueuedLaunchIfIdle();
  void EmulatorThread(const std::filesystem::path& game_path, bool require_cpu_backend);
  void ApplyGuestDisplayRefreshCapToWindowFromUIThread(bool capped);
  bool EnsureProfileServicesReady();

  static std::unique_ptr<apu::AudioSystem> CreateAudioSystem(cpu::Processor* processor);
  static std::unique_ptr<gpu::GraphicsSystem> CreateGraphicsSystem();
  static std::vector<std::unique_ptr<hid::InputDriver>> CreateInputDrivers(ui::Window* window);

  std::unique_ptr<Emulator> emulator_;
  std::filesystem::path storage_root_;
  std::unique_ptr<ui::Window> window_;
  std::thread emulator_thread_;
  std::atomic<bool> emulator_thread_running_{false};
  std::atomic<bool> game_stop_requested_{false};
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> gameplay_input_blocked_{false};
  std::atomic<bool> gameplay_input_blocker_applied_{false};
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
  ConfigureAudioSession();
  LogIOSProvisioningProfileHints();

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
  XELOGI("iOS: Cache root: {}", cache_root);

  // Create the emulator instance.
  emulator_ = std::make_unique<Emulator>("", storage_root, content_root, cache_root);

  // iOS title-to-title relaunch flow:
  // 1. XamLoaderLaunchTitle invokes this callback with relaunch parameters.
  // 2. We queue the resolved target and preserve launch metadata in cvars.
  // 3. XamLoaderLaunchTitle then terminates the current title.
  // 4. Emulator thread exit triggers StartQueuedLaunchIfIdle().
  emulator_->set_on_launch_new_title([this](const std::string& host_path,
                                            const std::string& launch_module, uint32_t launch_flags,
                                            const std::string& launch_data_hex) {
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
      } else if (relaunch_target.extension() == ".xex" || relaunch_target.extension() == ".XEX") {
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
           relaunch_target.string(), launch_module, launch_flags, launch_data_hex.size());
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
    if (enabled &&
        XeniaIOSCurrentWindowScalingMode() == XeniaIOSWindowScalingModeStretch) {
      enabled = false;
    }
    cvars::present_letterbox = enabled;
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
                                      [&](const patcher::BundledPatchFile& patch_file) {
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
  auto& ios_context = static_cast<ui::IOSWindowedAppContext&>(app_context());
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
#if XE_PLATFORM_IOS
  xe::threading::set_current_thread_qos(xe::threading::ThreadQoS::kDefault);
  if (cvars::ios_emulator_thread_user_initiated_qos) {
    if (xe::threading::set_current_thread_qos(xe::threading::ThreadQoS::kUserInitiated)) {
      XELOGI("iOS: Emulator Thread QoS set to user-initiated");
    } else {
      XELOGW("iOS: Emulator Thread QoS request failed");
    }
  }
#endif  // XE_PLATFORM_IOS
  const bool launched_with_game = !game_path.empty();

  if (launched_with_game) {
    XELOGI("iOS: Loading game config for: {}", game_path.string());
    config::LoadGameConfigForFile(game_path);
    app_context().CallInUIThread([this]() {
      ApplyGuestDisplayRefreshCapToWindowFromUIThread(cvars::guest_display_refresh_cap);
    });
  }

  bool game_exit_notified = false;
  auto notify_game_exited = [this, &game_exit_notified]() {
    if (game_exit_notified) {
      return;
    }
    game_exit_notified = true;
    app_context().CallInUIThread([this]() {
      auto& ios_context = static_cast<ui::IOSWindowedAppContext&>(app_context());
      ios_context.NotifyGameExited();
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
      emulator_->ShutdownForTitleExitIOS();
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
        notify_game_exited();
      }
      return;
    }

    if (require_cpu_backend) {
      X_STATUS subsystem_result = emulator_->SetupSubsystems();
      if (XFAILED(subsystem_result)) {
        XELOGE("iOS: Emulator::SetupSubsystems failed with status {:08X}", subsystem_result);
        emulator_initialized_.store(false, std::memory_order_release);
        emulator_cpu_initialized_.store(false, std::memory_order_release);
        if (launched_with_game) {
          notify_game_exited();
        }
        return;
      }
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
        XELOGI("iOS: cache: mounted to {}", cache_target);
      } else {
        XELOGW("iOS: cache: mount missing after MountStandardDrives");
      }
    }

    auto* graphics_system = emulator_->graphics_system();
    if (graphics_system) {
      auto* ios_context = &static_cast<ui::IOSWindowedAppContext&>(app_context());
      graphics_system->SetScaledAspectRatioChangedCallback([ios_context](uint32_t, uint32_t) {
        ios_context->NotifyGuestDisplayAspectRatioChanged();
      });
    }
    auto* presenter = graphics_system ? graphics_system->presenter() : nullptr;
    if (presenter && !app_context().CallInUIThreadSynchronous([this, presenter]() {
          if (window_) {
            window_->SetPresenter(presenter);
          }
        })) {
      XELOGE("iOS: Failed to attach presenter to display window");
      emulator_initialized_.store(false, std::memory_order_release);
      emulator_cpu_initialized_.store(false, std::memory_order_release);
      if (launched_with_game) {
        notify_game_exited();
      }
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

  if (!game_path.empty()) {
    auto abs_path = std::filesystem::absolute(game_path);
    XELOGI("iOS: Launching game: {}", abs_path);

    {
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
            uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            loader_data.launch_data.push_back(byte);
          }
        }
      }
    }

    auto teardown_title_session = [this]() {
      if (auto* kernel_state = emulator_->kernel_state()) {
        if (!kernel_state->WaitForTitleThreadsToExitIOS(2000)) {
          XELOGW("iOS: title stop did not complete cleanly before emulator "
                 "teardown");
        }
      }

      ClearGameplayInputBlockerIfApplied();
      ClearPresenterForTitleExit();

      // Let GraphicsSystem::Shutdown drain the Metal command processor before
      // its UI-thread presenter reset disconnects the CAMetalLayer surface.
      emulator_->ShutdownForTitleExitIOS();
      emulator_initialized_.store(false, std::memory_order_release);
      emulator_cpu_initialized_.store(false, std::memory_order_release);
    };

    X_STATUS launch_result = emulator_->LaunchPath(abs_path);
    cvars::launch_module = "";
    cvars::launch_flags = 0;
    cvars::launch_data = "";
    if (XFAILED(launch_result)) {
#if XE_PLATFORM_IOS
      const bool launch_interrupted_by_title_stop =
          emulator_->kernel_state() && emulator_->kernel_state()->IsTitleStopRequestedIOS();
      if (launch_result == X_STATUS_PROCESS_IS_TERMINATING || launch_interrupted_by_title_stop) {
        XELOGI("iOS: Game launch interrupted by title stop; running title-exit "
               "teardown");
        teardown_title_session();
        return;
      }
#endif  // XE_PLATFORM_IOS
      XELOGE("iOS: Failed to launch game: {:08X}", launch_result);
      notify_game_exited();
      return;
    }

    XELOGI("iOS: Game launched successfully");
    emulator_->WaitUntilExit();
    XELOGI("iOS: Game execution finished (exit wait completed)");

    teardown_title_session();
  }
}

std::unique_ptr<apu::AudioSystem> EmulatorAppIOS::CreateAudioSystem(cpu::Processor* processor) {
  // Opt-in PHASE backend renders the native 5.1 bed as spatial audio; default
  // SDL uses CoreAudio on iOS for audio output.
  if (cvars::apu == "phase") {
    XELOGI("Audio backend: PHASE (spatial) selected via --apu/apu cvar");
    return std::make_unique<apu::phase::PHASEAudioSystem>(processor);
  }
  XELOGI("Audio backend: SDL (apu cvar = '{}')", std::string(cvars::apu));
  return std::make_unique<apu::sdl::SDLAudioSystem>(processor);
}

std::unique_ptr<gpu::GraphicsSystem> EmulatorAppIOS::CreateGraphicsSystem() {
  const std::string gpu_implementation_name = cvars::gpu;
#if defined(XE_IOS_MOLTENVK_ENABLED)
  if (gpu_implementation_name == "vulkan") {
    return std::make_unique<gpu::vulkan::VulkanGraphicsSystem>();
  }
#endif
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

  // SDL uses the GameController framework on iOS for MFi/Bluetooth gamepads.
  auto sdl_driver = xe::hid::sdl::Create(window, kZOrderHidInput);
  if (sdl_driver && XSUCCEEDED(sdl_driver->Setup())) {
    drivers.emplace_back(std::move(sdl_driver));
  } else if (drivers.empty()) {
    XELOGW("iOS: SDL input driver setup failed, falling back to nop input");
    drivers.emplace_back(xe::hid::nop::Create(window, kZOrderHidInput));
  }
  return drivers;
}

}  // namespace app
}  // namespace xe

XE_DEFINE_WINDOWED_APP(xenia, xe::app::EmulatorAppIOS::Create);
