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
#include <vector>

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
  // The profiles can't be read right now (a game is starting or stopping, so the kernel and its
  // profiles are being rebuilt). The open connection stays as it is until they're back.
  std::function<bool()> profiles_busy;
  // Turns online play on for the profile (it gets an online XUID).
  std::function<bool(uint64_t xuid)> enable_online;
  // The service moved the account to another online XUID: keep it.
  std::function<void(uint64_t xuid, uint64_t online_xuid)> set_online_xuid;
  // Every profile on this device.
  std::function<std::vector<XeniaIOSLiveProfile>()> profiles;
  // Turns online play off again (going online failed before an account was made).
  std::function<bool(uint64_t xuid)> disable_online;
  // Renames the profile in place; its saves and achievements stay.
  std::function<bool(uint64_t xuid, const std::string& gamertag)> rename_profile;
  // This device joined the account |online_xuid| (a link or recovery code): the profile that has
  // it, or a new one named |gamertag|, online, signed in to the first slot. Its XUID, or 0.
  std::function<uint64_t(uint64_t online_xuid, const std::string& gamertag)> adopt_account;
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
// In a System Link game: joinable, but from the game's own System Link screen
// (no Live session), once in their room.
@property(nonatomic, readonly) BOOL systemLink;
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

typedef NS_ENUM(NSInteger, XeniaLiveGamePlayerRelation) {
  XeniaLiveGamePlayerRelationNone = 0,
  XeniaLiveGamePlayerRelationFriend,
  XeniaLiveGamePlayerRelationRequestSent,      // This profile asked them.
  XeniaLiveGamePlayerRelationRequestReceived,  // They asked this profile.
};

// Someone else in the game this app is running.
@interface XeniaLiveGamePlayer : NSObject
@property(nonatomic, readonly) uint64_t xuid;
// Nil while this profile doesn't know it: a stranger, until a friend request or the party
// names them.
@property(nonatomic, readonly, copy) NSString* gamertag;
@property(nonatomic, readonly) XeniaLiveGamePlayerRelation relation;
// This profile's friend request to them was refused because they take requests by friend code
// only. Known once a request was tried, until XeniOS quits.
@property(nonatomic, readonly) BOOL takesRequestsByCodeOnly;
@end

// Someone this profile played an online game with on this device (Recent Players).
@interface XeniaLiveRecentPlayer : XeniaLiveGamePlayer
// The game they were last seen in.
@property(nonatomic, readonly) uint32_t titleId;
@property(nonatomic, readonly, copy) NSDate* lastSeen;
@end

// Who may send this profile friend requests (the service keeps it, for all its devices).
typedef NS_ENUM(NSInteger, XeniaLiveFriendRequestsSetting) {
  // Not online, or the service doesn't have the setting yet.
  XeniaLiveFriendRequestsSettingUnknown = 0,
  XeniaLiveFriendRequestsSettingAnyone,
  // Only players who have the profile's friend code, link or QR code.
  XeniaLiveFriendRequestsSettingCode,
};

// Someone this profile blocked.
@interface XeniaLiveBlockedPlayer : NSObject
@property(nonatomic, readonly) uint64_t xuid;
// Nil when this device never knew it (blocked on another device, say).
@property(nonatomic, readonly, copy) NSString* gamertag;
@end

// Why a player is reported; the service's reasons.
typedef NS_ENUM(NSInteger, XeniaLiveReportReason) {
  XeniaLiveReportReasonCheating = 0,
  XeniaLiveReportReasonHarassment,
  XeniaLiveReportReasonOffensiveName,
  XeniaLiveReportReasonOffensiveContent,
  XeniaLiveReportReasonSpam,
  XeniaLiveReportReasonOther,
};
// The most a report's note holds, in NSString length (UTF-16 units, as the service counts).
extern const NSUInteger XeniaLiveReportNoteLimit;

// Posted on the main thread when anything a screen shows may have changed:
// the state, friends, presence or invites.
extern NSNotificationName const XeniaLiveSocialDidChangeNotification;
// Posted on the main thread for each invite as it arrives; object is the
// XeniaLiveInvite.
extern NSNotificationName const XeniaLiveSocialInviteReceivedNotification;
// Posted on the main thread when the service couldn't do something asked of
// it; userInfo[XeniaLiveSocialMessageKey] says what, for a toast.
extern NSNotificationName const XeniaLiveSocialRequestFailedNotification;
// Posted on the main thread when the running game couldn't join the game an
// invite (or the party's leader) led to; userInfo[XeniaLiveSocialMessageKey]
// says why, for a toast over the game.
extern NSNotificationName const XeniaLiveSocialJoinFailedNotification;
// Posted on the main thread when an invite accepted (or a friend joined) leads
// to a System Link game, which the player joins from the game's own System
// Link screen; userInfo[XeniaLiveSocialMessageKey] says so, for a toast.
extern NSNotificationName const XeniaLiveSocialSystemLinkJoinNotification;
// Posted on the main thread when the running game accepted an invite to another game (or one
// came with no game running): userInfo[XeniaLiveSocialTitleKey] (NSNumber) is the game to start
// and userInfo[XeniaLiveSocialGamertagKey] the inviter. The app starts it from the library,
// asking first when a game runs, and calls acceptHeldInviteForLaunch just before.
extern NSNotificationName const XeniaLiveSocialInviteForOtherGameNotification;
extern NSString* const XeniaLiveSocialTitleKey;
extern NSString* const XeniaLiveSocialGamertagKey;
extern NSString* const XeniaLiveSocialMessageKey;
// In a RequestFailed userInfo: the request the service refused ("friend_add", ...).
extern NSString* const XeniaLiveSocialRequestKey;
// In a RequestFailed userInfo for a friend request a player refused: their XUID (NSNumber).
extern NSString* const XeniaLiveSocialPlayerKey;
// Posted on the main thread when something asked of the service worked and the player should
// hear so (a report was stored); userInfo[XeniaLiveSocialMessageKey] says it, for a toast.
extern NSNotificationName const XeniaLiveSocialNoticeNotification;
// Posted on the main thread when the service didn't take a report; userInfo[
// XeniaLiveSocialMessageKey] says why, for a toast.
extern NSNotificationName const XeniaLiveSocialReportFailedNotification;
// Posted on the main thread when the lobby signed this device out of the account, so the profile
// is offline now: userInfo[XeniaLiveSocialMessageKey] says what happened and where, and
// userInfo[XeniaLiveSocialSignedOutReasonKey] why: "elsewhere" (the account signed in on another
// device; signing in here again takes it back), "removed" (another device removed this one; a
// code from it signs in again) or "deleted" (the account is gone).
extern NSNotificationName const XeniaLiveSocialSignedOutNotification;
extern NSString* const XeniaLiveSocialSignedOutReasonKey;

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
// ProfileOffline because the account signed in on another device: statusText says where, and
// turnOnOnlineForProfile signs in here again.
@property(nonatomic, readonly) BOOL signedInElsewhere;
@property(nonatomic, readonly, copy) NSString* gamertag;
// The code friends add this profile with, once online.
@property(nonatomic, readonly, copy) NSString* friendCode;
// The lobby's web pages (https://host for wss://host), for links that open in any browser and
// show a card in chats; nil while offline or when the lobby has none (an older one).
@property(nonatomic, readonly, copy) NSURL* shareWebRoot;
// What sharing the friend code sends: the lobby's page for it (https://…/f/CODE), which shows a
// card in chats and opens XeniOS from its button, or xenios://friend?c=CODE from a lobby without
// pages. Nil while offline.
@property(nonatomic, readonly, copy) NSURL* friendShareURL;
// The lobby's page for joining this profile in the running game's System Link room
// (https://…/j/TITLE/ROOM): nil when the game isn't in a room, or the lobby has no pages.
@property(nonatomic, readonly, copy) NSURL* joinShareURL;

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
// The other players in the running game with an account on this service: the players the game
// put in its sessions. Named ones by name, then the rest. Empty at the library or offline.
@property(nonatomic, readonly) NSArray<XeniaLiveGamePlayer*>* gamePlayers;
// The players this profile was in online games with on this device, newest first: at most 50,
// kept per profile, blocked ones left out. Names come as the service gives them.
@property(nonatomic, readonly) NSArray<XeniaLiveRecentPlayer*>* recentPlayers;
// Who may send this profile friend requests, as the service says it (Unknown when it doesn't).
// Setting it asks the service; the value changes when the service answers, here and on the
// profile's other devices. Setting does nothing while it's Unknown.
@property(nonatomic) XeniaLiveFriendRequestsSetting friendRequestsSetting;
// Whether creating, leaving and inviting to a party can be asked now (signed
// in).
@property(nonatomic, readonly) BOOL partyActionsAvailable;

// Looks at the profile, Online Features and System Link again, and connects,
// reconnects or disconnects. Cheap; the bridge calls it itself on a timer and
// when the app becomes active.
- (void)refresh;
- (BOOL)turnOnOnlineForProfile;
// The lobby ended this profile's session for one on another device; |message| says where. Kept
// until the profile goes online again.
- (void)noteSignedInElsewhere:(NSString*)message profile:(uint64_t)profile_xuid;

- (void)addFriendWithCode:(NSString*)code;
- (void)acceptFriendRequest:(uint64_t)xuid;
// Asks a player to be friends (someone in the game, or a recent player). Their request to this
// profile, if they sent one, is accepted instead.
- (void)sendFriendRequestTo:(uint64_t)xuid;
- (void)declineFriendRequest:(uint64_t)xuid;
// Removes a friend, or cancels a request this profile sent.
- (void)removeFriend:(uint64_t)xuid;
// Looks players up by the start of their gamertag (3 or more characters); playersMatching: has
// them once the service answers.
- (void)searchPlayers:(NSString*)prefix;
// The players the last search for |query| found (not this profile, nor blocked players, nor ones
// who take requests by code only); empty until the service answers that search.
- (NSArray<XeniaLiveGamePlayer*>*)playersMatching:(NSString*)query;
// Whether the service has answered a search for |query| (found players or not).
- (BOOL)searchAnswered:(NSString*)query;

// YES for another account's online XUID while online: the only players a friend request, block
// or report can name (the service drops the connection over any other).
- (BOOL)canActOnPlayer:(uint64_t)xuid;
// YES once the service refused this profile's request to |xuid|: they take requests only by
// their friend code.
- (BOOL)playerTakesRequestsByCodeOnly:(uint64_t)xuid;

// Blocking: a blocked player can't send this profile friend requests or invites, and is left out
// of every list here. Blocking also ends a friendship or request. |gamertag| (when known) is kept
// on this device so the blocked list can name them.
- (void)blockPlayer:(uint64_t)xuid gamertag:(NSString*)gamertag;
- (void)unblockPlayer:(uint64_t)xuid;
- (BOOL)isBlocked:(uint64_t)xuid;
// Everyone this profile blocked, by name (unnamed ones last).
@property(nonatomic, readonly) NSArray<XeniaLiveBlockedPlayer*>* blockedPlayers;
// How many there are, without naming them.
@property(nonatomic, readonly) NSUInteger blockedPlayerCount;
// Reports a player to the people who run the service, with the running game and its session
// when there is one. The answer comes as a Notice (stored) or a ReportFailed. NO when not online.
// |note| may be nil.
- (BOOL)reportPlayer:(uint64_t)xuid reason:(XeniaLiveReportReason)reason note:(NSString*)note;

// Appear offline: friends see this profile as offline while it still signs in, joins games and
// gets invites. Kept per profile on this device, and applied whenever the profile connects.
@property(nonatomic) BOOL appearOffline;

- (void)declineInvite:(XeniaLiveInvite*)invite;
// Answers yes and leaves the invite for the title to read, before the app
// starts it: the title sees it was started from an invite
// (XInviteGetAcceptedInfo, XN_LIVE_INVITE_ACCEPTED). Also points System Link
// at the invite's room.
- (void)acceptInviteForLaunch:(XeniaLiveInvite*)invite;
// The same for the invite to another game the running one accepted
// (XeniaLiveSocialInviteForOtherGameNotification). Nothing when none is held.
- (void)acceptHeldInviteForLaunch;
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
// Any player, a friend or not, as from a gamer card: the lobby turns away one who takes friend
// requests by code only, and drops one from a player they blocked.
- (BOOL)invitePlayerToRunningGame:(uint64_t)xuid;
- (BOOL)invitePlayerToParty:(uint64_t)xuid;
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
// "xenios://friend?c=K7Q2M9XD": what Add Friend's QR code and shared link hold.
NSString* xe_ios_friend_link(NSString* code);
// The code in a friend link (xenios://friend?c=CODE or a lobby page, https://…/f/CODE), or in
// text that is just a code; nil otherwise.
NSString* xe_ios_friend_code_from_text(NSString* text);

#endif  // XENIA_UI_IOS_SHARED_IOS_LIVE_SOCIAL_BRIDGE_H_
