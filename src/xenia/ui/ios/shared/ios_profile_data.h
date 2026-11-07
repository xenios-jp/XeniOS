/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_PROFILE_DATA_H_
#define XENIA_UI_IOS_SHARED_IOS_PROFILE_DATA_H_

#import <Foundation/Foundation.h>

#include <cstdint>
#include <filesystem>
#include <functional>

#include "xenia/kernel/xam/profile_package.h"

// Moving a profile between devices as a .xeniosprofile file (xam/profile_package): what the
// app tells the account service about its profiles, and the messages for what goes wrong.

// Called on the main thread, except profile_in_game, which an import also asks from its own.
struct XeniaIOSProfileDataHost {
  // The emulator's content folder (profiles and saves).
  std::function<std::filesystem::path()> content_root;
  // Whether profile |xuid| is signed in and a game is running: an import into it waits.
  std::function<bool(uint64_t xuid)> profile_in_game;
  // An import wrote profile |xuid|'s files: the profiles are read again, a signed-in one is
  // signed out and back in. |created|: it's new here, and signed in when nobody is.
  std::function<void(uint64_t xuid, bool created)> profile_imported;
};
// Once, at startup (xenia_main_ios.mm). Until then the account screens hide profile data.
void xe_ios_profile_data_install(XeniaIOSProfileDataHost host);
// The installed host, or null.
const XeniaIOSProfileDataHost* xe_ios_profile_data_host(void);

// Posted on the main queue while an import or export runs. userInfo:
// XeniaProfileDataProgressFractionKey, an NSNumber from 0 to 1.
extern NSNotificationName const XeniaProfileDataProgressNotification;
extern NSString* const XeniaProfileDataProgressFractionKey;

// The message for a failed export (|exporting|) or import, as an NSError for the screens.
NSError* xe_ios_profile_package_error(xe::kernel::xam::ProfilePackageError error, BOOL exporting);

// "Saves for 12 games and achievements": what a package carries, for the confirm sheet.
NSString* xe_ios_profile_package_contents_text(
    const xe::kernel::xam::ProfilePackageManifest& manifest);

#endif  // XENIA_UI_IOS_SHARED_IOS_PROFILE_DATA_H_
