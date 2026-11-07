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
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/xam_live_invites.h"
#include "xenia/kernel/xam/xam_live_social.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/net/live_account_hub.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_system_link.h"

NSNotificationName const XeniaLiveSocialDidChangeNotification =
    @"XeniaLiveSocialDidChangeNotification";
NSNotificationName const XeniaLiveSocialInviteReceivedNotification =
    @"XeniaLiveSocialInviteReceivedNotification";
NSNotificationName const XeniaLiveSocialRequestFailedNotification =
    @"XeniaLiveSocialRequestFailedNotification";
NSString* const XeniaLiveSocialMessageKey = @"message";

namespace live = xe::net::live;

namespace {

XeniaIOSLiveSocialHost* g_host = nullptr;

NSString* ToNS(const std::string& text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
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
    return NSLocalizedString(@"You can only invite friends.",
                             @"Friends toast: inviting someone who isn't a friend");
  }
  if (request == "invite_reply" && reason == "not_found") {
    return NSLocalizedString(@"That invite has expired.",
                             @"Friends toast: answering an invite that's gone");
  }
  return NSLocalizedString(@"The online service couldn't do that. Try again in a moment.",
                           @"Friends toast: any other refused request");
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
  NSString* session_id_;
}
@synthesize sessionId = session_id_;
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

@implementation XeniaLiveSocial {
  std::shared_ptr<live::AccountShare> share_;
  // What the share was opened for.
  std::string lobby_;
  uint64_t profile_xuid_;
  uint64_t online_xuid_;
  XeniaLiveSocialState state_;
  NSString* status_text_;
  NSString* gamertag_;
  // When each invite came, by ID (the lobby doesn't say).
  NSMutableDictionary<NSString*, NSDate*>* received_;
  // Party invites waiting for an answer, oldest first. The account client
  // keeps game invites only (a running title's kernel holds the party's), so
  // the library keeps these. Main thread.
  std::vector<live::Invite> party_invites_;
  // Presence the bridge last set: the library (no title).
  BOOL presence_at_library_;
  BOOL change_posted_;
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
    [self setState:XeniaLiveSocialStateNoProfile
              text:NSLocalizedString(@"Sign in to a profile to see your friends.",
                                     @"Friends status: no profile is signed in")];
    return;
  }
  if (!profile->live_enabled || !profile->online_xuid) {
    [self releaseShare];
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
      [[XeniaLiveSocial shared] postChange];
    });
  };
  auto changed = [] {
    dispatch_async(dispatch_get_main_queue(), ^{
      [[XeniaLiveSocial shared] refresh];
      [[XeniaLiveSocial shared] postChange];
    });
  };
  events.connection_changed = changed;
  events.friends_changed = changed;
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
  events.invited = [](const live::Invite& invite) {
    const live::Invite copy = invite;
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveSocial* social = [XeniaLiveSocial shared];
      if (copy.party) {
        XELOGI("Live account (app): party invite {} from {} (party {})", copy.id, copy.gamertag,
               copy.room);
        [social holdPartyInvite:copy];
      }
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
    NSString* text = RequestFailedText(request, reason);
    dispatch_async(dispatch_get_main_queue(), ^{
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaLiveSocialRequestFailedNotification
                        object:[XeniaLiveSocial shared]
                      userInfo:@{XeniaLiveSocialMessageKey : text}];
    });
  };
  share_ = live::ShareAccount(lobby, g_host->storage_root / "live" / "device_key",
                              profile.online_xuid, profile.gamertag, std::move(events));
  if (share_) {
    lobby_ = lobby;
    profile_xuid_ = profile.xuid;
    online_xuid_ = profile.online_xuid;
    presence_at_library_ = NO;
    XELOGI("Live account (app): keeping {} online", profile.gamertag);
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

- (NSString*)friendCode {
  if (!share_) {
    return nil;
  }
  const std::string code = (*share_)->friend_code();
  return code.empty() ? nil : ToNS(code);
}

- (NSArray<XeniaLiveFriend*>*)peersInState:(XeniaLiveFriendState)state {
  NSMutableArray* result = [NSMutableArray array];
  if (!share_) {
    return result;
  }
  const auto& client = *share_;
  for (const auto& f : client->Friends()) {
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
  NSInteger count =
      (NSInteger)((*share_)->Invites().size() + [self livePartyInvites].size());
  for (const auto& f : (*share_)->Friends()) {
    count += f.state == live::Friend::State::kIncoming;
  }
  return count;
}

- (NSInteger)onlineFriendCount {
  if (!share_) {
    return 0;
  }
  NSInteger count = 0;
  for (const auto& f : (*share_)->Friends()) {
    if (f.state == live::Friend::State::kFriend) {
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
  if (!self.partyActionsAvailable) {
    return NO;
  }
  // The game this profile is in, or the dashboard's at the library.
  const uint32_t running = running_title_provider_ ? running_title_provider_() : 0;
  (*share_)->InviteToParty(peer.xuid, running ? running : xe::kernel::kDashboardID);
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
  return [NSString stringWithFormat:NSLocalizedString(@"Playing %@",
                                                      @"Friends: %@ is the game a friend is in"),
                                    [self nameForTitle:peer.titleId]];
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
  [self refresh];
  return YES;
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
  xe::kernel::xam::SetAcceptedInvite(0, std::move(accepted));
  (*share_)->ReplyInvite(raw.id, true);
  xe_ios_join_system_link_room(raw.title_id, ToNS(raw.room));
  [self postChange];
}

- (BOOL)acceptInviteInRunningGame:(XeniaLiveInvite*)invite {
  if (!share_) {
    return NO;
  }
  live::Invite raw = invite.raw;
  if (raw.info.size() != sizeof(xe::kernel::XSESSION_INFO)) {
    raw.info = [self sessionForInvite:raw];
  }
  if (!xe::kernel::xam::AcceptLiveInvite(0, raw)) {
    return NO;
  }
  // The title's account is this one: answering it there answered it here too.
  (*share_)->ReplyInvite(raw.id, true);
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
  xe::kernel::xam::SetAcceptedInvite(0, std::move(accepted));
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
  const uint32_t running = running_title_provider_ ? running_title_provider_() : 0;
  if (!share_ || !running) {
    return NO;
  }
  // The game's session when it has one friends may join; else the title's public lobby, and
  // the friend finds the session from presence.
  auto joinable = xe::kernel::xam::LiveOwnJoinable(0);
  (*share_)->SendInvite(peer.xuid, running, joinable.value_or(live::Joinable{}), {});
  XELOGI("Live invite: the app invited {:016X} to {:08X}", peer.xuid, running);
  return YES;
}

- (void)forgetAcceptedInvite {
  xe::kernel::xam::ClearAcceptedInvites();
}

@end

void xe_ios_live_social_install(XeniaIOSLiveSocialHost host) {
  delete g_host;
  g_host = new XeniaIOSLiveSocialHost(std::move(host));
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
