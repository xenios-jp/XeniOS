/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_BACKGROUND_REFRESH_H_
#define XENIA_UI_IOS_SHARED_IOS_BACKGROUND_REFRESH_H_

#import <Foundation/Foundation.h>

#include <cstdint>
#include <vector>

#include "xenia/net/live_account.h"

// Notifications while XeniOS is closed. A sideloaded app is signed by its user, so it can't get
// pushes; instead iOS wakes it now and then (Background App Refresh) and it looks for itself:
// a newer release, and the online profile's new friend requests and game invites, read over a
// short connection that appears offline. What it finds becomes local notifications, each once.

// Registers the refresh task. Call from application:didFinishLaunchingWithOptions:.
void xe_ios_background_refresh_register(void);
// Asks iOS for the next wake. Call when the app goes to the background.
void xe_ios_background_refresh_schedule(void);
// Asks once whether XeniOS may notify; later calls do nothing. Call when there's first
// something worth notifying about (a profile went online), from the foreground.
void xe_ios_request_notification_permission(void);

// The account the refresh checks: the profile online now. Kept in the user defaults, since a
// closed app wakes without its emulator or profiles. An empty |lobby| forgets it (the profile
// went offline or signed out).
void xe_ios_background_refresh_set_account(NSString* lobby, uint64_t online_xuid,
                                           NSString* gamertag, NSString* key_path);

// What the open app showed: a later check doesn't notify these again.
void xe_ios_background_refresh_note_seen(const std::vector<xe::net::live::Friend>& friends,
                                         const std::vector<xe::net::live::Invite>& invites);

#endif  // XENIA_UI_IOS_SHARED_IOS_BACKGROUND_REFRESH_H_
