/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_SYSTEM_LINK_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_SYSTEM_LINK_VIEW_CONTROLLER_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include <cstdint>
#include <vector>

#import "xenia/ui/ios/shared/ios_view_helpers.h"

// Picks the room a game plays System Link in: its public lobby or a private
// room, created here or joined with a code, with an invite to share.
@interface XeniaIOSSystemLinkViewController : XESheetTableViewController

// Shows a Play button that dismisses the sheet and calls it.
@property(nonatomic, copy) void (^playHandler)(void);

- (instancetype)initWithGameTitle:(NSString*)gameTitle titleID:(uint32_t)titleID;

@end

// Presents the sheet for a game over |presenter|.
void xe_ios_present_system_link_sheet(UIViewController* presenter, NSString* game_title,
                                      uint32_t title_id, void (^play)(void));

// Presents the System Link status sheet for the running game over |presenter|: how it's
// connected, its room and players, and who's online.
void xe_ios_present_system_link_status_sheet(UIViewController* presenter);

// A game in the player's library, for the Online sheet.
struct XeniaIOSOnlineLibraryGame {
  uint32_t title_id = 0;
  NSString* name = nil;
  NSData* icon = nil;  // PNG or JPEG, or nil
};

// Presents the Online sheet over |presenter|: how many players are in each game's public
// lobby, the player's games first. Choosing one of them dismisses the sheet and calls
// |open_game| with its title ID.
void xe_ios_present_online_sheet(UIViewController* presenter,
                                 const std::vector<XeniaIOSOnlineLibraryGame>& library,
                                 void (^open_game)(uint32_t title_id));

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_SYSTEM_LINK_VIEW_CONTROLLER_H_
