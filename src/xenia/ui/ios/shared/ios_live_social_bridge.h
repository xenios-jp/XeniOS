/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_LIVE_SOCIAL_BRIDGE_H_
#define XENIA_UI_IOS_SHARED_IOS_LIVE_SOCIAL_BRIDGE_H_

#import <UIKit/UIKit.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

// Friends, presence and game invites for the app's own screens: the active
// profile's account on the online service (the System Link lobby, protocol
// v2), kept connected while XeniOS is open with online play on, whether or not
// a game runs. A game that starts shares this connection
// (xe::net::live::ShareAccount), so the profile signs in once.

// What the app tells the bridge about its profiles. Called on the main thread.
struct XeniaIOSLiveProfile {
  uint64_t xuid = 0;  // The profile's own (offline) XUID.
  uint64_t online_xuid = 0;  // 0 until online play is on for the profile.
  std::string gamertag;
  bool live_enabled = false;
};
struct XeniaIOSLiveSocialHost {
  // The profile signed in to the first slot, if any.
  std::function<std::optional<XeniaIOSLiveProfile>()> active_profile;
  // Turns online play on for the profile (it gets an online XUID).
  std::function<bool(uint64_t xuid)> enable_online;
  // The service moved the account to another online XUID: keep it.
  std::function<void(uint64_t xuid, uint64_t online_xuid)> set_online_xuid;
  // Where the device key lives: <storage_root>/live/device_key.
  std::filesystem::path storage_root;
};
// Once, at startup (xenia_main_ios.mm).
void xe_ios_live_social_install(XeniaIOSLiveSocialHost host);

typedef NS_ENUM(NSInteger, XeniaLiveSocialState) {
  // Online Features or System Link over the internet is off.
  XeniaLiveSocialStateOff = 0,
  // No profile is signed in.
  XeniaLiveSocialStateNoProfile,
  // The profile hasn't got online play turned on.
  XeniaLiveSocialStateProfileOffline,
  XeniaLiveSocialStateConnecting,
  XeniaLiveSocialStateOnline,
  // The service refused the profile, or can't be reached (problemText says).
  XeniaLiveSocialStateProblem,
};

typedef NS_ENUM(NSInteger, XeniaLiveFriendState) {
  XeniaLiveFriendStateFriend = 0,
  XeniaLiveFriendStateIncoming,  // Asked to be friends with this profile.
  XeniaLiveFriendStateOutgoing,  // This profile asked them.
};

@interface XeniaLiveFriend : NSObject
@property(nonatomic, readonly) uint64_t xuid;
@property(nonatomic, readonly, copy) NSString* gamertag;
@property(nonatomic, readonly) XeniaLiveFriendState state;
@property(nonatomic, readonly) BOOL online;
// The game they're in; 0 when none (or offline).
@property(nonatomic, readonly) uint32_t titleId;
// In a session friends may join.
@property(nonatomic, readonly) BOOL joinable;
// The game session they're in and others may join (16 hex digits), or nil.
@property(nonatomic, readonly, copy) NSString* sessionId;
@end

@interface XeniaLiveInvite : NSObject
@property(nonatomic, readonly, copy) NSString* inviteId;
@property(nonatomic, readonly) uint64_t fromXuid;
@property(nonatomic, readonly, copy) NSString* gamertag;
@property(nonatomic, readonly) uint32_t titleId;
@property(nonatomic, readonly, copy) NSString* message;
@property(nonatomic, readonly, copy) NSDate* received;
// An invite to the sender's party, not to a game (titleId is the game they're in).
@property(nonatomic, readonly) BOOL party;
@end

// Someone in this profile's party.
@interface XeniaLivePartyMember : NSObject
@property(nonatomic, readonly) uint64_t xuid;
@property(nonatomic, readonly, copy) NSString* gamertag;
@property(nonatomic, readonly) BOOL leader;
// This profile.
@property(nonatomic, readonly) BOOL me;
@end

// Posted on the main thread when anything a screen shows may have changed:
// the state, friends, presence or invites.
extern NSNotificationName const XeniaLiveSocialDidChangeNotification;
// Posted on the main thread for each invite as it arrives; object is the
// XeniaLiveInvite.
extern NSNotificationName const XeniaLiveSocialInviteReceivedNotification;
// Posted on the main thread when the service couldn't do something asked of
// it; userInfo[XeniaLiveSocialMessageKey] says what, for a toast.
extern NSNotificationName const XeniaLiveSocialRequestFailedNotification;
extern NSString* const XeniaLiveSocialMessageKey;

@interface XeniaLiveSocial : NSObject

+ (XeniaLiveSocial*)shared;

// The app's side. Called on the main thread.
// The title running now, 0 at the library.
@property(nonatomic, copy) uint32_t (^runningTitleProvider)(void);
// A game's name, from the library or the game database; nil when unknown.
@property(nonatomic, copy) NSString* (^titleNameProvider)(uint32_t title_id);

@property(nonatomic, readonly) XeniaLiveSocialState state;
// One sentence for the Problem, Off and ProfileOffline states.
@property(nonatomic, readonly, copy) NSString* statusText;
@property(nonatomic, readonly, copy) NSString* gamertag;
// The code friends add this profile with, once online.
@property(nonatomic, readonly, copy) NSString* friendCode;

@property(nonatomic, readonly) NSArray<XeniaLiveFriend*>* friends;
@property(nonatomic, readonly) NSArray<XeniaLiveFriend*>* incomingRequests;
@property(nonatomic, readonly) NSArray<XeniaLiveFriend*>* outgoingRequests;
// Newest first.
@property(nonatomic, readonly) NSArray<XeniaLiveInvite*>* invites;
// Invites plus friend requests, for a badge.
@property(nonatomic, readonly) NSInteger attentionCount;
// Friends online now, for the library's Online button.
@property(nonatomic, readonly) NSInteger onlineFriendCount;

// The party this profile is in: its members, the leader first; empty when in none.
@property(nonatomic, readonly) NSArray<XeniaLivePartyMember*>* partyMembers;
// Whether creating, leaving and inviting to a party can be asked now (signed
// in).
@property(nonatomic, readonly) BOOL partyActionsAvailable;

// Looks at the profile, Online Features and System Link again, and connects,
// reconnects or disconnects. Cheap; the bridge calls it itself on a timer and
// when the app becomes active.
- (void)refresh;
- (BOOL)turnOnOnlineForProfile;

- (void)addFriendWithCode:(NSString*)code;
- (void)acceptFriendRequest:(uint64_t)xuid;
- (void)declineFriendRequest:(uint64_t)xuid;
// Removes a friend, or cancels a request this profile sent.
- (void)removeFriend:(uint64_t)xuid;

- (void)declineInvite:(XeniaLiveInvite*)invite;
// Answers yes and leaves the invite for the title to read, before the app
// starts it: the title sees it was started from an invite
// (XInviteGetAcceptedInfo, XN_LIVE_INVITE_ACCEPTED). Also points System Link
// at the invite's room.
- (void)acceptInviteForLaunch:(XeniaLiveInvite*)invite;
// Accepts in the game that's running (its title is the invite's).
- (BOOL)acceptInviteInRunningGame:(XeniaLiveInvite*)invite;
// The same for a friend's joinable game, from their presence.
- (BOOL)joinFriendForLaunch:(XeniaLiveFriend*)peer;
- (BOOL)joinFriendInRunningGame:(XeniaLiveFriend*)peer;
// Invites a friend to the running game. NO when no game runs.
- (BOOL)inviteFriendToRunningGame:(XeniaLiveFriend*)peer;
// Drops an invite accepted for a launch that didn't happen (another game
// started).
- (void)forgetAcceptedInvite;

// Parties. Each returns NO when it can't be asked (offline, or
// partyActionsAvailable is NO).
- (BOOL)createParty;
- (BOOL)leaveParty;
- (BOOL)inviteFriendToParty:(XeniaLiveFriend*)peer;
// Joins the party of a party invite (leaving the one this profile is in).
- (BOOL)acceptPartyInvite:(XeniaLiveInvite*)invite;

// Whether the running game plays online through this profile's account (it signed in to it).
@property(nonatomic, readonly) BOOL runningGameOnline;
// The friends in the running game and the party's other members, as "Ann, Bo"; nil when none.
// Friends in the same game session as you (not just the same game).
- (NSString*)playersWithYouText;
- (BOOL)friendInYourGame:(XeniaLiveFriend*)peer;

- (XeniaLiveFriend*)friendWithXuid:(uint64_t)xuid;
// "Halo 3", or a generic name when the game isn't known.
- (NSString*)nameForTitle:(uint32_t)title_id;
// "Playing Halo 3", "Online", "Offline".
- (NSString*)presenceTextForFriend:(XeniaLiveFriend*)peer;

@end

// "K7Q2M9XD" as "K7Q2 M9XD" (codes of other lengths as they are).
NSString* xe_ios_format_friend_code(NSString* code);
// What the player typed, uppercased without spaces or dashes; nil when it
// can't be a code.
NSString* xe_ios_normalize_friend_code(NSString* text);

#endif  // XENIA_UI_IOS_SHARED_IOS_LIVE_SOCIAL_BRIDGE_H_
