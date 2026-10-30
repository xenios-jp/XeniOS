/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_EXTERNAL_FOLDERS_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_EXTERNAL_FOLDERS_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/shared/ios_view_helpers.h"

// Posted on the main thread whenever the set of linked external library folders
// changes (one was added or removed) from the management UI. The launcher
// observes this to rescan its library so newly linked folders appear and
// unlinked ones disappear.
extern NSString* const kXeniaIOSExternalLibraryDidChangeNotification;

// A self-contained sheet that lists the linked external library folders and lets
// the user add another (via a folder picker) or unlink an existing one. It talks
// to the library store directly and posts
// kXeniaIOSExternalLibraryDidChangeNotification on changes, so it can be
// presented from anywhere (the launcher's Add-to-Library sheet or Settings)
// without a host object.
@interface XeniaIOSExternalFoldersViewController : XESheetTableViewController
@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_EXTERNAL_FOLDERS_VIEW_CONTROLLER_H_
