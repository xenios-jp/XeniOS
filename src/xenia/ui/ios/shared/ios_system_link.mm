/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_system_link.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "xenia/base/cvar.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/net/nat_check.h"
#include "xenia/net/packet_transport.h"
#include "xenia/net/relay_protocol.h"
#import "xenia/ui/ios/launcher/ios_system_link_view_controller.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

DECLARE_string(systemlink_debug_nat);
DECLARE_string(systemlink_lobby);
DECLARE_string(systemlink_transport);

namespace {

using xe::net::relay::NatType;
using xe::net::relay::RelayStatus;
using State = RelayStatus::State;

// Title ID in hex to room code.
NSString* const kRoomsKey = @"XeniaSystemLinkRooms";
// The app's last check-in with the lobby: {"id": ID, "at": seconds since 1970}.
NSString* const kHereKey = @"XeniaSystemLinkHere";
// NAT checks by network: {key: {"nat": "strict", "at": seconds since 1970}}. Keys are hashes.
NSString* const kNatKey = @"XeniaSystemLinkNAT";
// Whether the lobby named a TURN server when a game last joined it.
NSString* const kLobbyTurnKey = @"XeniaSystemLinkLobbyTURN";
// Networks change their routers now and then, so a check is trusted for this long.
constexpr NSTimeInterval kNatLifetime = 30 * 24 * 60 * 60.0;
// Networks remembered, at most; the oldest go first.
constexpr NSUInteger kMaxNatNetworks = 16;

// With the online counts, below.
NSString* HereID();
void AskOnline();

void (^g_toast)(NSString*, XeniaIOSStatusToastStyle) = nil;
// What the player was last told, so each change is told once.
RelayStatus g_last;
// Whether a game plays System Link through the relay now.
bool g_active = false;
bool g_told_online = false;
bool g_told_offline = false;
bool g_told_unreachable = false;
// The room the running game asked for, and the game.
NSString* g_room_code = nil;
uint32_t g_title_id = 0;

// Rooms from invite links, for the next launch of each game only. The saved room stays as it
// was, so one stranger's link doesn't decide every later launch.
constexpr NSTimeInterval kInviteLifetime = 5 * 60.0;
std::mutex g_invite_mutex;
std::map<uint32_t, std::pair<std::string, NSTimeInterval>> g_invites;

// The invited room of |title_id|'s launch, nil if there is none or it went stale.
NSString* TakeInviteRoom(uint32_t title_id) {
  std::lock_guard lock(g_invite_mutex);
  auto found = g_invites.find(title_id);
  if (found == g_invites.end()) {
    return nil;
  }
  const auto invite = std::move(found->second);
  g_invites.erase(found);
  if ([NSProcessInfo processInfo].systemUptime - invite.second > kInviteLifetime) {
    return nil;
  }
  return [NSString stringWithUTF8String:invite.first.c_str()];
}

NSString* TitleKey(uint32_t title_id) { return [NSString stringWithFormat:@"%08X", title_id]; }

void Toast(NSString* message, XeniaIOSStatusToastStyle style) {
  if (g_toast && message.length) {
    g_toast(message, style);
  }
}

NSString* RoomDescription() {
  if (g_room_code.length) {
    return [NSString
        stringWithFormat:NSLocalizedString(@"room %@",
                                           @"System Link: names a private room; %@ is its code"),
                         g_room_code];
  }
  return NSLocalizedString(@"the public lobby", @"System Link: names the game's public room");
}

NSString* RejectMessage(xe::net::relay::Reject reason) {
  using xe::net::relay::Reject;
  switch (reason) {
    case Reject::kFull:
      return NSLocalizedString(@"This System Link room is full.",
                               @"System Link toast: the room has no space");
    case Reject::kDuplicateMac:
      return NSLocalizedString(
          @"Someone in this System Link room uses the same profile. Switch profiles to join.",
          @"System Link toast: another player has the same console identity");
    case Reject::kVersion:
      return NSLocalizedString(@"The System Link server needs a newer version of XeniOS.",
                               @"System Link toast: the relay speaks a newer protocol");
    default:
      return NSLocalizedString(@"The System Link server turned this player away.",
                               @"System Link toast: the relay refused for another reason");
  }
}

NSString* OnlineMessage(size_t players) {
  if (players == 0) {
    return [NSString
        stringWithFormat:NSLocalizedString(@"System Link is online in %@. Waiting for players.",
                                           @"System Link toast; %@ names the room"),
                         RoomDescription()];
  }
  if (players == 1) {
    return [NSString
        stringWithFormat:NSLocalizedString(@"System Link is online in %@ with one other player.",
                                           @"System Link toast; %@ names the room"),
                         RoomDescription()];
  }
  return [NSString
      stringWithFormat:NSLocalizedString(@"System Link is online in %1$@ with %2$lu other players.",
                                         @"System Link toast; %1$@ names the room, %2$lu is the "
                                         @"number of other players"),
                       RoomDescription(), (unsigned long)players];
}

NSString* VersionMismatchMessage() {
  return NSLocalizedString(@"Another player in the room runs a different version of this game. "
                           @"Games usually only see players on the same version.",
                           @"System Link toast: title versions differ");
}

XeniaIOSNatType ToIOS(NatType nat) {
  switch (nat) {
    case NatType::kOpen:
      return XeniaIOSNatType::kOpen;
    case NatType::kModerate:
      return XeniaIOSNatType::kModerate;
    case NatType::kStrict:
      return XeniaIOSNatType::kStrict;
    case NatType::kBlocked:
      return XeniaIOSNatType::kBlocked;
    case NatType::kUnknown:
      break;
  }
  return XeniaIOSNatType::kUnknown;
}

// What the debug setting fakes, or none.
std::optional<NatType> NatOverride() {
  auto nat = xe::net::relay::ParseNatType(cvars::systemlink_debug_nat);
  return nat == NatType::kUnknown ? std::nullopt : nat;
}

// Keeps a check's result for the network it was made on, across launches.
void SaveNat(NatType nat) {
  if (nat == NatType::kUnknown || NatOverride()) {
    return;
  }
  NSString* network = @(xe::net::relay::NetworkKey().c_str());
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSMutableDictionary* saved = [[[defaults dictionaryForKey:kNatKey] mutableCopy] autorelease]
                                   ?: [NSMutableDictionary dictionary];
  saved[network] = @{
    @"nat" : @(std::string(xe::net::relay::ToString(nat)).c_str()),
    @"at" : @([NSDate date].timeIntervalSince1970),
  };
  while (saved.count > kMaxNatNetworks) {
    NSString* oldest = nil;
    double oldest_at = 0.0;
    for (NSString* key in saved) {
      const double at = [saved[key][@"at"] doubleValue];
      if (!oldest || at < oldest_at) {
        oldest = key;
        oldest_at = at;
      }
    }
    [saved removeObjectForKey:oldest];
  }
  [defaults setObject:saved forKey:kNatKey];
}

// Hands the saved checks that are still fresh to System Link, which skips checking those
// networks again.
void LoadNat() {
  NSDictionary* saved = [[NSUserDefaults standardUserDefaults] dictionaryForKey:kNatKey];
  const double now = [NSDate date].timeIntervalSince1970;
  for (NSString* network in saved) {
    NSDictionary* entry = saved[network];
    if (![entry isKindOfClass:[NSDictionary class]] ||
        now - [entry[@"at"] doubleValue] > kNatLifetime) {
      continue;
    }
    NSString* name = entry[@"nat"];
    if (![name isKindOfClass:[NSString class]]) {
      continue;
    }
    if (auto nat = xe::net::relay::ParseNatType(name.UTF8String)) {
      xe::net::relay::RememberNat(network.UTF8String, *nat);
    }
  }
}

// Told once per game, as soon as the room is online: players in a room see each other's IP
// address, and a host waiting alone otherwise sees nothing.
void OnStatus(const RelayStatus& status) {
  const RelayStatus last = g_last;
  g_last = status;
  // Out of the room, the app checks in for itself again.
  if (last.app_counted && !status.app_counted) {
    dispatch_async(dispatch_get_main_queue(), ^{
      AskOnline();
    });
  }
  const bool chose_room = g_room_code.length != 0;
  const bool was_active = g_active;
  g_active = status.state != State::kStopped && status.state != State::kPaused;
  switch (status.state) {
    case State::kPaused:
    case State::kStopped:
      // Online Features went off while the game plays: say so, as the card goes away.
      if (status.state == State::kPaused && was_active) {
        Toast(NSLocalizedString(@"System Link stopped: Online Features is off.",
                                @"System Link toast: online features were turned off while a "
                                @"game was playing System Link"),
              XeniaIOSStatusToastStyleWarning);
      }
      g_last = {};
      g_told_online = false;
      g_told_offline = false;
      g_told_unreachable = false;
      return;
    case State::kOffline:
      if (chose_room && !g_told_offline) {
        g_told_offline = true;
        Toast(NSLocalizedString(@"System Link can't reach its server. Still trying.",
                                @"System Link toast: the relay doesn't answer"),
              XeniaIOSStatusToastStyleWarning);
      }
      return;
    case State::kRejected:
      if (last.state != State::kRejected || last.reject != status.reject) {
        Toast(RejectMessage(status.reject), XeniaIOSStatusToastStyleError);
      }
      return;
    case State::kReconnecting:
      // The server still holds the old connection: told as back online once it clears.
      g_told_offline = g_told_online;
      return;
    case State::kConnecting:
      return;
    case State::kOnline:
      break;
  }
  if (!g_told_online || g_told_offline) {
    NSString* message = g_told_online
                            ? NSLocalizedString(@"System Link is back online.",
                                                @"System Link toast: the relay answers again")
                            : OnlineMessage(status.players);
    g_told_online = true;
    g_told_offline = false;
    Toast(message, XeniaIOSStatusToastStyleSuccess);
  } else if (g_told_online && last.state == State::kOnline && status.players != last.players) {
    Toast(status.players > last.players
              ? NSLocalizedString(@"A player joined System Link.",
                                  @"System Link toast: someone entered the room")
              : NSLocalizedString(@"A player left System Link.",
                                  @"System Link toast: someone left the room"),
          XeniaIOSStatusToastStyleInfo);
  }
  if (status.version_mismatch && !last.version_mismatch) {
    Toast(VersionMismatchMessage(), XeniaIOSStatusToastStyleWarning);
  }
  if (status.nat != last.nat) {
    SaveNat(status.nat);
  }
  if (status.turn != last.turn) {
    [[NSUserDefaults standardUserDefaults] setBool:status.turn forKey:kLobbyTurnKey];
  }
  // Once per game: the sheet keeps saying it while it lasts.
  if (status.unreachable && !g_told_unreachable) {
    g_told_unreachable = true;
    Toast(xe_ios_system_link_unreachable_message(status.unreachable, ToIOS(status.nat)),
          XeniaIOSStatusToastStyleWarning);
  }
}

}  // namespace

NSString* const XeniaIOSSystemLinkStatusDidChangeNotification =
    @"XeniaIOSSystemLinkStatusDidChangeNotification";

XeniaIOSSystemLinkSnapshot xe_ios_system_link_snapshot(void) {
  using Snapshot = XeniaIOSSystemLinkSnapshot;
  Snapshot snapshot;
  if (!g_active) {
    return snapshot;
  }
  const RelayStatus& status = g_last;
  switch (status.state) {
    case State::kConnecting:
      snapshot.state = Snapshot::State::kConnecting;
      break;
    case State::kReconnecting:
      snapshot.state = Snapshot::State::kReconnecting;
      break;
    case State::kOnline:
      snapshot.state = Snapshot::State::kOnline;
      break;
    case State::kOffline:
      snapshot.state = Snapshot::State::kOffline;
      break;
    case State::kRejected:
      snapshot.state = Snapshot::State::kRejected;
      snapshot.problem = RejectMessage(status.reject);
      break;
    case State::kPaused:
    case State::kStopped:
      return snapshot;
  }
  snapshot.title_id = g_title_id;
  snapshot.room_code = g_room_code;
  snapshot.nat = ToIOS(status.nat);
  snapshot.turn = status.turn;
  if (status.state == State::kOnline) {
    snapshot.others = status.players;
    snapshot.direct = status.direct;
    snapshot.relayed = status.relayed;
    snapshot.unreachable = status.unreachable;
    snapshot.other_version = status.version_mismatch;
    snapshot.ping_ms = status.ping_ms;
  }
  return snapshot;
}

namespace {

using Snapshot = XeniaIOSSystemLinkSnapshot;

NSString* CardTitle(const Snapshot& s) {
  switch (s.state) {
    case Snapshot::State::kConnecting:
      return NSLocalizedString(@"System Link · Connecting…", @"In-game menu: System Link card");
    case Snapshot::State::kReconnecting:
      return NSLocalizedString(@"System Link · Reconnecting…",
                               @"In-game menu: System Link card");
    case Snapshot::State::kOnline:
      if (s.ping_ms) {
        return [NSString
            stringWithFormat:NSLocalizedString(@"System Link · %u ms",
                                               @"In-game menu: System Link card; %u is the "
                                               @"round trip to the other players"),
                             s.ping_ms];
      }
      return NSLocalizedString(@"System Link · Online", @"In-game menu: System Link card");
    case Snapshot::State::kOffline:
      return NSLocalizedString(@"System Link · Can't Reach the Server",
                               @"In-game menu: System Link card");
    case Snapshot::State::kRejected:
      return NSLocalizedString(@"System Link · Turned Away", @"In-game menu: System Link card");
    case Snapshot::State::kOff:
      return nil;
  }
}

// "1 other player, connected directly", or nil while not in the room.
NSString* PlayersSummary(const Snapshot& s) {
  if (s.state != Snapshot::State::kOnline) {
    return nil;
  }
  const size_t waiting = s.others - std::min(s.others, s.direct + s.relayed);
  if (s.others == 0) {
    return NSLocalizedString(@"Waiting for players",
                             @"In-game menu: System Link card, nobody else is in the room");
  }
  if (s.others == 1) {
    return s.direct    ? NSLocalizedString(@"1 other player, connected directly",
                                           @"In-game menu: System Link card")
           : s.relayed ? NSLocalizedString(@"1 other player, connected through the relay",
                                           @"In-game menu: System Link card")
                       : NSLocalizedString(@"1 other player, not connected yet",
                                           @"In-game menu: System Link card");
  }
  if (!waiting) {
    return [NSString
        stringWithFormat:NSLocalizedString(@"%lu other players, all connected",
                                           @"In-game menu: System Link card; %lu is a count"),
                         (unsigned long)s.others];
  }
  return [NSString
      stringWithFormat:NSLocalizedString(@"%1$lu other players, %2$lu not connected yet",
                                         @"In-game menu: System Link card; %1$lu and %2$lu are "
                                         @"counts"),
                       (unsigned long)s.others, (unsigned long)waiting];
}

}  // namespace

NSString* xe_ios_system_link_room_name(NSString* code) {
  return code.length
             ? [NSString stringWithFormat:NSLocalizedString(@"Private Room %@",
                                                            @"system link view controller: room "
                                                            @"row; %@ is the room code"),
                                          code]
             : NSLocalizedString(@"Public Lobby", @"system link view controller: room row");
}

// The in-game menu's System Link card, which keeps itself current.
@interface XeniaIOSSystemLinkStatusCard : UIButton
@end

namespace {

UIConfigurationTextAttributesTransformer CardText(UIFont* font, UIColor* color) {
  return [[^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
    NSMutableDictionary<NSAttributedStringKey, id>* out = [[in mutableCopy] autorelease];
    out[NSFontAttributeName] = font;
    out[NSForegroundColorAttributeName] = color;
    return out;
  } copy] autorelease];
}

}  // namespace

@implementation XeniaIOSSystemLinkStatusCard

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    UIButtonConfiguration* configuration = [UIButtonConfiguration glassButtonConfiguration];
    configuration.cornerStyle = UIButtonConfigurationCornerStyleLarge;
    configuration.titleAlignment = UIButtonConfigurationTitleAlignmentLeading;
    configuration.titlePadding = 2.0;
    configuration.subtitleTextAttributesTransformer =
        CardText(xe_scaled_system_font(UIFontTextStyleFootnote, 13.0, UIFontWeightRegular),
                 [XeniaTheme textSecondary]);
    self.configuration = configuration;
    self.translatesAutoresizingMaskIntoConstraints = NO;
    // The menu's rows have fixed heights, so the card is the one that would be squeezed.
    [self setContentCompressionResistancePriority:UILayoutPriorityRequired
                                          forAxis:UILayoutConstraintAxisVertical];
    self.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    self.accessibilityHint =
        NSLocalizedString(@"Shows the System Link room, its players and who's online.",
                          @"In-game menu, VoiceOver: System Link card hint");
    [self addTarget:self action:@selector(tapped:) forControlEvents:UIControlEventTouchUpInside];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(statusChanged:)
                                                 name:XeniaIOSSystemLinkStatusDidChangeNotification
                                               object:nil];
    [self registerForTraitChanges:@[ [UITraitVerticalSizeClass class] ]
                       withAction:@selector(statusChanged:)];
    [self statusChanged:nil];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [super dealloc];
}

- (void)statusChanged:(id __unused)sender {
  const Snapshot snapshot = xe_ios_system_link_snapshot();
  self.hidden = snapshot.state == Snapshot::State::kOff;
  [self setNeedsUpdateConfiguration];
}

- (void)updateConfiguration {
  const Snapshot snapshot = xe_ios_system_link_snapshot();
  const bool problem = snapshot.state == Snapshot::State::kOffline ||
                       snapshot.state == Snapshot::State::kRejected || snapshot.other_version ||
                       (snapshot.others && snapshot.direct + snapshot.relayed < snapshot.others);
  NSString* detail = snapshot.problem;
  if (snapshot.state == Snapshot::State::kOffline) {
    detail = NSLocalizedString(@"Still trying.",
                               @"In-game menu: System Link card, the server doesn't answer");
  } else if (NSString* players = PlayersSummary(snapshot)) {
    detail = [NSString
        stringWithFormat:@"%@ · %@", xe_ios_system_link_room_name(snapshot.room_code), players];
  } else if (snapshot.state == Snapshot::State::kConnecting ||
             snapshot.state == Snapshot::State::kReconnecting) {
    detail = xe_ios_system_link_room_name(snapshot.room_code);
  }
  if (snapshot.other_version) {
    detail = [detail
        stringByAppendingFormat:@" · %@",
                                NSLocalizedString(@"Different game version",
                                                  @"In-game menu: System Link card, another "
                                                  @"player runs another version of the game")];
  }
  UIButtonConfiguration* configuration = self.configuration;
  NSString* title = CardTitle(snapshot);
  // Short of height, a phone in landscape: one line, small, cut short if it must be. The icon's
  // color says it's online.
  const bool compact = self.traitCollection.verticalSizeClass == UIUserInterfaceSizeClassCompact;
  if (compact) {
    NSString* line = snapshot.state == Snapshot::State::kOnline
                         ? NSLocalizedString(@"System Link", @"In-game menu: System Link card")
                         : title;
    configuration.title = detail ? [NSString stringWithFormat:@"%@ · %@", line, detail] : line;
    configuration.subtitle = nil;
    configuration.titleLineBreakMode = NSLineBreakByTruncatingTail;
    configuration.titleTextAttributesTransformer =
        CardText(xe_scaled_system_font(UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold),
                 [XeniaTheme textPrimary]);
    configuration.contentInsets = NSDirectionalEdgeInsetsMake(7.0, 12.0, 7.0, 12.0);
    configuration.imagePadding = 8.0;
  } else {
    configuration.title = title;
    configuration.subtitle = detail;
    configuration.titleLineBreakMode = NSLineBreakByWordWrapping;
    configuration.titleTextAttributesTransformer =
        CardText(xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold),
                 [XeniaTheme textPrimary]);
    configuration.contentInsets = NSDirectionalEdgeInsetsMake(10.0, 14.0, 10.0, 14.0);
    configuration.imagePadding = 12.0;
  }
  UIColor* tint = problem                                      ? [XeniaTheme statusWarning]
                  : snapshot.state == Snapshot::State::kOnline ? [XeniaTheme accent]
                                                               : [XeniaTheme textSecondary];
  configuration.image =
      [[UIImage systemImageNamed:problem ? @"exclamationmark.triangle.fill" : @"network"]
          imageWithTintColor:tint
               renderingMode:UIImageRenderingModeAlwaysOriginal];
  configuration.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration
      configurationWithTextStyle:compact ? UIFontTextStyleSubheadline : UIFontTextStyleTitle3];
  self.configuration = configuration;
  self.accessibilityLabel = title;
  self.accessibilityValue = detail;
}

- (void)tapped:(id __unused)sender {
  UIResponder* responder = self.nextResponder;
  while (responder && ![responder isKindOfClass:[UIViewController class]]) {
    responder = responder.nextResponder;
  }
  UIViewController* presenter = (UIViewController*)responder;
  while (presenter.presentedViewController) {
    presenter = presenter.presentedViewController;
  }
  if (presenter) {
    xe_ios_present_system_link_status_sheet(presenter);
  }
}

@end

UIView* xe_ios_new_system_link_status_view(void) {
  return [[XeniaIOSSystemLinkStatusCard alloc] initWithFrame:CGRectZero];
}

NSString* xe_ios_system_link_room(uint32_t title_id) {
  NSDictionary* rooms = [[NSUserDefaults standardUserDefaults] dictionaryForKey:kRoomsKey];
  id code = rooms[TitleKey(title_id)];
  return [code isKindOfClass:[NSString class]] ? xe_ios_normalize_system_link_room_code(code) : nil;
}

void xe_ios_set_system_link_room(uint32_t title_id, NSString* code) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSMutableDictionary* rooms = [[[defaults dictionaryForKey:kRoomsKey] mutableCopy] autorelease]
                                   ?: [NSMutableDictionary dictionary];
  NSString* normalized = xe_ios_normalize_system_link_room_code(code);
  if (normalized) {
    rooms[TitleKey(title_id)] = normalized;
  } else {
    [rooms removeObjectForKey:TitleKey(title_id)];
  }
  [defaults setObject:rooms forKey:kRoomsKey];
  // A game that's running moves to its new room. Another game's room doesn't concern it, and
  // asking it again would drop the room an invite gave this launch.
  if (!g_title_id || g_title_id == title_id) {
    xe::kernel::xam::SystemLinkRoomChanged();
  }
}

NSString* xe_ios_new_system_link_room_code(void) {
  return [NSString stringWithUTF8String:xe::net::relay::NewRoomCode().c_str()];
}

NSString* xe_ios_normalize_system_link_room_code(NSString* text) {
  if (!text.length) {
    return nil;
  }
  auto code = xe::net::relay::NormalizeRoomCode(text.UTF8String);
  return code ? [NSString stringWithUTF8String:code->c_str()] : nil;
}

NSURL* xe_ios_system_link_invite_url(uint32_t title_id, NSString* code) {
  NSURLComponents* components = [[[NSURLComponents alloc] init] autorelease];
  components.scheme = @"xenios";
  components.host = @"link";
  components.path = [@"/" stringByAppendingString:code ?: @""];
  components.queryItems = @[ [NSURLQueryItem queryItemWithName:@"title" value:TitleKey(title_id)] ];
  return components.URL;
}

NSString* xe_ios_system_link_invite_room(NSURL* url, uint32_t* title_id_out) {
  if (![url.scheme.lowercaseString isEqualToString:@"xenios"] ||
      ![url.host.lowercaseString isEqualToString:@"link"]) {
    return nil;
  }
  NSURLComponents* components = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
  NSString* title = nil;
  for (NSURLQueryItem* item in components.queryItems) {
    if ([item.name.lowercaseString isEqualToString:@"title"]) {
      title = item.value;
    }
  }
  NSString* code = xe_ios_normalize_system_link_room_code(url.lastPathComponent);
  NSScanner* scanner = title.length == 8 ? [NSScanner scannerWithString:title] : nil;
  unsigned int title_id = 0;
  if (!code || ![scanner scanHexInt:&title_id] || !scanner.atEnd || !title_id) {
    return nil;
  }
  if (title_id_out) {
    *title_id_out = title_id;
  }
  return code;
}

NSString* xe_ios_system_link_invite_note(NSString* code, BOOL restarts) {
  NSString* note = [NSString
      stringWithFormat:NSLocalizedString(@"This launch joins System Link room %@, once. Players in "
                                         @"the room can see your IP address and a profile ID. "
                                         @"System Link needs Online Features to be on.",
                                         @"Launch prompt for an invite link; %@ is the room code"),
                       code];
  if (restarts) {
    note = [note stringByAppendingFormat:@" %@",
                                         NSLocalizedString(@"The running game restarts to join.",
                                                           @"Launch prompt for an invite link to "
                                                           @"the game that is already running")];
  }
  return note;
}

void xe_ios_join_system_link_room(uint32_t title_id, NSString* code) {
  if (NSString* normalized = xe_ios_normalize_system_link_room_code(code)) {
    std::lock_guard lock(g_invite_mutex);
    g_invites[title_id] = {normalized.UTF8String, [NSProcessInfo processInfo].systemUptime};
  }
  if (!xe_ios_online_features_enabled()) {
    Toast([NSString stringWithFormat:@"%@ %@",
                                     NSLocalizedString(@"System Link needs Online features.",
                                                       @"System Link toast: an invite arrived "
                                                       @"while online features are off"),
                                     xe_ios_online_features_off_message()],
          XeniaIOSStatusToastStyleWarning);
  }
}

BOOL xe_ios_system_link_uses_relay(void) { return cvars::systemlink_transport == "relay"; }

XeniaIOSNatType xe_ios_system_link_nat(void) {
  if (auto nat = NatOverride()) {
    return ToIOS(*nat);
  }
  return ToIOS(xe::net::relay::RememberedNat(xe::net::relay::NetworkKey()));
}

BOOL xe_ios_nat_blocks_direct(XeniaIOSNatType nat) {
  return nat == XeniaIOSNatType::kStrict || nat == XeniaIOSNatType::kBlocked;
}

NSString* xe_ios_nat_type_name(XeniaIOSNatType nat) {
  switch (nat) {
    case XeniaIOSNatType::kOpen:
      return NSLocalizedString(@"Open", @"NAT type: no NAT in the way of other players");
    case XeniaIOSNatType::kModerate:
      return NSLocalizedString(@"Moderate", @"NAT type: a router most players get through");
    case XeniaIOSNatType::kStrict:
      return NSLocalizedString(@"Strict", @"NAT type: a router that keeps most players out");
    case XeniaIOSNatType::kBlocked:
      return NSLocalizedString(@"Blocked", @"NAT type: the network lets no System Link traffic "
                                           @"out");
    case XeniaIOSNatType::kUnknown:
      break;
  }
  return nil;
}

NSString* xe_ios_nat_type_explanation(XeniaIOSNatType nat) {
  switch (nat) {
    case XeniaIOSNatType::kOpen:
      return NSLocalizedString(@"Other players can connect to you directly.",
                               @"NAT type explanation: open");
    case XeniaIOSNatType::kModerate:
      return NSLocalizedString(@"Most players can connect to you directly.",
                               @"NAT type explanation: moderate");
    case XeniaIOSNatType::kStrict:
      return NSLocalizedString(@"Your network blocks most direct connections, so some players "
                               @"can't connect to you.",
                               @"NAT type explanation: strict");
    case XeniaIOSNatType::kBlocked:
      return NSLocalizedString(@"Your network blocks System Link, so you can't play with anyone "
                               @"on it.",
                               @"NAT type explanation: blocked");
    case XeniaIOSNatType::kUnknown:
      break;
  }
  return nil;
}

BOOL xe_ios_system_link_lobby_has_turn(void) {
  return [[NSUserDefaults standardUserDefaults] boolForKey:kLobbyTurnKey];
}

NSString* xe_ios_system_link_unreachable_message(size_t count, XeniaIOSNatType nat) {
  const bool ours = xe_ios_nat_blocks_direct(nat);
  if (count == 1) {
    return ours ? NSLocalizedString(@"Can't connect to 1 player. Your network blocks direct "
                                    @"connections.",
                                    @"System Link: a player no path reaches, because of this "
                                    @"network")
                : NSLocalizedString(@"Can't connect to 1 player. Their network may be blocking "
                                    @"direct connections.",
                                    @"System Link: a player no path reaches, most likely because "
                                    @"of theirs");
  }
  return [NSString
      localizedStringWithFormat:ours ? NSLocalizedString(@"Can't connect to %lu players. Your "
                                                         @"network blocks direct connections.",
                                                         @"System Link: players no path reaches, "
                                                         @"because of this network; %lu is how "
                                                         @"many")
                                     : NSLocalizedString(@"Can't connect to %lu players. Their "
                                                         @"networks may be blocking direct "
                                                         @"connections.",
                                                         @"System Link: players no path reaches, "
                                                         @"most likely because of theirs; %lu is "
                                                         @"how many"),
                                (unsigned long)count];
}

void xe_ios_system_link_install(void (^toast)(NSString* message, XeniaIOSStatusToastStyle style)) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    g_toast = [toast copy];
    LoadNat();
    xe::kernel::xam::SetSystemLinkRelayPolicy({
        [] { return bool(xe_ios_online_features_enabled()); },
        [](uint32_t title_id) {
          @autoreleasepool {
            NSString* code = TakeInviteRoom(title_id) ?: xe_ios_system_link_room(title_id);
            dispatch_async(dispatch_get_main_queue(), ^{
              [g_room_code release];
              g_room_code = [code copy];
              g_title_id = title_id;
            });
            return code ? std::string(code.UTF8String) : std::string();
          }
        },
        // The room counts the app while it plays there, in place of check-ins.
        [] {
          @autoreleasepool {
            return std::string(HereID().UTF8String);
          }
        },
    });
    // Games ask whether System Link is allowed on every frame, so the answer is kept and read
    // again when the defaults change, which covers Online Features and its notice.
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSUserDefaultsDidChangeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* __unused note) {
                  xe::kernel::xam::SystemLinkAllowedChanged();
                }];
    xe::net::secure::SetRelayStatusListener([](const RelayStatus& relay_status) {
      // A copy: a block captures a C++ reference as a reference, and the caller's status is gone
      // by the time the main queue runs it (garbage counts like "17,170,505 players").
      const RelayStatus status = relay_status;
      dispatch_async(dispatch_get_main_queue(), ^{
        OnStatus(status);
        [[NSNotificationCenter defaultCenter]
            postNotificationName:XeniaIOSSystemLinkStatusDidChangeNotification
                          object:nil];
      });
    });
  });
}

// ---- Who's online ----

NSString* const XeniaIOSSystemLinkOnlineDidChangeNotification =
    @"XeniaIOSSystemLinkOnlineDidChangeNotification";

// "3 online" with a people symbol, in a game tile's footer row.
@interface XeniaIOSSystemLinkOnlineLabel : UILabel
@end

// The library's Online button, which keeps its count current.
@interface XeniaIOSSystemLinkOnlineButton : UIButton
@end

@implementation XeniaIOSSystemLinkOnlineLabel
@end

namespace {

// The lobby answers from a copy up to two minutes old, so asking more often gains nothing.
constexpr NSTimeInterval kOnlineInterval = 120.0;
// Counts that haven't been answered again for this long are dropped rather than shown.
constexpr NSTimeInterval kOnlineStale = 5 * 60.0;
// How often the app tells the lobby it's open, in the library or in a game.
constexpr NSTimeInterval kHereInterval = 5 * 60.0;
// How long the lobby counts an app after it checks in. An app relaunched sooner keeps its ID, so
// it isn't counted twice.
constexpr NSTimeInterval kHereCounted = 6 * 60.0;

// Players by title ID, or nil while unknown, and apps open, or -1 while unknown.
NSDictionary<NSNumber*, NSNumber*>* g_online = nil;
NSInteger g_here = -1;
NSInteger g_private = -1;
NSTimeInterval g_here_said = 0;
NSTimeInterval g_online_asked = 0;
NSTimeInterval g_online_answered = 0;
bool g_online_asking = false;
int g_online_kept = 0;
BOOL (^g_online_wanted)(void) = nil;
void (^g_online_changed)(void) = nil;

NSTimeInterval Uptime() { return [NSProcessInfo processInfo].systemUptime; }

bool OnlineAllowed() {
  return xe_ios_online_features_enabled() && !xe_ios_online_features_notice_pending() &&
         xe_ios_system_link_uses_relay();
}

// GET /v1/online on the lobby, over https:// for a wss:// lobby. Never logged, like the lobby.
NSURL* OnlineURL() {
  NSString* text = [NSString stringWithUTF8String:(cvars::systemlink_lobby + "/v1/online").c_str()];
  NSURLComponents* url = text ? [NSURLComponents componentsWithString:text] : nil;
  NSString* scheme = url.scheme.lowercaseString;
  if ([scheme isEqualToString:@"wss"]) {
    url.scheme = @"https";
  } else if ([scheme isEqualToString:@"ws"]) {
    url.scheme = @"http";
  } else {
    return nil;
  }
  return url.host.length ? url.URL : nil;
}

// The app to the lobby, counting apps open: made up at launch, so it says nothing else, unless the
// app's last check-in is still counted.
NSString* HereID() {
  static NSString* here_id = [] {
    NSDictionary* last = [[NSUserDefaults standardUserDefaults] dictionaryForKey:kHereKey];
    id last_id = last[@"id"];
    id last_at = last[@"at"];
    if ([last_id isKindOfClass:[NSString class]] && [last_id length] == 16 &&
        [last_id rangeOfCharacterFromSet:[[NSCharacterSet
                                             characterSetWithCharactersInString:@"0123456789abcdef"]
                                             invertedSet]]
                .location == NSNotFound &&
        [last_at isKindOfClass:[NSNumber class]]) {
      const NSTimeInterval since = [NSDate date].timeIntervalSince1970 - [last_at doubleValue];
      if (since >= 0 && since < kHereCounted) {
        return [last_id copy];
      }
    }
    uint8_t bytes[8];
    arc4random_buf(bytes, sizeof(bytes));
    NSMutableString* hex = [NSMutableString string];
    for (uint8_t byte : bytes) {
      [hex appendFormat:@"%02x", byte];
    }
    return [hex copy];
  }();
  return here_id;
}

// The app is checking in now. Saved before the lobby answers, so a relaunch (one to turn on the
// JIT comes within seconds) keeps the ID.
void SaveHere() {
  [[NSUserDefaults standardUserDefaults] setObject:@{
    @"id" : HereID(),
    @"at" : @([NSDate date].timeIntervalSince1970)
  }
                                            forKey:kHereKey];
}

NSURL* OnlineURLSaying(NSString* name) {
  NSURL* url = OnlineURL();
  if (!url) {
    return nil;
  }
  NSURLComponents* components = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
  components.queryItems = @[ [NSURLQueryItem queryItemWithName:name value:HereID()] ];
  return components.URL;
}

NSURLSession* OnlineSession() {
  static NSURLSession* session = [] {
    NSURLSessionConfiguration* configuration =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.timeoutIntervalForRequest = 10.0;
    configuration.waitsForConnectivity = NO;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    return [[NSURLSession sessionWithConfiguration:configuration] retain];
  }();
  return session;
}

// A count in the lobby's answer, or -1 when it isn't there.
NSInteger CountIn(NSDictionary* body, NSString* key) {
  id count = body[key];
  return [count isKindOfClass:[NSNumber class]] && [count integerValue] >= 0 ? [count integerValue]
                                                                             : -1;
}

// {"v": 1, "titles": {"4D5307E6": 3}, "private": 2, "here": 7} -> @{@0x4D5307E6: @3}, 2 and 7,
// or nil when it isn't that.
NSDictionary<NSNumber*, NSNumber*>* ParseOnline(NSData* data, NSInteger* here_out,
                                                NSInteger* private_out) {
  id body = data.length ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
  if (![body isKindOfClass:[NSDictionary class]] || ![body[@"v"] isEqual:@1]) {
    return nil;
  }
  *here_out = CountIn(body, @"here");
  *private_out = CountIn(body, @"private");
  id titles = body[@"titles"];
  if (![titles isKindOfClass:[NSDictionary class]]) {
    return nil;
  }
  NSCharacterSet* not_hex =
      [[NSCharacterSet characterSetWithCharactersInString:@"0123456789ABCDEF"] invertedSet];
  NSMutableDictionary<NSNumber*, NSNumber*>* online = [NSMutableDictionary dictionary];
  for (id key in titles) {
    id players = titles[key];
    if (![key isKindOfClass:[NSString class]] || [key length] != 8 ||
        [key rangeOfCharacterFromSet:not_hex].location != NSNotFound ||
        ![players isKindOfClass:[NSNumber class]] || [players integerValue] <= 0) {
      continue;
    }
    online[@(uint32_t(strtoul([key UTF8String], nullptr, 16)))] = @([players integerValue]);
  }
  return online;
}

void SetOnline(NSDictionary<NSNumber*, NSNumber*>* online, NSInteger here = -1,
               NSInteger in_private = -1) {
  if (!online) {
    here = -1;
    in_private = -1;
  }
  if ((online == g_online || [online isEqualToDictionary:g_online]) && here == g_here &&
      in_private == g_private) {
    return;
  }
  [g_online release];
  g_online = [online copy];
  g_here = here;
  g_private = in_private;
  [[NSNotificationCenter defaultCenter]
      postNotificationName:XeniaIOSSystemLinkOnlineDidChangeNotification
                    object:nil];
  if (g_online_changed) {
    g_online_changed();
  }
}

// While the app is in the foreground with Online Features on, it keeps a socket open to the lobby,
// which counts the app while it's open and sends the counts on it when they change, in place of
// asking for them and checking in. The app sends only WebSocket pings on it. While it fails to
// open (an older lobby, or one that's full), the app asks and checks in meanwhile.
constexpr NSTimeInterval kPresencePing = 30.0;
constexpr NSTimeInterval kPresenceRetry = 5.0;
constexpr NSTimeInterval kPresenceMaxRetry = 5 * 60.0;

NSURLSessionWebSocketTask* g_presence = nil;
NSTimer* g_presence_pinger = nil;
// Counts have arrived on the socket.
bool g_presence_up = false;
// Sockets that failed in a row without counts arriving, and when to try again.
int g_presence_failures = 0;
NSTimeInterval g_presence_retry_at = 0;

// wss://<lobby>/v1/presence?id=<the app's ID>.
NSURL* PresenceURL() {
  NSString* text =
      [NSString stringWithUTF8String:(cvars::systemlink_lobby + "/v1/presence").c_str()];
  NSURLComponents* url = text ? [NSURLComponents componentsWithString:text] : nil;
  NSString* scheme = url.scheme.lowercaseString;
  if (!([scheme isEqualToString:@"wss"] || [scheme isEqualToString:@"ws"]) || !url.host.length) {
    return nil;
  }
  url.queryItems = @[ [NSURLQueryItem queryItemWithName:@"id" value:HereID()] ];
  return url.URL;
}

NSURLSession* PresenceSession() {
  static NSURLSession* session = [] {
    NSURLSessionConfiguration* configuration =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    // Pongs arrive every kPresencePing, so a socket quiet for longer than this is gone.
    configuration.timeoutIntervalForRequest = 2 * kPresencePing;
    configuration.waitsForConnectivity = NO;
    return [[NSURLSession sessionWithConfiguration:configuration] retain];
  }();
  return session;
}

// Closes the socket normally, which tells the lobby the app is leaving.
void ClosePresence() {
  [g_presence_pinger invalidate];
  [g_presence_pinger release];
  g_presence_pinger = nil;
  g_presence_up = false;
  if (!g_presence) {
    return;
  }
  NSURLSessionWebSocketTask* task = g_presence;
  g_presence = nil;
  [task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
  [task release];
}

void PresenceFailed(NSURLSessionWebSocketTask* task) {
  if (task != g_presence) {
    return;
  }
  const bool was_up = g_presence_up;
  ClosePresence();
  // One that was working is tried again soon, and the counts it sent stay until they're stale.
  g_presence_failures = was_up ? 0 : g_presence_failures + 1;
  g_presence_retry_at = Uptime() + std::min(kPresenceMaxRetry,
                                            kPresenceRetry * (1 << std::min(g_presence_failures, 6)));
  AskOnline();
}

void ReceivePresence(NSURLSessionWebSocketTask* task) {
  [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message,
                                              NSError* error) {
    NSDictionary<NSNumber*, NSNumber*>* online = nil;
    NSInteger here = -1;
    NSInteger in_private = -1;
    if (!error && message.type == NSURLSessionWebSocketMessageTypeString) {
      online =
          ParseOnline([message.string dataUsingEncoding:NSUTF8StringEncoding], &here, &in_private);
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (task != g_presence) {
        return;
      }
      if (error) {
        PresenceFailed(task);
        return;
      }
      if (online && OnlineAllowed()) {
        g_presence_up = true;
        g_presence_failures = 0;
        g_online_answered = Uptime();
        SaveHere();
        SetOnline(online, here, in_private);
      }
      ReceivePresence(task);
    });
  }];
}

// Opens the socket while it's wanted, and closes it when it isn't.
void KeepPresence() {
  if (!OnlineAllowed() ||
      [UIApplication sharedApplication].applicationState != UIApplicationStateActive) {
    ClosePresence();
    return;
  }
  if (g_presence || Uptime() < g_presence_retry_at) {
    return;
  }
  NSURL* url = PresenceURL();
  if (!url) {
    return;
  }
  SaveHere();
  NSURLSessionWebSocketTask* task = [[PresenceSession() webSocketTaskWithURL:url] retain];
  g_presence = task;
  [task resume];
  ReceivePresence(task);
  g_presence_pinger = [[NSTimer
      timerWithTimeInterval:kPresencePing
                    repeats:YES
                      block:^(NSTimer* __unused t) {
                        [task sendPingWithPongReceiveHandler:^(NSError* error) {
                          dispatch_async(dispatch_get_main_queue(), ^{
                            if (task != g_presence) {
                              return;
                            }
                            if (error) {
                              PresenceFailed(task);
                            } else if (g_presence_up) {
                              // Unchanged counts are still current.
                              g_online_answered = Uptime();
                            }
                          });
                        }];
                      }] retain];
  g_presence_pinger.tolerance = 5.0;
  [[NSRunLoop mainRunLoop] addTimer:g_presence_pinger forMode:NSRunLoopCommonModes];
}

// Keeps the presence socket open. Without it, asks for the counts every 2 minutes while they're
// wanted, and every 5 minutes says the app is open, also while a game runs, except while a System
// Link room counts it (the room tells the lobby). Coming to the foreground always says so, which
// also undoes a `gone`.
void AskOnline() {
  if (!OnlineAllowed()) {
    ClosePresence();
    SetOnline(nil);
    return;
  }
  const NSTimeInterval now = Uptime();
  if (g_online && now - g_online_answered >= kOnlineStale) {
    SetOnline(nil);
  }
  KeepPresence();
  // The socket answers, or is about to; asking starts once it has failed.
  if (g_presence_up || !g_presence_failures) {
    return;
  }
  const bool wanted = !g_online_wanted || g_online_wanted() || g_online_kept;
  const bool say_here =
      !g_here_said || (now - g_here_said >= kHereInterval && !g_last.app_counted);
  if (g_online_asking ||
      [UIApplication sharedApplication].applicationState != UIApplicationStateActive ||
      (!say_here && (!wanted || (g_online_asked && now - g_online_asked < kOnlineInterval)))) {
    return;
  }
  NSURL* url = say_here ? OnlineURLSaying(@"here") : OnlineURL();
  if (!url) {
    SetOnline(nil);
    return;
  }
  if (say_here) {
    g_here_said = now;
    SaveHere();
  }
  g_online_asking = true;
  g_online_asked = now;
  [[OnlineSession() dataTaskWithURL:url
                  completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                    NSDictionary<NSNumber*, NSNumber*>* online = nil;
                    NSInteger here = -1;
                    NSInteger in_private = -1;
                    if (!error && [response isKindOfClass:[NSHTTPURLResponse class]] &&
                        ((NSHTTPURLResponse*)response).statusCode == 200) {
                      online = ParseOnline(data, &here, &in_private);
                    }
                    dispatch_async(dispatch_get_main_queue(), ^{
                      g_online_asking = false;
                      // A check-in the lobby didn't count is tried again in a minute.
                      if (say_here && g_here_said == now && !(online && here >= 0)) {
                        g_here_said = now - kHereInterval + kOnlineInterval;
                      }
                      // A failed answer keeps the last counts until they're stale.
                      if (online && OnlineAllowed()) {
                        g_online_answered = Uptime();
                        SetOnline(online, here, in_private);
                      }
                    });
                  }] resume];
}

// Tells the lobby the app has gone to the background or is closing, so it stops counting it right
// away rather than in 6 minutes: by closing the socket, and with `gone` if it checked in. Coming
// back opens the socket or checks in again, with the same ID: a lobby that missed this, or doesn't
// know it, still counts the app once.
void SayGone() {
  const bool present = g_presence != nil;
  ClosePresence();
  g_presence_retry_at = 0;
  NSURL* url = g_here_said && OnlineAllowed() ? OnlineURLSaying(@"gone") : nil;
  g_here_said = 0;
  if (!present && !url) {
    return;
  }
  UIApplication* app = [UIApplication sharedApplication];
  __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
  void (^end)(void) = ^{
    if (task != UIBackgroundTaskInvalid) {
      [app endBackgroundTask:task];
      task = UIBackgroundTaskInvalid;
    }
  };
  task = [app beginBackgroundTaskWithName:@"SystemLinkGone" expirationHandler:end];
  if (!url) {
    // Time for the close to go out.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(),
                   end);
    return;
  }
  [[OnlineSession() dataTaskWithURL:url
                  completionHandler:^(NSData* __unused data, NSURLResponse* __unused response,
                                      NSError* __unused error) {
                    dispatch_async(dispatch_get_main_queue(), end);
                  }] resume];
}

}  // namespace

void xe_ios_system_link_watch_online(BOOL (^wanted)(void), void (^changed)(void)) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    g_online_wanted = [wanted copy];
    g_online_changed = [changed copy];
    // Checks often, asks only every 2 minutes: a player leaving a game sees counts within seconds.
    NSTimer* timer = [NSTimer timerWithTimeInterval:15.0
                                            repeats:YES
                                              block:^(NSTimer* __unused t) {
                                                AskOnline();
                                              }];
    timer.tolerance = 5.0;
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    [[NSNotificationCenter defaultCenter]
        addObserverForName:UIApplicationDidBecomeActiveNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* __unused note) {
                  AskOnline();
                }];
    for (NSNotificationName name in
         @[ UIApplicationDidEnterBackgroundNotification, UIApplicationWillTerminateNotification ]) {
      [[NSNotificationCenter defaultCenter] addObserverForName:name
                                                        object:nil
                                                         queue:[NSOperationQueue mainQueue]
                                                    usingBlock:^(NSNotification* __unused note) {
                                                      SayGone();
                                                    }];
    }
    AskOnline();
  });
}

void xe_ios_system_link_refresh_online(void) { AskOnline(); }

void xe_ios_system_link_keep_online(BOOL keep) {
  g_online_kept = std::max(0, g_online_kept + (keep ? 1 : -1));
  AskOnline();
}

NSDictionary<NSNumber*, NSNumber*>* xe_ios_system_link_online_titles(void) { return g_online; }

NSInteger xe_ios_system_link_online_here(void) { return g_online ? g_here : -1; }

NSInteger xe_ios_system_link_online_private(void) { return g_online ? g_private : -1; }

NSInteger xe_ios_system_link_online_players(uint32_t title_id) {
  return title_id ? [g_online[@(title_id)] integerValue] : 0;
}

NSString* xe_ios_system_link_online_description(uint32_t title_id) {
  const NSInteger players = xe_ios_system_link_online_players(title_id);
  if (players <= 0) {
    return nil;
  }
  if (players == 1) {
    return NSLocalizedString(@"1 player online",
                             @"Game library, VoiceOver: one player in the game's public System "
                             @"Link lobby");
  }
  return [NSString
      localizedStringWithFormat:NSLocalizedString(@"%ld players online",
                                                  @"Game library, VoiceOver: players in the "
                                                  @"game's public System Link lobby"),
                                long(players)];
}

void xe_ios_system_link_show_online_label(UIView* row, uint32_t title_id) {
  if (![row isKindOfClass:[UIStackView class]]) {
    return;
  }
  UIStackView* stack = (UIStackView*)row;
  XeniaIOSSystemLinkOnlineLabel* label = nil;
  for (UIView* view in stack.arrangedSubviews) {
    if ([view isKindOfClass:[XeniaIOSSystemLinkOnlineLabel class]]) {
      label = (XeniaIOSSystemLinkOnlineLabel*)view;
      break;
    }
  }
  const NSInteger players = xe_ios_system_link_online_players(title_id);
  if (!label) {
    if (players <= 0) {
      return;
    }
    label = [[[XeniaIOSSystemLinkOnlineLabel alloc] initWithFrame:CGRectZero] autorelease];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.isAccessibilityElement = NO;
    xe_apply_label_font(label, UIFontTextStyleCaption1, 12.0, UIFontWeightSemibold);
    // The first to give way when the row is short of room.
    [label setContentCompressionResistancePriority:UILayoutPriorityDefaultLow - 1
                                           forAxis:UILayoutConstraintAxisHorizontal];
    [stack addArrangedSubview:label];
  }
  label.hidden = players <= 0;
  if (players <= 0) {
    return;
  }
  UIColor* color = [XeniaTheme accent];
  UIImage* symbol = [[UIImage
       systemImageNamed:@"person.2.fill"
      withConfiguration:[UIImageSymbolConfiguration configurationWithFont:label.font
                                                                    scale:UIImageSymbolScaleSmall]]
      imageWithTintColor:color
           renderingMode:UIImageRenderingModeAlwaysOriginal];
  NSTextAttachment* icon = [NSTextAttachment textAttachmentWithImage:symbol];
  NSMutableAttributedString* text = [[[NSMutableAttributedString alloc]
      initWithAttributedString:[NSAttributedString attributedStringWithAttachment:icon]]
      autorelease];
  [text appendAttributedString:
            [[[NSAttributedString alloc]
                initWithString:[@" " stringByAppendingString:
                                         [NSString
                                             localizedStringWithFormat:
                                                 NSLocalizedString(
                                                     @"%ld online",
                                                     @"Game library: players in the game's public "
                                                     @"System Link lobby; %ld is how many"),
                                                 long(players)]]
                    attributes:@{
                      NSForegroundColorAttributeName : color,
                      NSFontAttributeName : label.font
                    }] autorelease]];
  label.attributedText = text;
}

BOOL xe_ios_system_link_online_connected(void) { return g_online != nil; }

NSInteger xe_ios_system_link_online_total(void) {
  NSInteger total = 0;
  for (NSNumber* title_id in g_online) {
    total += g_online[title_id].integerValue;
  }
  return total;
}

namespace {
void (^g_online_menu)(void) = nil;
}  // namespace

void xe_ios_system_link_set_online_menu(void (^open)(void)) {
  [g_online_menu release];
  g_online_menu = [open copy];
}

@implementation XeniaIOSSystemLinkOnlineButton

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(onlineChanged:)
                                                 name:XeniaIOSSystemLinkOnlineDidChangeNotification
                                               object:nil];
    [self addTarget:self action:@selector(tapped:) forControlEvents:UIControlEventTouchUpInside];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [super dealloc];
}

- (void)onlineChanged:(NSNotification* __unused)note {
  [self setNeedsUpdateConfiguration];
}

- (void)tapped:(id)__unused sender {
  if (g_online_menu) {
    g_online_menu();
  }
}

- (void)updateConfiguration {
  UIButtonConfiguration* configuration = self.configuration;
  // The Online sheet's headline: apps open (one device each), or the players in lobbies and
  // rooms when the lobby doesn't say how many apps are open.
  const NSInteger total =
      std::max(xe_ios_system_link_online_here(),
               xe_ios_system_link_online_total() +
                   std::max<NSInteger>(0, xe_ios_system_link_online_private()));
  // A status dot, green while the lobby answers, so the library says whether the app is online.
  const BOOL connected = xe_ios_system_link_online_connected();
  UIColor* status = connected ? [XeniaTheme accent] : [XeniaTheme textMuted];
  configuration.image = [[UIImage
       systemImageNamed:@"circle.fill"
      withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:8.0
                                                                        weight:UIImageSymbolWeightBold]]
      imageWithTintColor:status
           renderingMode:UIImageRenderingModeAlwaysOriginal];
  configuration.baseForegroundColor = connected ? [XeniaTheme textPrimary] : [XeniaTheme textMuted];
  configuration.title =
      !connected ? NSLocalizedString(@"Offline", @"Game library: Online button while the app "
                                                 @"can't reach the System Link lobby")
      : total > 0
          ? [NSString localizedStringWithFormat:NSLocalizedString(
                                                    @"%ld Online",
                                                    @"online view controller: summary title; "
                                                    @"%ld is how many devices have XeniOS open"),
                                                long(total)]
          : NSLocalizedString(@"Online", @"Game library: Online button");
  self.configuration = configuration;
  self.accessibilityValue =
      total == 1
          ? NSLocalizedString(@"1 device online", @"Game library, VoiceOver: Online button value, "
                                                  @"when only one device has XeniOS open")
      : total > 1
          ? [NSString localizedStringWithFormat:NSLocalizedString(
                                                    @"%ld devices online",
                                                    @"Game library, VoiceOver: Online button "
                                                    @"value; %ld is how many devices have XeniOS "
                                                    @"open"),
                                                long(total)]
          : nil;
}

@end

UIButton* xe_ios_new_system_link_online_button(void) {
  // Glass, like the Library header's other buttons.
  UIButtonConfiguration* configuration = [UIButtonConfiguration glassButtonConfiguration];
  configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  configuration.baseForegroundColor = [XeniaTheme textPrimary];
  UIFont* font = xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
  configuration.imagePadding = 7.0;
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(0.0, 13.0, 0.0, 14.0);
  configuration.titleTextAttributesTransformer =
      ^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
    NSMutableDictionary<NSAttributedStringKey, id>* out = [[in mutableCopy] autorelease];
    out[NSFontAttributeName] = font;
    return out;
  };
  XeniaIOSSystemLinkOnlineButton* button =
      [[[XeniaIOSSystemLinkOnlineButton alloc] initWithFrame:CGRectZero] autorelease];
  button.configuration = configuration;
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.accessibilityLabel = NSLocalizedString(@"Online", @"Game library: Online button");
  button.accessibilityHint =
      NSLocalizedString(@"Shows who has XeniOS open and the games they're playing.",
                        @"Game library, VoiceOver: Online button hint");
  [button setContentCompressionResistancePriority:UILayoutPriorityRequired
                                          forAxis:UILayoutConstraintAxisHorizontal];
  return button;
}
