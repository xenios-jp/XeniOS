/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_UPDATE_CHECK_H_
#define XENIA_UI_IOS_SHARED_IOS_UPDATE_CHECK_H_

#import <Foundation/Foundation.h>

typedef NS_ENUM(NSInteger, XeniaUpdateStatus) {
  // The check could not be completed (offline, server error, unparseable).
  XeniaUpdateStatusUnknown = 0,
  // The running build is the latest published release (or newer).
  XeniaUpdateStatusUpToDate,
  // A newer release is available.
  XeniaUpdateStatusUpdateAvailable,
  // The running build was not stamped by the release pipeline (local/dev build);
  // comparing it against published releases is meaningless, so don't nag.
  XeniaUpdateStatusDevelopmentBuild,
};

@interface XeniaUpdateInfo : NSObject
@property(nonatomic, assign) XeniaUpdateStatus status;
// Human-readable description of the running build, e.g. "2.0 (9712)".
@property(nonatomic, copy) NSString* currentVersionText;
// Latest release version components, e.g. "2.0.1" and 1314.
@property(nonatomic, copy) NSString* latestVersionString;
@property(nonatomic, assign) long long latestBuild;
// Human-readable latest release, e.g. "2.0.1 (1314)". nil when unknown.
@property(nonatomic, copy) NSString* latestVersionText;
// Where to send the user to update (release page, falling back to downloads).
@property(nonatomic, copy) NSString* downloadURLString;
@end

// Running-build version helpers (read from the app's Info.plist).
NSString* xe_current_app_version_string(void);  // CFBundleShortVersionString
long long xe_current_app_build_number(void);    // CFBundleVersion as integer
NSString* xe_current_app_version_text(void);    // "2.0 (9712)"

// YES when the bundle carries the CMake placeholder version (build <= 1), i.e.
// it was not stamped by tools/build_apple_release.sh. Such builds skip the
// update prompt entirely.
BOOL xe_is_development_build(void);

// Compares two (version, build) pairs. Version strings are compared
// component-wise and numerically ("2.0.1" > "2.0"); the build number is only a
// tiebreaker when the version strings are equal. Returns <0, 0, or >0.
NSInteger xe_compare_app_versions(NSString* lhs_version, long long lhs_build, NSString* rhs_version,
                                  long long rhs_build);

// Queries the latest XeniOS GitHub release. Throttled to once per day unless
// force_refresh is YES (e.g. a manual "Check Now" tap). The most recent result
// is cached so the throttled path and offline launches still report a status.
// completion is always invoked, on the main queue.
void xe_check_for_update(BOOL force_refresh, void (^completion)(XeniaUpdateInfo* info));

#endif  // XENIA_UI_IOS_SHARED_IOS_UPDATE_CHECK_H_
