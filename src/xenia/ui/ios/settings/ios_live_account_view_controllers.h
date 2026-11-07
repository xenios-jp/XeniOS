/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SETTINGS_IOS_LIVE_ACCOUNT_VIEW_CONTROLLERS_H_
#define XENIA_UI_IOS_SETTINGS_IOS_LIVE_ACCOUNT_VIEW_CONTROLLERS_H_

#import <UIKit/UIKit.h>

// The Live account screens (Profile > Online Account), on XeniaLiveAccountService in
// shared/ios_live_account.h.

// The account page, to push inside the Profile sheet: Go Online for an offline profile;
// gamertag, devices, linking, recovery, and export and import once online.
// |profile_gamertag| is the active profile's (offline) name.
UIViewController* xe_ios_new_account_view_controller(NSString* profile_gamertag);

// Move Profile: exports the online profile to a file, for AirDrop or Files.
void xe_ios_present_move_profile(UIViewController* presenter);

// The gamertag sheet for going online (claims the name, renames the profile in place),
// then the recovery code once for a new account. |done| runs when the profile is online.
void xe_ios_present_go_online(UIViewController* presenter, NSString* profile_gamertag,
                              void (^done)(void));

// "Please pick a new gamertag", when the account is flagged. Asks once per launch; "Not
// now" dismisses it for the session. Call after sign-in.
void xe_ios_present_flagged_rename_if_needed(UIViewController* presenter);

// A sign-in on a new device is waiting for this one to approve it (Face ID or Touch ID, then
// the passcode). Presents the approval sheet if there is one and the account is online.
void xe_ios_present_incoming_approval_if_any(UIViewController* presenter);

// Shows that sheet whenever an approval arrives. |presenter| returns the controller to present
// from (nil: not now). Call once at startup.
void xe_ios_observe_incoming_approvals(UIViewController* (^presenter)(void));

// A .xeniosprofile file opened from AirDrop or Files: asks, then imports it through the account
// service. Returns NO (and does nothing) for any other file.
BOOL xe_ios_handle_opened_profile_file(NSURL* url, UIViewController* presenter);

#endif  // XENIA_UI_IOS_SETTINGS_IOS_LIVE_ACCOUNT_VIEW_CONTROLLERS_H_
