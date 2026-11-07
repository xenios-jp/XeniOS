/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_IN_GAME_MENU_OVERLAY_H_
#define XENIA_UI_IOS_APP_IOS_IN_GAME_MENU_OVERLAY_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger, XeniaIOSInGameMenuAction) {
  XeniaIOSInGameMenuActionNone = -1,
  XeniaIOSInGameMenuActionResume = 0,
  XeniaIOSInGameMenuActionEditControls,
  XeniaIOSInGameMenuActionAchievements,
  XeniaIOSInGameMenuActionDisplay = 4,
  XeniaIOSInGameMenuActionSettings,
  XeniaIOSInGameMenuActionLiveLog,
  XeniaIOSInGameMenuActionExit,
  XeniaIOSInGameMenuActionGraphics = 8,
  XeniaIOSInGameMenuActionPauseEmulator,
  XeniaIOSInGameMenuActionFriends,
};

@class XeniaLiveFriend;
@class XeniaLiveInvite;

@interface XeniaIOSInGameMenuOverlay : UIView

@property(nonatomic, copy) void (^resumeHandler)(void);
@property(nonatomic, copy) void (^editControlsHandler)(void);
@property(nonatomic, copy) void (^achievementsHandler)(void);
@property(nonatomic, copy) void (^settingsHandler)(void);
@property(nonatomic, copy) void (^liveLogHandler)(void);
@property(nonatomic, copy) void (^exitHandler)(void);
@property(nonatomic, copy) void (^graphicsHandler)(void);
@property(nonatomic, copy) void (^pauseEmulatorHandler)(void);
// Game invites and friend requests waiting; shown beside "Friends".
@property(nonatomic) NSInteger friendsAttentionCount;
// The Online panel's Join on a game invite, and Join Game on a friend's card.
@property(nonatomic, copy) void (^onlineAcceptInviteHandler)(XeniaLiveInvite* invite);
@property(nonatomic, copy) void (^onlineJoinFriendHandler)(XeniaLiveFriend* peer);
@property(nonatomic, strong) UIMenu* displayMenu;
// Whether the user paused the game from the menu: the pause button then offers to resume.
@property(nonatomic, assign) BOOL emulatorPaused;
// A game the Xbox 360 Dashboard started exits to the dashboard, not the library.
@property(nonatomic) BOOL exitsToDashboard;
// The header: the running game's name, its cover (from the title ID) and how long it has run.
@property(nonatomic, copy) NSString* gameTitle;
@property(nonatomic) uint32_t titleID;
@property(nonatomic, retain) NSDate* sessionStartDate;

- (BOOL)isOverlayVisible;
- (BOOL)isActionEnabled:(XeniaIOSInGameMenuAction)action;
- (void)performAction:(XeniaIOSInGameMenuAction)action;
- (void)setControllerNavigationEnabled:(BOOL)enabled
                         focusedAction:(XeniaIOSInGameMenuAction)focusedAction;
- (void)setOverlayVisible:(BOOL)visible
                 animated:(BOOL)animated
               completion:(void (^)(BOOL finished))completion;

// The Online panel beside the menu (or dropped down under the Online pill when there's no room
// beside it): opened and closed by the pill.
- (void)toggleOnlinePanel;
- (void)hideOnlinePanel;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_APP_IOS_IN_GAME_MENU_OVERLAY_H_
