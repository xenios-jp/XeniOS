/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_live_social_bridge.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/xam_live_invites.h"
#include "xenia/kernel/xam/xam_live_social.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/kernel/xsession.h"
#include "xenia/net/live_account_hub.h"
#include "xenia/net/relay_protocol.h"
#import "xenia/ui/ios/shared/ios_background_refresh.h"
#import "xenia/ui/ios/shared/ios_live_account_real.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_system_link.h"

NSNotificationName const XeniaLiveSocialDidChangeNotification =
    @"XeniaLiveSocialDidChangeNotification";
NSNotificationName const XeniaLiveSocialInviteReceivedNotification =
    @"XeniaLiveSocialInviteReceivedNotification";
NSNotificationName const XeniaLiveSocialRequestFailedNotification =
    @"XeniaLiveSocialRequestFailedNotification";
NSNotificationName const XeniaLiveSocialJoinFailedNotification =
    @"XeniaLiveSocialJoinFailedNotification";
NSNotificationName const XeniaLiveSocialSystemLinkJoinNotification =
    @"XeniaLiveSocialSystemLinkJoinNotification";
NSNotificationName const XeniaLiveSocialInviteForOtherGameNotification =
    @"XeniaLiveSocialInviteForOtherGameNotification";
NSString* const XeniaLiveSocialTitleKey = @"XeniaLiveSocialTitle";
NSString* const XeniaLiveSocialGamertagKey = @"XeniaLiveSocialGamertag";
NSNotificationName const XeniaLiveSocialReportFailedNotification =
    @"XeniaLiveSocialReportFailedNotification";
NSString* const XeniaLiveSocialMessageKey = @"message";
NSString* const XeniaLiveSocialRequestKey = @"request";
NSString* const XeniaLiveSocialPlayerKey = @"player";
NSNotificationName const XeniaLiveSocialNoticeNotification =
    @"XeniaLiveSocialNoticeNotification";
NSNotificationName const XeniaLiveSocialSignedOutNotification =
    @"XeniaLiveSocialSignedOutNotification";
NSString* const XeniaLiveSocialSignedOutReasonKey = @"reason";
const NSUInteger XeniaLiveReportNoteLimit = xe::net::live::kMaxReportNote;

namespace live = xe::net::live;

namespace {

XeniaIOSLiveSocialHost* g_host = nullptr;

NSString* ToNS(const std::string& text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

// Why joining a game failed, in a sentence for a toast.
NSString* JoinFailedText(const std::string& gamertag, xe::kernel::xam::LiveJoinFailure reason) {
  const bool unreachable = reason == xe::kernel::xam::LiveJoinFailure::kHostUnreachable;
  if (gamertag.empty()) {
    return unreachable
               ? NSLocalizedString(@"Couldn't connect to the host's game. Their network may be "
                                   @"blocking connections.",
                                   @"Join toast: the host of the game didn't answer")
               : NSLocalizedString(@"The host left that game before you could join.",
                                   @"Join toast: the game's host left it");
  }
  NSString* name = [NSString stringWithUTF8String:gamertag.c_str()] ?: @"";
  return [NSString
      stringWithFormat:unreachable
                           ? NSLocalizedString(@"Couldn't connect to %@'s game. Their network may "
                                               @"be blocking connections.",
                                               @"Join toast: the host (a gamertag) didn't answer")
                           : NSLocalizedString(@"%@ left that game before you could join.",
                                               @"Join toast: the host (a gamertag) left the game"),
                       name];
}

// What the service said no to, in a sentence for a toast.
NSString* RequestFailedText(const std::string& request, const std::string& reason) {
  if (request == "friend_add") {
    if (reason == "not_found") {
      return NSLocalizedString(@"No player has that friend code. Check it and try again.",
                               @"Friends toast: the friend code typed matches nobody");
    }
    if (reason == "full") {
      return NSLocalizedString(@"One of you already has 100 friends.",
                               @"Friends toast: a friend list is full");
    }
    if (reason == "limit") {
      return NSLocalizedString(@"You have too many friend requests waiting. Cancel some first.",
                               @"Friends toast: too many requests sent");
    }
    if (reason == "blocked") {
      return NSLocalizedString(@"You blocked this player.",
                               @"Friends toast: adding a player the user blocked");
    }
  }
  if (request == "invite" && reason == "not_friends") {
    // An older lobby.
    return NSLocalizedString(@"You can only invite friends.",
                             @"Friends toast: inviting someone who isn't a friend");
  }
  if (request == "invite" && reason == "not_allowed") {
    return NSLocalizedString(@"This player only takes invites from friends.",
                             @"Friends toast: inviting a stranger whose privacy setting refuses it");
  }
  if (request == "invite" && reason == "in_party") {
    return NSLocalizedString(@"They're already in your party.",
                             @"Friends toast: a party invite to a member");
  }
  if (request == "invite" && reason == "blocked") {
    return NSLocalizedString(@"You blocked this player.",
                             @"Friends toast: inviting a player the user blocked");
  }
  if (request == "invite" && reason == "no_party") {
    return NSLocalizedString(@"Start a party first, then invite friends to it.",
                             @"Friends toast: a party invite without a party");
  }
  if (request == "invite_reply" && reason == "not_found") {
    return NSLocalizedString(@"That invite has expired.",
                             @"Friends toast: answering an invite that's gone");
  }
  return NSLocalizedString(@"The online service couldn't do that. Try again in a moment.",
                           @"Friends toast: any other refused request");
}

// A friend request the player refused: they take requests by friend code only.
NSString* CodeOnlyText(const std::string& gamertag) {
  if (gamertag.empty()) {
    return NSLocalizedString(@"This player only takes requests by friend code.",
                             @"Friends toast: a player who takes friend requests only by code, "
                             @"name unknown");
  }
  return [NSString stringWithFormat:NSLocalizedString(@"%@ only takes requests by friend code.",
                                                      @"Friends toast: a player who takes friend "
                                                      @"requests only by code; %@ is their "
                                                      @"gamertag"),
                                    ToNS(gamertag)];
}

// Why the service didn't take a report, in a sentence for a toast.
NSString* ReportFailedText(const std::string& reason) {
  if (reason == "duplicate") {
    return NSLocalizedString(@"You already reported this player today.",
                             @"Report toast: this player was reported already today");
  }
  if (reason == "limit") {
    return NSLocalizedString(@"You can send 5 reports a day.",
                             @"Report toast: the daily limit of reports was reached");
  }
  if (reason == "not_found") {
    return NSLocalizedString(@"That player's account no longer exists.",
                             @"Report toast: the reported account is gone");
  }
  if (reason == "invalid") {
    return NSLocalizedString(@"You can't report yourself.",
                             @"Report toast: the player reported is the user");
  }
  return NSLocalizedString(@"Your report couldn't be sent. Try again in a moment.",
                           @"Report toast: any other refused report");
}

const char* ReportReasonName(XeniaLiveReportReason reason) {
  switch (reason) {
    case XeniaLiveReportReasonCheating:
      return "cheating";
    case XeniaLiveReportReasonHarassment:
      return "harassment";
    case XeniaLiveReportReasonOffensiveName:
      return "offensive_name";
    case XeniaLiveReportReasonOffensiveContent:
      return "offensive_content";
    case XeniaLiveReportReasonSpam:
      return "spam";
    case XeniaLiveReportReasonOther:
      return "other";
  }
  return "other";
}

// Players by name, the ones without a known name last (in a steady order).
NSComparisonResult ComparePlayers(NSString* a_name, uint64_t a_xuid, NSString* b_name,
                                  uint64_t b_xuid) {
  if (!a_name != !b_name) {
    return a_name ? NSOrderedAscending : NSOrderedDescending;
  }
  if (!a_name) {
    return a_xuid < b_xuid ? NSOrderedAscending
                           : (a_xuid > b_xuid ? NSOrderedDescending : NSOrderedSame);
  }
  return [a_name localizedCaseInsensitiveCompare:b_name];
}

NSString* XuidKey(uint64_t xuid) {
  return [NSString stringWithFormat:@"%016llX", (unsigned long long)xuid];
}

// Per-profile settings in the user defaults, by the profile's own XUID.
NSString* AppearOfflineKey(uint64_t profile_xuid) {
  return [@"XeniaLiveAppearOffline." stringByAppendingString:XuidKey(profile_xuid)];
}
// The same setting by the profile's online XUID, for the account hub: a title can open the
// connection first, on its own thread, knowing only that XUID.
NSString* AppearOfflineOnlineKey(uint64_t online_xuid) {
  return [@"XeniaLiveAppearOffline.Online." stringByAppendingString:XuidKey(online_xuid)];
}
void KeepAppearOfflineForHub(uint64_t online_xuid, BOOL appear_offline) {
  if (!online_xuid) {
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if (appear_offline) {
    [defaults setBool:YES forKey:AppearOfflineOnlineKey(online_xuid)];
  } else {
    [defaults removeObjectForKey:AppearOfflineOnlineKey(online_xuid)];
  }
}
// The gamertags of the players a profile blocked from this device, by XUID: the lobby's list
// has XUIDs only, and a blocked player is no longer a friend whose name the list would have.
NSString* BlockedNamesKey(uint64_t profile_xuid) {
  return [@"XeniaLiveBlockedNames." stringByAppendingString:XuidKey(profile_xuid)];
}

// Recent Players: how many a profile keeps, and how often a player still in the game has their
// last-seen time written again.
constexpr NSUInteger kRecentPlayersKept = 50;
constexpr NSTimeInterval kRecentSeenStep = 60.0;

// Each profile's Recent Players, one small JSON file per profile beside the device key:
// <storage_root>/live/recent_players/<profile XUID>.json, an array of {"xuid": "0009…",
// "gamertag": "…" (once known), "title": "4D5307E6", "seen": seconds since 1970}, newest first.
NSString* RecentPlayersPath(uint64_t profile_xuid) {
  if (!g_host || !profile_xuid) {
    return nil;
  }
  const std::filesystem::path path = g_host->storage_root / "live" / "recent_players" /
                                     (XuidKey(profile_xuid).UTF8String + std::string(".json"));
  return [NSString stringWithUTF8String:path.c_str()];
}

// Why the connection is down, in a sentence.
NSString* ProblemText(const std::string& problem) {
  if (problem.find("4003") != std::string::npos || problem.find("version") != std::string::npos) {
    return NSLocalizedString(@"This version of XeniOS is too old for the online service. Update "
                             @"XeniOS to see your friends.",
                             @"Friends status: the lobby refused the app's protocol version");
  }
  if (problem.find("4004") != std::string::npos || problem.find("denied") != std::string::npos ||
      problem.find("4005") != std::string::npos) {
    return NSLocalizedString(@"The online service didn't accept this device for your profile.",
                             @"Friends status: the lobby refused the device key");
  }
  return NSLocalizedString(@"Can't reach the online service. XeniOS keeps trying.",
                           @"Friends status: the connection is down and retrying");
}

// Simulator only: -XeniaLiveSocialPreviewFriends N (or the same defaults key) adds N made-up
// friends to the real list, so a long list can be looked at. Device builds have none.
NSInteger PreviewFriendCount() {
#if TARGET_OS_SIMULATOR
  return MAX((NSInteger)0, MIN((NSInteger)200, [[NSUserDefaults standardUserDefaults]
                                                   integerForKey:@"XeniaLiveSocialPreviewFriends"]));
#else
  return 0;
#endif
}

void AddPreviewFriends(NSMutableArray* result);

}  // namespace

#pragma mark - Models

@interface XeniaLiveFriend ()
- (instancetype)initWithFriend:(const live::Friend&)f
                      presence:(const std::optional<live::Presence>&)presence;
@end

@implementation XeniaLiveFriend {
  uint64_t xuid_;
  NSString* gamertag_;
  XeniaLiveFriendState state_;
  BOOL online_;
  uint32_t title_id_;
  BOOL joinable_;
  BOOL system_link_;
  NSString* session_id_;
}
@synthesize sessionId = session_id_;
@synthesize systemLink = system_link_;
@synthesize xuid = xuid_;
@synthesize gamertag = gamertag_;
@synthesize state = state_;
@synthesize online = online_;
@synthesize titleId = title_id_;
@synthesize joinable = joinable_;

- (instancetype)initWithFriend:(const live::Friend&)f
                      presence:(const std::optional<live::Presence>&)presence {
  if ((self = [super init])) {
    xuid_ = f.xuid;
    gamertag_ = [ToNS(f.gamertag) copy];
    switch (f.state) {
      case live::Friend::State::kFriend:
        state_ = XeniaLiveFriendStateFriend;
        break;
      case live::Friend::State::kIncoming:
        state_ = XeniaLiveFriendStateIncoming;
        break;
      case live::Friend::State::kOutgoing:
        state_ = XeniaLiveFriendStateOutgoing;
        break;
    }
    if (presence && presence->online) {
      online_ = YES;
      title_id_ = presence->title_id;
      joinable_ = presence->joinable.has_value() && presence->title_id != 0;
      if (joinable_ && !presence->joinable->session_id.empty()) {
        session_id_ = [ToNS(presence->joinable->session_id) copy];
      }
      system_link_ = joinable_ && presence->joinable->info.empty();
      if (!presence->gamertag.empty()) {
        [gamertag_ release];
        gamertag_ = [ToNS(presence->gamertag) copy];
      }
    }
  }
  return self;
}

- (void)dealloc {
  [gamertag_ release];
  [session_id_ release];
  [super dealloc];
}
@end

namespace {

void AddPreviewFriends(NSMutableArray* result) {
#if TARGET_OS_SIMULATOR
  static const char* const kNames[] = {
      "Bo",          "Aria Vance",   "NightOwl 42", "Kenji",       "Marisol",     "TurboTaco",
      "Quiet Storm", "Lumen",        "Hollis",      "xXSniperXx",  "Priya",       "Cobalt Fox",
      "Dee",         "MidnightRun",  "Oskar",       "Tamsin",      "Rook",        "Juniper Ash",
      "Wren",        "PixelPirate",  "Sol",         "Ingrid",      "Mako",        "Velvet Echo"};
  static const uint32_t kTitles[] = {0x4D5307E6, 0x4D530877, 0x415607F7, 0x584109F0, 0x4D5308AB};
  const NSInteger count = PreviewFriendCount();
  const NSInteger names = sizeof(kNames) / sizeof(kNames[0]);
  for (NSInteger i = 0; i < count; ++i) {
    live::Friend f;
    f.xuid = 0xE000000000000000ull + (uint64_t)i;
    f.gamertag = kNames[i % names];
    if (i >= names) {
      f.gamertag += " " + std::to_string(i / names + 1);
    }
    std::optional<live::Presence> presence;
    if (i % 3 != 2) {  // two in three online
      presence.emplace();
      presence->xuid = f.xuid;
      presence->online = true;
      if (i % 2 == 0) {
        presence->title_id = kTitles[(i / 2) % 5];
        if (i % 4 == 0) {
          presence->joinable.emplace();
          presence->joinable->session_id = "00000000000000" + std::to_string(10 + i % 90);
        }
      }
    }
    [result addObject:[[[XeniaLiveFriend alloc] initWithFriend:f presence:presence] autorelease]];
  }
#else
  (void)result;
#endif
}

}  // namespace

@interface XeniaLiveInvite ()
- (instancetype)initWithInvite:(const live::Invite&)invite received:(NSDate*)received;
@property(nonatomic, readonly) const live::Invite& raw;
@end

@implementation XeniaLiveInvite {
  live::Invite invite_;
  NSString* invite_id_;
  NSString* gamertag_;
  NSString* message_;
  NSDate* received_;
}
@synthesize inviteId = invite_id_;
@synthesize gamertag = gamertag_;
@synthesize message = message_;
@synthesize received = received_;

- (instancetype)initWithInvite:(const live::Invite&)invite received:(NSDate*)received {
  if ((self = [super init])) {
    invite_ = invite;
    invite_id_ = [ToNS(invite.id) copy];
    gamertag_ = [ToNS(invite.gamertag) copy];
    message_ = [ToNS(invite.message) copy];
    received_ = [received copy];
  }
  return self;
}

- (void)dealloc {
  [invite_id_ release];
  [gamertag_ release];
  [message_ release];
  [received_ release];
  [super dealloc];
}

- (const live::Invite&)raw {
  return invite_;
}
- (uint64_t)fromXuid {
  return invite_.from;
}
- (uint32_t)titleId {
  return invite_.title_id;
}
- (BOOL)party {
  return invite_.party;
}
@end

@interface XeniaLiveGamePlayer ()
- (instancetype)initWithXuid:(uint64_t)xuid
                    gamertag:(NSString*)gamertag
                    relation:(XeniaLiveGamePlayerRelation)relation
                    codeOnly:(BOOL)code_only;
@end

@implementation XeniaLiveGamePlayer {
  NSString* gamertag_;
}
@synthesize xuid = xuid_;
@synthesize relation = relation_;
@synthesize takesRequestsByCodeOnly = code_only_;

- (instancetype)initWithXuid:(uint64_t)xuid
                    gamertag:(NSString*)gamertag
                    relation:(XeniaLiveGamePlayerRelation)relation
                    codeOnly:(BOOL)code_only {
  if ((self = [super init])) {
    xuid_ = xuid;
    gamertag_ = [gamertag copy];
    relation_ = relation;
    code_only_ = code_only;
  }
  return self;
}

- (void)dealloc {
  [gamertag_ release];
  [super dealloc];
}

- (NSString*)gamertag {
  return gamertag_;
}

@end

@interface XeniaLiveRecentPlayer ()
- (instancetype)initWithXuid:(uint64_t)xuid
                    gamertag:(NSString*)gamertag
                    relation:(XeniaLiveGamePlayerRelation)relation
                    codeOnly:(BOOL)code_only
                     titleId:(uint32_t)title_id
                    lastSeen:(NSDate*)last_seen;
@end

@implementation XeniaLiveRecentPlayer {
  NSDate* last_seen_;
}
@synthesize titleId = title_id_;
@synthesize lastSeen = last_seen_;

- (instancetype)initWithXuid:(uint64_t)xuid
                    gamertag:(NSString*)gamertag
                    relation:(XeniaLiveGamePlayerRelation)relation
                    codeOnly:(BOOL)code_only
                     titleId:(uint32_t)title_id
                    lastSeen:(NSDate*)last_seen {
  if ((self = [super initWithXuid:xuid gamertag:gamertag relation:relation codeOnly:code_only])) {
    title_id_ = title_id;
    last_seen_ = [last_seen copy];
  }
  return self;
}

- (void)dealloc {
  [last_seen_ release];
  [super dealloc];
}

@end

@interface XeniaLiveBlockedPlayer ()
- (instancetype)initWithXuid:(uint64_t)xuid gamertag:(NSString*)gamertag;
@end

@implementation XeniaLiveBlockedPlayer {
  NSString* gamertag_;
}
@synthesize xuid = xuid_;

- (instancetype)initWithXuid:(uint64_t)xuid gamertag:(NSString*)gamertag {
  if ((self = [super init])) {
    xuid_ = xuid;
    gamertag_ = [gamertag copy];
  }
  return self;
}

- (void)dealloc {
  [gamertag_ release];
  [super dealloc];
}

- (NSString*)gamertag {
  return gamertag_;
}

@end

@interface XeniaLivePartyMember ()
- (instancetype)initWithXuid:(uint64_t)xuid
                    gamertag:(NSString*)gamertag
                      leader:(BOOL)leader
                          me:(BOOL)me;
@end

@implementation XeniaLivePartyMember {
  NSString* gamertag_;
}
@synthesize xuid = xuid_;
@synthesize leader = leader_;
@synthesize me = me_;

- (instancetype)initWithXuid:(uint64_t)xuid
                    gamertag:(NSString*)gamertag
                      leader:(BOOL)leader
                          me:(BOOL)me {
  if ((self = [super init])) {
    xuid_ = xuid;
    gamertag_ = [gamertag copy];
    leader_ = leader;
    me_ = me;
  }
  return self;
}

- (void)dealloc {
  [gamertag_ release];
  [super dealloc];
}

- (NSString*)gamertag {
  return gamertag_;
}

@end

#pragma mark - Bridge

@interface XeniaLiveSocial ()
- (void)refresh;
- (void)holdLaunchInvite:(const xe::kernel::xam::LaunchInvite&)launch;
- (std::shared_ptr<live::AccountShare>)currentShare;
@end

@implementation XeniaLiveSocial {
  std::shared_ptr<live::AccountShare> share_;
  // What the share was opened for.
  std::string lobby_;
  uint64_t profile_xuid_;
  uint64_t online_xuid_;
  XeniaLiveSocialState state_;
  NSString* status_text_;
  NSString* gamertag_;
  // Where the account signed in instead, for the profile it signed out (0 for none).
  NSString* elsewhere_message_;
  uint64_t elsewhere_profile_;
  // When each invite came, by ID (the lobby doesn't say).
  NSMutableDictionary<NSString*, NSDate*>* received_;
  // Party invites waiting for an answer, oldest first. The account client
  // keeps game invites only (a running title's kernel holds the party's), so
  // the library keeps these. Main thread.
  std::vector<live::Invite> party_invites_;
  // Presence the bridge last set: the library (no title).
  BOOL presence_at_library_;
  BOOL change_posted_;
  // Recent Players of |recent_profile_xuid_|, as in its file (dictionaries, newest first),
  // loaded when first needed. Main thread.
  NSMutableArray<NSMutableDictionary*>* recent_;
  uint64_t recent_profile_xuid_;
  // Players whose friend request from this profile was refused: they take requests by code only.
  std::unordered_set<uint64_t> code_only_;
  // When the profiles became unreadable with the connection open, or 0.
  CFAbsoluteTime profiles_busy_since_;
  // The last player search the service answered: what was searched for and who it found.
  std::string search_prefix_;
  std::vector<std::pair<uint64_t, std::string>> search_hits_;
  // The invite to another game the running one accepted, until the app starts that game (or
  // another invite replaces it). Main thread.
  std::optional<xe::kernel::xam::LaunchInvite> held_launch_invite_;
  NSTimer* timer_;
  uint32_t (^running_title_provider_)(void);
  NSString* (^title_name_provider_)(uint32_t);
}
@synthesize state = state_;
@synthesize statusText = status_text_;
@synthesize gamertag = gamertag_;
@synthesize runningTitleProvider = running_title_provider_;
@synthesize titleNameProvider = title_name_provider_;

+ (XeniaLiveSocial*)shared {
  static XeniaLiveSocial* shared = [[XeniaLiveSocial alloc] init];
  return shared;
}

- (instancetype)init {
  if ((self = [super init])) {
    received_ = [[NSMutableDictionary alloc] init];
    state_ = XeniaLiveSocialStateOff;
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    for (NSNotificationName name in @[
           UIApplicationDidBecomeActiveNotification, XeniaIOSSystemLinkStatusDidChangeNotification
         ]) {
      [center addObserver:self selector:@selector(refreshSoon) name:name object:nil];
    }
    // NSUserDefaults is thread safe; the hub asks on whichever thread opens a connection.
    live::SetAppearOfflineProvider([](uint64_t online_xuid) {
      return bool(
          [[NSUserDefaults standardUserDefaults] boolForKey:AppearOfflineOnlineKey(online_xuid)]);
    });
    // Players joining and leaving the running game's sessions, on the guest's threads.
    xe::kernel::SetSessionRemotePlayersListener([] {
      dispatch_async(dispatch_get_main_queue(), ^{
        XeniaLiveSocial* social = [XeniaLiveSocial shared];
        [social noteRecentPlayers];
        [social postChange];
      });
    });
    // A game joined from an invite (or by following the party) that the
    // title couldn't connect to: says why, instead of the title's bare
    // "Unable to Join".
    // The title can't join a System Link game from an invite: the player does, from the
    // game's System Link screen, once in the friend's room (the kernel moved there).
    xe::kernel::xam::SetLiveSystemLinkJoinHandler(
        [](uint32_t, const std::string& gamertag, uint32_t title_id, const std::string&) {
          NSString* name = [NSString stringWithUTF8String:gamertag.c_str()] ?: @"";
          [[XeniaLiveSocial shared] postSystemLinkJoin:name title:title_id];
        });
    // The running game accepted an invite to another (in-game, or by live_auto_accept_invites):
    // the app starts that one from the library, as the console does.
    xe::kernel::xam::SetLiveInviteTitleHandler([](uint32_t user_index, const std::string& from,
                                                  xe::kernel::xam::AcceptedInvite invite,
                                                  const std::string& room) {
      auto launch = std::make_shared<xe::kernel::xam::LaunchInvite>();
      launch->user_index = user_index;
      launch->invite = std::move(invite);
      launch->room = room;
      NSString* name = [NSString stringWithUTF8String:from.c_str()] ?: @"";
      dispatch_async(dispatch_get_main_queue(), ^{
        [[XeniaLiveSocial shared] holdLaunchInvite:*launch];
        [[NSNotificationCenter defaultCenter]
            postNotificationName:XeniaLiveSocialInviteForOtherGameNotification
                          object:[XeniaLiveSocial shared]
                        userInfo:@{
                          XeniaLiveSocialTitleKey : @(launch->invite.title_id),
                          XeniaLiveSocialGamertagKey : name
                        }];
      });
    });
    xe::kernel::xam::SetLiveJoinFailedHandler(
        [](uint32_t, const std::string& gamertag, xe::kernel::xam::LiveJoinFailure reason) {
          NSString* text = JoinFailedText(gamertag, reason);
          dispatch_async(dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter]
                postNotificationName:XeniaLiveSocialJoinFailedNotification
                              object:[XeniaLiveSocial shared]
                            userInfo:@{XeniaLiveSocialMessageKey : text}];
          });
        });
  }
  return self;
}

- (void)start {
  if (timer_) {
    return;
  }
  // Profiles come and go, and Online Features may change in Settings: look again now and then.
  __unsafe_unretained XeniaLiveSocial* unsafe_self = self;
  timer_ = [[NSTimer timerWithTimeInterval:5.0
                                   repeats:YES
                                     block:^(NSTimer* __unused timer) {
                                       [unsafe_self refresh];
                                     }] retain];
  timer_.tolerance = 2.0;
  [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
  [self refresh];
}

- (void)refreshSoon {
  dispatch_async(dispatch_get_main_queue(), ^{
    [self refresh];
  });
}

// One change notification per run loop turn, however many events arrive.
- (void)postChange {
  if (change_posted_) {
    return;
  }
  change_posted_ = YES;
  dispatch_async(dispatch_get_main_queue(), ^{
    change_posted_ = NO;
    [[NSNotificationCenter defaultCenter] postNotificationName:XeniaLiveSocialDidChangeNotification
                                                        object:self];
  });
}

- (void)setState:(XeniaLiveSocialState)state text:(NSString*)text {
  if (state == state_ && (text == status_text_ || [text isEqualToString:status_text_])) {
    return;
  }
  state_ = state;
  [status_text_ release];
  status_text_ = [text copy];
  [self postChange];
}

// Lets go of the connection off the main thread: closing it waits for its thread.
- (void)releaseShare {
  if (!share_) {
    return;
  }
  auto* doomed = new std::shared_ptr<live::AccountShare>(std::move(share_));
  share_.reset();
  party_invites_.clear();
  lobby_.clear();
  online_xuid_ = 0;
  profile_xuid_ = 0;
  presence_at_library_ = NO;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    delete doomed;
  });
  [self postChange];
}

- (void)refresh {
  if (!g_host) {
    return;
  }
  // Quitting a game rebuilds the kernel, and for a moment no profile is signed in. Dropping the
  // connection then would sign out, empty Friends and Online, and sign in again.
  // A launch that failed can leave them unreadable for good: hold on for a while only.
  if (share_ && g_host->profiles_busy && g_host->profiles_busy()) {
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (!profiles_busy_since_) {
      profiles_busy_since_ = now;
    }
    if (now - profiles_busy_since_ < 60.0) {
      return;
    }
  } else {
    profiles_busy_since_ = 0;
  }
  const std::string lobby = xe::kernel::xam::LiveSessionLobby();
  std::optional<XeniaIOSLiveProfile> profile;
  if (g_host->active_profile) {
    profile = g_host->active_profile();
  }
  NSString* gamertag = profile ? ToNS(profile->gamertag) : nil;
  if (!(gamertag == gamertag_ || [gamertag isEqualToString:gamertag_])) {
    [gamertag_ release];
    gamertag_ = [gamertag copy];
    [self postChange];
  }

  if (!xe_ios_online_features_enabled() || lobby.empty()) {
    [self releaseShare];
    xe_ios_background_refresh_set_account(nil, 0, nil, nil);
    [self setState:XeniaLiveSocialStateOff
              text:xe_ios_online_features_enabled()
                       ? NSLocalizedString(@"Friends need System Link over the internet. Turn "
                                           @"it on in Settings.",
                                           @"Friends status: System Link isn't on the relay")
                       : NSLocalizedString(@"Friends need Online Features. Turn them on in "
                                           @"Settings.",
                                           @"Friends status: Online Features are off")];
    return;
  }
  if (!profile) {
    [self releaseShare];
    xe_ios_background_refresh_set_account(nil, 0, nil, nil);
    [self setState:XeniaLiveSocialStateNoProfile
              text:NSLocalizedString(@"Sign in to a profile to see your friends.",
                                     @"Friends status: no profile is signed in")];
    return;
  }
  if (!profile->live_enabled || !profile->online_xuid) {
    [self releaseShare];
    xe_ios_background_refresh_set_account(nil, 0, nil, nil);
    if (self.signedInElsewhere) {
      [self setState:XeniaLiveSocialStateProfileOffline text:elsewhere_message_];
      return;
    }
    [self setState:XeniaLiveSocialStateProfileOffline
              text:[NSString stringWithFormat:NSLocalizedString(
                                                  @"Turn on online play for %@ to add friends "
                                                  @"and get game invites. Friends see your "
                                                  @"gamertag and which game you're playing.",
                                                  @"Friends status; %@ is the gamertag"),
                                              gamertag]];
    return;
  }

  if (share_ && (lobby_ != lobby || profile_xuid_ != profile->xuid ||
                 (online_xuid_ != profile->online_xuid &&
                  (*share_)->xuid() != profile->online_xuid))) {
    [self releaseShare];
  }
  if (!share_) {
    [self openShareOn:lobby profile:*profile];
  }
  if (!share_) {
    [self setState:XeniaLiveSocialStateProblem
              text:NSLocalizedString(@"XeniOS couldn't make this device's online key.",
                                     @"Friends status: the device key couldn't be written")];
    return;
  }

  // At the library, friends see the profile online and in no game. A game sets its own.
  const uint32_t running = running_title_provider_ ? running_title_provider_() : 0;
  if (!running && !presence_at_library_) {
    presence_at_library_ = YES;
    (*share_)->SetPresence(0, 0, {}, std::nullopt);
  } else if (running) {
    presence_at_library_ = NO;
  }

  // Players still in the game stay recent: their last-seen time moves on.
  if (running) {
    [self noteRecentPlayers];
  }

  const auto& client = *share_;
  if (client->signed_in()) {
    [self setState:XeniaLiveSocialStateOnline text:nil];
  } else {
    const std::string problem = client->problem();
    if (problem.empty()) {
      [self setState:XeniaLiveSocialStateConnecting
                text:NSLocalizedString(@"Connecting…", @"Friends status: signing in")];
    } else {
      [self setState:XeniaLiveSocialStateProblem text:ProblemText(problem)];
    }
  }
}

- (void)openShareOn:(const std::string&)lobby profile:(const XeniaIOSLiveProfile&)profile {
  live::AccountClient::Events events;
  const uint64_t profile_xuid = profile.xuid;
  const uint64_t asked_xuid = profile.online_xuid;
  events.signed_in = [profile_xuid, asked_xuid](uint64_t xuid, const std::string& friend_code) {
    // A copy: the block would capture the reference, gone once this returns.
    const std::string code = friend_code;
    dispatch_async(dispatch_get_main_queue(), ^{
      XELOGI("Live account (app): online, friend code {}", code);
      if (xuid != asked_xuid && g_host && g_host->set_online_xuid) {
        g_host->set_online_xuid(profile_xuid, xuid);
      }
      [[XeniaLiveSocial shared] refresh];
      [[XeniaLiveSocial shared] keepAccountForBackground];
      [[XeniaLiveSocial shared] postChange];
    });
  };
  auto changed = [] {
    dispatch_async(dispatch_get_main_queue(), ^{
      [[XeniaLiveSocial shared] refresh];
      [[XeniaLiveSocial shared] noteSeenForBackground];
      [[XeniaLiveSocial shared] postChange];
    });
  };
  events.connection_changed = changed;
  events.friends_changed = changed;
  events.blocked_changed = changed;
  events.presence_changed = [changed](const live::Presence&) { changed(); };
  events.invite_answered = [](const std::string& id) {
    const std::string copy = id;
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveSocial* social = [XeniaLiveSocial shared];
      [social forgetPartyInvite:copy];
      [social refresh];
      [social postChange];
    });
  };
  events.party_changed = changed;
  events.settings_changed = changed;
  events.names = [](const std::vector<std::pair<uint64_t, std::string>>& names) {
    const std::vector<std::pair<uint64_t, std::string>> copy = names;
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveSocial* social = [XeniaLiveSocial shared];
      [social namesHeard:copy];
      [social postChange];
    });
  };
  events.search_results = [](const std::string& prefix,
                              const std::vector<std::pair<uint64_t, std::string>>& users) {
    XELOGI("Live account (app): player search found {}", users.size());
    const std::string prefix_copy = prefix;
    const std::vector<std::pair<uint64_t, std::string>> copy = users;
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveSocial* social = [XeniaLiveSocial shared];
      [social searchHeard:prefix_copy users:copy];
      [social postChange];
    });
  };
  events.friend_add_refused = [](uint64_t xuid, const std::string& gamertag,
                                 const std::string& reason) {
    XELOGI("Live account (app): friend request to {:016X} refused ({})", xuid, reason);
    NSString* text = CodeOnlyText(gamertag);
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveSocial* social = [XeniaLiveSocial shared];
      [social noteCodeOnly:xuid];
      [social postChange];
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaLiveSocialRequestFailedNotification
                        object:social
                      userInfo:@{
                        XeniaLiveSocialMessageKey : text,
                        XeniaLiveSocialRequestKey : @"friend_add",
                        XeniaLiveSocialPlayerKey : @(xuid),
                      }];
    });
  };
  events.invited = [](const live::Invite& invite) {
    const live::Invite copy = invite;
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveSocial* social = [XeniaLiveSocial shared];
      // Nothing from a blocked player is shown, even an invite sent before the block.
      if ([social isBlocked:copy.from]) {
        return;
      }
      if (copy.party) {
        XELOGI("Live account (app): party invite {} from {} (party {})", copy.id, copy.gamertag,
               copy.room);
        [social holdPartyInvite:copy];
      }
      [social noteSeenForBackground];
      [social postChange];
      XeniaLiveInvite* model = [[[XeniaLiveInvite alloc] initWithInvite:copy
                                                               received:[social receivedFor:copy]]
          autorelease];
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaLiveSocialInviteReceivedNotification
                        object:model];
    });
  };
  events.request_failed = [](const std::string& request, const std::string& reason) {
    // The account screens say these themselves.
    if (xe_ios_live_account_real_handles(request)) {
      return;
    }
    NSString* text = RequestFailedText(request, reason);
    NSString* request_name = ToNS(request);
    dispatch_async(dispatch_get_main_queue(), ^{
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaLiveSocialRequestFailedNotification
                        object:[XeniaLiveSocial shared]
                      userInfo:@{
                        XeniaLiveSocialMessageKey : text,
                        XeniaLiveSocialRequestKey : request_name,
                      }];
    });
  };
  events.reported = [](const std::string& id, uint64_t xuid) {
    XELOGI("Live account (app): report {} of {:016X} sent", id, xuid);
    dispatch_async(dispatch_get_main_queue(), ^{
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaLiveSocialNoticeNotification
                        object:[XeniaLiveSocial shared]
                      userInfo:@{
                        XeniaLiveSocialMessageKey :
                            NSLocalizedString(@"Report sent. Thanks.",
                                              @"Report toast: the service stored the report"),
                      }];
    });
  };
  events.report_failed = [](uint64_t xuid, const std::string& reason) {
    XELOGW("Live account (app): the report of {:016X} was refused ({})", xuid, reason);
    NSString* text = ReportFailedText(reason);
    dispatch_async(dispatch_get_main_queue(), ^{
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaLiveSocialReportFailedNotification
                        object:[XeniaLiveSocial shared]
                      userInfo:@{XeniaLiveSocialMessageKey : text}];
    });
  };
  xe_ios_live_account_real_add_events(events);
  share_ = live::ShareAccount(lobby, g_host->storage_root / "live" / "device_key",
                              profile.online_xuid, profile.gamertag, std::move(events));
  if (share_) {
    // Before the lobby's challenge comes back, so the sign-in itself appears offline; a share
    // that joined a connection open already (a title's) sends it as presence.
    const BOOL appear_offline = [self appearOfflineForProfile:profile.xuid];
    (*share_)->SetAppearOffline(appear_offline);
    KeepAppearOfflineForHub(profile.online_xuid, appear_offline);
    lobby_ = lobby;
    profile_xuid_ = profile.xuid;
    online_xuid_ = profile.online_xuid;
    presence_at_library_ = NO;
    [self keepAccountForBackground];
    XELOGI("Live account (app): keeping {} online", profile.gamertag);
  }
}

// The account a closed app checks for requests and invites (the lobby may have moved it to
// another XUID since the share opened).
- (void)keepAccountForBackground {
  if (!share_ || !g_host) {
    return;
  }
  const std::filesystem::path key_path = g_host->storage_root / "live" / "device_key";
  xe_ios_background_refresh_set_account(ToNS(lobby_), (*share_)->xuid(), gamertag_,
                                        ToNS(key_path.string()));
}

// Requests and invites seen while open aren't notified later.
- (void)noteSeenForBackground {
  if (share_ && [UIApplication sharedApplication].applicationState == UIApplicationStateActive) {
    xe_ios_background_refresh_note_seen((*share_)->Friends(), (*share_)->Invites());
  }
}

- (void)holdPartyInvite:(const live::Invite&)invite {
  // The newest from each friend replaces an earlier one.
  std::erase_if(party_invites_, [&](const live::Invite& held) {
    return held.id == invite.id || held.from == invite.from;
  });
  party_invites_.push_back(invite);
}

- (void)forgetPartyInvite:(const std::string&)invite_id {
  std::erase_if(party_invites_,
                [&](const live::Invite& held) { return held.id == invite_id; });
}

// The party invites still waiting, oldest first (expired ones dropped).
- (const std::vector<live::Invite>&)livePartyInvites {
  NSDate* now = [NSDate date];
  std::erase_if(party_invites_, [&](const live::Invite& held) {
    return [now timeIntervalSinceDate:[self receivedFor:held]] > live::kInviteLifetimeSeconds;
  });
  return party_invites_;
}

- (NSDate*)receivedFor:(const live::Invite&)invite {
  NSString* key = ToNS(invite.id);
  NSDate* when = received_[key];
  if (!when) {
    when = [NSDate date];
    received_[key] = when;
  }
  return when;
}

#pragma mark Reading

- (std::shared_ptr<live::AccountShare>)currentShare {
  return share_;
}

- (NSString*)friendCode {
  if (!share_) {
    return nil;
  }
  const std::string code = (*share_)->friend_code();
  return code.empty() ? nil : ToNS(code);
}

// Its welcome has no friend-request setting on a lobby too old to have pages: the two came
// together.
- (NSURL*)shareWebRoot {
  if (!share_ || !(*share_)->FriendRequestsSetting()) {
    return nil;
  }
  NSURLComponents* lobby = [NSURLComponents componentsWithString:ToNS(lobby_)];
  NSString* scheme = lobby.scheme.lowercaseString;
  if (!lobby.host.length || !([scheme isEqualToString:@"wss"] || [scheme isEqualToString:@"ws"])) {
    return nil;
  }
  NSURLComponents* root = [[[NSURLComponents alloc] init] autorelease];
  root.scheme = [scheme isEqualToString:@"wss"] ? @"https" : @"http";
  root.host = lobby.host;
  root.port = lobby.port;
  // Links name the service's own domain, also from a config that kept the Workers address.
  NSURLComponents* workers =
      [NSURLComponents componentsWithString:ToNS(std::string(xe::net::relay::kWorkersLobby))];
  NSURLComponents* public_lobby =
      [NSURLComponents componentsWithString:ToNS(std::string(xe::net::relay::kDefaultLobby))];
  if ([lobby.host caseInsensitiveCompare:workers.host] == NSOrderedSame) {
    root.scheme = @"https";
    root.host = public_lobby.host;
    root.port = nil;
  }
  return root.URL;
}

- (NSURL*)friendShareURL {
  NSString* code = xe_ios_normalize_friend_code(self.friendCode ?: @"");
  if (!code) {
    return nil;
  }
  NSURL* root = [self shareWebRoot];
  return root ? [root URLByAppendingPathComponent:[@"f/" stringByAppendingString:code]]
              : [NSURL URLWithString:xe_ios_friend_link(code)];
}

- (NSURL*)joinShareURL {
  const uint32_t running = running_title_provider_ ? running_title_provider_() : 0;
  const auto joinable = running ? xe::kernel::xam::LiveOwnJoinable(0) : std::nullopt;
  if (!joinable || joinable->room.empty() || !self.shareWebRoot) {
    return nil;
  }
  return xe_ios_system_link_share_url(running, ToNS(joinable->room));
}

- (NSArray<XeniaLiveFriend*>*)peersInState:(XeniaLiveFriendState)state {
  NSMutableArray* result = [NSMutableArray array];
  if (state == XeniaLiveFriendStateFriend) {
    AddPreviewFriends(result);
  }
  if (!share_) {
    return result;
  }
  const auto& client = *share_;
  for (const auto& f : client->Friends()) {
    // The lobby drops a blocked player's friendship or request; this covers the moment between.
    if (client->IsBlocked(f.xuid)) {
      continue;
    }
    XeniaLiveFriend* peer =
        [[[XeniaLiveFriend alloc] initWithFriend:f
                                        presence:f.state == live::Friend::State::kFriend
                                                     ? client->FriendPresence(f.xuid)
                                                     : std::nullopt] autorelease];
    if (peer.state == state) {
      [result addObject:peer];
    }
  }
  [result sortUsingComparator:^NSComparisonResult(XeniaLiveFriend* a, XeniaLiveFriend* b) {
    // Joinable, then in a game, then online, then the rest; by name within.
    auto rank = [](XeniaLiveFriend* p) {
      return p.joinable ? 0 : p.titleId ? 1 : p.online ? 2 : 3;
    };
    if (rank(a) != rank(b)) {
      return rank(a) < rank(b) ? NSOrderedAscending : NSOrderedDescending;
    }
    return [a.gamertag localizedCaseInsensitiveCompare:b.gamertag];
  }];
  return result;
}

- (NSArray<XeniaLiveFriend*>*)friends {
  return [self peersInState:XeniaLiveFriendStateFriend];
}
- (NSArray<XeniaLiveFriend*>*)incomingRequests {
  return [self peersInState:XeniaLiveFriendStateIncoming];
}
- (NSArray<XeniaLiveFriend*>*)outgoingRequests {
  return [self peersInState:XeniaLiveFriendStateOutgoing];
}

- (XeniaLiveFriend*)friendWithXuid:(uint64_t)xuid {
  for (XeniaLiveFriend* peer in [self friends]) {
    if (peer.xuid == xuid) {
      return peer;
    }
  }
  return nil;
}

- (NSArray<XeniaLiveInvite*>*)invites {
  NSMutableArray* result = [NSMutableArray array];
  if (!share_) {
    return result;
  }
  auto invites = (*share_)->Invites();
  const auto& party = [self livePartyInvites];
  invites.insert(invites.end(), party.begin(), party.end());
  for (auto it = invites.rbegin(); it != invites.rend(); ++it) {
    // One sent before the block is still held until it expires; the lobby refuses answers to it.
    if ((*share_)->IsBlocked(it->from)) {
      continue;
    }
    [result addObject:[[[XeniaLiveInvite alloc] initWithInvite:*it
                                                      received:[self receivedFor:*it]]
                          autorelease]];
  }
  return result;
}

- (NSInteger)attentionCount {
  if (!share_) {
    return 0;
  }
  NSInteger count = (NSInteger)self.invites.count;
  for (const auto& f : (*share_)->Friends()) {
    count += f.state == live::Friend::State::kIncoming && !(*share_)->IsBlocked(f.xuid);
  }
  return count;
}

- (NSInteger)onlineFriendCount {
  NSInteger count = 0;
  if (PreviewFriendCount()) {
    NSMutableArray* preview = [NSMutableArray array];
    AddPreviewFriends(preview);
    for (XeniaLiveFriend* peer in preview) {
      count += peer.online;
    }
  }
  if (!share_) {
    return count;
  }
  for (const auto& f : (*share_)->Friends()) {
    if (f.state == live::Friend::State::kFriend && !(*share_)->IsBlocked(f.xuid)) {
      auto presence = (*share_)->FriendPresence(f.xuid);
      count += presence && presence->online;
    }
  }
  return count;
}

- (NSArray<XeniaLivePartyMember*>*)partyMembers {
  NSMutableArray* result = [NSMutableArray array];
  if (!share_) {
    return result;
  }
  auto party = (*share_)->Party();
  if (!party) {
    return result;
  }
  const uint64_t me = (*share_)->xuid();
  for (const auto& [xuid, gamertag] : party->members) {
    XeniaLivePartyMember* member =
        [[[XeniaLivePartyMember alloc] initWithXuid:xuid
                                           gamertag:ToNS(gamertag)
                                             leader:xuid == party->leader
                                                 me:xuid == me] autorelease];
    if (member.leader) {
      [result insertObject:member atIndex:0];
    } else {
      [result addObject:member];
    }
  }
  return result;
}

// What this profile knows of other players: the friend list (requests too), then the party.
- (std::unordered_map<uint64_t, std::pair<std::string, XeniaLiveGamePlayerRelation>>)knownPlayers {
  std::unordered_map<uint64_t, std::pair<std::string, XeniaLiveGamePlayerRelation>> known;
  if (!share_) {
    return known;
  }
  for (const auto& f : (*share_)->Friends()) {
    XeniaLiveGamePlayerRelation relation = XeniaLiveGamePlayerRelationFriend;
    if (f.state == live::Friend::State::kIncoming) {
      relation = XeniaLiveGamePlayerRelationRequestReceived;
    } else if (f.state == live::Friend::State::kOutgoing) {
      relation = XeniaLiveGamePlayerRelationRequestSent;
    }
    known[f.xuid] = {f.gamertag, relation};
  }
  if (auto party = (*share_)->Party()) {
    for (const auto& [xuid, gamertag] : party->members) {
      known.try_emplace(xuid, gamertag, XeniaLiveGamePlayerRelationNone);
    }
  }
  return known;
}

// A player's gamertag: what the friend list or party says, else a name the service gave.
- (NSString*)gamertagFor:(uint64_t)xuid
                   known:(const std::unordered_map<
                             uint64_t, std::pair<std::string, XeniaLiveGamePlayerRelation>>&)known {
  auto it = known.find(xuid);
  if (it != known.end() && !it->second.first.empty()) {
    return ToNS(it->second.first);
  }
  const std::string name = share_ ? (*share_)->GamertagOf(xuid) : std::string();
  return name.empty() ? nil : ToNS(name);
}

- (NSArray<XeniaLiveGamePlayer*>*)gamePlayers {
  NSMutableArray<XeniaLiveGamePlayer*>* result = [NSMutableArray array];
  if (!share_ || self.state != XeniaLiveSocialStateOnline) {
    return result;
  }
  const uint64_t me = (*share_)->xuid();
  // Gamertags are one account's each: a player with this account's is the player, on another
  // device or profile, never a friend to list.
  NSString* my_gamertag = ToNS((*share_)->gamertag());
  const auto known = [self knownPlayers];
  std::vector<uint64_t> unnamed;
  std::vector<uint64_t> session_players = xe::kernel::SessionRemotePlayers();
  for (uint64_t xuid : session_players) {
    // Only accounts here can be friends; a game's offline or guest players can't.
    if (xuid == me || !xe::kernel::IsOnlineXUID(xuid) || xe::kernel::IsGuestXUID(xuid) ||
        (*share_)->IsBlocked(xuid)) {
      continue;
    }
    auto it = known.find(xuid);
    NSString* gamertag = [self gamertagFor:xuid known:known];
    if (gamertag && my_gamertag.length &&
        [gamertag caseInsensitiveCompare:my_gamertag] == NSOrderedSame) {
      continue;
    }
    if (!gamertag) {
      unnamed.push_back(xuid);
    }
    XeniaLiveGamePlayerRelation relation =
        it != known.end() ? it->second.second : XeniaLiveGamePlayerRelationNone;
    [result addObject:[[[XeniaLiveGamePlayer alloc] initWithXuid:xuid
                                                        gamertag:gamertag
                                                        relation:relation
                                                        codeOnly:code_only_.count(xuid) != 0]
                          autorelease]];
  }
  // Strangers get their names from the service; the list changes again when they come.
  if (!unnamed.empty()) {
    (*share_)->RequestNames(unnamed);
  }
  [result sortUsingComparator:^NSComparisonResult(XeniaLiveGamePlayer* a, XeniaLiveGamePlayer* b) {
    return ComparePlayers(a.gamertag, a.xuid, b.gamertag, b.xuid);
  }];
  return result;
}

#pragma mark Recent players

// The profile Recent Players belong to: the connected one, else the one signed in.
- (uint64_t)recentProfileXuid {
  return share_ ? profile_xuid_ : [self activeProfileXuid];
}

// The list of |profile_xuid|, read from its file the first time.
- (NSMutableArray<NSMutableDictionary*>*)recentFor:(uint64_t)profile_xuid {
  if (recent_ && recent_profile_xuid_ == profile_xuid) {
    return recent_;
  }
  [recent_ release];
  recent_ = [[NSMutableArray alloc] init];
  recent_profile_xuid_ = profile_xuid;
  NSString* path = RecentPlayersPath(profile_xuid);
  NSData* data = path ? [NSData dataWithContentsOfFile:path] : nil;
  id parsed = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
  if ([parsed isKindOfClass:[NSArray class]]) {
    for (id entry in (NSArray*)parsed) {
      // Only what this app writes: a XUID string and a time; the rest is optional.
      if ([entry isKindOfClass:[NSDictionary class]] &&
          [entry[@"xuid"] isKindOfClass:[NSString class]] &&
          [entry[@"seen"] isKindOfClass:[NSNumber class]] &&
          recent_.count < kRecentPlayersKept) {
        [recent_ addObject:[[entry mutableCopy] autorelease]];
      }
    }
  }
  return recent_;
}

- (void)saveRecent {
  NSString* path = RecentPlayersPath(recent_profile_xuid_);
  if (!path || !recent_) {
    return;
  }
  NSData* data = [NSJSONSerialization dataWithJSONObject:recent_ options:0 error:nil];
  if (!data) {
    return;
  }
  [[NSFileManager defaultManager] createDirectoryAtPath:[path stringByDeletingLastPathComponent]
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  if (![data writeToFile:path atomically:YES]) {
    XELOGW("Live account (app): couldn't save Recent Players");
  }
}

// Puts everyone in the running game's sessions at the top of Recent Players, with the game and
// now. Writes the file only when something other than a few seconds changed.
- (void)noteRecentPlayers {
  const uint32_t running = running_title_provider_ ? running_title_provider_() : 0;
  NSArray<XeniaLiveGamePlayer*>* players = self.gamePlayers;
  const uint64_t profile_xuid = [self recentProfileXuid];
  if (!running || !players.count || !profile_xuid) {
    return;
  }
  NSMutableArray<NSMutableDictionary*>* recent = [self recentFor:profile_xuid];
  NSNumber* now = @([[NSDate date] timeIntervalSince1970]);
  NSString* title = [NSString stringWithFormat:@"%08X", running];
  BOOL changed = NO;
  for (XeniaLiveGamePlayer* player in players) {
    NSString* key = XuidKey(player.xuid);
    NSMutableDictionary* entry = nil;
    for (NSMutableDictionary* kept in recent) {
      if ([kept[@"xuid"] isEqualToString:key]) {
        entry = kept;
        break;
      }
    }
    if (!entry) {
      entry = [NSMutableDictionary dictionaryWithObject:key forKey:@"xuid"];
      [recent insertObject:entry atIndex:0];
      changed = YES;
    }
    if (![entry[@"title"] isEqual:title]) {
      entry[@"title"] = title;
      changed = YES;
    }
    if (player.gamertag.length && ![entry[@"gamertag"] isEqual:player.gamertag]) {
      entry[@"gamertag"] = player.gamertag;
      changed = YES;
    }
    if (now.doubleValue - [entry[@"seen"] doubleValue] >= kRecentSeenStep) {
      entry[@"seen"] = now;
      changed = YES;
    }
  }
  if (!changed) {
    return;
  }
  // Newest first; ties keep the order above.
  [recent sortWithOptions:NSSortStable
          usingComparator:^NSComparisonResult(NSDictionary* a, NSDictionary* b) {
            return [b[@"seen"] compare:a[@"seen"]];
          }];
  while (recent.count > kRecentPlayersKept) {
    [recent removeLastObject];
  }
  [self saveRecent];
  [self postChange];
}

- (void)searchHeard:(const std::string&)prefix
              users:(const std::vector<std::pair<uint64_t, std::string>>&)users {
  search_prefix_ = prefix;
  search_hits_ = users;
}

- (void)searchPlayers:(NSString*)prefix {
  if (share_ && self.state == XeniaLiveSocialStateOnline && prefix.length) {
    (*share_)->SearchPlayers(prefix.UTF8String);
  }
}

- (BOOL)searchAnswered:(NSString*)query {
  NSString* trimmed =
      [query stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  return share_ && self.state == XeniaLiveSocialStateOnline && trimmed.length &&
         [ToNS(search_prefix_) caseInsensitiveCompare:trimmed] == NSOrderedSame;
}

- (NSArray<XeniaLiveGamePlayer*>*)playersMatching:(NSString*)query {
  NSMutableArray<XeniaLiveGamePlayer*>* result = [NSMutableArray array];
  NSString* trimmed =
      [query stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  if (!share_ || self.state != XeniaLiveSocialStateOnline || !trimmed.length ||
      [ToNS(search_prefix_) caseInsensitiveCompare:trimmed] != NSOrderedSame) {
    return result;
  }
  const uint64_t me = (*share_)->xuid();
  const auto known = [self knownPlayers];
  for (const auto& [xuid, gamertag] : search_hits_) {
    if (xuid == me || (*share_)->IsBlocked(xuid)) {
      continue;
    }
    auto it = known.find(xuid);
    XeniaLiveGamePlayerRelation relation =
        it != known.end() ? it->second.second : XeniaLiveGamePlayerRelationNone;
    [result addObject:[[[XeniaLiveGamePlayer alloc] initWithXuid:xuid
                                                        gamertag:ToNS(gamertag)
                                                        relation:relation
                                                        codeOnly:code_only_.count(xuid) != 0]
                          autorelease]];
  }
  return result;
}

// Names the service gave: keep them for Recent Players, so the list names them offline too.
- (void)namesHeard:(const std::vector<std::pair<uint64_t, std::string>>&)names {
  const uint64_t profile_xuid = [self recentProfileXuid];
  if (!profile_xuid) {
    return;
  }
  NSMutableArray<NSMutableDictionary*>* recent = [self recentFor:profile_xuid];
  BOOL changed = NO;
  for (const auto& [xuid, gamertag] : names) {
    NSString* key = XuidKey(xuid);
    NSString* name = ToNS(gamertag);
    for (NSMutableDictionary* entry in recent) {
      if ([entry[@"xuid"] isEqualToString:key] && name.length &&
          ![entry[@"gamertag"] isEqual:name]) {
        entry[@"gamertag"] = name;
        changed = YES;
      }
    }
  }
  if (changed) {
    [self saveRecent];
  }
}

- (void)noteCodeOnly:(uint64_t)xuid {
  code_only_.insert(xuid);
}

- (NSArray<XeniaLiveRecentPlayer*>*)recentPlayers {
  NSMutableArray<XeniaLiveRecentPlayer*>* result = [NSMutableArray array];
  const uint64_t profile_xuid = [self recentProfileXuid];
  if (!profile_xuid) {
    return result;
  }
  const auto known = [self knownPlayers];
  std::vector<uint64_t> unnamed;
  for (NSDictionary* entry in [self recentFor:profile_xuid]) {
    const uint64_t xuid = strtoull([entry[@"xuid"] UTF8String], nullptr, 16);
    if (!xuid || [self isBlocked:xuid]) {
      continue;
    }
    // The name now (a friend may have renamed), else the one kept.
    NSString* gamertag = [self gamertagFor:xuid known:known];
    if (!gamertag) {
      id kept = entry[@"gamertag"];
      gamertag = [kept isKindOfClass:[NSString class]] && [kept length] ? kept : nil;
    }
    if (!gamertag) {
      unnamed.push_back(xuid);
    }
    auto it = known.find(xuid);
    id title = entry[@"title"];
    const uint32_t title_id =
        [title isKindOfClass:[NSString class]] ? uint32_t(strtoul([title UTF8String], nullptr, 16))
                                               : 0;
    [result addObject:[[[XeniaLiveRecentPlayer alloc]
                          initWithXuid:xuid
                              gamertag:gamertag
                              relation:it != known.end() ? it->second.second
                                                         : XeniaLiveGamePlayerRelationNone
                              codeOnly:code_only_.count(xuid) != 0
                               titleId:title_id
                              lastSeen:[NSDate dateWithTimeIntervalSince1970:[entry[@"seen"]
                                                                                 doubleValue]]]
                          autorelease]];
  }
  if (!unnamed.empty() && share_ && self.state == XeniaLiveSocialStateOnline) {
    (*share_)->RequestNames(unnamed);
  }
  return result;
}

#pragma mark Friend requests setting

- (XeniaLiveFriendRequestsSetting)friendRequestsSetting {
  if (!share_ || self.state != XeniaLiveSocialStateOnline) {
    return XeniaLiveFriendRequestsSettingUnknown;
  }
  auto setting = (*share_)->FriendRequestsSetting();
  if (!setting) {
    return XeniaLiveFriendRequestsSettingUnknown;
  }
  return *setting == live::FriendRequests::kCode ? XeniaLiveFriendRequestsSettingCode
                                                 : XeniaLiveFriendRequestsSettingAnyone;
}

- (void)setFriendRequestsSetting:(XeniaLiveFriendRequestsSetting)setting {
  if (setting == XeniaLiveFriendRequestsSettingUnknown || !share_ ||
      self.state != XeniaLiveSocialStateOnline) {
    return;
  }
  XELOGI("Live account (app): friend requests from {}",
         setting == XeniaLiveFriendRequestsSettingCode ? "friend codes only" : "anyone");
  (*share_)->SetFriendRequests(setting == XeniaLiveFriendRequestsSettingCode
                                   ? live::FriendRequests::kCode
                                   : live::FriendRequests::kAnyone);
}

- (BOOL)partyActionsAvailable {
  return share_ && self.state == XeniaLiveSocialStateOnline;
}

- (BOOL)createParty {
  if (!self.partyActionsAvailable) {
    return NO;
  }
  (*share_)->CreateParty();
  return YES;
}

- (BOOL)leaveParty {
  if (!self.partyActionsAvailable) {
    return NO;
  }
  (*share_)->LeaveParty();
  return YES;
}

- (BOOL)inviteFriendToParty:(XeniaLiveFriend*)peer {
  return [self invitePlayerToParty:peer.xuid];
}

- (BOOL)invitePlayerToParty:(uint64_t)xuid {
  if (!self.partyActionsAvailable) {
    return NO;
  }
  // The game this profile is in, or the dashboard's at the library.
  const uint32_t running = running_title_provider_ ? running_title_provider_() : 0;
  (*share_)->InviteToParty(xuid, running ? running : xe::kernel::kDashboardID);
  return YES;
}

- (BOOL)acceptPartyInvite:(XeniaLiveInvite*)invite {
  if (!share_ || !invite.party) {
    return NO;
  }
  XELOGI("Live account (app): joining party {} (invite {})", invite.raw.room, invite.raw.id);
  (*share_)->ReplyInvite(invite.raw.id, true);
  [self forgetPartyInvite:invite.raw.id];
  [self postChange];
  return YES;
}

- (NSString*)nameForTitle:(uint32_t)title_id {
  NSString* name = title_name_provider_ ? title_name_provider_(title_id) : nil;
  return name.length ? name
                     : NSLocalizedString(@"a game that isn't in your library",
                                         @"Friends: the name of a game the library doesn't have");
}

- (BOOL)runningGameOnline {
  const uint32_t running = running_title_provider_ ? running_title_provider_() : 0;
  return running && xe::kernel::xam::LiveAccount(0) != nullptr;
}

// Same game session, both published in presence: playing the same game isn't enough.
- (BOOL)friendInYourGame:(XeniaLiveFriend*)peer {
  if (!peer.online || !peer.sessionId.length) {
    return NO;
  }
  const auto own = xe::kernel::xam::LiveOwnJoinable(0);
  if (!own || own->session_id.empty()) {
    return NO;
  }
  return [ToNS(own->session_id) caseInsensitiveCompare:peer.sessionId] == NSOrderedSame;
}

- (NSString*)playersWithYouText {
  NSMutableOrderedSet<NSString*>* names = [NSMutableOrderedSet orderedSet];
  for (XeniaLiveFriend* peer in self.friends) {
    if ([self friendInYourGame:peer]) {
      [names addObject:peer.gamertag];
    }
  }
  return names.count ? [names.array componentsJoinedByString:@", "] : nil;
}

- (NSString*)presenceTextForFriend:(XeniaLiveFriend*)peer {
  if (!peer.online) {
    return NSLocalizedString(@"Offline", @"Friends: a friend who isn't online");
  }
  if (!peer.titleId) {
    return NSLocalizedString(@"Online", @"Friends: a friend online but in no game");
  }
  if (peer.systemLink) {
    return [NSString
        stringWithFormat:NSLocalizedString(@"In a System Link game of %@",
                                           @"Friends: %@ is the game; a friend in a System Link "
                                           @"game (no Live session), joined from the game's "
                                           @"System Link screen"),
                         [self nameForTitle:peer.titleId]];
  }
  return [NSString stringWithFormat:NSLocalizedString(@"Playing %@",
                                                      @"Friends: %@ is the game a friend is in"),
                                    [self nameForTitle:peer.titleId]];
}

// What to do to join a friend's System Link game, for a toast.
- (NSString*)systemLinkJoinText:(NSString*)gamertag title:(uint32_t)title_id {
  NSString* name = gamertag.length ? gamertag
                                   : NSLocalizedString(@"Your friend",
                                                       @"A friend whose gamertag isn't known");
  return [NSString
      stringWithFormat:NSLocalizedString(@"%1$@ is in a System Link game: open System Link (or "
                                         @"LAN) in %2$@ to join.",
                                         @"System Link join toast: %1$@ is the friend, %2$@ the "
                                         @"game"),
                       name, [self nameForTitle:title_id]];
}

- (void)postSystemLinkJoin:(NSString*)gamertag title:(uint32_t)title_id {
  NSString* text = [self systemLinkJoinText:gamertag title:title_id];
  dispatch_async(dispatch_get_main_queue(), ^{
    [[NSNotificationCenter defaultCenter]
        postNotificationName:XeniaLiveSocialSystemLinkJoinNotification
                      object:[XeniaLiveSocial shared]
                    userInfo:@{XeniaLiveSocialMessageKey : text}];
  });
}

#pragma mark Acting

- (BOOL)turnOnOnlineForProfile {
  if (!g_host || !g_host->active_profile || !g_host->enable_online) {
    return NO;
  }
  auto profile = g_host->active_profile();
  if (!profile || !g_host->enable_online(profile->xuid)) {
    return NO;
  }
  if (profile->xuid == elsewhere_profile_) {
    elsewhere_profile_ = 0;
  }
  [self refresh];
  return YES;
}

- (BOOL)signedInElsewhere {
  if (!elsewhere_profile_ || !g_host || !g_host->active_profile) {
    return NO;
  }
  auto profile = g_host->active_profile();
  return profile && profile->xuid == elsewhere_profile_;
}

- (void)noteSignedInElsewhere:(NSString*)message profile:(uint64_t)profile_xuid {
  [elsewhere_message_ release];
  elsewhere_message_ = [message copy];
  elsewhere_profile_ = profile_xuid;
  [self refresh];
}

- (void)addFriendWithCode:(NSString*)code {
  NSString* normalized = xe_ios_normalize_friend_code(code);
  if (share_ && normalized) {
    (*share_)->AddFriendByCode(normalized.UTF8String);
  }
}

- (void)acceptFriendRequest:(uint64_t)xuid {
  if (share_) {
    (*share_)->AddFriend(xuid);
  }
}

- (void)sendFriendRequestTo:(uint64_t)xuid {
  if ([self canActOnPlayer:xuid]) {
    (*share_)->AddFriend(xuid);
  }
}

- (void)declineFriendRequest:(uint64_t)xuid {
  if (share_) {
    (*share_)->DeclineFriend(xuid);
  }
}

- (void)removeFriend:(uint64_t)xuid {
  if (share_) {
    (*share_)->RemoveFriend(xuid);
  }
}

#pragma mark Blocking, reports, appearing offline

- (uint64_t)activeProfileXuid {
  if (!g_host || !g_host->active_profile) {
    return 0;
  }
  auto profile = g_host->active_profile();
  return profile ? profile->xuid : 0;
}

- (BOOL)canActOnPlayer:(uint64_t)xuid {
  return share_ && self.state == XeniaLiveSocialStateOnline && xe::kernel::IsOnlineXUID(xuid) &&
         !xe::kernel::IsGuestXUID(xuid) && xuid != (*share_)->xuid();
}

- (BOOL)playerTakesRequestsByCodeOnly:(uint64_t)xuid {
  return code_only_.count(xuid) != 0;
}

- (void)blockPlayer:(uint64_t)xuid gamertag:(NSString*)gamertag {
  if (![self canActOnPlayer:xuid]) {
    return;
  }
  if (gamertag.length) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSString* key = BlockedNamesKey(profile_xuid_);
    NSMutableDictionary* names = [[[defaults dictionaryForKey:key] mutableCopy] autorelease]
                                     ?: [NSMutableDictionary dictionary];
    names[XuidKey(xuid)] = gamertag;
    [defaults setObject:names forKey:key];
  }
  XELOGI("Live account (app): blocking {:016X}", xuid);
  (*share_)->Block(xuid);
}

- (void)unblockPlayer:(uint64_t)xuid {
  if (![self canActOnPlayer:xuid]) {
    return;
  }
  // The name stays: it names them again if they're blocked again, and is a few bytes.
  XELOGI("Live account (app): unblocking {:016X}", xuid);
  (*share_)->Unblock(xuid);
}

- (BOOL)isBlocked:(uint64_t)xuid {
  return share_ && (*share_)->IsBlocked(xuid);
}

- (NSUInteger)blockedPlayerCount {
  return share_ ? (*share_)->Blocked().size() : 0;
}

- (NSArray<XeniaLiveBlockedPlayer*>*)blockedPlayers {
  NSMutableArray<XeniaLiveBlockedPlayer*>* result = [NSMutableArray array];
  if (!share_) {
    return result;
  }
  NSDictionary* names =
      [[NSUserDefaults standardUserDefaults] dictionaryForKey:BlockedNamesKey(profile_xuid_)];
  for (uint64_t xuid : (*share_)->Blocked()) {
    id name = names[XuidKey(xuid)];
    [result addObject:[[[XeniaLiveBlockedPlayer alloc]
                          initWithXuid:xuid
                              gamertag:[name isKindOfClass:[NSString class]] ? name : nil]
                          autorelease]];
  }
  [result sortUsingComparator:^NSComparisonResult(XeniaLiveBlockedPlayer* a,
                                                  XeniaLiveBlockedPlayer* b) {
    return ComparePlayers(a.gamertag, a.xuid, b.gamertag, b.xuid);
  }];
  return result;
}

- (BOOL)reportPlayer:(uint64_t)xuid reason:(XeniaLiveReportReason)reason note:(NSString*)note {
  if (![self canActOnPlayer:xuid]) {
    return NO;
  }
  // Where it happened: the running game, and its session's room when friends could join it.
  const uint32_t running = running_title_provider_ ? running_title_provider_() : 0;
  std::string room;
  if (running) {
    if (auto joinable = xe::kernel::xam::LiveOwnJoinable(0)) {
      room = joinable->room;
    }
  }
  XELOGI("Live account (app): reporting {:016X} ({}) in {:08X}", xuid, ReportReasonName(reason),
         running);
  return (*share_)->Report(xuid, ReportReasonName(reason), note.UTF8String ?: "", running, room);
}

- (BOOL)appearOfflineForProfile:(uint64_t)profile_xuid {
  return profile_xuid &&
         [[NSUserDefaults standardUserDefaults] boolForKey:AppearOfflineKey(profile_xuid)];
}

- (BOOL)appearOffline {
  return [self appearOfflineForProfile:share_ ? profile_xuid_ : [self activeProfileXuid]];
}

- (void)setAppearOffline:(BOOL)appear_offline {
  const uint64_t profile_xuid = share_ ? profile_xuid_ : [self activeProfileXuid];
  if (!profile_xuid) {
    return;
  }
  [[NSUserDefaults standardUserDefaults] setBool:appear_offline
                                          forKey:AppearOfflineKey(profile_xuid)];
  if (share_) {
    (*share_)->SetAppearOffline(appear_offline);
    KeepAppearOfflineForHub(online_xuid_, appear_offline);
  } else if (g_host && g_host->active_profile) {
    auto profile = g_host->active_profile();
    if (profile && profile->xuid == profile_xuid) {
      KeepAppearOfflineForHub(profile->online_xuid, appear_offline);
    }
  }
  [self postChange];
}

- (void)declineInvite:(XeniaLiveInvite*)invite {
  if (share_) {
    (*share_)->ReplyInvite(invite.raw.id, false);
    [self forgetPartyInvite:invite.raw.id];
    [self postChange];
  }
}

// The session to join: the invite's, or else what the inviter's presence shows.
- (std::vector<uint8_t>)sessionForInvite:(const live::Invite&)invite {
  if (invite.info.size() == sizeof(xe::kernel::XSESSION_INFO)) {
    return invite.info;
  }
  if (share_) {
    auto presence = (*share_)->FriendPresence(invite.from);
    if (presence && presence->joinable && presence->title_id == invite.title_id) {
      return presence->joinable->info;
    }
  }
  return {};
}

- (void)acceptInviteForLaunch:(XeniaLiveInvite*)invite {
  if (!share_) {
    return;
  }
  const live::Invite& raw = invite.raw;
  xe::kernel::xam::AcceptedInvite accepted;
  accepted.invitee = (*share_)->xuid();
  accepted.inviter = raw.from;
  accepted.title_id = raw.title_id;
  accepted.info = [self sessionForInvite:raw];
  (*share_)->ReplyInvite(raw.id, true);
  if (accepted.info.empty()) {
    // A System Link game: the launch goes to the friend's room, and the player joins from the
    // game's System Link screen; the title would find no invite to read.
    xe_ios_join_system_link_room(raw.title_id, ToNS([self roomForInvite:raw]));
    [self postSystemLinkJoin:ToNS(raw.gamertag) title:raw.title_id];
  } else {
    xe::kernel::xam::SetAcceptedInvite(0, std::move(accepted));
    xe_ios_join_system_link_room(raw.title_id, ToNS(raw.room));
  }
  [self postChange];
}

- (void)holdLaunchInvite:(const xe::kernel::xam::LaunchInvite&)launch {
  held_launch_invite_ = launch;
}

- (void)acceptHeldInviteForLaunch {
  if (!held_launch_invite_) {
    return;
  }
  auto launch = std::move(*held_launch_invite_);
  held_launch_invite_.reset();
  const uint32_t title_id = launch.invite.title_id;
  // Answered already, by the game that accepted it.
  if (launch.invite.info.empty()) {
    xe_ios_join_system_link_room(title_id, ToNS(launch.room));
    const auto presence =
        share_ ? (*share_)->FriendPresence(launch.invite.inviter) : std::nullopt;
    [self postSystemLinkJoin:presence ? ToNS(presence->gamertag) : @"" title:title_id];
  } else {
    xe::kernel::xam::SetAcceptedInvite(launch.user_index, std::move(launch.invite));
    xe_ios_join_system_link_room(title_id, ToNS(launch.room));
  }
  [self postChange];
}

// Where the inviter plays now: their presence's room, else the invite's.
- (std::string)roomForInvite:(const live::Invite&)invite {
  if (share_) {
    auto presence = (*share_)->FriendPresence(invite.from);
    if (presence && presence->joinable && presence->title_id == invite.title_id) {
      return presence->joinable->room;
    }
  }
  return invite.room;
}

- (BOOL)acceptInviteInRunningGame:(XeniaLiveInvite*)invite {
  if (!share_) {
    return NO;
  }
  live::Invite raw = invite.raw;
  if (raw.info.size() != sizeof(xe::kernel::XSESSION_INFO)) {
    raw.info = [self sessionForInvite:raw];
  }
  // Answers the invite on the title's account, which is this one: answering
  // it again here would find it gone ("That invite has expired").
  if (!xe::kernel::xam::AcceptLiveInvite(0, raw)) {
    return NO;
  }
  [self postChange];
  return YES;
}

- (BOOL)fillJoin:(XeniaLiveFriend*)peer
        accepted:(xe::kernel::xam::AcceptedInvite*)accepted
            room:(std::string*)room {
  if (!share_ || !peer.joinable) {
    return NO;
  }
  auto presence = (*share_)->FriendPresence(peer.xuid);
  if (!presence || !presence->joinable || !presence->title_id) {
    return NO;
  }
  accepted->invitee = (*share_)->xuid();
  accepted->inviter = peer.xuid;
  accepted->title_id = presence->title_id;
  accepted->info = presence->joinable->info;
  accepted->from_game_invite = false;
  *room = presence->joinable->room;
  return YES;
}

- (BOOL)joinFriendForLaunch:(XeniaLiveFriend*)peer {
  xe::kernel::xam::AcceptedInvite accepted;
  std::string room;
  if (![self fillJoin:peer accepted:&accepted room:&room]) {
    return NO;
  }
  const uint32_t title_id = accepted.title_id;
  if (accepted.info.empty()) {
    [self postSystemLinkJoin:peer.gamertag title:title_id];
  } else {
    xe::kernel::xam::SetAcceptedInvite(0, std::move(accepted));
  }
  xe_ios_join_system_link_room(title_id, ToNS(room));
  return YES;
}

- (BOOL)joinFriendInRunningGame:(XeniaLiveFriend*)peer {
  xe::kernel::xam::AcceptedInvite accepted;
  std::string room;
  if (![self fillJoin:peer accepted:&accepted room:&room]) {
    return NO;
  }
  // The same path as an invite the title hears about while it runs.
  live::Invite invite;
  invite.from = peer.xuid;
  invite.gamertag = peer.gamertag.UTF8String;
  invite.title_id = accepted.title_id;
  invite.room = room;
  invite.info = accepted.info;
  if (!xe::kernel::xam::AcceptLiveInvite(0, invite)) {
    return NO;
  }
  return YES;
}

- (BOOL)inviteFriendToRunningGame:(XeniaLiveFriend*)peer {
  return [self invitePlayerToRunningGame:peer.xuid];
}

- (BOOL)invitePlayerToRunningGame:(uint64_t)xuid {
  const uint32_t running = running_title_provider_ ? running_title_provider_() : 0;
  if (!share_ || !running) {
    return NO;
  }
  // The game's session when it has one friends may join; else the title's public lobby, and
  // the friend finds the session from presence.
  auto joinable = xe::kernel::xam::LiveOwnJoinable(0);
  (*share_)->SendInvite(xuid, running, joinable.value_or(live::Joinable{}), {});
  XELOGI("Live invite: the app invited {:016X} to {:08X}", xuid, running);
  return YES;
}

- (void)forgetAcceptedInvite {
  xe::kernel::xam::ClearAcceptedInvites();
}

@end

std::shared_ptr<live::AccountShare> xe_ios_live_social_current_share() {
  return [[XeniaLiveSocial shared] currentShare];
}

void xe_ios_live_social_refresh_now() { [[XeniaLiveSocial shared] refresh]; }
void xe_ios_live_social_reconnect() {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  [social releaseShare];
  [social refresh];
}

void xe_ios_live_social_install(XeniaIOSLiveSocialHost host) {
  delete g_host;
  g_host = new XeniaIOSLiveSocialHost(std::move(host));
  xe_ios_live_account_real_install(g_host);
  dispatch_async(dispatch_get_main_queue(), ^{
    [[XeniaLiveSocial shared] performSelector:@selector(start)];
  });
}

NSString* xe_ios_format_friend_code(NSString* code) {
  if (code.length != 8) {
    return code;
  }
  return [NSString
      stringWithFormat:@"%@ %@", [code substringToIndex:4], [code substringFromIndex:4]];
}

NSString* xe_ios_normalize_friend_code(NSString* text) {
  NSMutableString* code = [NSMutableString string];
  NSCharacterSet* allowed =
      [NSCharacterSet characterSetWithCharactersInString:@"0123456789ABCDEFGHJKMNPQRSTVWXYZ"];
  NSString* upper = text.uppercaseString;
  for (NSUInteger i = 0; i < upper.length; ++i) {
    unichar c = [upper characterAtIndex:i];
    if (c == ' ' || c == '-') {
      continue;
    }
    // Look-alikes the codes leave out.
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    if (![allowed characterIsMember:c]) {
      return nil;
    }
    [code appendFormat:@"%C", c];
  }
  return code.length == 8 ? code : nil;
}

NSString* xe_ios_friend_link(NSString* code) {
  return [@"xenios://friend?c=" stringByAppendingString:xe_ios_normalize_friend_code(code) ?: @""];
}

NSString* xe_ios_friend_code_from_text(NSString* text) {
  NSString* trimmed =
      [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  NSURLComponents* components = [NSURLComponents componentsWithString:trimmed];
  if (components.scheme) {
    NSString* scheme = components.scheme.lowercaseString;
    // A lobby's page: /f/CODE on any host (the code is checked, and adding still asks).
    if ([scheme isEqualToString:@"https"] || [scheme isEqualToString:@"http"]) {
      NSArray<NSString*>* parts = components.URL.pathComponents;
      if (parts.count == 3 && [parts[1] isEqualToString:@"f"]) {
        return xe_ios_normalize_friend_code(parts[2]);
      }
      return nil;
    }
    if (![scheme isEqualToString:@"xenios"] ||
        ![components.host.lowercaseString isEqualToString:@"friend"]) {
      return nil;
    }
    for (NSURLQueryItem* item in components.queryItems) {
      if ([item.name isEqualToString:@"c"]) {
        return xe_ios_normalize_friend_code(item.value ?: @"");
      }
    }
    return nil;
  }
  return xe_ios_normalize_friend_code(trimmed);
}
