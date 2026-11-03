/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "xenia/emulator.h"
#include "xenia/ui/ios/launcher/ios_game_library.h"
#include "xenia/ui/system_files_installer_wx.h"

#ifdef __OBJC__
#import <Foundation/Foundation.h>

// Posted on the main thread when the system software is installed or removed.
extern NSNotificationName const XeniaIOSSystemSoftwareDidChangeNotification;
// Posted on the main thread before a title the emulator queued itself starts,
// such as a game the dashboard launches, with its path under
// XeniaIOSTitlePathKey.
extern NSNotificationName const XeniaIOSTitleWillStartNotification;
extern NSString* const XeniaIOSTitlePathKey;
// Microsoft's page for the Xbox 360 system update, which links the USB update.
extern NSString* const XeniaIOSSystemUpdateSupportURL;
#endif  // __OBJC__

namespace xe {
namespace ui {

// The Xbox 360 system software is a USB system update installed in the system
// folder of the storage root, which the emulator mounts on every launch.
std::filesystem::path IOSSystemFilesPath();
// The dashboard in it, which the library starts.
std::filesystem::path IOSInstalledDashboardPath();
bool IsIOSDashboardInstalled();
bool IsIOSInstalledDashboardPath(const std::filesystem::path& path);

struct IOSSystemSoftwareInfo {
  bool installed = false;
  // The dashboard's, like "2.0.17559.0". Empty when it can't be read.
  std::string version;
  uint64_t size = 0;
  // An install that failed left the previous one in a folder of its own.
  bool has_unrestored_files = false;
};
// Walks the folder, so keep it off the main thread.
IOSSystemSoftwareInfo ReadIOSSystemSoftwareInfo();

// Installs the update from its zip or the folder Files expands it to. Blocks,
// so keep it off the main thread.
SystemFilesResult InstallIOSSystemSoftware(
    const std::filesystem::path& source_path,
    const std::function<bool(size_t done, size_t total)>& progress);
bool RemoveIOSSystemSoftware();

// Keeps the library for the dashboard to list as its games, writing the icons
// it shows. Takes a copy, and does the work on a queue of its own.
void PublishIOSLibraryToDashboard(std::vector<IOSDiscoveredGame> games);
std::vector<Emulator::LibraryTitle> IOSDashboardLibraryTitles();

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_H_
