/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SETTINGS_IOS_PROFILE_IMPORT_H_
#define XENIA_UI_IOS_SETTINGS_IOS_PROFILE_IMPORT_H_

#import <UIKit/UIKit.h>

// A .xeniosprofile opened from AirDrop, Files or another app's share sheet ("Open in
// XeniOS"): a confirm sheet saying what's in it ("Import “Bo”? Saves for 12 games and
// achievements. …"), then the import through the account service, with its progress and
// result as toasts. Returns NO for any other URL. Removes an Inbox copy when done.
BOOL xe_ios_profile_import_handle_url(NSURL* url, UIViewController* presenter);

#endif  // XENIA_UI_IOS_SETTINGS_IOS_PROFILE_IMPORT_H_
