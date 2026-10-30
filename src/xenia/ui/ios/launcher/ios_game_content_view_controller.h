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

#include "xenia/ui/ios/shared/ios_view_helpers.h"

// Hosts the Manage Content sheet. The launcher's main view controller adopts
// this protocol so the sheet can ask it to install a freshly-imported title
// update via the same code path that the library's import flow uses.
@protocol XeniaGameContentHost <NSObject>
- (BOOL)installTitleUpdateAtPath:(NSString*)path
                          status:(NSString**)status_out
                  notTitleUpdate:(BOOL*)not_title_update_out;
- (void)refreshImportedGames;
@end

// Per-game content management sheet: lists installed title updates and DLC
// for `title_id`, and adds new ones via UIDocumentPicker. Calls back into the
// launcher (`host`) to refresh the library when installs succeed.
@interface XeniaGameContentViewController : XESheetTableViewController <UIDocumentPickerDelegate>
- (instancetype)initWithTitleID:(uint32_t)title_id
                          title:(NSString*)title
                           host:(id<XeniaGameContentHost>)host;
@end

#endif  // XENIA_UI_IOS_GAME_CONTENT_VIEW_CONTROLLER_H_
