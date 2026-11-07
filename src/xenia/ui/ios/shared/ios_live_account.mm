/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_live_account.h"

NSNotificationName const XeniaLiveAccountDidChangeNotification =
    @"XeniaLiveAccountDidChangeNotification";
NSNotificationName const XeniaLiveIncomingApprovalNotification =
    @"XeniaLiveIncomingApprovalNotification";

@implementation XeniaLiveLocalProfile
- (void)dealloc {
  [_profileId release];
  [_gamertag release];
  [super dealloc];
}
@end

@implementation XeniaLiveSaveGame
- (void)dealloc {
  [_name release];
  [super dealloc];
}
@end

@implementation XeniaLiveApprovalRequest
- (void)dealloc {
  [_requestId release];
  [_qrPayload release];
  [_matchNumber release];
  [_expires release];
  [super dealloc];
}
@end

@implementation XeniaLiveIncomingApproval
- (void)dealloc {
  [_requestId release];
  [_deviceName release];
  [_platform release];
  [_platformText release];
  [_choices release];
  [_expires release];
  [super dealloc];
}
@end

@implementation XeniaLiveNameResult
- (instancetype)init {
  if ((self = [super init])) {
    _suggestions = @[];
  }
  return self;
}
- (void)dealloc {
  [_suggestions release];
  [_nextRename release];
  [_recoveryCode release];
  [super dealloc];
}
@end

@implementation XeniaLiveAccountSummary
- (instancetype)init {
  if ((self = [super init])) {
    _gamertag = @"";
  }
  return self;
}
- (void)dealloc {
  [_gamertag release];
  [_nextRename release];
  [super dealloc];
}
@end

@implementation XeniaLiveDevice
- (void)dealloc {
  [_deviceId release];
  [_name release];
  [_kind release];
  [_lastSeen release];
  [super dealloc];
}
@end

@implementation XeniaLiveExportOptions
@end

NSString* xe_ios_gamertag_local_error(NSString* name) {
  if (!name.length) {
    return nil;
  }
  if (name.length > 15) {
    return NSLocalizedString(@"Too long: 15 characters at most",
                             @"Gamertag field: message under the field");
  }
  for (NSUInteger i = 0; i < name.length; ++i) {
    const unichar c = [name characterAtIndex:i];
    const BOOL letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    const BOOL digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != ' ') {
      return NSLocalizedString(@"Letters, digits and single spaces only",
                               @"Gamertag field: message under the field");
    }
  }
  const unichar first = [name characterAtIndex:0];
  if (first >= '0' && first <= '9') {
    return NSLocalizedString(@"Can’t start with a number",
                             @"Gamertag field: message under the field");
  }
  if (first == ' ') {
    return NSLocalizedString(@"Must start with a letter",
                             @"Gamertag field: message under the field");
  }
  if ([name containsString:@"  "]) {
    return NSLocalizedString(@"Letters, digits and single spaces only",
                             @"Gamertag field: message under the field");
  }
  if ([name characterAtIndex:name.length - 1] == ' ') {
    return NSLocalizedString(@"Can’t end with a space", @"Gamertag field: message under the field");
  }
  return nil;
}

// ---- The fake service ------------------------------------------------------------------

namespace {

NSString* RandomCode(NSUInteger length, NSUInteger group) {
  static NSString* const kAlphabet = @"ABCDEFGHJKMNPQRSTUVWXYZ23456789";
  NSMutableString* code = [NSMutableString string];
  for (NSUInteger i = 0; i < length; ++i) {
    if (i && i % group == 0) {
      [code appendString:@"-"];
    }
    [code appendFormat:@"%C", [kAlphabet characterAtIndex:arc4random_uniform((uint32_t)kAlphabet.length)]];
  }
  return code;
}

NSString* Squeezed(NSString* code) {
  return [[[code uppercaseString] stringByReplacingOccurrencesOfString:@"-" withString:@""]
      stringByReplacingOccurrencesOfString:@" " withString:@""];
}

NSString* Cut(NSString* name) {
  return name.length > 15 ? [name substringToIndex:15] : name;
}

}  // namespace

// Fakes the lobby in memory, with the answers the screens have to handle: "Bo", "Master
// Chief", "Arbiter" and "Cortana" are taken; names with Xbox, XeniOS, Admin, Support,
// Moderator or Microsoft are reserved; a few rude ones are refused.
@interface XeniaLiveAccountStub : NSObject <XeniaLiveAccountService> {
  XeniaLiveAccountSummary* _summary;
  NSMutableArray<XeniaLiveDevice*>* _devices;
  NSString* _linkCode;
  NSDate* _linkExpires;
  NSMutableArray<NSDate*>* _checks;
  // The new device's pending approval (one at a time).
  XeniaLiveApprovalRequest* _request;
  NSString* _requestName;
  void (^_requestDone)(XeniaLiveApprovalStatus);
  NSUInteger _requestGeneration;
  // The signed-in device's incoming one.
  XeniaLiveIncomingApproval* _incoming;
  NSString* _incomingMatch;
  BOOL _sharedAll;
  NSMutableSet<NSString*>* _sharedGames;  // "profile:title"
}
@end

@implementation XeniaLiveAccountStub

- (instancetype)init {
  if ((self = [super init])) {
    _summary = [[XeniaLiveAccountSummary alloc] init];
    _devices = [[NSMutableArray alloc] init];
    _checks = [[NSMutableArray alloc] init];
    _sharedGames = [[NSMutableSet alloc] initWithObjects:@"mia:1414", nil];
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if ([defaults boolForKey:@"XeniaLiveAccountPreviewOnline"]) {
      _summary.online = YES;
      _summary.gamertag = @"Bo Bandit";
      _summary.nextRename = [NSDate dateWithTimeIntervalSinceNow:24 * 3600.0 * 22];
      _summary.renameFlagged = [defaults boolForKey:@"XeniaLiveAccountPreviewFlagged"];
      [self addDevices];
      if ([defaults boolForKey:@"XeniaLiveAccountPreviewIncoming"]) {
        // A sign-in from "Bo’s iPad" shows up a few seconds after launch.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(6.0 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
                         [self simulateIncomingFromQR:NO];
                       });
      }
    }
  }
  return self;
}

- (void)dealloc {
  [_summary release];
  [_devices release];
  [_linkCode release];
  [_linkExpires release];
  [_checks release];
  [_request release];
  [_requestName release];
  [_requestDone release];
  [_incoming release];
  [_incomingMatch release];
  [_sharedGames release];
  [super dealloc];
}

- (void)addDevices {
  [_devices removeAllObjects];
  NSArray* rows = @[
    @[ @"this", @"Bo’s iPhone", @"iphone", @YES, @0 ],
    @[ @"ipad", @"Bo’s iPad", @"ipad", @NO, @(24 * 3600) ],
    @[ @"mac", @"Bo’s MacBook", @"mac", @NO, @(24 * 3600 * 21) ],
  ];
  for (NSArray* row in rows) {
    XeniaLiveDevice* device = [[[XeniaLiveDevice alloc] init] autorelease];
    device.deviceId = row[0];
    device.name = row[1];
    device.kind = row[2];
    device.thisDevice = [row[3] boolValue];
    device.lastSeen = device.thisDevice ? nil : [NSDate dateWithTimeIntervalSinceNow:-[row[4] doubleValue]];
    [_devices addObject:device];
  }
}

- (XeniaLiveAccountSummary*)summary {
  return _summary;
}

- (void)changed {
  [[NSNotificationCenter defaultCenter]
      postNotificationName:XeniaLiveAccountDidChangeNotification
                    object:nil];
}

- (void)later:(void (^)(void))block {
  void (^copy)(void) = [block copy];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.35 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   copy();
                   [copy release];
                 });
}

- (NSArray<NSString*>*)suggestionsFor:(NSString*)name {
  NSString* compact = [name stringByReplacingOccurrencesOfString:@" " withString:@""];
  return @[
    Cut([name stringByAppendingString:@" 7"]),
    Cut([compact stringByAppendingString:@"117"]),
    Cut([@"The " stringByAppendingString:name]),
  ];
}

- (XeniaLiveNameStatus)statusForName:(NSString*)name {
  NSString* lower = name.lowercaseString;
  for (NSString* word in @[ @"xbox", @"xenios", @"admin", @"support", @"moderator", @"microsoft" ]) {
    if ([lower containsString:word]) {
      return XeniaLiveNameStatusReserved;
    }
  }
  for (NSString* word in @[ @"shit", @"fuck" ]) {
    if ([lower containsString:word]) {
      return XeniaLiveNameStatusNotAllowed;
    }
  }
  if ([@[ @"bo", @"master chief", @"arbiter", @"cortana" ] containsObject:lower] &&
      ![lower isEqualToString:_summary.gamertag.lowercaseString]) {
    return XeniaLiveNameStatusTaken;
  }
  return XeniaLiveNameStatusAvailable;
}

- (XeniaLiveNameResult*)resultForName:(NSString*)name {
  XeniaLiveNameResult* result = [[[XeniaLiveNameResult alloc] init] autorelease];
  result.status = xe_ios_gamertag_local_error(name).length ? XeniaLiveNameStatusInvalid
                                                           : [self statusForName:name];
  if (result.status == XeniaLiveNameStatusTaken || result.status == XeniaLiveNameStatusReserved ||
      result.status == XeniaLiveNameStatusNotAllowed) {
    result.suggestions = [self suggestionsFor:name];
  }
  return result;
}

- (void)checkGamertag:(NSString*)gamertag completion:(void (^)(XeniaLiveNameResult*))completion {
  void (^done)(XeniaLiveNameResult*) = [completion copy];
  [self later:^{
    const NSTimeInterval now = NSDate.date.timeIntervalSinceReferenceDate;
    [_checks filterUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSDate* date, id unused) {
               return now - date.timeIntervalSinceReferenceDate < 60;
             }]];
    XeniaLiveNameResult* result;
    if (_checks.count >= 30) {
      result = [[[XeniaLiveNameResult alloc] init] autorelease];
      result.status = XeniaLiveNameStatusLimit;
    } else {
      [_checks addObject:NSDate.date];
      result = [self resultForName:gamertag];
    }
    done(result);
    [done release];
  }];
}

- (void)goOnlineWithGamertag:(NSString*)gamertag
                  completion:(void (^)(XeniaLiveNameResult*))completion {
  void (^done)(XeniaLiveNameResult*) = [completion copy];
  [self later:^{
    XeniaLiveNameResult* result = [self resultForName:gamertag];
    if (result.status == XeniaLiveNameStatusAvailable) {
      _summary.online = YES;
      _summary.gamertag = gamertag;
      _summary.nextRename = nil;
      result.recoveryCode = RandomCode(12, 4);
      [self addDevices];
      [self changed];
    }
    done(result);
    [done release];
  }];
}

- (void)renameToGamertag:(NSString*)gamertag completion:(void (^)(XeniaLiveNameResult*))completion {
  void (^done)(XeniaLiveNameResult*) = [completion copy];
  [self later:^{
    XeniaLiveNameResult* result = [self resultForName:gamertag];
    const BOOL capitals_only = [gamertag.lowercaseString isEqualToString:_summary.gamertag.lowercaseString];
    if (result.status == XeniaLiveNameStatusAvailable && !capitals_only && !_summary.renameFlagged &&
        _summary.nextRename.timeIntervalSinceNow > 0) {
      result.status = XeniaLiveNameStatusTooSoon;
      result.nextRename = _summary.nextRename;
    } else if (result.status == XeniaLiveNameStatusAvailable) {
      _summary.gamertag = gamertag;
      if (!capitals_only) {
        _summary.renameFlagged = NO;
        _summary.nextRename = [NSDate dateWithTimeIntervalSinceNow:24 * 3600.0 * 30];
      }
      [self changed];
    }
    done(result);
    [done release];
  }];
}

- (void)createLinkCodeWithCompletion:(void (^)(NSString*, NSDate*, NSError*))completion {
  void (^done)(NSString*, NSDate*, NSError*) = [completion copy];
  [self later:^{
    [_linkCode release];
    _linkCode = [RandomCode(8, 4) retain];
    [_linkExpires release];
    _linkExpires = [[NSDate dateWithTimeIntervalSinceNow:5 * 60] retain];
    done(_linkCode, _linkExpires, nil);
    [done release];
  }];
}

- (void)linkWithCode:(NSString*)code completion:(void (^)(XeniaLiveLinkStatus))completion {
  void (^done)(XeniaLiveLinkStatus) = [completion copy];
  [self later:^{
    NSString* squeezed = Squeezed(code);
    XeniaLiveLinkStatus status = XeniaLiveLinkStatusBadCode;
    if ([squeezed isEqualToString:@"EXPIRED1"]) {
      status = XeniaLiveLinkStatusExpired;
    } else if (squeezed.length == 8 &&
               ([squeezed isEqualToString:Squeezed(_linkCode ?: @"")] || [squeezed isEqualToString:@"K7Q2M9XD"])) {
      status = XeniaLiveLinkStatusLinked;
      _summary.online = YES;
      _summary.gamertag = @"Bo Bandit";
      [self addDevices];
      [self changed];
    }
    done(status);
    [done release];
  }];
}

- (void)loadDevicesWithCompletion:(void (^)(NSArray<XeniaLiveDevice*>*, NSError*))completion {
  void (^done)(NSArray*, NSError*) = [completion copy];
  [self later:^{
    done([[_devices copy] autorelease], nil);
    [done release];
  }];
}

- (void)removeDevice:(XeniaLiveDevice*)device completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [completion copy];
  NSString* identifier = [[device.deviceId copy] autorelease];
  [self later:^{
    for (XeniaLiveDevice* entry in [[_devices copy] autorelease]) {
      if ([entry.deviceId isEqualToString:identifier]) {
        [_devices removeObject:entry];
      }
    }
    [self changed];
    done(nil);
    [done release];
  }];
}

- (void)createRecoveryCodeWithCompletion:(void (^)(NSString*, NSError*))completion {
  void (^done)(NSString*, NSError*) = [completion copy];
  [self later:^{
    done(RandomCode(12, 4), nil);
    [done release];
  }];
}

- (void)recoverWithCode:(NSString*)code completion:(void (^)(XeniaLiveLinkStatus))completion {
  void (^done)(XeniaLiveLinkStatus) = [completion copy];
  [self later:^{
    XeniaLiveLinkStatus status = XeniaLiveLinkStatusBadCode;
    if (Squeezed(code).length == 12) {
      status = XeniaLiveLinkStatusLinked;
      _summary.online = YES;
      _summary.gamertag = @"Bo Bandit";
      [self addDevices];
      [self changed];
    }
    done(status);
    [done release];
  }];
}

- (void)exportSizesWithCompletion:(void (^)(uint64_t, uint64_t, uint64_t))completion {
  void (^done)(uint64_t, uint64_t, uint64_t) = [completion copy];
  [self later:^{
    done(212ull << 20, 1468006ull, 104857ull);
    [done release];
  }];
}

- (void)exportProfileWithOptions:(XeniaLiveExportOptions*)options
                      completion:(void (^)(NSURL*, NSError*))completion {
  void (^done)(NSURL*, NSError*) = [completion copy];
  [self later:^{
    NSURL* url = [NSURL fileURLWithPath:[NSTemporaryDirectory()
                                            stringByAppendingPathComponent:@"Preview Profile.xeniosprofile"]];
    NSDictionary* body = @{
      @"preview" : @YES,
      @"saves" : @(options.saves),
      @"achievements" : @(options.achievements),
      @"settings" : @(options.settings)
    };
    [[NSJSONSerialization dataWithJSONObject:body options:0 error:nil] writeToURL:url atomically:YES];
    done(url, nil);
    [done release];
  }];
}

- (void)importProfileFromURL:(NSURL*)url completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [completion copy];
  [self later:^{
    done(nil);
    [done release];
  }];
}


// ---- Approval ---------------------------------------------------------------------------

- (NSString*)randomNumber {
  return [NSString stringWithFormat:@"%02u", 10 + arc4random_uniform(90)];
}

- (void)finishRequest:(XeniaLiveApprovalStatus)status generation:(NSUInteger)generation {
  if (generation != _requestGeneration || !_requestDone) {
    return;
  }
  void (^done)(XeniaLiveApprovalStatus) = [_requestDone autorelease];
  _requestDone = nil;
  if (status == XeniaLiveApprovalStatusApproved) {
    _summary.online = YES;
    _summary.gamertag = _requestName.length ? _requestName : @"Bo Bandit";
    _summary.nextRename = nil;
    [self addDevices];
    [self changed];
  }
  done(status);
}

- (void)requestApprovalForGamertag:(NSString*)gamertag
                        completion:(void (^)(XeniaLiveApprovalRequest*, XeniaLiveApprovalStatus))completion {
  void (^done)(XeniaLiveApprovalRequest*, XeniaLiveApprovalStatus) = [completion copy];
  [self later:^{
    [_request release];
    _request = [[XeniaLiveApprovalRequest alloc] init];
    _request.requestId = [NSUUID UUID].UUIDString;
    _request.matchNumber = [self randomNumber];
    // "slow" times out quickly so the timeout screen is easy to reach.
    const BOOL slow = [gamertag.lowercaseString containsString:@"slow"];
    _request.expires = [NSDate dateWithTimeIntervalSinceNow:slow ? 15 : 120];
    [_requestName release];
    _requestName = [gamertag copy];
    done(_request, XeniaLiveApprovalStatusApproved);
    [done release];
  }];
}

- (void)requestQRLinkWithCompletion:(void (^)(XeniaLiveApprovalRequest*, XeniaLiveApprovalStatus))completion {
  void (^done)(XeniaLiveApprovalRequest*, XeniaLiveApprovalStatus) = [completion copy];
  [self later:^{
    [_request release];
    _request = [[XeniaLiveApprovalRequest alloc] init];
    _request.requestId = [NSUUID UUID].UUIDString;
    _request.qrPayload = [@"xenios://link?r=" stringByAppendingString:_request.requestId];
    _request.expires = [NSDate dateWithTimeIntervalSinceNow:120];
    [_requestName release];
    _requestName = nil;
    done(_request, XeniaLiveApprovalStatusApproved);
    [done release];
  }];
}

- (void)waitForApproval:(XeniaLiveApprovalRequest*)request
             completion:(void (^)(XeniaLiveApprovalStatus))completion {
  [_requestDone release];
  _requestDone = [completion copy];
  const NSUInteger generation = ++_requestGeneration;
  const NSTimeInterval timeout = MAX(request.expires.timeIntervalSinceNow, 1.0);
  NSString* name = _requestName.lowercaseString;
  // Previews: "deny" is refused after 3 s, "slow" and "ghost" (no such account) time out, a QR
  // join is approved after 25 s, anything else after 5 s.
  NSTimeInterval answer = request.qrPayload ? 25.0 : 5.0;
  XeniaLiveApprovalStatus status = XeniaLiveApprovalStatusApproved;
  if ([name containsString:@"deny"]) {
    answer = 3.0;
    status = XeniaLiveApprovalStatusDenied;
  } else if ([name containsString:@"slow"] || [name containsString:@"ghost"]) {
    answer = timeout;
    status = XeniaLiveApprovalStatusTimedOut;
  }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(MIN(answer, timeout) * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   [self finishRequest:answer >= timeout && status == XeniaLiveApprovalStatusApproved
                                           ? XeniaLiveApprovalStatusTimedOut
                                           : status
                            generation:generation];
                 });
}

- (void)cancelApproval:(XeniaLiveApprovalRequest* __unused)request {
  [self finishRequest:XeniaLiveApprovalStatusCancelled generation:_requestGeneration];
}

- (XeniaLiveIncomingApproval*)pendingApproval {
  return _incoming;
}

- (void)simulateIncomingFromQR:(BOOL)qr {
  [_incoming release];
  _incoming = [[XeniaLiveIncomingApproval alloc] init];
  _incoming.requestId = [NSUUID UUID].UUIDString;
  _incoming.deviceName = @"Bo’s iPad";
  _incoming.platform = @"ipad";
  _incoming.platformText = @"iPad · iPadOS 26";
  NSString* match = [self randomNumber];
  [_incomingMatch release];
  _incomingMatch = [match copy];
  NSMutableArray* choices = [NSMutableArray arrayWithObject:match];
  while (choices.count < 3) {
    NSString* other = [self randomNumber];
    if (![choices containsObject:other]) {
      [choices insertObject:other atIndex:arc4random_uniform((uint32_t)choices.count + 1)];
    }
  }
  _incoming.choices = choices;
  _incoming.expires = [NSDate dateWithTimeIntervalSinceNow:120];
  if (!qr) {  // A scanned one goes straight to the scanner, not to the whole app.
    [[NSNotificationCenter defaultCenter] postNotificationName:XeniaLiveIncomingApprovalNotification
                                                        object:nil];
  }
}

- (void)clearIncoming {
  [_incoming release];
  _incoming = nil;
  [[NSNotificationCenter defaultCenter] postNotificationName:XeniaLiveIncomingApprovalNotification
                                                      object:nil];
}

- (void)approveIncoming:(XeniaLiveIncomingApproval*)approval
                 chosen:(NSString*)chosen
             completion:(void (^)(XeniaLiveApprovalStatus))completion {
  void (^done)(XeniaLiveApprovalStatus) = [completion copy];
  NSString* picked = [[chosen copy] autorelease];
  [self later:^{
    const BOOL right = [picked isEqualToString:_incomingMatch ?: @""];
    (void)approval;
    [self clearIncoming];
    done(right ? XeniaLiveApprovalStatusApproved : XeniaLiveApprovalStatusDenied);
    [done release];
  }];
}

- (void)denyIncoming:(XeniaLiveIncomingApproval* __unused)approval {
  [self clearIncoming];
}

- (void)beginApprovalFromQRPayload:(NSString*)payload
                        completion:(void (^)(XeniaLiveIncomingApproval*, XeniaLiveLinkStatus))completion {
  void (^done)(XeniaLiveIncomingApproval*, XeniaLiveLinkStatus) = [completion copy];
  NSString* text = [[payload copy] autorelease];
  [self later:^{
    if (![text hasPrefix:@"xenios://link"]) {
      done(nil, XeniaLiveLinkStatusBadCode);
    } else {
      [self simulateIncomingFromQR:YES];
      done(_incoming, XeniaLiveLinkStatusLinked);
    }
    [done release];
  }];
}


// ---- Saves between profiles -------------------------------------------------------------

- (void)localProfilesWithCompletion:(void (^)(NSArray<XeniaLiveLocalProfile*>*))completion {
  void (^done)(NSArray*) = [completion copy];
  [self later:^{
    NSMutableArray* list = [NSMutableArray array];
    NSArray* rows = @[
      @[ @"this", _summary.gamertag.length ? _summary.gamertag : @"Test", @(_summary.online), @YES ],
      @[ @"mia", @"Mia", @NO, @NO ],
      @[ @"jonas", @"Jonas", @NO, @NO ],
    ];
    for (NSArray* row in rows) {
      XeniaLiveLocalProfile* profile = [[[XeniaLiveLocalProfile alloc] init] autorelease];
      profile.profileId = row[0];
      profile.gamertag = row[1];
      profile.online = [row[2] boolValue];
      profile.thisProfile = [row[3] boolValue];
      [list addObject:profile];
    }
    done(list);
    [done release];
  }];
}

- (BOOL)savesSharedAcrossProfiles {
  return _sharedAll;
}

- (void)setSavesSharedAcrossProfiles:(BOOL)shared completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [completion copy];
  [self later:^{
    _sharedAll = shared;
    done(nil);
    [done release];
  }];
}

- (void)saveGamesSharedWithProfile:(NSString*)other completion:(void (^)(NSArray<XeniaLiveSaveGame*>*))completion {
  void (^done)(NSArray*) = [completion copy];
  NSString* who = [[other copy] autorelease];
  [self later:^{
    NSArray* rows = @[ @[ @1414, @"Halo 3", @(84ull << 20) ], @[ @1415, @"Halo: Reach", @(61ull << 20) ],
                       @[ @1416, @"Skate 3", @(12ull << 20) ] ];
    NSMutableArray* games = [NSMutableArray array];
    for (NSArray* row in rows) {
      XeniaLiveSaveGame* game = [[[XeniaLiveSaveGame alloc] init] autorelease];
      game.titleId = [row[0] unsignedIntValue];
      game.name = row[1];
      game.bytes = [row[2] unsignedLongLongValue];
      game.shared = [_sharedGames containsObject:[NSString stringWithFormat:@"%@:%u", who, game.titleId]];
      [games addObject:game];
    }
    done(games);
    [done release];
  }];
}

- (void)setSharing:(BOOL)shared
          forTitle:(uint32_t)titleId
       withProfile:(NSString*)other
        completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [completion copy];
  NSString* key = [NSString stringWithFormat:@"%@:%u", other, titleId];
  [self later:^{
    if (shared) {
      [_sharedGames addObject:key];
    } else {
      [_sharedGames removeObject:key];
    }
    done(nil);
    [done release];
  }];
}

- (void)copySavesToProfile:(NSString* __unused)other
                completion:(void (^)(NSUInteger, NSUInteger, NSError*))completion {
  void (^done)(NSUInteger, NSUInteger, NSError*) = [completion copy];
  [self later:^{
    done(5, 2, nil);
    [done release];
  }];
}

@end

namespace {
id<XeniaLiveAccountService> g_service = nil;
id<XeniaLiveAccountService> g_stub = nil;
}  // namespace

id<XeniaLiveAccountService> xe_ios_live_account_service(void) {
  if (g_service) {
    return g_service;
  }
  if (!g_stub) {
    g_stub = [[XeniaLiveAccountStub alloc] init];
  }
  return g_stub;
}

void xe_ios_live_account_set_service(id<XeniaLiveAccountService> service) {
  if (service != g_service) {
    [g_service release];
    g_service = [service retain];
  }
  [[NSNotificationCenter defaultCenter] postNotificationName:XeniaLiveAccountDidChangeNotification
                                                      object:nil];
}

BOOL xe_ios_live_account_service_is_stub(void) { return g_service == nil; }

BOOL xe_ios_live_account_ui_enabled(void) {
  return g_service != nil ||
         [[NSUserDefaults standardUserDefaults] boolForKey:@"XeniaLiveAccountPreview"];
}
