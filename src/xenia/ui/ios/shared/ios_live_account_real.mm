/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_live_account_real.h"

#include <optional>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/kernel/xam/xam_system_link.h"
#import "xenia/ui/ios/shared/ios_live_account.h"
#import "xenia/ui/ios/shared/ios_live_social_bridge.h"
#import "xenia/ui/ios/shared/ios_online_features.h"

namespace live = xe::net::live;

namespace {

const XeniaIOSLiveSocialHost* g_real_host = nullptr;

// How long a request waits for the lobby's answer.
constexpr NSTimeInterval kAnswerTimeout = 15.0;
// Going online or joining also connects first.
constexpr NSTimeInterval kConnectTimeout = 25.0;

NSString* ToNS(const std::string& text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

std::string ToStd(NSString* text) { return text.UTF8String ?: ""; }

XeniaLiveNameStatus NameStatus(const std::string& status) {
  if (status == "available") return XeniaLiveNameStatusAvailable;
  if (status == "taken") return XeniaLiveNameStatusTaken;
  if (status == "reserved") return XeniaLiveNameStatusReserved;
  if (status == "not_allowed") return XeniaLiveNameStatusNotAllowed;
  if (status == "invalid") return XeniaLiveNameStatusInvalid;
  if (status == "too_soon") return XeniaLiveNameStatusTooSoon;
  if (status == "banned") return XeniaLiveNameStatusBanned;
  if (status == "limit") return XeniaLiveNameStatusLimit;
  return XeniaLiveNameStatusUnreachable;
}

XeniaLiveNameResult* NameResult(XeniaLiveNameStatus status) {
  XeniaLiveNameResult* result = [[[XeniaLiveNameResult alloc] init] autorelease];
  result.status = status;
  result.suggestions = @[];
  return result;
}

NSDate* DateFromMs(uint64_t ms) {
  return ms ? [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)ms / 1000.0] : nil;
}

NSError* Error(NSString* text) {
  return [NSError errorWithDomain:@"XeniaLiveAccount"
                             code:1
                         userInfo:@{NSLocalizedDescriptionKey : text}];
}

NSError* UnreachableError() {
  return Error(NSLocalizedString(@"Can’t reach XeniOS. Check your connection and try again.",
                                 @"Online account: the lobby didn't answer"));
}

NSError* NotYetError() {
  return Error(NSLocalizedString(@"Moving profile data isn’t available yet.",
                                 @"Online account: a profile data feature that isn't built"));
}

NSError* RefusedError(const std::string& request, const std::string& reason) {
  if (reason == "limit") {
    return Error(request == "link_code"
                     ? NSLocalizedString(@"You’ve made a lot of codes. Try again in an hour.",
                                         @"Online account: too many link codes")
                     : NSLocalizedString(@"Too many tries. Wait a while and try again.",
                                         @"Online account: rate limited"));
  }
  if (reason == "last_device") {
    return Error(NSLocalizedString(@"This is the account’s only device.",
                                   @"Online account: removing the last device"));
  }
  return Error(NSLocalizedString(@"XeniOS couldn’t do that. Try again in a moment.",
                                 @"Online account: any other refused request"));
}

XeniaLiveLinkStatus JoinStatus(const std::string& reason) {
  if (reason == "limit" || reason == "full") return XeniaLiveLinkStatusLimit;
  if (reason == "not_found" || reason == "denied" || reason == "bad_request") {
    return XeniaLiveLinkStatusBadCode;
  }
  return XeniaLiveLinkStatusUnreachable;
}

// 8 characters as K7Q2-M9XD.
NSString* FormatLinkCode(const std::string& code) {
  NSString* text = ToNS(code);
  if (text.length == 8) {
    return [NSString
        stringWithFormat:@"%@-%@", [text substringToIndex:4], [text substringFromIndex:4]];
  }
  return text;
}

std::optional<XeniaIOSLiveProfile> ActiveProfile() {
  if (!g_real_host || !g_real_host->active_profile) {
    return std::nullopt;
  }
  return g_real_host->active_profile();
}

// The active profile's open connection, signed in; null otherwise.
std::shared_ptr<live::AccountShare> SignedInShare() {
  auto share = xe_ios_live_social_current_share();
  if (!share || !(*share)->signed_in()) {
    return nullptr;
  }
  return share;
}

XeniaLiveDevice* DeviceFrom(const live::Device& device) {
  XeniaLiveDevice* result = [[[XeniaLiveDevice alloc] init] autorelease];
  result.deviceId = ToNS(device.key);
  const BOOL pad = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
  NSString* name = ToNS(device.name);
  if (!name.length) {
    if (device.current) {
      name = pad ? @"iPad" : @"iPhone";
    } else if (device.platform == "macos") {
      name = @"Mac";
    } else if (device.platform == "windows") {
      name = NSLocalizedString(@"Windows PC", @"Online account: a device with no name");
    } else if (device.platform == "linux") {
      name = NSLocalizedString(@"Linux PC", @"Online account: a device with no name");
    } else if (device.platform == "ios") {
      name = NSLocalizedString(@"iPhone or iPad", @"Online account: a device with no name");
    } else {
      name = NSLocalizedString(@"Device", @"Online account: a device with no name");
    }
  }
  result.name = name;
  if (device.current) {
    result.kind = pad ? @"ipad" : @"iphone";
  } else if (device.platform == "ios") {
    result.kind = [name rangeOfString:@"iPad"].location != NSNotFound ? @"ipad" : @"iphone";
  } else {
    result.kind = @"mac";
  }
  result.thisDevice = device.current;
  result.lastSeen = device.current ? nil : DateFromMs(device.seen_ms);
  return result;
}

void AddWaiter(NSMutableArray* list, id block) {
  id copy = [block copy];
  [list addObject:copy];
  [copy release];
}

// Takes every waiter out of |list|, for calling.
NSArray* TakeWaiters(NSMutableArray* list) {
  NSArray* taken = [[list copy] autorelease];
  [list removeAllObjects];
  return taken;
}

}  // namespace

@interface XeniaLiveAccountReal : NSObject <XeniaLiveAccountService>
+ (XeniaLiveAccountReal*)shared;
@end

@implementation XeniaLiveAccountReal {
  // Name checks on the socket, by the name asked about.
  NSMutableDictionary<NSString*, NSMutableArray*>* _checks;
  NSMutableArray* _linkCodes;
  NSMutableArray* _devices;
  NSMutableArray* _recoveryCodes;
  // Removals waiting for the device list without their device: {key, block}.
  NSMutableArray<NSArray*>* _removals;

  // Going online.
  void (^_goOnline)(XeniaLiveNameResult*);
  NSString* _goOnlineName;
  NSString* _goOnlineOldName;
  uint64_t _goOnlineProfile;
  NSString* _goOnlineRecovery;

  void (^_rename)(XeniaLiveNameResult*);
  NSString* _renameName;

  // Joining an account with a link or recovery code.
  std::shared_ptr<live::AccountClient> _join;
  void (^_joinDone)(XeniaLiveLinkStatus);
  uint64_t _joinMoved;
  NSUInteger _joinGeneration;

  // What the screens last saw, to post a change only when there is one.
  NSString* _lastSignature;
}

+ (XeniaLiveAccountReal*)shared {
  static XeniaLiveAccountReal* shared = [[XeniaLiveAccountReal alloc] init];
  return shared;
}

- (instancetype)init {
  if ((self = [super init])) {
    _checks = [[NSMutableDictionary alloc] init];
    _linkCodes = [[NSMutableArray alloc] init];
    _devices = [[NSMutableArray alloc] init];
    _recoveryCodes = [[NSMutableArray alloc] init];
    _removals = [[NSMutableArray alloc] init];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(socialChanged)
                                                 name:XeniaLiveSocialDidChangeNotification
                                               object:nil];
  }
  return self;
}

// Never deallocated (the shared instance), but kept complete.
- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_checks release];
  [_linkCodes release];
  [_devices release];
  [_recoveryCodes release];
  [_removals release];
  [_goOnline release];
  [_goOnlineName release];
  [_goOnlineOldName release];
  [_goOnlineRecovery release];
  [_rename release];
  [_renameName release];
  [_joinDone release];
  [_lastSignature release];
  [super dealloc];
}

#pragma mark Changes

- (void)socialChanged {
  XeniaLiveAccountSummary* summary = self.summary;
  NSString* signature = [NSString
      stringWithFormat:@"%d|%@|%f|%d", summary.online, summary.gamertag,
                       summary.nextRename.timeIntervalSince1970, summary.renameFlagged];
  if ([signature isEqualToString:_lastSignature]) {
    return;
  }
  [_lastSignature release];
  _lastSignature = [signature copy];
  [self postChange];
}

- (void)postChange {
  [[NSNotificationCenter defaultCenter] postNotificationName:XeniaLiveAccountDidChangeNotification
                                                      object:nil];
}

// The local profile takes the account's gamertag (renamed on another device, or after a link).
- (void)syncGamertag {
  auto share = SignedInShare();
  auto profile = ActiveProfile();
  if (!share || !profile || !g_real_host->rename_profile) {
    return;
  }
  const std::string gamertag = (*share)->gamertag();
  if (!gamertag.empty() && gamertag != profile->gamertag &&
      g_real_host->rename_profile(profile->xuid, gamertag)) {
    XELOGI("Live account (app): the profile is now {}", gamertag);
    [self postChange];
  }
}

#pragma mark Events (main thread)

- (void)signedIn {
  [self syncGamertag];
  if (_goOnline) {
    // The welcome's recovery code comes as the next event: finish after it.
    dispatch_async(dispatch_get_main_queue(), ^{
      [self finishGoOnline:XeniaLiveNameStatusAvailable];
    });
  }
  [self postChange];
}

- (void)nameChecked:(const std::string&)gamertag
             status:(const std::string&)status
        suggestions:(const std::vector<std::string>&)suggestions {
  NSString* key = ToNS(gamertag);
  NSMutableArray* waiters = _checks[key];
  if (!waiters) {
    return;
  }
  XeniaLiveNameResult* result = NameResult(NameStatus(status));
  NSMutableArray* names = [NSMutableArray array];
  for (const auto& name : suggestions) {
    [names addObject:ToNS(name)];
  }
  result.suggestions = names;
  NSArray* taken = TakeWaiters(waiters);
  [_checks removeObjectForKey:key];
  for (void (^waiter)(XeniaLiveNameResult*) in taken) {
    waiter(result);
  }
}

- (void)linkCode:(const std::string&)code ttl:(uint32_t)ttl_s {
  NSString* text = FormatLinkCode(code);
  NSDate* expires = [NSDate dateWithTimeIntervalSinceNow:ttl_s];
  for (void (^waiter)(NSString*, NSDate*, NSError*) in TakeWaiters(_linkCodes)) {
    waiter(text, expires, nil);
  }
}

- (void)recoveryCode:(const std::string&)code {
  NSString* text = ToNS(code);
  if (_goOnline) {
    [_goOnlineRecovery release];
    _goOnlineRecovery = [text copy];
    return;
  }
  for (void (^waiter)(NSString*, NSError*) in TakeWaiters(_recoveryCodes)) {
    waiter(text, nil);
  }
}

- (NSArray<XeniaLiveDevice*>*)currentDevices {
  NSMutableArray* devices = [NSMutableArray array];
  if (auto share = xe_ios_live_social_current_share()) {
    for (const auto& device : (*share)->Devices()) {
      [devices addObject:DeviceFrom(device)];
    }
  }
  // This device first.
  [devices sortWithOptions:NSSortStable
           usingComparator:^NSComparisonResult(XeniaLiveDevice* a, XeniaLiveDevice* b) {
             return a.thisDevice == b.thisDevice ? NSOrderedSame
                                                 : (a.thisDevice ? NSOrderedAscending
                                                                 : NSOrderedDescending);
           }];
  return devices;
}

- (void)devicesChanged {
  NSArray<XeniaLiveDevice*>* devices = [self currentDevices];
  for (void (^waiter)(NSArray*, NSError*) in TakeWaiters(_devices)) {
    waiter(devices, nil);
  }
  for (NSArray* removal in [[_removals copy] autorelease]) {
    NSString* key = removal[0];
    BOOL still = NO;
    for (XeniaLiveDevice* device in devices) {
      still = still || [device.deviceId isEqualToString:key];
    }
    if (!still) {
      [_removals removeObjectIdenticalTo:removal];
      ((void (^)(NSError*))removal[1])(nil);
    }
  }
}

- (void)profileChanged {
  [self syncGamertag];
  if (_rename) {
    auto share = xe_ios_live_social_current_share();
    if (share &&
        [ToNS((*share)->gamertag()).lowercaseString isEqualToString:_renameName.lowercaseString]) {
      [self finishRename:NameResult(XeniaLiveNameStatusAvailable)];
    }
  }
  [self postChange];
}

- (void)renameFailed:(const std::string&)reason next:(uint64_t)next_ms {
  if (!_rename) {
    return;
  }
  XeniaLiveNameResult* result = NameResult(NameStatus(reason));
  result.nextRename = DateFromMs(next_ms);
  [self finishRename:result];
}

- (void)registerRefused:(const std::string&)reason {
  if (_goOnline) {
    [self finishGoOnline:NameStatus(reason)];
  }
}

- (void)requestFailed:(const std::string&)request reason:(const std::string&)reason {
  XELOGW("Live account (app): {} refused: {}", request, reason);
  if (request == "check") {
    XeniaLiveNameResult* result =
        NameResult(reason == "limit" ? XeniaLiveNameStatusLimit : XeniaLiveNameStatusUnreachable);
    NSDictionary* all = [[_checks copy] autorelease];
    [_checks removeAllObjects];
    for (NSMutableArray* waiters in all.allValues) {
      for (void (^waiter)(XeniaLiveNameResult*) in waiters) {
        waiter(result);
      }
    }
  } else if (request == "rename" && _rename) {
    [self finishRename:NameResult(reason == "limit" ? XeniaLiveNameStatusLimit
                                                    : XeniaLiveNameStatusUnreachable)];
  } else if (request == "register" && _goOnline) {
    [self finishGoOnline:reason == "limit" ? XeniaLiveNameStatusLimit : NameStatus(reason)];
  } else if (request == "link_code") {
    NSError* error = RefusedError(request, reason);
    for (void (^waiter)(NSString*, NSDate*, NSError*) in TakeWaiters(_linkCodes)) {
      waiter(nil, nil, error);
    }
  } else if (request == "recovery_new") {
    NSError* error = RefusedError(request, reason);
    for (void (^waiter)(NSString*, NSError*) in TakeWaiters(_recoveryCodes)) {
      waiter(nil, error);
    }
  } else if (request == "devices") {
    NSError* error = RefusedError(request, reason);
    for (void (^waiter)(NSArray*, NSError*) in TakeWaiters(_devices)) {
      waiter(nil, error);
    }
  } else if (request == "remove_device") {
    NSError* error = RefusedError(request, reason);
    NSArray* removals = [[_removals copy] autorelease];
    [_removals removeAllObjects];
    for (NSArray* removal in removals) {
      ((void (^)(NSError*))removal[1])(error);
    }
  }
}

// Another device removed this one: the profile isn't on the account any more.
- (void)removedFromAccount {
  auto profile = ActiveProfile();
  if (profile && g_real_host->disable_online) {
    XELOGW("Live account (app): this device was removed from {}'s account", profile->gamertag);
    // Keeps the online XUID: a link or recovery code for the account later finds this profile
    // (and its saves) again instead of making another.
    g_real_host->disable_online(profile->xuid);
  }
  xe_ios_live_social_refresh_now();
  [self postChange];
}

#pragma mark XeniaLiveAccountService: the account

- (XeniaLiveAccountSummary*)summary {
  XeniaLiveAccountSummary* summary = [[[XeniaLiveAccountSummary alloc] init] autorelease];
  auto profile = ActiveProfile();
  summary.online = profile && profile->live_enabled && profile->online_xuid;
  summary.gamertag = profile ? ToNS(profile->gamertag) : @"";
  if (summary.online) {
    if (auto share = SignedInShare()) {
      const std::string gamertag = (*share)->gamertag();
      if (!gamertag.empty()) {
        summary.gamertag = ToNS(gamertag);
      }
      NSDate* next = DateFromMs((*share)->next_rename_ms());
      summary.nextRename = next.timeIntervalSinceNow > 0 ? next : nil;
      summary.renameFlagged = !(*share)->rename_prompt().empty();
    }
  }
  return summary;
}

- (void)checkGamertag:(NSString*)gamertag completion:(void (^)(XeniaLiveNameResult*))completion {
  if (auto share = SignedInShare()) {
    NSMutableArray* waiters = _checks[gamertag];
    if (!waiters) {
      waiters = [NSMutableArray array];
      _checks[gamertag] = waiters;
    }
    AddWaiter(waiters, completion);
    id waiter = waiters.lastObject;
    if ((*share)->CheckName(ToStd(gamertag))) {
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kAnswerTimeout * NSEC_PER_SEC)),
                     dispatch_get_main_queue(), ^{
                       NSMutableArray* list = _checks[gamertag];
                       if ([list indexOfObjectIdenticalTo:waiter] != NSNotFound) {
                         [[waiter retain] autorelease];
                         [list removeObjectIdenticalTo:waiter];
                         ((void (^)(XeniaLiveNameResult*))waiter)(
                             NameResult(XeniaLiveNameStatusUnreachable));
                       }
                     });
      return;
    }
    [waiters removeObjectIdenticalTo:waiter];
  }
  [self checkOverHTTP:gamertag completion:completion];
}

// Before the profile is online: the lobby's web form.
- (void)checkOverHTTP:(NSString*)gamertag completion:(void (^)(XeniaLiveNameResult*))completion {
  void (^done)(XeniaLiveNameResult*) = [[completion copy] autorelease];
  const std::string lobby = xe::kernel::xam::LiveSessionLobby();
  if (lobby.empty() || !xe_ios_online_features_enabled()) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(NameResult(XeniaLiveNameStatusUnreachable));
    });
    return;
  }
  NSURL* url = [NSURL URLWithString:ToNS(live::NameCheckUrl(lobby))];
  if (!url) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(NameResult(XeniaLiveNameStatusUnreachable));
    });
    return;
  }
  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
  request.HTTPMethod = @"POST";
  request.timeoutInterval = kAnswerTimeout;
  [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
  const std::string body = live::EncodeNameCheckBody(ToStd(gamertag));
  request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
  const std::string name = ToStd(gamertag);
  NSURLSessionDataTask* task = [[NSURLSession sharedSession]
      dataTaskWithRequest:request
        completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
          const NSInteger code =
              [response isKindOfClass:[NSHTTPURLResponse class]]
                  ? ((NSHTTPURLResponse*)response).statusCode
                  : 0;
          XeniaLiveNameResult* result = nil;
          if (code == 429) {
            result = NameResult(XeniaLiveNameStatusLimit);
          } else if (!error && code == 200 && data) {
            auto check = live::ParseNameCheckResponse(
                name, std::string_view(static_cast<const char*>(data.bytes), data.length));
            if (check) {
              result = NameResult(NameStatus(check->status));
              NSMutableArray* names = [NSMutableArray array];
              for (const auto& suggestion : check->suggestions) {
                [names addObject:ToNS(suggestion)];
              }
              result.suggestions = names;
            }
          }
          if (!result) {
            result = NameResult(XeniaLiveNameStatusUnreachable);
          }
          dispatch_async(dispatch_get_main_queue(), ^{
            done(result);
          });
        }];
  [task resume];
}

- (void)goOnlineWithGamertag:(NSString*)gamertag
                  completion:(void (^)(XeniaLiveNameResult*))completion {
  void (^done)(XeniaLiveNameResult*) = [[completion copy] autorelease];
  auto profile = ActiveProfile();
  if (!profile || _goOnline || !g_real_host->enable_online || !g_real_host->rename_profile) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(NameResult(XeniaLiveNameStatusUnreachable));
    });
    return;
  }
  if (profile->live_enabled && profile->online_xuid) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(NameResult(XeniaLiveNameStatusAvailable));
    });
    return;
  }
  const uint64_t profile_xuid = profile->xuid;
  NSString* old_name = ToNS(profile->gamertag);
  // Nothing changes on this device until the name is free.
  [self checkOverHTTP:gamertag
           completion:^(XeniaLiveNameResult* check) {
             if (check.status != XeniaLiveNameStatusAvailable) {
               done(check);
               return;
             }
             auto now = ActiveProfile();
             if (!now || now->xuid != profile_xuid || _goOnline) {
               done(NameResult(XeniaLiveNameStatusUnreachable));
               return;
             }
             _goOnline = [done copy];
             _goOnlineName = [gamertag copy];
             _goOnlineOldName = [old_name copy];
             _goOnlineProfile = profile_xuid;
             if (!g_real_host->rename_profile(profile_xuid, ToStd(gamertag)) ||
                 !g_real_host->enable_online(profile_xuid)) {
               [self finishGoOnline:XeniaLiveNameStatusUnreachable];
               return;
             }
             XELOGI("Live account (app): going online as {}", ToStd(gamertag));
             xe_ios_live_social_refresh_now();
             [self postChange];
             void (^waiting)(XeniaLiveNameResult*) = _goOnline;
             dispatch_after(
                 dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kConnectTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_goOnline == waiting) {
                     [self finishGoOnline:XeniaLiveNameStatusUnreachable];
                   }
                 });
           }];
}

// Available: the account is made (or signed in). Anything else puts the profile back as it was.
- (void)finishGoOnline:(XeniaLiveNameStatus)status {
  if (!_goOnline) {
    return;
  }
  void (^done)(XeniaLiveNameResult*) = [_goOnline autorelease];
  _goOnline = nil;
  XeniaLiveNameResult* result = NameResult(status);
  if (status == XeniaLiveNameStatusAvailable) {
    result.recoveryCode = _goOnlineRecovery;
  } else {
    XELOGW("Live account (app): going online failed ({}), so the profile stays offline",
           (int)status);
    if (g_real_host->disable_online) {
      g_real_host->disable_online(_goOnlineProfile);
    }
    if (_goOnlineOldName.length) {
      g_real_host->rename_profile(_goOnlineProfile, ToStd(_goOnlineOldName));
    }
    xe_ios_live_social_refresh_now();
  }
  [_goOnlineName release];
  _goOnlineName = nil;
  [_goOnlineOldName release];
  _goOnlineOldName = nil;
  [_goOnlineRecovery release];
  _goOnlineRecovery = nil;
  _goOnlineProfile = 0;
  [self postChange];
  done(result);
}

- (void)renameToGamertag:(NSString*)gamertag completion:(void (^)(XeniaLiveNameResult*))completion {
  void (^done)(XeniaLiveNameResult*) = [[completion copy] autorelease];
  auto share = SignedInShare();
  if (!share || _rename) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(NameResult(XeniaLiveNameStatusUnreachable));
    });
    return;
  }
  _rename = [done copy];
  _renameName = [gamertag copy];
  (*share)->Rename(ToStd(gamertag));
  void (^waiting)(XeniaLiveNameResult*) = _rename;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kAnswerTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_rename == waiting) {
                     [self finishRename:NameResult(XeniaLiveNameStatusUnreachable)];
                   }
                 });
}

- (void)finishRename:(XeniaLiveNameResult*)result {
  if (!_rename) {
    return;
  }
  void (^done)(XeniaLiveNameResult*) = [_rename autorelease];
  _rename = nil;
  [_renameName release];
  _renameName = nil;
  done(result);
}

#pragma mark Devices

// Adds |completion| to |list| and fails it with |timeout| if nothing answers in time.
- (void)wait:(id)completion in:(NSMutableArray*)list timeout:(void (^)(id waiter))timeout {
  AddWaiter(list, completion);
  id waiter = list.lastObject;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kAnswerTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if ([list indexOfObjectIdenticalTo:waiter] != NSNotFound) {
                     [[waiter retain] autorelease];
                     [list removeObjectIdenticalTo:waiter];
                     timeout(waiter);
                   }
                 });
}

- (void)createLinkCodeWithCompletion:(void (^)(NSString*, NSDate*, NSError*))completion {
  auto share = SignedInShare();
  if (!share) {
    void (^done)(NSString*, NSDate*, NSError*) = [[completion copy] autorelease];
    dispatch_async(dispatch_get_main_queue(), ^{
      done(nil, nil, UnreachableError());
    });
    return;
  }
  [self wait:completion
            in:_linkCodes
       timeout:^(id waiter) {
         ((void (^)(NSString*, NSDate*, NSError*))waiter)(nil, nil, UnreachableError());
       }];
  (*share)->RequestLinkCode();
}

- (void)loadDevicesWithCompletion:(void (^)(NSArray<XeniaLiveDevice*>*, NSError*))completion {
  auto share = SignedInShare();
  if (!share) {
    void (^done)(NSArray*, NSError*) = [[completion copy] autorelease];
    dispatch_async(dispatch_get_main_queue(), ^{
      done(nil, UnreachableError());
    });
    return;
  }
  [self wait:completion
            in:_devices
       timeout:^(id waiter) {
         ((void (^)(NSArray*, NSError*))waiter)(nil, UnreachableError());
       }];
  (*share)->RequestDevices();
}

- (void)removeDevice:(XeniaLiveDevice*)device completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [[completion copy] autorelease];
  auto share = SignedInShare();
  if (!share) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(UnreachableError());
    });
    return;
  }
  NSArray* removal = @[ [[device.deviceId copy] autorelease], done ];
  [_removals addObject:removal];
  (*share)->RemoveDevice(ToStd(device.deviceId));
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kAnswerTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if ([_removals indexOfObjectIdenticalTo:removal] != NSNotFound) {
                     [[removal retain] autorelease];
                     [_removals removeObjectIdenticalTo:removal];
                     done(UnreachableError());
                   }
                 });
}

- (void)createRecoveryCodeWithCompletion:(void (^)(NSString*, NSError*))completion {
  auto share = SignedInShare();
  if (!share) {
    void (^done)(NSString*, NSError*) = [[completion copy] autorelease];
    dispatch_async(dispatch_get_main_queue(), ^{
      done(nil, UnreachableError());
    });
    return;
  }
  [self wait:completion
            in:_recoveryCodes
       timeout:^(id waiter) {
         ((void (^)(NSString*, NSError*))waiter)(nil, UnreachableError());
       }];
  (*share)->NewRecoveryCode();
}

#pragma mark Joining an account on this device

- (void)linkWithCode:(NSString*)code completion:(void (^)(XeniaLiveLinkStatus))completion {
  [self joinWithCode:code recover:NO completion:completion];
}

- (void)recoverWithCode:(NSString*)code completion:(void (^)(XeniaLiveLinkStatus))completion {
  [self joinWithCode:code recover:YES completion:completion];
}

- (void)joinWithCode:(NSString*)typed
             recover:(BOOL)recover
          completion:(void (^)(XeniaLiveLinkStatus))completion {
  void (^done)(XeniaLiveLinkStatus) = [[completion copy] autorelease];
  const std::string code = recover ? live::NormalizeRecoveryCode(ToStd(typed))
                                   : live::NormalizeLinkCode(ToStd(typed));
  if (code.empty()) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(XeniaLiveLinkStatusBadCode);
    });
    return;
  }
  const std::string lobby = xe::kernel::xam::LiveSessionLobby();
  std::optional<live::DeviceKey> key;
  if (!lobby.empty() && xe_ios_online_features_enabled() && g_real_host &&
      g_real_host->adopt_account) {
    key = live::DeviceKey::LoadOrCreate(g_real_host->storage_root / "live" / "device_key");
  }
  if (!key) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(XeniaLiveLinkStatusUnreachable);
    });
    return;
  }
  [self finishJoin:XeniaLiveLinkStatusUnreachable gamertag:std::string()];

  const NSUInteger generation = ++_joinGeneration;
  live::AccountClient::Events events;
  events.moved = [generation](uint64_t xuid) {
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveAccountReal* service = [XeniaLiveAccountReal shared];
      if (service->_joinGeneration == generation) {
        service->_joinMoved = xuid;
      }
    });
  };
  events.signed_in = [generation](uint64_t xuid, const std::string& friend_code) {
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveAccountReal* service = [XeniaLiveAccountReal shared];
      if (service->_joinGeneration != generation || !service->_join) {
        return;
      }
      service->_joinMoved = xuid;
      [service finishJoin:XeniaLiveLinkStatusLinked gamertag:service->_join->gamertag()];
    });
  };
  events.join_failed = [generation](const std::string& reason) {
    const std::string copy = reason;
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveAccountReal* service = [XeniaLiveAccountReal shared];
      if (service->_joinGeneration == generation) {
        XELOGW("Live account (app): joining the account failed: {}", copy);
        [service finishJoin:JoinStatus(copy) gamertag:std::string()];
      }
    });
  };
  live::AccountOptions options;
  options.join = live::AccountJoin{
      recover ? live::AccountJoin::Mode::kRecover : live::AccountJoin::Mode::kLink, code};
  _joinDone = [done copy];
  _joinMoved = 0;
  _join = std::make_shared<live::AccountClient>(lobby, std::move(*key), 0, std::string(),
                                                std::move(events), std::move(options));
  XELOGI("Live account (app): joining an account with a {} code", recover ? "recovery" : "link");
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kConnectTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_joinGeneration == generation && _joinDone) {
                     // Moved but never signed in: the device is on the account all the same.
                     [self finishJoin:_joinMoved ? XeniaLiveLinkStatusLinked
                                                 : XeniaLiveLinkStatusUnreachable
                             gamertag:std::string()];
                   }
                 });
}

- (void)finishJoin:(XeniaLiveLinkStatus)status gamertag:(std::string)gamertag {
  if (_join) {
    // Closing waits for the client's thread: not on the main thread.
    auto* doomed = new std::shared_ptr<live::AccountClient>(std::move(_join));
    _join.reset();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      delete doomed;
    });
  }
  if (!_joinDone) {
    return;
  }
  void (^done)(XeniaLiveLinkStatus) = [_joinDone autorelease];
  _joinDone = nil;
  ++_joinGeneration;
  if (status == XeniaLiveLinkStatusLinked) {
    const uint64_t online_xuid = _joinMoved;
    if (!online_xuid || !g_real_host->adopt_account(online_xuid, gamertag)) {
      status = XeniaLiveLinkStatusUnreachable;
    } else {
      XELOGI("Live account (app): this device joined {:016X} ({})", online_xuid, gamertag);
      // A connection opened for this account before the join was refused (this key wasn't on it
      // yet), and may have been moved to another XUID: sign in again on a new one.
      live::ForgetAccount(xe::kernel::xam::LiveSessionLobby(), online_xuid);
      xe_ios_live_social_reconnect();
      [self postChange];
    }
  }
  _joinMoved = 0;
  done(status);
}

#pragma mark Moving a profile's data: not built yet

- (BOOL)supportsProfileData {
  return NO;
}

- (void)exportSizesWithCompletion:(void (^)(uint64_t, uint64_t, uint64_t))completion {
  void (^done)(uint64_t, uint64_t, uint64_t) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(0, 0, 0);
  });
}

- (void)exportProfileWithOptions:(XeniaLiveExportOptions*)options
                      completion:(void (^)(NSURL*, NSError*))completion {
  void (^done)(NSURL*, NSError*) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(nil, NotYetError());
  });
}

- (void)importProfileFromURL:(NSURL*)url completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(NotYetError());
  });
}

- (void)localProfilesWithCompletion:(void (^)(NSArray<XeniaLiveLocalProfile*>*))completion {
  void (^done)(NSArray*) = [[completion copy] autorelease];
  NSMutableArray* result = [NSMutableArray array];
  auto active = ActiveProfile();
  if (g_real_host && g_real_host->profiles) {
    for (const auto& profile : g_real_host->profiles()) {
      XeniaLiveLocalProfile* entry = [[[XeniaLiveLocalProfile alloc] init] autorelease];
      entry.profileId = [NSString stringWithFormat:@"%016llX", (unsigned long long)profile.xuid];
      entry.gamertag = ToNS(profile.gamertag);
      entry.online = profile.live_enabled && profile.online_xuid;
      entry.thisProfile = active && active->xuid == profile.xuid;
      if (entry.thisProfile) {
        [result insertObject:entry atIndex:0];
      } else {
        [result addObject:entry];
      }
    }
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    done(result);
  });
}

- (BOOL)savesSharedAcrossProfiles {
  return NO;
}

- (void)setSavesSharedAcrossProfiles:(BOOL)shared completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(NotYetError());
  });
}

- (void)saveGamesSharedWithProfile:(NSString*)otherProfileId
                        completion:(void (^)(NSArray<XeniaLiveSaveGame*>*))completion {
  void (^done)(NSArray*) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(@[]);
  });
}

- (void)setSharing:(BOOL)shared
          forTitle:(uint32_t)titleId
       withProfile:(NSString*)otherProfileId
        completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(NotYetError());
  });
}

- (void)copySavesToProfile:(NSString*)otherProfileId
                completion:(void (^)(NSUInteger, NSUInteger, NSError*))completion {
  void (^done)(NSUInteger, NSUInteger, NSError*) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(0, 0, NotYetError());
  });
}

#pragma mark Approving on another device: not in the lobby

- (XeniaLiveIncomingApproval*)pendingApproval {
  return nil;
}

- (void)requestApprovalForGamertag:(NSString*)gamertag
                        completion:(void (^)(XeniaLiveApprovalRequest*,
                                             XeniaLiveApprovalStatus))completion {
  void (^done)(XeniaLiveApprovalRequest*, XeniaLiveApprovalStatus) =
      [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(nil, XeniaLiveApprovalStatusUnreachable);
  });
}

- (void)requestQRLinkWithCompletion:(void (^)(XeniaLiveApprovalRequest*,
                                              XeniaLiveApprovalStatus))completion {
  [self requestApprovalForGamertag:@"" completion:completion];
}

- (void)waitForApproval:(XeniaLiveApprovalRequest*)request
             completion:(void (^)(XeniaLiveApprovalStatus))completion {
  void (^done)(XeniaLiveApprovalStatus) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(XeniaLiveApprovalStatusUnreachable);
  });
}

- (void)cancelApproval:(XeniaLiveApprovalRequest*)request {}

- (void)approveIncoming:(XeniaLiveIncomingApproval*)approval
                 chosen:(NSString*)chosen
             completion:(void (^)(XeniaLiveApprovalStatus))completion {
  void (^done)(XeniaLiveApprovalStatus) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(XeniaLiveApprovalStatusUnreachable);
  });
}

- (void)denyIncoming:(XeniaLiveIncomingApproval*)approval {}

- (void)beginApprovalFromQRPayload:(NSString*)payload
                        completion:(void (^)(XeniaLiveIncomingApproval*,
                                             XeniaLiveLinkStatus))completion {
  void (^done)(XeniaLiveIncomingApproval*, XeniaLiveLinkStatus) = [[completion copy] autorelease];
  dispatch_async(dispatch_get_main_queue(), ^{
    done(nil, XeniaLiveLinkStatusBadCode);
  });
}

@end

void xe_ios_live_account_real_install(const XeniaIOSLiveSocialHost* host) {
  g_real_host = host;
  dispatch_async(dispatch_get_main_queue(), ^{
    xe_ios_live_account_set_service([XeniaLiveAccountReal shared]);
  });
}

bool xe_ios_live_account_real_handles(const std::string& request) {
  return request == "check" || request == "rename" || request == "register" ||
         request == "link_code" || request == "recovery_new" || request == "devices" ||
         request == "remove_device";
}

void xe_ios_live_account_real_add_events(live::AccountClient::Events& events) {
  auto on_main = [](void (^block)(XeniaLiveAccountReal*)) {
    void (^copy)(XeniaLiveAccountReal*) = [block copy];
    dispatch_async(dispatch_get_main_queue(), ^{
      copy([XeniaLiveAccountReal shared]);
      [copy release];
    });
  };
  auto signed_in = std::move(events.signed_in);
  events.signed_in = [signed_in, on_main](uint64_t xuid, const std::string& code) {
    if (signed_in) signed_in(xuid, code);
    on_main(^(XeniaLiveAccountReal* service) {
      [service signedIn];
    });
  };
  auto request_failed = std::move(events.request_failed);
  events.request_failed = [request_failed, on_main](const std::string& request,
                                                    const std::string& reason) {
    if (request_failed) request_failed(request, reason);
    const std::string r = request, why = reason;
    on_main(^(XeniaLiveAccountReal* service) {
      [service requestFailed:r reason:why];
    });
  };
  events.name_checked = [on_main](const std::string& gamertag, const std::string& status,
                                  const std::vector<std::string>& suggestions) {
    const std::string name = gamertag, s = status;
    const std::vector<std::string> names = suggestions;
    on_main(^(XeniaLiveAccountReal* service) {
      [service nameChecked:name status:s suggestions:names];
    });
  };
  events.link_code = [on_main](const std::string& code, uint32_t ttl_s) {
    const std::string copy = code;
    on_main(^(XeniaLiveAccountReal* service) {
      [service linkCode:copy ttl:ttl_s];
    });
  };
  events.recovery_code = [on_main](const std::string& code) {
    const std::string copy = code;
    on_main(^(XeniaLiveAccountReal* service) {
      [service recoveryCode:copy];
    });
  };
  events.devices_changed = [on_main] {
    on_main(^(XeniaLiveAccountReal* service) {
      [service devicesChanged];
    });
  };
  events.profile_changed = [on_main] {
    on_main(^(XeniaLiveAccountReal* service) {
      [service profileChanged];
    });
  };
  events.rename_failed = [on_main](const std::string& reason, uint64_t next_ms) {
    const std::string copy = reason;
    on_main(^(XeniaLiveAccountReal* service) {
      [service renameFailed:copy next:next_ms];
    });
  };
  events.register_refused = [on_main](const std::string& gamertag, const std::string& reason) {
    const std::string copy = reason;
    on_main(^(XeniaLiveAccountReal* service) {
      [service registerRefused:copy];
    });
  };
  events.removed = [on_main] {
    on_main(^(XeniaLiveAccountReal* service) {
      [service removedFromAccount];
    });
  };
}
