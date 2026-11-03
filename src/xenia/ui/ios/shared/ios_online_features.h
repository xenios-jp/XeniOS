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

// The one switch for everything XeniOS itself fetches or sends over the network:
// the update check, compatibility data, cover art, compatibility discussions and
// compatibility reports. It does not affect what a game does. Stored in
// NSUserDefaults; the default is a single constant in ios_online_features.mm.
BOOL xe_ios_online_features_enabled(void);
void xe_ios_set_online_features_enabled(BOOL enabled);

// Short text for the unavailable state of a feature, e.g. in an alert.
NSString* xe_ios_online_features_off_message(void);

// YES until the first-launch notice about online features has been answered. Nothing
// is fetched at launch while it is pending.
BOOL xe_ios_online_features_notice_pending(void);
void xe_ios_set_online_features_notice_answered(void);

#endif  // XENIA_UI_IOS_SHARED_IOS_ONLINE_FEATURES_H_
