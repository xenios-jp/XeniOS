/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_FRIENDS_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_FRIENDS_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/shared/ios_live_social_bridge.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

typedef NS_ENUM(NSInteger, XeniaIOSFriendsMode) {
  // From the library: game invites, requests, friends with Join, sent requests.
  XeniaIOSFriendsModeBrowse = 0,
  // From the in-game menu: friends with Invite for the running game, and the
  // same invites and Join as Browse.
  XeniaIOSFriendsModeInvite,
};

// Online (the library's Online button) and Friends (the in-game menu): the
// game invites waiting (the inbox), friend requests, the party, friends with
// what they play and Join when their game takes players, requests sent, and
// the games people play over System Link.
@interface XeniaIOSFriendsViewController : XESheetTableViewController

- (instancetype)initWithMode:(XeniaIOSFriendsMode)mode gameTitle:(NSString*)game_title;

// The host starts the game (or joins it in the running one). The screen is
// dismissed first.
@property(nonatomic, copy) void (^acceptInviteHandler)(XeniaLiveInvite* invite);
@property(nonatomic, copy) void (^joinFriendHandler)(XeniaLiveFriend* peer);
// Browse mode: the System Link page (games and their public lobbies), pushed
// from the screen's Games Online row. Unset hides the row.
@property(nonatomic, copy) UIViewController* (^gamesOnlineProvider)(void);

@end

// Add Friend: a friend code to send a request to, and this profile's own code
// with Copy and Share.
@interface XeniaIOSAddFriendViewController : XESheetTableViewController
@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_FRIENDS_VIEW_CONTROLLER_H_
