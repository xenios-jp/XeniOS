/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_LIVE_SOCIAL_H_
#define XENIA_KERNEL_XAM_XAM_LIVE_SOCIAL_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "xenia/net/live_account.h"

namespace xe {
namespace kernel {
class KernelState;

namespace xam {

// Friends, presence, invites and party without Xbox Live. Each profile signed
// in to Live gets an account on the System Link lobby (lobby protocol v2,
// "Accounts") when System Link goes through the relay: registered on first use
// without asking anything, signed in with the device's key. Games see the
// account's friends through XFriends*, XPresence* and XamUserAreUsersFriends,
// and its changes as notifications.

// Starts accounts for profiles that signed in to Live and, unless
// |start_only|, stops those of profiles that signed out (on a thread of their
// own, so any caller may hold the kernel's global lock). Cheap when nothing
// changed.
void LiveSocialSignInChanged(KernelState* kernel_state,
                             bool start_only = false);

// Lets go of the kernel's shares of the accounts and forgets invites, waiting
// for their events, before the kernel goes away. A connection the app shares
// stays open. Don't hold the kernel's global lock.
void LiveSocialShutdown();

// The account of the profile at |user_index|; null when it has none.
std::shared_ptr<net::live::AccountClient> LiveAccount(uint32_t user_index);

// The friends of the profile at |user_index| (accepted ones only), and their
// presence when heard.
std::vector<net::live::Friend> LiveFriends(uint32_t user_index);
bool LiveAreFriends(uint32_t user_index, uint64_t xuid);
// Whether the profile at |user_index| blocks |xuid|. Titles see the blocked
// as muted (XUserMuteListQuery).
bool LiveIsBlocked(uint32_t user_index, uint64_t xuid);
std::optional<net::live::Presence> LiveFriendPresence(uint32_t user_index,
                                                      uint64_t xuid);

// What friends see of the profile at |user_index|: the title, its presence
// context and the session they may join.
void LiveSetPresence(uint32_t user_index, uint32_t title_id, uint32_t context,
                     std::optional<net::live::Joinable> joinable);
// Something friends see of a System Link game changed: whether the title has
// a session with Live presence open, or the room it plays in. Presence
// follows.
void LiveSystemLinkChanged();
// What friends may join of the profile, if anything: LiveSetPresence's Live
// session (in the title's System Link room), or else, in a System Link game,
// that room alone (no session ID or XSESSION_INFO).
std::optional<net::live::Joinable> LiveOwnJoinable(uint32_t user_index);

// Called on the account's thread for each invite a friend sends the profile
// at |user_index| (xam_live_invites).
using LiveInviteHandler =
    std::function<void(uint32_t user_index, const net::live::Invite& invite)>;
void SetLiveInviteHandler(LiveInviteHandler handler);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_LIVE_SOCIAL_H_
