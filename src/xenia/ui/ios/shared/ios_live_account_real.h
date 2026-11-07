/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_LIVE_ACCOUNT_REAL_H_
#define XENIA_UI_IOS_SHARED_IOS_LIVE_ACCOUNT_REAL_H_

#include <memory>
#include <string>

#include "xenia/net/live_account_hub.h"

struct XeniaIOSLiveSocialHost;

// The account screens' service (XeniaLiveAccountService) on the real account: the active
// profile's connection that ios_live_social_bridge keeps open, plus a joining client of its own
// for a link or recovery code. Between the bridge and the service only.

// Installs the service (xe_ios_live_account_set_service). |host| outlives it.
void xe_ios_live_account_real_install(const XeniaIOSLiveSocialHost* host);
// Adds the service's handlers to a share's events, before ShareAccount.
void xe_ios_live_account_real_add_events(xe::net::live::AccountClient::Events& events);
// Whether a refused request is one the account screens report (so the bridge shows no toast).
bool xe_ios_live_account_real_handles(const std::string& request);

// The bridge's side, for the service.
std::shared_ptr<xe::net::live::AccountShare> xe_ios_live_social_current_share();
void xe_ios_live_social_refresh_now();
// Drops the connection and opens a new one for the active profile.
void xe_ios_live_social_reconnect();

#endif  // XENIA_UI_IOS_SHARED_IOS_LIVE_ACCOUNT_REAL_H_
