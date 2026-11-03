/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_online_features.h"

namespace {

// On unless turned off; the first-launch notice says so before anything is fetched.
constexpr BOOL kOnlineFeaturesDefault = YES;

NSString* const kOnlineFeaturesKey = @"XeniaOnlineFeaturesEnabled";
// The notice revision last answered. 1 covered fetching and reports, 2 added System
// Link, which shows other players the device's IP address.
NSString* const kOnlineFeaturesNoticeKey = @"XeniaOnlineFeaturesNoticeRevision";
constexpr NSInteger kOnlineFeaturesNoticeRevision = 2;

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
      @"Online features are turned off. Turn them on in Settings > About > Privacy.",
      @"Message shown when a feature needs online features turned on");
}

BOOL xe_ios_online_features_notice_pending(void) {
  return [[NSUserDefaults standardUserDefaults] integerForKey:kOnlineFeaturesNoticeKey] <
         kOnlineFeaturesNoticeRevision;
}

void xe_ios_set_online_features_notice_answered(void) {
  [[NSUserDefaults standardUserDefaults] setInteger:kOnlineFeaturesNoticeRevision
                                             forKey:kOnlineFeaturesNoticeKey];
}
