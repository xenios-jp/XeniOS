/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_LIVE_PARTY_H_
#define XENIA_KERNEL_XAM_XAM_LIVE_PARTY_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "xenia/kernel/xsession.h"
#include "xenia/net/live_account.h"

namespace xe {
namespace kernel {
namespace xam {

// Parties without Xbox Live, through the profile's account on the lobby
// (xam_live_social). A party lasts across games: up to 8 players and a leader.
// The leader invites friends; an invitee joins by accepting. When the leader is
// in a session friends may join, the other members follow: those in the same
// title join it as if they had accepted the leader's game invite (the title
// reads it with XInviteGetAcceptedInfo); for the others the app is told
// (LivePartyEvents::follow), so it can offer to start that title. Titles that
// ask XAM about the party (XamPartyGetUserList) see its members.
//
// App API. Everything here may be called from any thread, with or without a
// running title, for a profile slot (0-3) whose account is started
// (LiveAccount(user_index) isn't null). The calls return at once; the lobby's
// answer comes as LivePartyEvents::changed. Voice chat, later, keys off
// LiveParty()'s room and members.

// The party the profile at |user_index| is in, with what's known of each
// member's presence (from the lobby's party message, else from friends'
// presence); nothing when it's in none.
std::optional<net::live::PartyState> LiveParty(uint32_t user_index);

// A new party led by the profile (leaving the one it's in). False without an
// account.
bool LivePartyCreate(uint32_t user_index);
// Leaves the party.
bool LivePartyLeave(uint32_t user_index);
// Invites a friend (an online XUID) to the profile's party. The lobby refuses
// non-friends and invites without a party (LivePartyCreate first).
bool LivePartyInvite(uint32_t user_index, uint64_t xuid);

// The party invite waiting for the profile's answer (the newest), if any.
std::optional<net::live::Invite> LivePartyPendingInvite(uint32_t user_index);
// Answers it: accepting joins that party (leaving the one the profile is in).
// False when none is waiting.
bool LivePartyAnswerInvite(uint32_t user_index, bool accept);

// What the leader's session is, for a member who can't join it from where it
// is (another title, or no title running).
struct LivePartyFollow {
  uint64_t leader = 0;
  std::string gamertag;
  uint32_t title_id = 0;
  net::live::Joinable joinable;
  // The party came back with the app (it was in it when it last quit) and
  // the leader was already in this session: ask the player before joining
  // (LivePartyJoinLeader), even in the leader's title.
  bool restored = false;
};

// Joins the leader's session the last LivePartyEvents::follow offered, in the
// running title (the player said yes). False when that title isn't running
// or nothing was offered.
bool LivePartyJoinLeader(uint32_t user_index);

// The app's callbacks, on the account's thread; keep them short.
struct LivePartyEvents {
  // The party changed (members, leader, joined or left) for |user_index|.
  std::function<void(uint32_t user_index)> changed;
  // A party invite came (also when live_auto_accept_invites accepts it).
  std::function<void(uint32_t user_index, const net::live::Invite& invite)>
      invited;
  // The leader went into a session this member's running title can't join,
  // or one to ask about (|follow.restored|).
  std::function<void(uint32_t user_index, const LivePartyFollow& follow)>
      follow;
};
void SetLivePartyEvents(LivePartyEvents events);

// Internal: called by xam_live_social and xam_live_invites.

// A party invite came for the profile at |user_index|.
void LivePartyOnInvite(uint32_t user_index, const net::live::Invite& invite);
// The account's party changed.
void LivePartyChanged(uint32_t user_index);
// Something the party watches changed (friends, presence, the party): follows
// the leader, and sends live_test_party_with's party and invites.
void LivePartyPoll(uint32_t user_index);

// What a member does about the leader's presence. Pure, for tests.
enum class PartyFollowAction {
  kNone,
  kJoin,     // Join the leader's session in the running title.
  kTellApp,  // The leader is in another title (or none is running).
  // The member invited the leader into its own session: the leader is on
  // the way (accepting moves the whole party), so the member stays.
  kWait,
  // The party was restored at sign-in and the leader was already in this
  // session: ask the player instead of joining by itself.
  kAsk,
};
struct PartyFollowInput {
  uint64_t self = 0;
  const net::live::PartyState* party = nullptr;
  const net::live::Presence* leader = nullptr;  // The leader's presence.
  std::string own_session;       // The session the member is in, if any.
  std::string followed_session;  // The session it last followed.
  uint32_t running_title = 0;    // 0: none.
  // The session the member invited the leader to lately (a game invite),
  // if any.
  std::string invited_leader_to;
  // The party came back with the app at sign-in, and nothing has been
  // followed or offered since.
  bool restored = false;
};
PartyFollowAction DecidePartyFollow(const PartyFollowInput& input);

// XamPartyGetUserList's layout, from the Xenia Netplay fork: a count, then 32
// users. The internal list's users have 8 bytes more.
constexpr uint32_t kPartyMaxUsers = 32;
struct X_PARTY_CUSTOM_DATA {
  xe::be<uint64_t> first;
  xe::be<uint64_t> second;
};
static_assert_size(X_PARTY_CUSTOM_DATA, 0x10);
struct X_PARTY_USER_INFO {
  xe::be<uint64_t> xuid;
  char gamertag[16];
  xe::be<uint32_t> user_index;  // The local profile's, else XUserIndexNone.
  xe::be<uint32_t> nat_type;
  xe::be<uint32_t> title_id;
  xe::be<uint32_t> flags;
  XSESSION_INFO session_info;
  X_PARTY_CUSTOM_DATA custom_data;
};
static_assert_size(X_PARTY_USER_INFO, 0x78);
struct X_PARTY_USER_LIST {
  xe::be<uint32_t> user_count;
  X_PARTY_USER_INFO users[kPartyMaxUsers];
};
static_assert_size(X_PARTY_USER_LIST, 0xF08);
struct X_PARTY_USER_INFO_INTERNAL {
  X_PARTY_USER_INFO info;
  xe::be<uint32_t> peer_id;
  xe::be<uint32_t> mute_mask;
};
static_assert_size(X_PARTY_USER_INFO_INTERNAL, 0x80);
struct X_PARTY_USER_LIST_INTERNAL {
  xe::be<uint32_t> user_count;
  X_PARTY_USER_INFO_INTERNAL users[kPartyMaxUsers];
};
static_assert_size(X_PARTY_USER_LIST_INTERNAL, 0x1008);

// Fills |out| with |party|'s members: |local_xuid| is the profile's online
// XUID at |local_index|, and |own_session| its session's XSESSION_INFO (or
// empty).
void FillPartyUser(const net::live::PartyState& party, size_t member,
                   uint64_t local_xuid, uint32_t local_index,
                   std::span<const uint8_t> own_session,
                   X_PARTY_USER_INFO* out);
// The first signed-in profile in a party, for XAM calls that name none.
std::optional<uint32_t> LivePartyUser();

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_LIVE_PARTY_H_
