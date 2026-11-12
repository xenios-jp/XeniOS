/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_xefu.h"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <mutex>
#include <system_error>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"

DECLARE_path(storage_root);
DECLARE_path(xefu_path);
DECLARE_string(xefu_launcher);

namespace xe {
namespace ui {

const char kIOSXeFuAutomaticLauncher[] = "xbox.xex";

namespace {

namespace fs = std::filesystem;

std::string Lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool HasPrefix(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

bool IsAutomaticName(const std::string& name) { return Lowercase(name) == kIOSXeFuAutomaticLauncher; }

// A file a game can start through: a build (the companion modules, xefutitle*, are not) or the
// config loader.
bool IsLauncherName(const std::string& name) {
  const std::string lower = Lowercase(name);
  if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".xex") != 0) {
    return false;
  }
  return (HasPrefix(lower, "xefu") && !HasPrefix(lower, "xefutitle")) ||
         HasPrefix(lower, "config_loader");
}

bool IsHidden(const fs::path& path) {
  const std::string name = path.filename().string();
  return !name.empty() && name[0] == '.';
}

fs::path SiblingPath(const char* suffix) {
  const fs::path path = IOSXeFuPath();
  return path.parent_path() / (path.filename().string() + suffix);
}

cvar::IConfigVar* FindLauncherVar() {
  if (!cvar::ConfigVars) {
    return nullptr;
  }
  const auto it = cvar::ConfigVars->find("xefu_launcher");
  return it != cvar::ConfigVars->end() ? it->second : nullptr;
}

// The setting every game starts through: the saved value, not what the game that ran last put
// in its place.
std::string DefaultLauncher() {
  if (auto* var = dynamic_cast<cvar::ConfigVar<std::string>*>(FindLauncherVar())) {
    return var->GetTypedConfigValue();
  }
  return cvars::xefu_launcher;
}

// Held by an install from its first copied file to the end of its swap.
std::mutex install_mutex;

// Install and Remove replace or delete the whole folder. A folder the xefu_path setting names
// has to be one of its own for that: not the storage root, nor a folder above it.
bool CanReplaceFolder() {
  if (cvars::xefu_path.empty()) {
    return true;
  }
  fs::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = xe_get_ios_documents_path();
  }
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(fs::absolute(storage_root), ec);
  const fs::path target = fs::weakly_canonical(fs::absolute(IOSXeFuPath()), ec);
  const fs::path relative = root.lexically_relative(target);
  return !relative.empty() && *relative.begin() == "..";
}

// What an install that was cut short left: the folder set aside during the swap, which is the
// install when the new one never took its place, and a half-made copy. Leaves an install that
// is running alone.
void SettleInstall() {
  std::unique_lock<std::mutex> lock(install_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }
  std::error_code ec;
  const fs::path previous = SiblingPath(".previous");
  if (!fs::exists(IOSXeFuPath(), ec) && fs::exists(previous, ec)) {
    fs::rename(previous, IOSXeFuPath(), ec);
  }
  fs::remove_all(previous, ec);
  fs::remove_all(SiblingPath(".installing"), ec);
}

struct PlannedFile {
  fs::path source;
  fs::path relative;
  uint64_t size = 0;
};

struct Plan {
  std::vector<PlannedFile> files;
  uint64_t total_size = 0;
  bool read_failed = false;
};

void AddFile(const fs::path& source, const fs::path& relative, Plan* plan) {
  std::error_code ec;
  const uint64_t size = fs::file_size(source, ec);
  if (ec) {
    plan->read_failed = true;
    return;
  }
  plan->files.push_back({source, relative, size});
  plan->total_size += size;
}

// Everything under |folder| but dotfiles, as |relative| in the install.
void AddTree(const fs::path& folder, const fs::path& relative, Plan* plan) {
  std::error_code ec;
  fs::recursive_directory_iterator it(folder, ec), end;
  if (ec) {
    plan->read_failed = true;
    return;
  }
  for (; it != end; it.increment(ec)) {
    if (ec) {
      plan->read_failed = true;
      return;
    }
    if (IsHidden(it->path())) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec)) {
      AddFile(it->path(), relative / it->path().lexically_relative(folder), plan);
    }
  }
}

bool IsCompatibilityFolder(const fs::path& path) {
  std::error_code ec;
  return fs::is_directory(path, ec) && Lowercase(path.filename().string()) == "compatibility";
}

// The Compatibility folder, a folder that has one, or the files, in the picked |paths|.
Plan MakePlan(const std::vector<fs::path>& paths) {
  Plan plan;
  std::error_code ec;
  if (paths.size() == 1 && fs::is_directory(paths[0], ec)) {
    // A folder with a Compatibility folder in it, or the one itself, or its content.
    fs::path folder = paths[0];
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
      if (IsCompatibilityFolder(entry.path())) {
        folder = entry.path();
        break;
      }
    }
    AddTree(folder, fs::path(), &plan);
  } else {
    for (const fs::path& path : paths) {
      if (IsHidden(path)) {
        continue;
      }
      if (fs::is_directory(path, ec)) {
        AddTree(path, IsCompatibilityFolder(path) ? fs::path() : path.filename(), &plan);
      } else {
        AddFile(path, path.filename(), &plan);
      }
    }
  }
  return plan;
}

// Whether the plan puts xbox.xex or a build at the top of the install.
bool PlanHasXeFu(const Plan& plan) {
  for (const PlannedFile& file : plan.files) {
    if (std::distance(file.relative.begin(), file.relative.end()) == 1 &&
        (IsAutomaticName(file.relative.string()) || IsLauncherName(file.relative.string()))) {
      return true;
    }
  }
  return false;
}

// The launchers in the folder. Quick.
IOSXeFuInfo ScanLaunchers() {
  IOSXeFuInfo info;
  std::error_code ec;
  fs::directory_iterator it(IOSXeFuPath(), ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code file_ec;
    if (!it->is_regular_file(file_ec)) {
      continue;
    }
    const std::string name = it->path().filename().string();
    if (IsAutomaticName(name)) {
      info.has_automatic = true;
    } else if (IsLauncherName(name)) {
      info.launchers.push_back(name);
    }
  }
  info.installed = info.has_automatic || !info.launchers.empty();
  return info;
}

IOSXeFuResult InstallPlan(const Plan& plan, const std::atomic<bool>& cancel,
                          const std::function<void(int)>& progress) {
  if (!CanReplaceFolder()) {
    return IOSXeFuResult::kCopyFailed;
  }
  SettleInstall();
  std::lock_guard<std::mutex> lock(install_mutex);
  const fs::path target = IOSXeFuPath();
  const fs::path staging = SiblingPath(".installing");
  const fs::path previous = SiblingPath(".previous");
  std::error_code ec;
  auto fail = [&](IOSXeFuResult result) {
    std::error_code ignored;
    fs::remove_all(staging, ignored);
    return result;
  };
  fs::create_directories(staging, ec);
  if (ec) {
    return fail(IOSXeFuResult::kCopyFailed);
  }
  uint64_t done = 0;
  for (const PlannedFile& file : plan.files) {
    if (cancel.load()) {
      return fail(IOSXeFuResult::kCancelled);
    }
    const fs::path destination = staging / file.relative;
    fs::create_directories(destination.parent_path(), ec);
    if (!ec) {
      fs::copy_file(file.source, destination, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
      XELOGE("iOS: copying an Xbox emulator file failed: {}", ec.message());
      return fail(IOSXeFuResult::kCopyFailed);
    }
    done += file.size;
    progress(plan.total_size ? static_cast<int>(done * 100 / plan.total_size) : 100);
  }
  if (cancel.load()) {
    return fail(IOSXeFuResult::kCancelled);
  }
  // Swap: the installed folder is only set aside until the new one is in its place.
  const bool had_install = fs::exists(target, ec);
  if (had_install) {
    fs::rename(target, previous, ec);
    if (ec) {
      return fail(IOSXeFuResult::kCopyFailed);
    }
  }
  fs::rename(staging, target, ec);
  if (ec) {
    if (had_install) {
      std::error_code restore_ec;
      fs::rename(previous, target, restore_ec);
    }
    return fail(IOSXeFuResult::kCopyFailed);
  }
  fs::remove_all(previous, ec);
  return IOSXeFuResult::kSuccess;
}

}  // namespace

fs::path IOSXeFuPath() {
  // As Emulator::xefu_path resolves it, with the storage root the app sets up.
  if (!cvars::xefu_path.empty()) {
    return cvars::xefu_path;
  }
  fs::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = xe_get_ios_documents_path();
  }
  return fs::absolute(storage_root) / "xefu";
}

IOSXeFuInfo ReadIOSXeFuLaunchers() {
  SettleInstall();
  IOSXeFuInfo info = ScanLaunchers();
  std::sort(info.launchers.begin(), info.launchers.end(),
            [](const std::string& a, const std::string& b) {
              return [@(a.c_str()) localizedStandardCompare:@(b.c_str())] == NSOrderedAscending;
            });
  return info;
}

IOSXeFuInfo ReadIOSXeFuInfo() {
  IOSXeFuInfo info = ReadIOSXeFuLaunchers();
  if (info.installed) {
    std::error_code ec;
    fs::recursive_directory_iterator it(IOSXeFuPath(), ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code file_ec;
      if (it->is_regular_file(file_ec)) {
        info.size += it->file_size(file_ec);
      }
    }
  }
  return info;
}

std::vector<std::string> IOSXeFuLauncherNames(const IOSXeFuInfo& info) {
  std::vector<std::string> names;
  if (info.has_automatic) {
    names.push_back(kIOSXeFuAutomaticLauncher);
  }
  names.insert(names.end(), info.launchers.begin(), info.launchers.end());
  return names;
}

void SetIOSXeFuLauncher(const std::string& launcher) {
  if (cvar::ConfigVars) {
    const auto it = cvar::ConfigVars->find("xefu_launcher");
    if (it != cvar::ConfigVars->end()) {
      if (auto* var = dynamic_cast<cvar::ConfigVar<std::string>*>(it->second)) {
        var->OverrideConfigValue(launcher);
      }
    }
  }
  config::SaveConfigValues({"xefu_launcher"});
  XELOGI("iOS: Xbox emulator launcher set to {}", launcher);
}

std::string ResolveIOSXeFuLauncher(const IOSXeFuInfo& info) {
  const std::string chosen = DefaultLauncher();
  if (!info.installed) {
    return chosen;
  }
  if (IsIOSXeFuLauncherInstalled(info, chosen)) {
    return chosen;
  }
  const std::string fallback =
      info.has_automatic ? std::string(kIOSXeFuAutomaticLauncher) : info.launchers.front();
  SetIOSXeFuLauncher(fallback);
  return fallback;
}

bool IsIOSXeFuLauncherInstalled(const IOSXeFuInfo& info, const std::string& launcher) {
  if (info.has_automatic && IsAutomaticName(launcher)) {
    return true;
  }
  return std::find(info.launchers.begin(), info.launchers.end(), launcher) != info.launchers.end();
}

std::string IOSXeFuGameLauncher(uint32_t title_id) {
  cvar::IConfigVar* var = FindLauncherVar();
  if (!var || !title_id) {
    return std::string();
  }
  const toml::table game_config = config::LoadGameConfig(title_id);
  if (const toml::node* node = config::FindConfigNode(game_config, *var)) {
    return node->value_or(std::string());
  }
  return std::string();
}

void ClearIOSXeFuGameLauncher(uint32_t title_id) {
  cvar::IConfigVar* var = FindLauncherVar();
  if (!var || !title_id) {
    return;
  }
  toml::table game_config = config::LoadGameConfig(title_id);
  if (!config::FindConfigNode(game_config, *var)) {
    return;
  }
  if (toml::table* section = config::ResolveSectionTable(game_config, var->category())) {
    section->erase(var->name());
  }
  config::SaveGameConfig(title_id, game_config);
  XELOGI("iOS: removed the Xbox emulator build from the settings of title {:08X}", title_id);
}

bool IsIOSXeFuReady(uint32_t title_id) {
  const IOSXeFuInfo info = ScanLaunchers();
  if (!info.installed) {
    return false;
  }
  const std::string game_launcher = IOSXeFuGameLauncher(title_id);
  if (!game_launcher.empty()) {
    if (IsIOSXeFuLauncherInstalled(info, game_launcher)) {
      return true;
    }
    // The build is gone (a reinstall without it): the game goes back to the default.
    ClearIOSXeFuGameLauncher(title_id);
  }
  std::error_code ec;
  return fs::is_regular_file(IOSXeFuPath() / ResolveIOSXeFuLauncher(info), ec);
}

NSString* IOSXeFuLauncherDisplayName(const std::string& launcher) {
  if (launcher.empty() || IsAutomaticName(launcher)) {
    return NSLocalizedString(@"Automatic", @"system software view controller: build choice");
  }
  return [NSString stringWithUTF8String:launcher.c_str()];
}

NSString* IOSXeFuAutomaticDescription() {
  return NSLocalizedString(@"xbox.xex picks a build for each game, as the console does.",
                           @"system software view controller: build choice");
}

bool RemoveIOSXeFu() {
  if (!CanReplaceFolder()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(install_mutex);
  std::error_code ec;
  fs::remove_all(IOSXeFuPath(), ec);
  return !ec;
}

NSString* IOSXeFuInstallFailureMessage(IOSXeFuResult result) {
  switch (result) {
    case IOSXeFuResult::kSuccess:
    case IOSXeFuResult::kCancelled:
      return nil;
    case IOSXeFuResult::kNotXeFu:
      return NSLocalizedString(
          @"This isn't the Xbox emulator. Pick the Compatibility folder from an Xbox 360's hard "
          @"drive, or the folder it is in, or its files: xbox.xex and the xefu*.xex builds.",
          @"system software view controller: install error");
    case IOSXeFuResult::kCopyFailed:
      return NSLocalizedString(
          @"The files couldn't be copied. Check that there is room on this device and try again. "
          @"What was installed before is unchanged.",
          @"system software view controller: install error");
  }
  return nil;
}

UIDocumentPickerViewController* NewIOSXeFuPicker(id<UIDocumentPickerDelegate> delegate) {
  // A folder, or any of its files: the builds are .xex and the configs .bin.
  UIDocumentPickerViewController* picker = [[[UIDocumentPickerViewController alloc]
      initForOpeningContentTypes:@[ UTTypeFolder, UTTypeItem ]
                          asCopy:NO] autorelease];
  picker.delegate = delegate;
  picker.allowsMultipleSelection = YES;
  return picker;
}

void InstallIOSXeFuAsync(NSArray<NSURL*>* urls, std::shared_ptr<std::atomic<bool>> cancel,
                         void (^progress)(int percent),
                         void (^confirm_replace)(void (^answer)(BOOL replace)),
                         void (^completion)(IOSXeFuResult result)) {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    // The picker hands over the originals, whose scope has to be open to read them, until the
    // copy is done.
    NSMutableArray<NSURL*>* scoped = [NSMutableArray array];
    std::vector<fs::path> paths;
    for (NSURL* url in urls) {
      if ([url startAccessingSecurityScopedResource]) {
        [scoped addObject:url];
      }
      paths.push_back(fs::path([url fileSystemRepresentation]));
    }
    void (^finish)(IOSXeFuResult) = ^(IOSXeFuResult result) {
      for (NSURL* url in scoped) {
        [url stopAccessingSecurityScopedResource];
      }
      XELOGI("iOS: Xbox emulator install finished with result {}", static_cast<int>(result));
      dispatch_async(dispatch_get_main_queue(), ^{
        completion(result);
      });
    };

    auto plan = std::make_shared<Plan>(MakePlan(paths));
    if (plan->read_failed || plan->files.empty()) {
      finish(plan->read_failed ? IOSXeFuResult::kCopyFailed : IOSXeFuResult::kNotXeFu);
      return;
    }
    if (!PlanHasXeFu(*plan)) {
      finish(IOSXeFuResult::kNotXeFu);
      return;
    }
    auto last_percent = std::make_shared<int>(-1);
    void (^run_copy)(void) = ^{
      finish(InstallPlan(*plan, *cancel, [last_percent, progress](int percent) {
        if (percent != *last_percent) {
          *last_percent = percent;
          dispatch_async(dispatch_get_main_queue(), ^{
            progress(percent);
          });
        }
      }));
    };
    std::error_code ec;
    if (confirm_replace && fs::exists(IOSXeFuPath(), ec)) {
      // The installed one stays as it is until the answer.
      dispatch_async(dispatch_get_main_queue(), ^{
        void (^answer)(BOOL) = ^(BOOL replace) {
          if (!replace) {
            finish(IOSXeFuResult::kCancelled);
            return;
          }
          dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), run_copy);
        };
        confirm_replace([[answer copy] autorelease]);
      });
      return;
    }
    run_copy();
  });
}

}  // namespace ui
}  // namespace xe
