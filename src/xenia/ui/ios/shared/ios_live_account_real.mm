/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_network_status.h"
#import "xenia/ui/ios/shared/ios_live_account_real.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/kernel/xam/profile_package.h"
#include "xenia/kernel/xam/xam_system_link.h"
#import "xenia/ui/ios/shared/ios_live_account.h"
#import "xenia/ui/ios/shared/ios_live_social_bridge.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_profile_data.h"

namespace live = xe::net::live;
using xe::kernel::xam::ProfilePackageError;

namespace {

const XeniaIOSLiveSocialHost* g_real_host = nullptr;

// How long a request waits for the lobby's answer.
constexpr NSTimeInterval kAnswerTimeout = 15.0;
// Deleting an account needs a connection already or very soon: the player is waiting on it.
constexpr NSTimeInterval kDeleteConnectTimeout = 6.0;
// How long deleting a profile from this device waits for the lobby to take the device off.
constexpr NSTimeInterval kRemoveThisDeviceTimeout = 1.5;
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
  return Error(xe_ios_connection_failure_text(
      NSLocalizedString(@"Can’t reach XeniOS. Check your connection and try again.",
                        @"Online account: the lobby didn't answer")));
}

NSError* DeleteResultUnknownError() {
  return [NSError errorWithDomain:@"XeniaLiveAccount"
                             code:XeniaLiveAccountErrorResultUnknown
                         userInfo:@{
                           NSLocalizedDescriptionKey :
                               NSLocalizedString(@"The online service didn’t answer.",
                                                 @"Online account: delete account got no answer")
                         }];
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
    return Error(NSLocalizedString(@"This is the account’s only device. Add another device "
                                   @"before removing it.",
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

// Posts XeniaProfileDataProgressNotification on the main queue, once per percent.
xe::kernel::xam::ProfilePackageProgress ProfileDataProgress() {
  auto last = std::make_shared<std::atomic<int>>(-1);
  return [last](uint64_t done, uint64_t total) {
    const int percent = total ? int(done * 100 / total) : 100;
    if (last->exchange(percent) == percent) {
      return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaProfileDataProgressNotification
                        object:nil
                      userInfo:@{
                        XeniaProfileDataProgressFractionKey : @(percent / 100.0)
                      }];
    });
  };
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

// What the signed-in device is asked about |request|: the new device and, unless it was scanned,
// the number to pick from three.
XeniaLiveIncomingApproval* IncomingFrom(const live::JoinRequest& request) {
  XeniaLiveIncomingApproval* result = [[[XeniaLiveIncomingApproval alloc] init] autorelease];
  result.requestId = ToNS(request.id);
  NSString* name = ToNS(request.device.name);
  const std::string& platform = request.device.platform;
  if (platform == "ios") {
    result.platform = [name rangeOfString:@"iPad"].location != NSNotFound ? @"ipad" : @"iphone";
    result.platformText = [result.platform isEqualToString:@"ipad"] ? @"iPad" : @"iPhone";
  } else {
    result.platform = @"mac";
    result.platformText = platform == "windows" ? @"Windows PC"
                          : platform == "linux" ? @"Linux PC"
                                                : @"Mac";
  }
  result.deviceName = name.length ? name : result.platformText;
  result.expires = [NSDate dateWithTimeIntervalSinceNow:request.ttl_s];
  NSMutableArray* choices = [NSMutableArray array];
  if (!request.number.empty()) {
    [choices addObject:ToNS(request.number)];
    while (choices.count < 3) {
      NSString* other = [NSString stringWithFormat:@"%02u", 10 + arc4random_uniform(90)];
      if (![choices containsObject:other]) {
        [choices insertObject:other atIndex:arc4random_uniform((uint32_t)choices.count + 1)];
      }
    }
  }
  result.choices = choices;
  return result;
}

void AddWaiterForKey(NSMutableDictionary* list, NSString* key, id block) {
  id copy = [block copy];
  list[key] = copy;
  [copy release];
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
  // Why the join failed, as the lobby said it ("declined", "expired"), or "cancelled".
  std::string _joinFailure;
  // The recovery code a recovery brought, shown once by the next createRecoveryCode.
  NSString* _joinRecovery;
  NSString* _recoveredCode;

  // This device asking the account's others to let it in.
  void (^_approvalStarted)(XeniaLiveApprovalRequest*, XeniaLiveApprovalStatus);
  void (^_approvalWait)(XeniaLiveApprovalStatus);
  XeniaLiveApprovalRequest* _approvalRequest;
  NSNumber* _approvalOutcome;  // It ended before waitForApproval: was called.

  // Another device asking this one: the sheet's request, the number each request showed ("" when
  // scanned), the scan waiting for its request, and approvals waiting for join_closed.
  XeniaLiveIncomingApproval* _incoming;
  NSMutableDictionary<NSString*, NSString*>* _incomingNumbers;
  void (^_peekDone)(XeniaLiveIncomingApproval*, XeniaLiveLinkStatus);
  NSString* _peekId;
  NSMutableDictionary<NSString*, void (^)(XeniaLiveApprovalStatus)>* _approving;

  // The profile whose device was removed from its account; offline until it joins one again.
  uint64_t _removedProfile;
  // Deleting the account before the profile is deleted (deleteAccountOfProfile:).
  void (^_deleteDone)(NSError*);
  uint64_t _deleteProfile;
  BOOL _deleteAsked;
  // The profile whose delete got no answer: the lobby may have done it, which it says next time.
  uint64_t _deleteUnansweredProfile;
  // Taking this device off the account as its profile is deleted (removeThisDeviceOfProfile:).
  void (^_removeThisDeviceDone)(void);
  // The lobby's answer to that is expected, even after the wait for it ended: the profile may be
  // gone by then, and no "removed" notice is due for whichever one is active.
  BOOL _removeThisDeviceAsked;
  NSUInteger _removeThisDeviceRequest;

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
    _incomingNumbers = [[NSMutableDictionary alloc] init];
    _approving = [[NSMutableDictionary alloc] init];
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
  [_deleteDone release];
  [_removeThisDeviceDone release];
  [_goOnline release];
  [_goOnlineName release];
  [_goOnlineOldName release];
  [_goOnlineRecovery release];
  [_rename release];
  [_renameName release];
  [_joinDone release];
  [_joinRecovery release];
  [_recoveredCode release];
  [_approvalStarted release];
  [_approvalWait release];
  [_approvalRequest release];
  [_approvalOutcome release];
  [_incoming release];
  [_incomingNumbers release];
  [_peekDone release];
  [_peekId release];
  [_approving release];
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
  [self continueDelete];
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
    return;
  }
  // Signing in with the profile's name was refused, and no one will answer what was asked
  // meanwhile (it is queued behind the sign-in): say so now instead of at the timeouts.
  XELOGW("Live account (app): the service refused the profile's gamertag ({})", reason);
  [self failWaitingRequests];
}

- (void)failWaitingRequests {
  if (_rename) {
    [self finishRename:NameResult(XeniaLiveNameStatusUnreachable)];
  }
  NSDictionary* checks = [[_checks copy] autorelease];
  [_checks removeAllObjects];
  for (NSMutableArray* waiters in checks.allValues) {
    for (void (^waiter)(XeniaLiveNameResult*) in waiters) {
      waiter(NameResult(XeniaLiveNameStatusUnreachable));
    }
  }
  NSError* error = UnreachableError();
  for (void (^waiter)(NSString*, NSDate*, NSError*) in TakeWaiters(_linkCodes)) {
    waiter(nil, nil, error);
  }
  for (void (^waiter)(NSString*, NSError*) in TakeWaiters(_recoveryCodes)) {
    waiter(nil, error);
  }
  for (void (^waiter)(NSArray*, NSError*) in TakeWaiters(_devices)) {
    waiter(nil, error);
  }
  NSArray* removals = [[_removals copy] autorelease];
  [_removals removeAllObjects];
  for (NSArray* removal in removals) {
    ((void (^)(NSError*))removal[1])(error);
  }
  [self finishDelete:error];
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
  } else if (request == "join_approve") {
    // The lobby doesn't say which one: every approval waiting fails.
    NSArray* waiters = [[_approving allValues] copy];
    [_approving removeAllObjects];
    for (void (^waiter)(XeniaLiveApprovalStatus) in waiters) {
      waiter(reason == "not_found" ? XeniaLiveApprovalStatusTimedOut
             : reason == "full"    ? XeniaLiveApprovalStatusDenied
                                   : XeniaLiveApprovalStatusUnreachable);
    }
    [waiters release];
  } else if (request == "join_peek" && _peekDone) {
    void (^done)(XeniaLiveIncomingApproval*, XeniaLiveLinkStatus) = [_peekDone autorelease];
    _peekDone = nil;
    done(nil, reason == "not_found" ? XeniaLiveLinkStatusBadCode
              : reason == "limit"   ? XeniaLiveLinkStatusLimit
                                    : XeniaLiveLinkStatusUnreachable);
  } else if (request == "delete_account") {
    [self finishDelete:RefusedError(request, reason)];
  } else if (request == "remove_device" && _removeThisDeviceAsked) {
    // "last_device": the account keeps it.
    _removeThisDeviceAsked = NO;
    [self finishRemoveThisDevice];
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
  if (_removeThisDeviceAsked) {
    _removeThisDeviceAsked = NO;
    [self finishRemoveThisDevice];
    return;
  }
  auto profile = ActiveProfile();
  if (profile && g_real_host->disable_online) {
    XELOGW("Live account (app): this device was removed from {}'s account", profile->gamertag);
    // Keeps the online XUID: a link or recovery code for the account later finds this profile
    // (and its saves) again instead of making another.
    g_real_host->disable_online(profile->xuid);
  }
  // Whatever the profile's flags say, it is signed out until a link or recovery code brings it
  // back (summary reads this).
  if (profile) {
    _removedProfile = profile->xuid;
  }
  xe_ios_live_social_refresh_now();
  [self postChange];
  if (!profile) {
    return;
  }
  // Said, with the way back: never a profile that just goes offline.
  NSString* message = [NSString
      stringWithFormat:NSLocalizedString(@"This %1$@ was removed from %2$@’s account. To sign in "
                                         @"again, use a code from one of its other devices.",
                                         @"Removed from the account; %1$@ is this device's kind "
                                         @"(iPhone, iPad), %2$@ the gamertag"),
                       [UIDevice currentDevice].model, ToNS(profile->gamertag)];
  [[NSNotificationCenter defaultCenter]
      postNotificationName:XeniaLiveSocialSignedOutNotification
                    object:[XeniaLiveSocial shared]
                  userInfo:@{
                    XeniaLiveSocialMessageKey : message,
                    XeniaLiveSocialSignedOutReasonKey : @"removed",
                  }];
}

// The account was deleted, here or on another device.
- (void)accountDeleted {
  auto profile = ActiveProfile();
  // This app asked for it, to delete the profile: nothing to tell the player. If a sign-in found
  // it already deleted, that was the answer to an earlier try.
  const BOOL own = _deleteDone && (_deleteAsked || (profile && profile->xuid == _deleteProfile));
  if (!profile) {
    if (own) {
      [self finishDelete:nil];
    }
    return;
  }
  XELOGW("Live account (app): {}'s account was deleted", profile->gamertag);
  // Online play off first: a Live profile with no online XUID is given a new one.
  if (g_real_host->disable_online) {
    g_real_host->disable_online(profile->xuid);
  }
  // Its XUID can't be used again: a new gamertag starts from none.
  if (g_real_host->set_online_xuid) {
    g_real_host->set_online_xuid(profile->xuid, 0);
  }
  _removedProfile = profile->xuid;
  xe_ios_live_social_refresh_now();
  [self postChange];
  // Kept offline first, in case the profile's files then can't be removed.
  if (own) {
    [self finishDelete:nil];
    return;
  }
  const BOOL asked_here = profile->xuid == _deleteUnansweredProfile;
  if (asked_here) {
    _deleteUnansweredProfile = 0;
  }
  NSString* message = [NSString
      stringWithFormat:asked_here
                           ? NSLocalizedString(
                                 @"%1$@’s online account was deleted, as you asked from this "
                                 @"%2$@. The profile and its saves stay here, offline.",
                                 @"Account deleted by an earlier request from this device; %1$@ "
                                 @"is the gamertag, %2$@ this device's kind (iPhone, iPad)")
                           : NSLocalizedString(
                                 @"%1$@’s online account was deleted on another device. "
                                 @"The profile and its saves stay on this %2$@, offline.",
                                 @"Account deleted; %1$@ is the gamertag, %2$@ this "
                                 @"device's kind (iPhone, iPad)"),
                       ToNS(profile->gamertag), [UIDevice currentDevice].model];
  [[NSNotificationCenter defaultCenter]
      postNotificationName:XeniaLiveSocialSignedOutNotification
                    object:[XeniaLiveSocial shared]
                  userInfo:@{
                    XeniaLiveSocialMessageKey : message,
                    XeniaLiveSocialSignedOutReasonKey : @"deleted",
                  }];
}

// The account signed in on another device, which ended its session here: one device at a time,
// as on the console. The profile goes offline, so starting the app again doesn't take the account
// back; Turn On Online Play does, with no code (this device stays on the account).
- (void)signedInElsewhere:(NSString*)device {
  auto profile = ActiveProfile();
  if (!profile) {
    return;
  }
  XELOGW("Live account (app): {} signed in on another device", profile->gamertag);
  if (g_real_host->disable_online) {
    g_real_host->disable_online(profile->xuid);
  }
  xe_ios_live_social_refresh_now();
  [self postChange];
  // Where it went, and that this device is offline now: "Reality signed in on Reality's iPhone.
  // You're offline on this iPad."
  NSString* gamertag = ToNS(profile->gamertag);
  NSString* where =
      device.length ? device
                    : NSLocalizedString(@"another device",
                                        @"Signed in elsewhere: the device, when it gave no name");
  NSString* message = [NSString
      stringWithFormat:NSLocalizedString(@"%1$@ signed in on %2$@. You’re offline on this %3$@.",
                                         @"Signed in elsewhere; %1$@ is the gamertag, %2$@ the "
                                         @"other device's name (or \"another device\"), %3$@ "
                                         @"this device's kind (iPhone, iPad)"),
                       gamertag, where, [UIDevice currentDevice].model];
  // Friends says it too, with Sign In Here, until the profile is online again.
  [[XeniaLiveSocial shared] noteSignedInElsewhere:message profile:profile->xuid];
  [[NSNotificationCenter defaultCenter]
      postNotificationName:XeniaLiveSocialSignedOutNotification
                    object:[XeniaLiveSocial shared]
                  userInfo:@{
                    XeniaLiveSocialMessageKey : message,
                    XeniaLiveSocialSignedOutReasonKey : @"elsewhere",
                  }];
}

#pragma mark XeniaLiveAccountService: the account

- (XeniaLiveAccountSummary*)summary {
  XeniaLiveAccountSummary* summary = [[[XeniaLiveAccountSummary alloc] init] autorelease];
  auto profile = ActiveProfile();
  summary.online =
      profile && profile->live_enabled && profile->online_xuid && profile->xuid != _removedProfile;
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
  if (lobby.empty() || !xe_ios_online_features_enabled() || !xe_ios_network_available()) {
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
  const BOOL already_online = profile && profile->live_enabled && profile->online_xuid &&
                              profile->xuid != _removedProfile;
  if (!profile || _goOnline || !g_real_host->enable_online || !g_real_host->rename_profile ||
      (!already_online && !xe_ios_network_available())) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(NameResult(XeniaLiveNameStatusUnreachable));
    });
    return;
  }
  if (profile->live_enabled && profile->online_xuid && profile->xuid != _removedProfile) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(NameResult(XeniaLiveNameStatusAvailable));
    });
    return;
  }
  const uint64_t profile_xuid = profile->xuid;
  if (profile_xuid == _removedProfile) {
    _removedProfile = 0;
  }
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

- (void)removeThisDeviceOfProfile:(uint64_t)xuid completion:(void (^)(void))completion {
  void (^done)(void) = [[completion copy] autorelease];
  auto profile = ActiveProfile();
  auto share = SignedInShare();
  if (_removeThisDeviceAsked || !profile || profile->xuid != xuid || !profile->online_xuid ||
      !share || (*share)->xuid() != profile->online_xuid) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done();
    });
    return;
  }
  _removeThisDeviceDone = [done retain];
  _removeThisDeviceAsked = YES;
  const NSUInteger request = ++_removeThisDeviceRequest;
  (*share)->RemoveThisDevice();
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                               (int64_t)(kRemoveThisDeviceTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_removeThisDeviceDone == done) {
                     [self finishRemoveThisDevice];
                   }
                 });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kAnswerTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_removeThisDeviceRequest == request) {
                     _removeThisDeviceAsked = NO;
                   }
                 });
}

- (void)finishRemoveThisDevice {
  if (!_removeThisDeviceDone) {
    return;
  }
  void (^done)(void) = [_removeThisDeviceDone autorelease];
  _removeThisDeviceDone = nil;
  done();
}

- (void)deleteAccountOfProfile:(uint64_t)xuid completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [[completion copy] autorelease];
  if (_deleteDone || !xe_ios_online_features_enabled() || !xe_ios_network_available()) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(UnreachableError());
    });
    return;
  }
  _deleteDone = [done retain];
  _deleteProfile = xuid;
  _deleteUnansweredProfile = 0;
  _deleteAsked = NO;
  // A profile that was just switched to connects now; signedIn continues from there.
  xe_ios_live_social_refresh_now();
  [self continueDelete];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kDeleteConnectTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_deleteDone == done && !_deleteAsked) {
                     [self finishDelete:UnreachableError()];
                   }
                 });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kAnswerTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_deleteDone == done) {
                     if (_deleteAsked) {
                       _deleteUnansweredProfile = _deleteProfile;
                     }
                     [self finishDelete:_deleteAsked ? DeleteResultUnknownError()
                                                     : UnreachableError()];
                   }
                 });
}

// Asks once the connection is the profile's own: just after a switch it can still be the previous
// profile's, whose account must not be touched.
- (void)continueDelete {
  auto profile = ActiveProfile();
  auto share = SignedInShare();
  if (!_deleteDone || _deleteAsked || !profile || profile->xuid != _deleteProfile || !share ||
      !profile->online_xuid || (*share)->xuid() != profile->online_xuid) {
    return;
  }
  _deleteAsked = YES;
  // The lobby closes this connection, so accountDeleted finishes it.
  (*share)->DeleteAccount();
}

- (void)finishDelete:(NSError*)error {
  if (!_deleteDone) {
    return;
  }
  void (^done)(NSError*) = [_deleteDone autorelease];
  _deleteDone = nil;
  done(error);
}

- (void)createRecoveryCodeWithCompletion:(void (^)(NSString*, NSError*))completion {
  if (_recoveredCode.length) {
    // A recovery just made this account's new code; it is shown once, here, instead of making
    // another and killing it.
    void (^done)(NSString*, NSError*) = [[completion copy] autorelease];
    NSString* code = [_recoveredCode autorelease];
    _recoveredCode = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
      done(code, nil);
    });
    return;
  }
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
  if (![self startJoin:live::AccountJoin{recover ? live::AccountJoin::Mode::kRecover
                                                 : live::AccountJoin::Mode::kLink,
                                         code}
               timeout:kConnectTimeout
                  done:done]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(XeniaLiveLinkStatusUnreachable);
    });
  }
}

// Opens a client that joins an account as |join| says and calls |done| once with the outcome
// (finishJoin). A |timeout| of 0 leaves the waiting to the caller. NO when it can't be tried.
- (BOOL)startJoin:(live::AccountJoin)join
          timeout:(NSTimeInterval)timeout
             done:(void (^)(XeniaLiveLinkStatus))done {
  const std::string lobby = xe::kernel::xam::LiveSessionLobby();
  std::optional<live::DeviceKey> key;
  if (!lobby.empty() && xe_ios_online_features_enabled() && xe_ios_network_available() &&
      g_real_host && g_real_host->adopt_account) {
    key = live::DeviceKey::LoadOrCreate(g_real_host->storage_root / "live" / "device_key");
  }
  if (!key) {
    return NO;
  }
  [self finishJoin:XeniaLiveLinkStatusUnreachable gamertag:std::string()];
  _joinFailure.clear();
  [_joinRecovery release];
  _joinRecovery = nil;

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
        service->_joinFailure = copy;
        [service finishJoin:JoinStatus(copy) gamertag:std::string()];
      }
    });
  };
  // A recovery brings the account's new recovery code, with the move.
  events.recovery_code = [generation](const std::string& code) {
    const std::string copy = code;
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveAccountReal* service = [XeniaLiveAccountReal shared];
      if (service->_joinGeneration == generation) {
        [service->_joinRecovery release];
        service->_joinRecovery = [ToNS(copy) copy];
      }
    });
  };
  events.join_pending = [generation](const std::string& id, const std::string& number,
                                     uint32_t ttl_s) {
    const std::string request = id, match = number;
    dispatch_async(dispatch_get_main_queue(), ^{
      XeniaLiveAccountReal* service = [XeniaLiveAccountReal shared];
      if (service->_joinGeneration == generation) {
        [service approvalPending:request number:match ttl:ttl_s];
      }
    });
  };
  live::AccountOptions options;
  options.join = std::move(join);
  _joinDone = [done copy];
  _joinMoved = 0;
  _join = std::make_shared<live::AccountClient>(lobby, std::move(*key), 0, std::string(),
                                                std::move(events), std::move(options));
  XELOGI("Live account (app): joining an account");
  if (timeout > 0) {
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
          if (_joinGeneration == generation && _joinDone) {
            // Moved but never signed in: the device is on the account all the same.
            [self finishJoin:_joinMoved ? XeniaLiveLinkStatusLinked : XeniaLiveLinkStatusUnreachable
                    gamertag:std::string()];
          }
        });
  }
  return YES;
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
  NSString* recovery = [_joinRecovery autorelease];
  _joinRecovery = nil;
  if (status == XeniaLiveLinkStatusLinked) {
    const uint64_t online_xuid = _joinMoved;
    if (!online_xuid || !g_real_host->adopt_account(online_xuid, gamertag)) {
      status = XeniaLiveLinkStatusUnreachable;
    } else {
      XELOGI("Live account (app): this device joined {:016X} ({})", online_xuid, gamertag);
      _removedProfile = 0;
      if (recovery.length) {
        [_recoveredCode release];
        _recoveredCode = [recovery copy];
      }
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

#pragma mark Moving a profile's data

// Whether the app said where its profiles are (xe_ios_profile_data_install).
// Saves stay each profile's own: moving them between profiles isn't built yet.
- (BOOL)supportsSharedSaves {
  return NO;
}

- (BOOL)supportsProfileData {
  return xe_ios_profile_data_host() != nullptr;
}

- (void)exportSizesWithCompletion:(void (^)(uint64_t, uint64_t, uint64_t))completion {
  void (^done)(uint64_t, uint64_t, uint64_t) = [[completion copy] autorelease];
  const XeniaIOSProfileDataHost* host = xe_ios_profile_data_host();
  auto active = ActiveProfile();
  if (!host || !host->content_root || !active) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(0, 0, 0);
    });
    return;
  }
  const std::filesystem::path content_root = host->content_root();
  const uint64_t xuid = active->xuid;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const xe::kernel::xam::ProfileDataSizes sizes =
        xe::kernel::xam::MeasureProfileData(content_root, xuid);
    dispatch_async(dispatch_get_main_queue(), ^{
      done(sizes.saves, sizes.achievements, sizes.settings);
    });
  });
}

- (void)exportProfileWithOptions:(XeniaLiveExportOptions*)options
                      completion:(void (^)(NSURL*, NSError*))completion {
  void (^done)(NSURL*, NSError*) = [[completion copy] autorelease];
  const XeniaIOSProfileDataHost* host = xe_ios_profile_data_host();
  auto active = ActiveProfile();
  if (!host || !host->content_root || !active) {
    NSError* error =
        host ? xe_ios_profile_package_error(ProfilePackageError::kNoProfile, YES) : NotYetError();
    dispatch_async(dispatch_get_main_queue(), ^{
      done(nil, error);
    });
    return;
  }
  xe::kernel::xam::ProfileExportOptions export_options;
  export_options.saves = options.saves;
  export_options.achievements = options.achievements;
  export_options.settings = options.settings;
  // Per-game settings (options.gameSettings) are host config, tuned for each device, and
  // don't go in the file.
  NSDictionary* info = NSBundle.mainBundle.infoDictionary;
  export_options.app = ToStd([NSString stringWithFormat:@"XeniOS %@ (%@)",
                                                        info[@"CFBundleShortVersionString"] ?: @"",
                                                        info[@"CFBundleVersion"] ?: @""]);
  export_options.platform =
      UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? "ipad" : "iphone";
  NSURL* url = [NSURL
      fileURLWithPath:[NSTemporaryDirectory()
                          stringByAppendingPathComponent:
                              ToNS(xe::kernel::xam::ProfilePackageFileName(active->gamertag))]];
  const std::filesystem::path out_path(url.fileSystemRepresentation);
  const std::filesystem::path content_root = host->content_root();
  const uint64_t xuid = active->xuid;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const ProfilePackageError error = xe::kernel::xam::ExportProfilePackage(
        content_root, xuid, export_options, out_path, ProfileDataProgress());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (error != ProfilePackageError::kNone) {
        XELOGW("Profile export failed: {}", xe::kernel::xam::ProfilePackageErrorName(error));
        done(nil, xe_ios_profile_package_error(error, YES));
        return;
      }
      done(url, nil);
    });
  });
}

// Into the profile with the file's XUID, or the one here with its online account, or a new one.
// The file is checked, then the profile (it can't be in a game), then written off the main
// thread.
- (void)importProfileFromURL:(NSURL*)url completion:(void (^)(NSError*))completion {
  void (^done)(NSError*) = [[completion copy] autorelease];
  const XeniaIOSProfileDataHost* host = xe_ios_profile_data_host();
  if (!host || !host->content_root || !url.isFileURL) {
    NSError* error =
        host ? xe_ios_profile_package_error(ProfilePackageError::kOpenFailed, NO) : NotYetError();
    dispatch_async(dispatch_get_main_queue(), ^{
      done(error);
    });
    return;
  }
  const std::filesystem::path content_root = host->content_root();
  const std::filesystem::path package(url.fileSystemRepresentation);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    xe::kernel::xam::ProfilePackageManifest manifest;
    const ProfilePackageError inspected =
        xe::kernel::xam::InspectProfilePackage(package, &manifest);
    const uint64_t target = inspected == ProfilePackageError::kNone
                                ? xe::kernel::xam::FindProfileForPackage(content_root, manifest)
                                : 0;
    const uint64_t into = target ? target : manifest.xuid;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (inspected != ProfilePackageError::kNone) {
        done(xe_ios_profile_package_error(inspected, NO));
        return;
      }
      const XeniaIOSProfileDataHost* now = xe_ios_profile_data_host();
      if (now && now->profile_in_game && now->profile_in_game(into)) {
        done(xe_ios_profile_package_error(ProfilePackageError::kProfileInUse, NO));
        return;
      }
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        xe::kernel::xam::ProfileImportOptions import_options;
        import_options.target_xuid = target;
        // Asked again just before anything here changes: a game may have started meanwhile.
        import_options.profile_in_use = [](uint64_t xuid) {
          const XeniaIOSProfileDataHost* current = xe_ios_profile_data_host();
          return current && current->profile_in_game && current->profile_in_game(xuid);
        };
        xe::kernel::xam::ProfileImportResult result;
        const ProfilePackageError imported = xe::kernel::xam::ImportProfilePackage(
            content_root, package, import_options, ProfileDataProgress(), &result);
        const uint64_t xuid = result.xuid;
        const bool created = result.created;
        dispatch_async(dispatch_get_main_queue(), ^{
          if (imported != ProfilePackageError::kNone) {
            XELOGW("Profile import failed: {}", xe::kernel::xam::ProfilePackageErrorName(imported));
            done(xe_ios_profile_package_error(imported, NO));
            return;
          }
          const XeniaIOSProfileDataHost* after = xe_ios_profile_data_host();
          if (after && after->profile_imported) {
            after->profile_imported(xuid, created);
          }
          [self postChange];
          done(nil);
        });
      });
    });
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
      entry.linked = profile.online_xuid != 0;
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

#pragma mark Approving on another device

// ---- This device asks the account's others ----

- (void)requestApprovalForGamertag:(NSString*)gamertag
                        completion:(void (^)(XeniaLiveApprovalRequest*,
                                             XeniaLiveApprovalStatus))completion {
  void (^done)(XeniaLiveApprovalRequest*, XeniaLiveApprovalStatus) =
      [[completion copy] autorelease];
  // The join ends the approval before it, if any, then this one is set up.
  XeniaLiveAccountReal* service = self;
  const BOOL started =
      [self startJoin:live::AccountJoin{live::AccountJoin::Mode::kRequest, ToStd(gamertag)}
              timeout:0
                 done:^(XeniaLiveLinkStatus status) {
                   [service endApproval:[service approvalStatusFor:status]];
                 }];
  if (!started) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(nil, XeniaLiveApprovalStatusUnreachable);
    });
    return;
  }
  [_approvalOutcome release];
  _approvalOutcome = nil;
  _approvalStarted = [done copy];
  const NSUInteger generation = _joinGeneration;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kConnectTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_joinGeneration == generation && _approvalStarted) {
                     [self finishJoin:XeniaLiveLinkStatusUnreachable gamertag:std::string()];
                   }
                 });
}

- (void)requestQRLinkWithCompletion:(void (^)(XeniaLiveApprovalRequest*,
                                              XeniaLiveApprovalStatus))completion {
  [self requestApprovalForGamertag:@"" completion:completion];
}

- (void)approvalPending:(const std::string&)requestId
                 number:(const std::string&)number
                    ttl:(uint32_t)ttl_s {
  if (!_approvalStarted) {
    return;
  }
  XeniaLiveApprovalRequest* request = [[[XeniaLiveApprovalRequest alloc] init] autorelease];
  request.requestId = ToNS(requestId);
  if (number.empty()) {
    request.qrPayload = ToNS(live::JoinQrPayload(requestId));
  } else {
    request.matchNumber = ToNS(number);
  }
  request.expires = [NSDate dateWithTimeIntervalSinceNow:ttl_s];
  [_approvalRequest release];
  _approvalRequest = [request retain];
  void (^started)(XeniaLiveApprovalRequest*, XeniaLiveApprovalStatus) =
      [_approvalStarted autorelease];
  _approvalStarted = nil;
  // The lobby ends the request itself at ttl_s; this only covers a lobby that went quiet.
  const NSUInteger generation = _joinGeneration;
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, (int64_t)((ttl_s + kConnectTimeout) * NSEC_PER_SEC)),
      dispatch_get_main_queue(), ^{
        if (_joinGeneration == generation && _joinDone) {
          _joinFailure = "expired";
          [self finishJoin:_joinMoved ? XeniaLiveLinkStatusLinked : XeniaLiveLinkStatusUnreachable
                  gamertag:std::string()];
        }
      });
  started(request, XeniaLiveApprovalStatusApproved);
}

// The outcome of the join, as the player sees it.
- (XeniaLiveApprovalStatus)approvalStatusFor:(XeniaLiveLinkStatus)status {
  const std::string why = _joinFailure;
  _joinFailure.clear();
  if (status == XeniaLiveLinkStatusLinked) {
    return XeniaLiveApprovalStatusApproved;
  }
  if (why == "declined") {
    return XeniaLiveApprovalStatusDenied;
  }
  if (why == "expired") {
    return XeniaLiveApprovalStatusTimedOut;
  }
  if (why == "cancelled") {
    return XeniaLiveApprovalStatusCancelled;
  }
  return XeniaLiveApprovalStatusUnreachable;
}

// Gives the outcome to whoever waits for it, or keeps it for waitForApproval:.
- (void)endApproval:(XeniaLiveApprovalStatus)status {
  [_approvalRequest release];
  _approvalRequest = nil;
  if (_approvalStarted) {
    void (^started)(XeniaLiveApprovalRequest*, XeniaLiveApprovalStatus) =
        [_approvalStarted autorelease];
    _approvalStarted = nil;
    started(nil, status);
  } else if (_approvalWait) {
    void (^wait)(XeniaLiveApprovalStatus) = [_approvalWait autorelease];
    _approvalWait = nil;
    wait(status);
  } else {
    [_approvalOutcome release];
    _approvalOutcome = [@(status) retain];
  }
}

- (void)waitForApproval:(XeniaLiveApprovalRequest*)request
             completion:(void (^)(XeniaLiveApprovalStatus))completion {
  void (^done)(XeniaLiveApprovalStatus) = [[completion copy] autorelease];
  if (_approvalOutcome) {
    const XeniaLiveApprovalStatus status = (XeniaLiveApprovalStatus)_approvalOutcome.integerValue;
    [_approvalOutcome release];
    _approvalOutcome = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
      done(status);
    });
    return;
  }
  if (!_approvalRequest || ![_approvalRequest.requestId isEqualToString:request.requestId]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(XeniaLiveApprovalStatusUnreachable);
    });
    return;
  }
  // One waiter: one waiting already hears it was cancelled rather than nothing.
  if (_approvalWait) {
    void (^replaced)(XeniaLiveApprovalStatus) = [_approvalWait autorelease];
    dispatch_async(dispatch_get_main_queue(), ^{
      replaced(XeniaLiveApprovalStatusCancelled);
    });
  }
  _approvalWait = [done copy];
}

- (void)cancelApproval:(XeniaLiveApprovalRequest*)request {
  if (!_joinDone || !(_approvalStarted || _approvalRequest)) {
    return;
  }
  if (_approvalRequest && ![_approvalRequest.requestId isEqualToString:request.requestId]) {
    return;
  }
  _joinFailure = "cancelled";
  [self finishJoin:XeniaLiveLinkStatusUnreachable gamertag:std::string()];
}

// ---- Another device asks this one ----

- (XeniaLiveIncomingApproval*)pendingApproval {
  return _incoming;
}

- (void)postIncoming {
  [[NSNotificationCenter defaultCenter] postNotificationName:XeniaLiveIncomingApprovalNotification
                                                      object:nil];
}

- (void)joinRequested:(const live::JoinRequest&)request {
  XeniaLiveIncomingApproval* approval = IncomingFrom(request);
  if (request.scanned) {
    // The answer to a scan goes to the scanner, not to the whole app; only this device's own
    // scan may go without a number (it saw the new device's code).
    if (_peekDone && [_peekId isEqualToString:approval.requestId]) {
      _incomingNumbers[approval.requestId] = ToNS(request.number);
      void (^done)(XeniaLiveIncomingApproval*, XeniaLiveLinkStatus) = [_peekDone autorelease];
      _peekDone = nil;
      done(approval, XeniaLiveLinkStatusLinked);
    }
    return;
  }
  if (request.number.empty()) {
    // Without a number to match, a tap could let in a device the player isn't holding.
    XELOGW("Live account (app): a sign-in request came without its number; ignored");
    return;
  }
  _incomingNumbers[approval.requestId] = ToNS(request.number);
  XELOGI("Live account (app): another device asks to sign in");
  [_incoming release];
  _incoming = [approval retain];
  [self postIncoming];
}

// The request ended, here or on another device: its prompt goes.
- (void)joinClosed:(const std::string&)requestId reason:(const std::string&)reason {
  NSString* key = ToNS(requestId);
  [_incomingNumbers removeObjectForKey:key];
  if ([_incoming.requestId isEqualToString:key]) {
    [_incoming release];
    _incoming = nil;
    [self postIncoming];
  }
  void (^waiter)(XeniaLiveApprovalStatus) = [[_approving[key] retain] autorelease];
  if (waiter) {
    [_approving removeObjectForKey:key];
    waiter(reason == "approved"  ? XeniaLiveApprovalStatusApproved
           : reason == "expired" ? XeniaLiveApprovalStatusTimedOut
           : reason == "left"    ? XeniaLiveApprovalStatusCancelled
                                 : XeniaLiveApprovalStatusDenied);
  }
}

- (void)approveIncoming:(XeniaLiveIncomingApproval*)approval
                 chosen:(NSString*)chosen
             completion:(void (^)(XeniaLiveApprovalStatus))completion {
  void (^done)(XeniaLiveApprovalStatus) = [[completion copy] autorelease];
  NSString* key = [[approval.requestId copy] autorelease];
  NSString* number = _incomingNumbers[key];
  auto share = SignedInShare();
  if (!share || !number) {
    // Not signed in, or the request ended already.
    dispatch_async(dispatch_get_main_queue(), ^{
      done(share ? XeniaLiveApprovalStatusTimedOut : XeniaLiveApprovalStatusUnreachable);
    });
    return;
  }
  // The number is checked here: the lobby only passes the request on.
  if (number.length && ![number isEqualToString:chosen ?: @""]) {
    (*share)->DenyJoin(ToStd(key));
    [self joinClosed:ToStd(key) reason:"declined"];
    dispatch_async(dispatch_get_main_queue(), ^{
      done(XeniaLiveApprovalStatusDenied);
    });
    return;
  }
  AddWaiterForKey(_approving, key, done);
  (*share)->ApproveJoin(ToStd(key));
  void (^waiting)(XeniaLiveApprovalStatus) = _approving[key];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kAnswerTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_approving[key] == waiting) {
                     [[waiting retain] autorelease];
                     [_approving removeObjectForKey:key];
                     waiting(XeniaLiveApprovalStatusUnreachable);
                   }
                 });
}

- (void)denyIncoming:(XeniaLiveIncomingApproval*)approval {
  NSString* key = [[approval.requestId copy] autorelease];
  if (auto share = SignedInShare()) {
    (*share)->DenyJoin(ToStd(key));
  }
  [self joinClosed:ToStd(key) reason:"declined"];
}

- (void)beginApprovalFromQRPayload:(NSString*)payload
                        completion:(void (^)(XeniaLiveIncomingApproval*,
                                             XeniaLiveLinkStatus))completion {
  void (^done)(XeniaLiveIncomingApproval*, XeniaLiveLinkStatus) = [[completion copy] autorelease];
  const std::string id = live::ParseJoinQrPayload(ToStd(payload));
  auto share = SignedInShare();
  if (id.empty() || !share) {
    dispatch_async(dispatch_get_main_queue(), ^{
      done(nil, id.empty() ? XeniaLiveLinkStatusBadCode : XeniaLiveLinkStatusUnreachable);
    });
    return;
  }
  if (_peekDone) {
    void (^previous)(XeniaLiveIncomingApproval*, XeniaLiveLinkStatus) = [_peekDone autorelease];
    _peekDone = nil;
    previous(nil, XeniaLiveLinkStatusUnreachable);
  }
  _peekDone = [done copy];
  [_peekId release];
  _peekId = [ToNS(id) copy];
  void (^waiting)(XeniaLiveIncomingApproval*, XeniaLiveLinkStatus) = _peekDone;
  (*share)->PeekJoin(id);
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kAnswerTimeout * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_peekDone == waiting) {
                     [self requestFailed:"join_peek" reason:"unreachable"];
                   }
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
         request == "remove_device" || request == "delete_account" ||
         request == "join_approve" || request == "join_deny" ||
         request == "join_peek";
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
  events.join_requested = [on_main](const live::JoinRequest& request) {
    const live::JoinRequest copy = request;
    on_main(^(XeniaLiveAccountReal* service) {
      [service joinRequested:copy];
    });
  };
  events.join_closed = [on_main](const std::string& id, const std::string& reason) {
    const std::string copy = id, why = reason;
    on_main(^(XeniaLiveAccountReal* service) {
      [service joinClosed:copy reason:why];
    });
  };
  events.removed = [on_main] {
    on_main(^(XeniaLiveAccountReal* service) {
      [service removedFromAccount];
    });
  };
  events.account_deleted = [on_main] {
    on_main(^(XeniaLiveAccountReal* service) {
      [service accountDeleted];
    });
  };
  events.signed_in_elsewhere = [on_main](const std::string& device) {
    const std::string name = device;
    on_main(^(XeniaLiveAccountReal* service) {
      [service signedInElsewhere:ToNS(name)];
    });
  };
}
