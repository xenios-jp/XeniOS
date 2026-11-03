/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_SYSTEM_LINK_H_
#define XENIA_UI_IOS_SHARED_IOS_SYSTEM_LINK_H_

#import <UIKit/UIKit.h>

#include <cstdint>

#import "xenia/ui/ios/shared/ios_status_toast.h"

// System Link over the internet: each game plays in its public lobby or in a
// private room with a code while Online features are on.

// The private room code |title_id| plays in, or nil for its public lobby.
// Stored in NSUserDefaults.
NSString* xe_ios_system_link_room(uint32_t title_id);
void xe_ios_set_system_link_room(uint32_t title_id, NSString* code);

// A new private room code.
NSString* xe_ios_new_system_link_room_code(void);
// The code |text| names, read leniently, or nil.
NSString* xe_ios_normalize_system_link_room_code(NSString* text);

// xenios://link/CODE?title=XXXXXXXX, which launches the game in that room.
NSURL* xe_ios_system_link_invite_url(uint32_t title_id, NSString* code);
// The room code an invite names, or nil for any other URL. Any app or web page
// can open one, so the room is only set once the player agrees to the launch.
NSString* xe_ios_system_link_invite_room(NSURL* url, uint32_t* title_id_out);
// A sentence for the launch prompt naming the room the invite sets.
NSString* xe_ios_system_link_invite_note(NSString* code);
// Sets the room an invite named, and tells the player when Online features are off.
void xe_ios_join_system_link_room(uint32_t title_id, NSString* code);

// Whether System Link is set to play through the relay.
BOOL xe_ios_system_link_uses_relay(void);

// What System Link is doing in the running game.
struct XeniaIOSSystemLinkSnapshot {
  // Off until the game starts System Link over the internet.
  enum class State { kOff, kConnecting, kReconnecting, kOnline, kOffline, kRejected };
  State state = State::kOff;
  // Why the server turned the player away, while rejected.
  NSString* problem = nil;
  // The game, and its private room's code, nil in the public lobby. Known once it started.
  uint32_t title_id = 0;
  NSString* room_code = nil;
  // The other players in the room, and how many are reached directly and through the relay.
  size_t others = 0;
  size_t direct = 0;
  size_t relayed = 0;
  // Another player runs another title or another version of it.
  bool other_version = false;
};
XeniaIOSSystemLinkSnapshot xe_ios_system_link_snapshot(void);
// "Private Room ABC123", or "Public Lobby" for a nil code.
NSString* xe_ios_system_link_room_name(NSString* code);
// Posted on the main thread when the snapshot may have changed.
extern NSString* const XeniaIOSSystemLinkStatusDidChangeNotification;
// The in-game menu's System Link card: how it's connected, the room and the other players,
// which opens the System Link status sheet. Hidden while System Link is off. Owned by the
// caller.
UIView* xe_ios_new_system_link_status_view(void);

// Who's online: the players in each game's public lobby, which the lobby counts, and the apps
// open. Private rooms aren't counted. Asked for about once a minute while |wanted| says so and
// Online features are on, and |changed| is called on the main thread when the counts change.
// Every 5 minutes the app also tells the lobby it's open, under an ID it makes up each launch.
// Call once.
void xe_ios_system_link_watch_online(BOOL (^wanted)(void), void (^changed)(void));
// Asks for the counts now unless they're under a minute old.
void xe_ios_system_link_refresh_online(void);
// Keeps the counts current while |wanted| says no, such as in a running game, until a NO
// balances each YES.
void xe_ios_system_link_keep_online(BOOL keep);
// Players by title ID, none with 0, or nil while the counts aren't known.
NSDictionary<NSNumber*, NSNumber*>* xe_ios_system_link_online_titles(void);
NSInteger xe_ios_system_link_online_players(uint32_t title_id);
// How many apps are open with Online features on, in the library or in a game, this one
// included, or -1 while unknown or the lobby doesn't say.
NSInteger xe_ios_system_link_online_here(void);
// How many players are in private rooms, all together, or -1 while unknown or the lobby doesn't
// say.
NSInteger xe_ios_system_link_online_private(void);
// "3 players online" for VoiceOver, or nil when there's nobody.
NSString* xe_ios_system_link_online_description(uint32_t title_id);
// Posted on the main thread when the counts change.
extern NSString* const XeniaIOSSystemLinkOnlineDidChangeNotification;
// Shows "3 online" at the end of a game tile's footer row (a UIStackView) while players are in
// its public lobby, and hides it otherwise. Call each time the tile shows a game, 0 for none.
void xe_ios_system_link_show_online_label(UIView* row, uint32_t title_id);
// All the players in public lobbies, 0 while the counts aren't known.
NSInteger xe_ios_system_link_online_total(void);
// The library's Online button, "Online" with how many players are online, which
// calls what xe_ios_system_link_set_online_menu set. Autoreleased.
UIButton* xe_ios_new_system_link_online_button(void);
void xe_ios_system_link_set_online_menu(void (^open)(void));

// Lets games play over the internet while Online features are on, each in its room, and
// tells the player what System Link does through |toast| on the main thread.
// Call once.
void xe_ios_system_link_install(void (^toast)(NSString* message, XeniaIOSStatusToastStyle style));

#endif  // XENIA_UI_IOS_SHARED_IOS_SYSTEM_LINK_H_
