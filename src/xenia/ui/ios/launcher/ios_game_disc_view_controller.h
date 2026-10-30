/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_DISC_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_GAME_DISC_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include <vector>

#include "xenia/ui/ios/launcher/ios_game_library.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

// Lets users choose a specific launch path for multi-disc titles while the
// launcher still presents the game as one library entry.
@interface XeniaGameDiscViewController : XESheetTableViewController
- (instancetype)initWithTitle:(NSString*)title
                        discs:(const std::vector<xe::ui::IOSDiscoveredGame::Disc>&)discs
             selectionHandler:(void (^)(NSString* path, NSString* label))selectionHandler;
@end

#endif  // XENIA_UI_IOS_GAME_DISC_VIEW_CONTROLLER_H_
