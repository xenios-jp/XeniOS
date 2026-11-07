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
// Stored in NSUserDefaults. With private rooms by default, a game without a choice gets a new
// room the first time it's asked.
NSString* xe_ios_system_link_room(uint32_t title_id);
void xe_ios_set_system_link_room(uint32_t title_id, NSString* code);
// NSUserDefaults BOOL, Settings → Online: games without a room chosen start in a new private
// room rather than their public lobby. Choices made on a game's page stay.
extern NSString* const XeniaIOSSystemLinkPrivateByDefaultKey;

// A new private room code.
NSString* xe_ios_new_system_link_room_code(void);
// The code |text| names, read leniently, or nil.
NSString* xe_ios_normalize_system_link_room_code(NSString* text);

// xenios://link/CODE?title=XXXXXXXX, which launches the game in that room.
NSURL* xe_ios_system_link_invite_url(uint32_t title_id, NSString* code);
// What sharing a room sends: the lobby's page for it (https://…/j/TITLE/CODE), which shows a
// card in chats and opens XeniOS from its button, or the xenios:// invite when the lobby has no
// pages.
NSURL* xe_ios_system_link_share_url(uint32_t title_id, NSString* code);
// The room code an invite names (xenios://link/CODE?title=…, the lobby page's
// xenios://join?t=TITLE&r=CODE, or the page itself), or nil for any other URL. Any app or web
// page can open one, so the room is only set once the player agrees to the launch.
NSString* xe_ios_system_link_invite_room(NSURL* url, uint32_t* title_id_out);
// Sentences for the launch prompt: the room the invite uses for this launch only, who sees the
// player's IP address, that it needs Online Features, and, when |restarts| is set, that the
// running game restarts.
NSString* xe_ios_system_link_invite_note(NSString* code, BOOL restarts);
// Makes the room an invite named the room of the game's next launch only, leaving the saved room
// alone, and tells the player when Online features are off. An empty code is the game's public
// lobby.
void xe_ios_join_system_link_room(uint32_t title_id, NSString* code);

// Whether System Link is set to play through the relay.
BOOL xe_ios_system_link_uses_relay(void);
// Sets System Link to play through the relay, as it does by default, and tells the running game.
void xe_ios_system_link_use_relay(void);

// How the network the device is on treats direct connections between players, as System Link's
// last check on it found.
enum class XeniaIOSNatType { kUnknown, kOpen, kModerate, kStrict, kBlocked };
// The current network's, checked when a game last started System Link on it. Unknown until then.
XeniaIOSNatType xe_ios_system_link_nat(void);
// Whether it keeps most direct connections out: strict or blocked.
BOOL xe_ios_nat_blocks_direct(XeniaIOSNatType nat);
// "Open", "Moderate", "Strict" or "Blocked", or nil while unknown.
NSString* xe_ios_nat_type_name(XeniaIOSNatType nat);
// What that means for playing, in a sentence, or nil while unknown.
NSString* xe_ios_nat_type_explanation(XeniaIOSNatType nat);
// Whether the lobby named a TURN server the last time a game joined it, so players can be
// reached through a relay. Off until then.
BOOL xe_ios_system_link_lobby_has_turn(void);
// "Can't connect to 1 player. Your network blocks direct connections." for |count| players that
// no path reaches, blaming this network when |nat| blocks direct connections and theirs
// otherwise.
NSString* xe_ios_system_link_unreachable_message(size_t count, XeniaIOSNatType nat);

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
  // Players reached neither way for about 10 seconds.
  size_t unreachable = 0;
  // This network's NAT type, once checked, and whether the lobby names a TURN server.
  XeniaIOSNatType nat = XeniaIOSNatType::kUnknown;
  bool turn = false;
  // Another player runs another title or another version of it.
  bool other_version = false;
  // The round trip to the slowest player reached directly; 0 until measured.
  uint32_t ping_ms = 0;
};
XeniaIOSSystemLinkSnapshot xe_ios_system_link_snapshot(void);
// "Private Room ABC123", or "Public Lobby" for a nil code.
NSString* xe_ios_system_link_room_name(NSString* code);
// Posted on the main thread when the snapshot may have changed.
extern NSString* const XeniaIOSSystemLinkStatusDidChangeNotification;
// The in-game menu's status pill, for the header beside the game's title: "Online · <friends>"
// in a game signed in to the account (opens Friends), else the System Link room's state (opens
// the System Link status sheet). Hidden while neither applies. Owned by the caller.
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
// Title ID to how many apps run it, wherever they play (public lobby, private room or alone);
// nil while the counts aren't known, empty from a lobby that doesn't count it.
NSDictionary<NSNumber*, NSNumber*>* xe_ios_system_link_online_playing(void);
NSInteger xe_ios_system_link_online_playing_count(uint32_t title_id);
// Tells the lobby which game this app runs (0 when it stops), on the presence socket, so the
// Online sheet can count the players of each game. Main thread.
void xe_ios_system_link_set_playing(uint32_t title_id);
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
// Whether the lobby answered lately, so the counts are known.
BOOL xe_ios_system_link_online_connected(void);
// All the players in public lobbies, 0 while the counts aren't known.
NSInteger xe_ios_system_link_online_total(void);
// The library's Online button, "Online" with how many players are online, which
// calls what xe_ios_system_link_set_online_menu set. Autoreleased.
UIButton* xe_ios_new_system_link_online_button(void);
void xe_ios_system_link_set_online_menu(void (^open)(void));
// The Online button's frame in window coordinates when it was last tapped, or CGRectNull.
CGRect xe_ios_system_link_online_button_rect(void);
// The library's globe button: a round glass button with the players online on XeniOS in a
// green badge, which calls what xe_ios_system_link_set_globe_action set. Autoreleased.
UIButton* xe_ios_new_system_link_globe_button(void);
void xe_ios_system_link_set_globe_action(void (^open)(void));
// The players on XeniOS other than this device, or 0 while signed out of Live or offline.
NSInteger xe_ios_system_link_players_elsewhere(void);
// "23", "1.2k", "12k".
NSString* xe_ios_system_link_players_text(NSInteger count);
// What the Online button's badge shows: |attention| (game and friend invites waiting) in the
// accent, else how many friends are online. Main thread.
void xe_ios_system_link_set_social_counts(NSInteger friends_online, NSInteger attention);
// A game that plays online through the profile's account (an invite, a friend's game) rides the
// System Link room without being a System Link game. While |is_live| says so, the in-game card
// reads "Playing Online · With <|players|>" and opens |open| (Friends), and the room's join,
// leave and version toasts stay quiet. Main thread.
void xe_ios_system_link_set_live_game(BOOL (^is_live)(void), NSString* (^players)(void),
                                      void (^open)(void));
// Something |is_live| or |players| depends on changed: the card shows it again.
void xe_ios_system_link_live_game_changed(void);
extern NSString* const XeniaIOSSystemLinkLiveGameDidChangeNotification;

// Lets games play over the internet while Online features are on, each in its room, and
// tells the player what System Link does through |toast| on the main thread. |link_off| is
// called on the main thread the first time each game looks for a network while System Link is
// off. Call once.
void xe_ios_system_link_install(void (^toast)(NSString* message, XeniaIOSStatusToastStyle style),
                                void (^link_off)(uint32_t title_id));

#endif  // XENIA_UI_IOS_SHARED_IOS_SYSTEM_LINK_H_
