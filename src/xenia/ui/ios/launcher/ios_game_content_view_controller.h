/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_CONTENT_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_GAME_CONTENT_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include <cstdint>

#import "xenia/ui/ios/launcher/ios_content_management.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

// Hosts the Manage Content sheet. The launcher's main view controller adopts
// this protocol so the sheet can ask it to install a freshly-imported title
// update via the same code path that the library's import flow uses.
@protocol XeniaGameContentHost <NSObject>
- (BOOL)installTitleUpdateAtPath:(NSString*)path
                          status:(NSString**)status_out
                  notTitleUpdate:(BOOL*)not_title_update_out;
// Rescans the library off the main thread.
- (void)refreshImportedGamesAsync;
@end

// Per-game content management sheet: lists installed title updates and DLC
// for `title_id`, and adds new ones via UIDocumentPicker. Calls back into the
// launcher (`host`) to refresh the library when installs succeed.
// Posted on the main thread after a title update or DLC was added or removed.
extern NSNotificationName const XeniaGameContentDidChangeNotification;

// Asks before deleting |entry| (or unlinking it, for linked DLC), then does it and calls
// |removed|; |cancelled| when the player keeps it. The question is anchored to |source| on iPad.
void XeniaConfirmRemoveInstalledContent(UIViewController* presenter,
                                        const IOSInstalledContentEntry& entry, UIView* source,
                                        void (^removed)(void), void (^cancelled)(void));

// Picks a title update or DLC for a game in Files and adds it, with its progress and result
// presented over |presenter|. Keep it alive until it is done.
@interface XeniaGameContentImporter : NSObject <UIDocumentPickerDelegate>
- (instancetype)initWithTitleID:(uint32_t)titleID
                           host:(id<XeniaGameContentHost>)host
                      presenter:(UIViewController*)presenter;
- (void)present;
@end

@interface XeniaGameContentViewController : XESheetTableViewController
- (instancetype)initWithTitleID:(uint32_t)title_id
                          title:(NSString*)title
                           host:(id<XeniaGameContentHost>)host;
@end

#endif  // XENIA_UI_IOS_GAME_CONTENT_VIEW_CONTROLLER_H_
