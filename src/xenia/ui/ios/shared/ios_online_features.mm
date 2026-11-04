/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_online_features.h"

#import "xenia/ui/ios/shared/ios_diagnostics_outbox.h"

namespace {

// On unless turned off; the first-launch notice says so before anything is fetched.
constexpr BOOL kOnlineFeaturesDefault = YES;

NSString* const kOnlineFeaturesKey = @"XeniaOnlineFeaturesEnabled";
// The notice revision last answered. 1 covered fetching and reports, 2 added System
// Link, which shows other players the device's IP address and a profile ID.
NSString* const kOnlineFeaturesNoticeKey = @"XeniaOnlineFeaturesNoticeRevision";
constexpr NSInteger kOnlineFeaturesNoticeRevision = 2;
NSString* const kShareDiagnosticsKey = @"XeniaShareDiagnosticsEnabled";
NSString* const kDiagnosticsNoticeKey = @"XeniaDiagnosticsNoticeAnswered";

}  // namespace

BOOL xe_ios_online_features_enabled(void) {
  // Nothing goes online before the first-launch notice has been answered.
  if (xe_ios_online_features_notice_pending()) {
    return NO;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults objectForKey:kOnlineFeaturesKey] == nil) {
    return kOnlineFeaturesDefault;
  }
  return [defaults boolForKey:kOnlineFeaturesKey];
}

void xe_ios_set_online_features_enabled(BOOL enabled) {
  [[NSUserDefaults standardUserDefaults] setBool:enabled forKey:kOnlineFeaturesKey];
}

NSString* xe_ios_online_features_off_message(void) {
  return NSLocalizedString(
      @"Online features are turned off. Turn them on in Settings > Privacy.",
      @"Message shown when a feature needs online features turned on");
}

BOOL xe_ios_online_features_notice_pending(void) {
  return [[NSUserDefaults standardUserDefaults] integerForKey:kOnlineFeaturesNoticeKey] <
         kOnlineFeaturesNoticeRevision;
}

void xe_ios_set_online_features_notice_answered(void) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults setInteger:kOnlineFeaturesNoticeRevision forKey:kOnlineFeaturesNoticeKey];
  // The first-launch notice covers diagnostics only in a build that sends them; after
  // one that doesn't, a later build still asks.
  if (xe_ios_diagnostics_available()) {
    [defaults setBool:YES forKey:kDiagnosticsNoticeKey];
  }
}

BOOL xe_ios_diagnostics_available(void) { return xe_ios_diagnostics_endpoint() != nil; }

BOOL xe_ios_share_diagnostics_enabled(void) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults objectForKey:kShareDiagnosticsKey] == nil) {
    return YES;
  }
  return [defaults boolForKey:kShareDiagnosticsKey];
}

void xe_ios_set_share_diagnostics_enabled(BOOL enabled) {
  [[NSUserDefaults standardUserDefaults] setBool:enabled forKey:kShareDiagnosticsKey];
}

BOOL xe_ios_diagnostics_notice_pending(void) {
  // With Online Features off nothing is sent, so there is nothing to ask yet;
  // turning them back on in Settings shows the Share Diagnostics switch.
  return xe_ios_diagnostics_available() && !xe_ios_online_features_notice_pending() &&
         xe_ios_online_features_enabled() &&
         ![[NSUserDefaults standardUserDefaults] boolForKey:kDiagnosticsNoticeKey];
}

void xe_ios_set_diagnostics_notice_answered(void) {
  [[NSUserDefaults standardUserDefaults] setBool:YES forKey:kDiagnosticsNoticeKey];
}

BOOL xe_ios_diagnostics_upload_allowed(void) {
  return xe_ios_diagnostics_available() && xe_ios_online_features_enabled() &&
         xe_ios_share_diagnostics_enabled() && !xe_ios_diagnostics_notice_pending();
}

BOOL xe_ios_title_reporting_allowed(void) {
  if (xe_ios_diagnostics_available()) {
    return xe_ios_diagnostics_upload_allowed();
  }
  return xe_ios_online_features_enabled() && !xe_ios_online_features_notice_pending();
}
