/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_ONLINE_PANEL_H_
#define XENIA_UI_IOS_APP_IOS_ONLINE_PANEL_H_

#import <UIKit/UIKit.h>

#include <cstdint>

@class XeniaLiveFriend;
@class XeniaLiveInvite;

// One player's card: who they are, what they play, and what you can do with them (join, invite,
// add as a friend). Lives inside the Online panel, or on its own over the game when the game asks
// for a gamer card.
@interface XeniaIOSGamerCardView : UIView
- (instancetype)initWithXuid:(uint64_t)xuid gamertag:(NSString*)gamertag;
@property(nonatomic, readonly) uint64_t xuid;
@property(nonatomic, copy) void (^joinFriendHandler)(XeniaLiveFriend* peer);
- (void)reload;
@end

// The in-game menu's Online panel, popped out beside the menu from its Online pill: who's playing
// with you, your party, invites waiting, and friends online to invite. Tapping a player shows
// their gamer card in place.
@interface XeniaIOSOnlinePanel : UIView
@property(nonatomic, copy) void (^acceptInviteHandler)(XeniaLiveInvite* invite);
@property(nonatomic, copy) void (^joinFriendHandler)(XeniaLiveFriend* peer);
// The full Friends sheet.
@property(nonatomic, copy) void (^allFriendsHandler)(void);
// Set when the panel fills a sheet of its own instead of floating over a game: no window chrome,
// a close button in the header, and (with |gamesHandler|) a Games Online row.
@property(nonatomic) BOOL embedded;
@property(nonatomic, copy) void (^closeHandler)(void);
@property(nonatomic, copy) void (^gamesHandler)(void);
- (void)reload;
// Back to the list from a gamer card.
- (void)showList;
@end

// The Online panel as a sheet over the home screen: the same pages as the in-game panel.
@interface XeniaIOSOnlineSheetViewController : UIViewController
@property(nonatomic, copy) void (^acceptInviteHandler)(XeniaLiveInvite* invite);
@property(nonatomic, copy) void (^joinFriendHandler)(XeniaLiveFriend* peer);
@property(nonatomic, copy) void (^gamesHandler)(void);
@end

// Public Games: everyone on XeniOS, as a full page. Lists the games with players in them (the
// library's first), live, with how many are playing. Choosing a library game opens its System
// Link sheet. View-only otherwise.
@interface XeniaIOSPublicGamesViewController : UIViewController
// The library's games by title ID, and what to do when one is chosen (after the page closes).
- (instancetype)initWithLibraryNames:(NSDictionary<NSNumber*, NSString*>*)names
                            openGame:(void (^)(uint32_t title_id))open_game;
@end

// A gamer card over the game, for a title's request to show one. |done| runs once it's closed.
void xe_ios_present_gamer_card(UIView* host, uint64_t xuid, void (^join_friend)(XeniaLiveFriend*),
                               void (^done)(void));

#endif  // XENIA_UI_IOS_APP_IOS_ONLINE_PANEL_H_
