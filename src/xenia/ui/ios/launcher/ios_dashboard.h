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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "xenia/emulator.h"
#include "xenia/ui/ios/launcher/ios_game_library.h"
#include "xenia/ui/ios/launcher/ios_system_versions.h"
#include "xenia/ui/system_files_installer_wx.h"

#ifdef __OBJC__
#import <Foundation/Foundation.h>

@class UIDocumentPickerViewController;
@class XeniaIOSExternalLibraryAccess;
@protocol UIDocumentPickerDelegate;

// Posted on the main thread when the system software is installed or removed.
extern NSNotificationName const XeniaIOSSystemSoftwareDidChangeNotification;
// Posted on the main thread before a title the emulator queued itself starts,
// such as a game the dashboard launches, with its path under
// XeniaIOSTitlePathKey.
extern NSNotificationName const XeniaIOSTitleWillStartNotification;
extern NSString* const XeniaIOSTitlePathKey;
#endif  // __OBJC__

namespace xe {
namespace ui {

// The Xbox 360 system software is a USB system update installed in the system
// folder of the storage root, which the emulator mounts on every launch. Other
// versions can be installed beside it (ios_system_versions.h).
std::filesystem::path IOSSystemFilesPath();
// The dashboard in it, which the library starts.
std::filesystem::path IOSInstalledDashboardPath();
bool IsIOSDashboardInstalled();
// boot_to_dashboard, as on edge: a dashboard is the home screen. XeniOS opens
// on it, and every title exits back to it.
bool IsIOSDashboardHome();
// The home dashboard: the one of the version boot_dashboard_version names, or
// of the version games use when it names none, or one that isn't installed
// (any more) or has no dashboard.
std::filesystem::path IOSHomeDashboardPath();
// The version boot_dashboard_version names when its dashboard is installed.
// Empty when the home dashboard follows the version games use.
std::string IOSHomeDashboardVersion();
// Sets both settings and saves them: whether XeniOS starts with a dashboard,
// and of which version (as IOSSystemVersion::version; empty for the one games
// use).
void SetIOSHomeDashboard(bool start_with_dashboard, const std::string& version);
// After a version was removed: when it was the one boot_dashboard_version
// names, the home dashboard follows the version games use again.
void ForgetRemovedIOSHomeDashboardVersion();
// Whether |path| is the dashboard of an installed version, the one games use
// or another.
bool IsIOSInstalledDashboardPath(const std::filesystem::path& path);

// The version games use, in the system folder, and every installed version.
struct IOSSystemSoftwareInfo {
  bool installed = false;
  // The dashboard's, like "2.0.17559.0". Empty when it can't be read.
  std::string version;
  uint64_t size = 0;
  // An install that failed left the previous one in a folder of its own.
  bool has_unrestored_files = false;
  // Every installed version with its size, the one games use first.
  std::vector<IOSSystemVersion> versions;
};
// Walks the folders, so keep it off the main thread.
IOSSystemSoftwareInfo ReadIOSSystemSoftwareInfo();

// Installs the update from its zip or the folder Files expands it to: as the
// version games use when there is none, in place of the installed version it
// is, or else as one more version. Blocks, so keep it off the main thread.
SystemFilesResult InstallIOSSystemSoftware(
    const std::filesystem::path& source_path,
    const std::function<bool(size_t done, size_t total)>& progress);
// Removes the version games use, with everything in the system folder.
bool RemoveIOSSystemSoftware();

// Keeps the library for the dashboard to list as its games, writing the icons
// it shows. Takes a copy, and does the work on a queue of its own.
void PublishIOSLibraryToDashboard(std::vector<IOSDiscoveredGame> games);
std::vector<Emulator::LibraryTitle> IOSDashboardLibraryTitles();

// The dashboard's Open Tray: asks which of the library's discs to put in the
// drive, or none. Waits for the answer, so it can't be asked on the main
// thread, and gives none there.
std::filesystem::path PickIOSDashboardDisc();

#ifdef __OBJC__
// Starts access to every linked library folder, which the dashboard's games
// are in, for as long as the dashboard runs.
NSArray<XeniaIOSExternalLibraryAccess*>* StartIOSDashboardLibraryAccess();

// What an install that didn't succeed says. Nil for a finished or cancelled
// one.
NSString* IOSSystemSoftwareInstallFailureMessage(SystemFilesResult result);

// Installs the update from |source| off the main thread, as
// InstallIOSSystemSoftware does. |progress| gets whole percents and
// |completion| the result, both on the main thread. Setting |cancel| stops the
// install early. Holds |source|'s security scope for the install, so a picked
// folder that wasn't copied can be read.
//
// When the update's version is already installed, |confirm_replace| is asked
// on the main thread, once the update is unpacked, whether to replace it, and
// answers through |answer|; declining finishes as cancelled and changes
// nothing. Nil replaces without asking.
void InstallIOSSystemSoftwareAsync(NSURL* source, std::shared_ptr<std::atomic<bool>> cancel,
                                   void (^progress)(int percent),
                                   void (^confirm_replace)(NSString* version,
                                                           void (^answer)(BOOL replace)),
                                   void (^completion)(SystemFilesResult result));

// The Files picker for what InstallIOSSystemSoftwareAsync installs: a system
// update's zip or folder, or a dashboard on its own, as its folder or as
// dash.xex picked with the files beside it. Nothing is copied: a folder can
// only be picked in place.
UIDocumentPickerViewController* NewIOSSystemSoftwarePicker(id<UIDocumentPickerDelegate> delegate);
// What to install for the picker's |urls|: the one item picked, or a folder
// in tmp with copies of the several files picked. Nil when they can't be
// copied.
NSURL* IOSSystemSoftwareSourceForPickedURLs(NSArray<NSURL*>* urls);

// What a download that didn't succeed says. Nil for a finished or cancelled
// one.
NSString* IOSSystemUpdateDownloadFailureMessage(SystemUpdateDownload result);

// Downloads the system update from Microsoft and installs it, off the main
// thread, as edge's Download & Install does. |progress| gets whole percents of
// the step under way, the download and then the install, and |completion|
// both results; the install's is kCancelled when the download didn't finish.
// Both run on the main thread. Setting |cancel| stops either step.
void DownloadAndInstallIOSSystemUpdateAsync(std::shared_ptr<std::atomic<bool>> cancel,
                                            void (^progress)(int percent, BOOL installing),
                                            void (^completion)(SystemUpdateDownload download,
                                                               SystemFilesResult install));
#endif  // __OBJC__

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_DASHBOARD_H_
