/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_LIVE_INVITES_H_
#define XENIA_KERNEL_XAM_XAM_LIVE_INVITES_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xsession.h"
#include "xenia/net/live_account.h"

namespace xe {
namespace kernel {
class KernelState;

namespace xam {

// Game invites without Xbox Live, through the profile's account on the lobby
// (xam_live_social). XInviteSend sends the sender's joinable session to each
// friend; an invite that comes is kept per profile until it's accepted (by
// the UI, or at once with live_auto_accept_invites), and the title reads the
// accepted one with XInviteGetAcceptedInfo. Layouts from the Xenia Netplay
// fork.

// XInviteSend's marshalled request, big-endian: user index, the invitees'
// count and XUIDs, the display string's length (characters) and UTF-16 text,
// and the message handle.
struct InviteSendRequest {
  uint32_t user_index = 0;
  std::vector<uint64_t> invitees;
  std::u16string text;
  uint32_t message_handle = 0;
};
constexpr uint32_t kInviteMaxInvitees = 100;
constexpr uint32_t kInviteMaxText = 255;
// Nothing when |bytes| is too short or a count is out of range.
std::optional<InviteSendRequest> ParseInviteSendRequest(
    std::span<const uint8_t> bytes);

#pragma pack(push, 4)
struct X_INVITE_INFO {
  xe::be<uint64_t> xuid_invitee;
  xe::be<uint64_t> xuid_inviter;
  xe::be<uint32_t> title_id;
  XSESSION_INFO host_info;
  xe::be<uint32_t> from_game_invite;
};
#pragma pack(pop)
static_assert_size(X_INVITE_INFO, 0x54);

// An invite the profile accepted.
struct AcceptedInvite {
  uint64_t invitee = 0;  // The profile's online XUID.
  uint64_t inviter = 0;
  uint32_t title_id = 0;
  std::vector<uint8_t> info;  // XSESSION_INFO, 0x3C bytes.
  // An invite; false when joining a friend from their presence.
  bool from_game_invite = true;
};
// False (and |out| cleared) when the invite has no XSESSION_INFO.
bool FillInviteInfo(const AcceptedInvite& invite, X_INVITE_INFO* out);

// An invite accepted in one title for another, carried to the process (or
// launch) that starts the other: the profile, the invite (its info empty for
// a System Link game) and the friend's System Link room ("" for the title's
// public lobby). Text for live_accepted_invite.
struct LaunchInvite {
  uint32_t user_index = 0;
  AcceptedInvite invite;
  std::string room;
};
std::string EncodeAcceptedInvite(uint32_t user_index,
                                const AcceptedInvite& invite,
                                const std::string& room);
std::optional<LaunchInvite> DecodeAcceptedInvite(std::string_view text);

// Takes incoming invites from the profiles' accounts. Cheap after the first
// call.
void InstallLiveInvites(KernelState* kernel_state);
// Forgets the pending and accepted invites of the profile at |user_index|,
// when it signs out.
void ForgetLiveInvites(uint32_t user_index);
// Forgets the pending invites and the kernel, before the kernel goes away.
// Accepted invites stay for the next title to read.
void ShutdownLiveInvites();

// XInviteSend: an invite to each invitee with the sender's joinable session.
// Returns how many were sent (none without an account).
uint32_t LiveSendInvites(KernelState* kernel_state,
                         const InviteSendRequest& request);

// The invite the profile at |user_index| accepted, if any, with the session
// to join. Reading it clears it, as in the Netplay fork; one whose session
// isn't known yet (neither in the invite nor in the inviter's presence) stays
// until it is. With |title_id|, only an invite to that title.
std::optional<AcceptedInvite> TakeAcceptedInvite(uint32_t user_index,
                                                 uint32_t title_id = 0);
// Accepts the pending invite of the profile at |user_index| (the UI's
// button). False when none is pending.
bool AcceptPendingInvite(uint32_t user_index);
// Accepts |invite| for the profile at |user_index| in the running title (an
// in-game banner): answers it on the profile's account and tells the title.
// An invite without an ID joins the friend's game from their presence. False
// without an account.
bool AcceptLiveInvite(uint32_t user_index, const net::live::Invite& invite);
// An invite (or a friend's game) the app accepted before starting its title:
// the title reads it with XInviteGetAcceptedInfo, and hears
// XN_LIVE_INVITE_ACCEPTED once it listens for Live notifications.
void SetAcceptedInvite(uint32_t user_index, AcceptedInvite invite);
// Drops an accepted invite no title read (the player started another game).
void ClearAcceptedInvites();
// The users with an accepted invite for |title_id| (XN_LIVE_INVITE_ACCEPTED
// for a title that starts listening).
std::vector<uint32_t> UsersWithAcceptedInvite(uint32_t title_id);

// Joins |joinable| (a friend's session) as if the profile at |user_index| had
// accepted an invite to it from |from|: the title is told and reads it with
// XInviteGetAcceptedInfo. The party's follow-the-leader uses it. False when
// |title_id| isn't the running title or the session has no XSESSION_INFO.
bool LiveJoinSession(uint32_t user_index, uint64_t from,
                     const std::string& gamertag, uint32_t title_id,
                     const net::live::Joinable& joinable);

// Why joining an accepted invite's session failed: XNet never reached its
// host, and the inviter still hosts it (kHostUnreachable) or no longer does
// (kHostLeft).
enum class LiveJoinFailure { kHostUnreachable, kHostLeft };
// The app's: called on a thread of its own, |settle| after the failure when no
// other association for that session connected meanwhile. |gamertag| is the
// inviter's, or empty when not known.
using LiveJoinFailedHandler =
    std::function<void(uint32_t user_index, const std::string& gamertag,
                       LiveJoinFailure reason)>;
void SetLiveJoinFailedHandler(
    LiveJoinFailedHandler handler,
    std::chrono::milliseconds settle = std::chrono::milliseconds(3000));
// The app's: an invite accepted (or a friend joined) who plays a System Link
// game, which has no Live session to join. The profile is moved to their room
// (|room|: a private room's code, or empty for the title's public lobby) when
// |title_id| is running; the player joins from the game's System Link screen.
// Called on the thread that accepted.
using LiveSystemLinkJoinHandler =
    std::function<void(uint32_t user_index, const std::string& gamertag,
                       uint32_t title_id, const std::string& room)>;
void SetLiveSystemLinkJoinHandler(LiveSystemLinkJoinHandler handler);
// The app's: the profile at |user_index| accepted |from|'s invite to
// |invite.title_id|, which isn't the running title. The app quits the running
// one (when the player agrees) and starts that game from its library with
// the invite (SetAcceptedInvite, or live_accepted_invite in a new process),
// in |room|; or says the game isn't in the library. Called on the thread that
// accepted.
using LiveInviteTitleHandler =
    std::function<void(uint32_t user_index, const std::string& from,
                       AcceptedInvite invite, const std::string& room)>;
void SetLiveInviteTitleHandler(LiveInviteTitleHandler handler);
// Internal: System Link's association outcomes (net::secure's observer) for
// the session key |key| (its XNKID).
void LiveJoinAssociation(const std::array<uint8_t, 8>& key, bool connected);

// The XSESSION_INFOs of sessions the signed-in profiles know of outside the
// session directory: friends' joinable presence and pending or accepted
// invites. Joining a friend looks these up by session ID.
std::vector<XSESSION_INFO> LiveKnownSessions();

// The account's state changed (presence or friends) for the profile at
// |user_index|: sends live_test_invite_xuid's test invite when it can.
void LiveInvitesPoll(uint32_t user_index);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_LIVE_INVITES_H_
