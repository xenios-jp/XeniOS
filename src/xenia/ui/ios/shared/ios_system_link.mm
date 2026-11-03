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
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/net/packet_transport.h"
#include "xenia/net/relay_protocol.h"
#import "xenia/ui/ios/launcher/ios_system_link_view_controller.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

DECLARE_string(systemlink_lobby);
DECLARE_string(systemlink_transport);

namespace {

using xe::net::relay::RelayStatus;
using State = RelayStatus::State;

// Title ID in hex to room code.
NSString* const kRoomsKey = @"XeniaSystemLinkRooms";
// The app's last check-in with the lobby: {"id": ID, "at": seconds since 1970}.
NSString* const kHereKey = @"XeniaSystemLinkHere";

void (^g_toast)(NSString*, XeniaIOSStatusToastStyle) = nil;
// What the player was last told, so each change is told once.
RelayStatus g_last;
// Whether a game plays System Link through the relay now.
bool g_active = false;
bool g_told_online = false;
bool g_told_offline = false;
// The room the running game asked for, and the game.
NSString* g_room_code = nil;
uint32_t g_title_id = 0;

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

// Told once per game, as soon as the room is online: players in a room see each other's IP
// address, and a host waiting alone otherwise sees nothing.
void OnStatus(const RelayStatus& status) {
  const RelayStatus last = g_last;
  g_last = status;
  const bool chose_room = g_room_code.length != 0;
  g_active = status.state != State::kStopped;
  switch (status.state) {
    case State::kStopped:
      g_last = {};
      g_told_online = false;
      g_told_offline = false;
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
    case State::kStopped:
      return snapshot;
  }
  snapshot.title_id = g_title_id;
  snapshot.room_code = g_room_code;
  if (status.state == State::kOnline) {
    snapshot.others = status.players;
    snapshot.direct = status.direct;
    snapshot.relayed = status.relayed;
    snapshot.other_version = status.version_mismatch;
  }
  return snapshot;
}

namespace {

using Snapshot = XeniaIOSSystemLinkSnapshot;

NSString* CardTitle(const Snapshot& s) {
  switch (s.state) {
    case Snapshot::State::kConnecting:
      return NSLocalizedString(@"System Link · Connecting…", @"In-game menu: System Link card");
    case Snapshot::State::kOnline:
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
  } else if (snapshot.state == Snapshot::State::kConnecting) {
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
  // A game that's running moves to the new room.
  xe::kernel::xam::SystemLinkRoomChanged();
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

NSString* xe_ios_system_link_invite_note(NSString* code) {
  return [NSString
      stringWithFormat:NSLocalizedString(@"It also sets System Link for this game to room %@.",
                                         @"Launch prompt for an invite link; %@ is the room code"),
                       code];
}

void xe_ios_join_system_link_room(uint32_t title_id, NSString* code) {
  xe_ios_set_system_link_room(title_id, code);
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

void xe_ios_system_link_install(void (^toast)(NSString* message, XeniaIOSStatusToastStyle style)) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    g_toast = [toast copy];
    xe::kernel::xam::SetSystemLinkRelayPolicy({
        [] { return bool(xe_ios_online_features_enabled()); },
        [](uint32_t title_id) {
          @autoreleasepool {
            NSString* code = xe_ios_system_link_room(title_id);
            dispatch_async(dispatch_get_main_queue(), ^{
              [g_room_code release];
              g_room_code = [code copy];
              g_title_id = title_id;
            });
            return code ? std::string(code.UTF8String) : std::string();
          }
        },
    });
    xe::net::secure::SetRelayStatusListener([](const RelayStatus& status) {
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

// The lobby answers from a copy up to a minute old, so asking more often gains nothing.
constexpr NSTimeInterval kOnlineInterval = 60.0;
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

// Asks for the counts about once a minute while they're wanted, and every 5 minutes says the app
// is open, also while a game runs.
void AskOnline() {
  if (!OnlineAllowed()) {
    SetOnline(nil);
    return;
  }
  const NSTimeInterval now = Uptime();
  if (g_online && now - g_online_answered >= kOnlineStale) {
    SetOnline(nil);
  }
  const bool wanted = !g_online_wanted || g_online_wanted() || g_online_kept;
  const bool say_here = !g_here_said || now - g_here_said >= kHereInterval;
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
// away rather than in 6 minutes. Coming back checks in again, with the same ID: a lobby that missed
// this, or doesn't know it, still counts the app once.
void SayGone() {
  if (!g_here_said) {
    return;
  }
  g_here_said = 0;
  NSURL* url = OnlineAllowed() ? OnlineURLSaying(@"gone") : nil;
  if (!url) {
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
    // Checks often, asks only once a minute: a player leaving a game sees counts within seconds.
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
  // The players in rooms run the app too, but a Mac or an older app doesn't say it's open.
  const NSInteger total = std::max(xe_ios_system_link_online_here(),
                                   xe_ios_system_link_online_total() +
                                       std::max<NSInteger>(0, xe_ios_system_link_online_private()));
  configuration.title =
      total > 0
          ? [NSString localizedStringWithFormat:NSLocalizedString(
                                                    @"%ld Online",
                                                    @"Game library: Online button, with how many "
                                                    @"players are in public System Link lobbies"),
                                                long(total)]
          : NSLocalizedString(@"Online", @"Game library: Online button");
  self.configuration = configuration;
  self.accessibilityValue =
      total == 1 ? NSLocalizedString(@"1 player online", @"Game library, VoiceOver: one player in "
                                                         @"the game's public System Link lobby")
      : total > 1
          ? [NSString localizedStringWithFormat:NSLocalizedString(@"%ld players online",
                                                                  @"Game library, VoiceOver: "
                                                                  @"players in the game's public "
                                                                  @"System Link lobby"),
                                                long(total)]
          : nil;
}

@end

UIButton* xe_ios_new_system_link_online_button(void) {
  UIButtonConfiguration* configuration = [UIButtonConfiguration tintedButtonConfiguration];
  configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  configuration.baseForegroundColor = [XeniaTheme accent];
  configuration.baseBackgroundColor = [XeniaTheme accent];
  UIFont* font = xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
  configuration.image = [UIImage
       systemImageNamed:@"person.2.fill"
      withConfiguration:[UIImageSymbolConfiguration configurationWithFont:font
                                                                    scale:UIImageSymbolScaleSmall]];
  configuration.imagePadding = 6.0;
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(6.0, 12.0, 6.0, 12.0);
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
      NSLocalizedString(@"Shows how many players are in each game's public System Link lobby.",
                        @"Game library, VoiceOver: Online button hint");
  [button setContentCompressionResistancePriority:UILayoutPriorityRequired
                                          forAxis:UILayoutConstraintAxisHorizontal];
  return button;
}
