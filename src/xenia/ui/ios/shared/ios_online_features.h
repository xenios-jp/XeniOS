/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_ONLINE_FEATURES_H_
#define XENIA_UI_IOS_SHARED_IOS_ONLINE_FEATURES_H_

#import <Foundation/Foundation.h>

// The one switch for everything XeniOS fetches or sends over the network: the
// update check, compatibility data, cover art, compatibility discussions and
// reports, and System Link over the internet. Stored in NSUserDefaults; the
// default is a single constant in ios_online_features.mm.
BOOL xe_ios_online_features_enabled(void);
void xe_ios_set_online_features_enabled(BOOL enabled);

// Short text for the unavailable state of a feature, e.g. in an alert.
NSString* xe_ios_online_features_off_message(void);

// YES until the notice about online features has been answered, on first launch and
// again once when online features gain something new. Nothing is fetched at launch
// while it is pending.
BOOL xe_ios_online_features_notice_pending(void);
// Also answers the diagnostics notice: the first-launch notice covers both.
void xe_ios_set_online_features_notice_answered(void);

// Whether this build can send diagnostics at all: it carries an endpoint
// (XeniOSDiagnosticsURL). Without one, the notices don't mention diagnostics and
// Settings has no Share Diagnostics switch.
BOOL xe_ios_diagnostics_available(void);

// Share Diagnostics: anonymous crash reports and performance statistics
// (ios_diagnostics.h). On unless turned off, and only in effect while Online
// Features is on too. Turning it off leaves Online Features (and what they
// cover, such as System Link over the internet) as they are.
BOOL xe_ios_share_diagnostics_enabled(void);
void xe_ios_set_share_diagnostics_enabled(BOOL enabled);

// YES for an install that answered the first-launch notice before it covered
// diagnostics, until the one-time diagnostics notice has been answered. Never
// in a build without diagnostics.
BOOL xe_ios_diagnostics_notice_pending(void);
void xe_ios_set_diagnostics_notice_answered(void);

// Online Features and Share Diagnostics both on, no notice pending, and a
// build with a diagnostics endpoint: the only state in which diagnostics are
// recorded for sending or sent.
BOOL xe_ios_diagnostics_upload_allowed(void);

// Whether Apple's state reporting may hear the running title, which labels
// MetricKit reports by game. That stays on the device until the system sends it
// with the user's own "Share with App Developers" choice. With a diagnostics
// endpoint it follows upload_allowed; without one there is no Share Diagnostics
// switch, so Online Features on and the first-launch notice answered are enough.
BOOL xe_ios_title_reporting_allowed(void);

#endif  // XENIA_UI_IOS_SHARED_IOS_ONLINE_FEATURES_H_
