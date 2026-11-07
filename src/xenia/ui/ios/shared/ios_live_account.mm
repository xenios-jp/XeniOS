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

namespace {
id<XeniaLiveAccountService> g_service = nil;
#if TARGET_OS_SIMULATOR
id<XeniaLiveAccountService> g_stub = nil;
#endif
}  // namespace

id<XeniaLiveAccountService> xe_ios_live_account_service(void) {
  if (g_service) {
    return g_service;
  }
#if TARGET_OS_SIMULATOR
  if (!g_stub) {
    g_stub = [xe_ios_live_account_preview_service() retain];
  }
  return g_stub;
#else
  return nil;
#endif
}

void xe_ios_live_account_set_service(id<XeniaLiveAccountService> service) {
  if (service != g_service) {
    [g_service release];
    g_service = [service retain];
  }
  [[NSNotificationCenter defaultCenter] postNotificationName:XeniaLiveAccountDidChangeNotification
                                                      object:nil];
}

BOOL xe_ios_live_account_service_is_stub(void) {
#if TARGET_OS_SIMULATOR
  return g_service == nil;
#else
  return NO;
#endif
}

BOOL xe_ios_live_account_supports_profile_data(void) {
  id<XeniaLiveAccountService> service = xe_ios_live_account_service();
  return ![service respondsToSelector:@selector(supportsProfileData)] ||
         service.supportsProfileData;
}

BOOL xe_ios_live_account_supports_shared_saves(void) {
  id<XeniaLiveAccountService> service = xe_ios_live_account_service();
  return xe_ios_live_account_supports_profile_data() &&
         (![service respondsToSelector:@selector(supportsSharedSaves)] ||
          service.supportsSharedSaves);
}

BOOL xe_ios_live_account_ui_enabled(void) {
#if TARGET_OS_SIMULATOR
  return g_service != nil ||
         [[NSUserDefaults standardUserDefaults] boolForKey:@"XeniaLiveAccountPreview"];
#else
  return g_service != nil;
#endif
}
