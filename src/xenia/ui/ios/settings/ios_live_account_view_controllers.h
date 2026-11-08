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

#include <cstdint>

// The Live account screens (Profile > Online Account), on XeniaLiveAccountService in
// shared/ios_live_account.h.

// A profile's page, pushed in the Profiles card from the profile's ⓘ: Get a Gamertag for an
// offline profile; devices, Shared Saves, Copy Profile, recovery and the gamertag once online.
// |active| is the signed-in profile; any other gets "Switch to …", which calls |switch_to|
// (YES when it signed in). The signed-in profile's avatar has an Edit button, which calls
// |change_picture| with the page. Its Delete Profile row calls |delete_profile| with the page and
// the row.
UIViewController* xe_ios_new_profile_page(
    NSString* gamertag, BOOL active, BOOL (^_Nullable switch_to)(void),
    void (^_Nullable change_picture)(UIViewController* page),
    void (^_Nullable delete_profile)(UIViewController* page, UIView* source));

// Report a player: the reason, an optional note and "Also block", then Send. The answer comes
// as XeniaLiveSocial's notice or failure, after the form closed.
void xe_ios_present_report_player(UIViewController* presenter, uint64_t xuid,
                                  NSString* _Nullable gamertag);

// I Have a Gamertag: sign this device in with a code from Add a Device (scanned or typed) or a
// recovery code. A non-nil |code| is tried at once.
void xe_ios_present_sign_in(UIViewController* presenter, NSString* _Nullable code);

// xenios://link?c=… (Add a Device's QR code, opened by the system Camera): the sign-in sheet
// with that code. Returns NO for any other URL.
BOOL xe_ios_handle_link_url(NSURL* url, UIViewController* presenter);

// Copy Profile: saves, achievements and settings to AirDrop or Files, or saves to another
// profile here.
void xe_ios_present_copy_profile(UIViewController* presenter);

// The QR code for |text| as a |side|-point image: round dots, black on clear (draw it on white).
UIImage* xe_ios_qr_image(NSString* text, CGFloat side);

// A camera page that reads a QR code. |on_text| takes what it read and says whether it was the
// right kind; the page closes on YES and looks again on NO. |test_text| stands in for a code on a
// device with no camera (the Simulator).
void xe_ios_present_code_scanner(UIViewController* presenter, NSString* title, NSString* hint,
                                 NSString* _Nullable test_text, BOOL (^on_text)(NSString* text));

// The gamertag sheet for going online (claims the name, renames the profile in place). |done|
// runs when the profile is online.
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

#endif  // XENIA_UI_IOS_SETTINGS_IOS_LIVE_ACCOUNT_VIEW_CONTROLLERS_H_
