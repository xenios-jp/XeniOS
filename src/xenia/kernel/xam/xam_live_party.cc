/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_live_party.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_live_invites.h"
#include "xenia/kernel/xam/xam_live_social.h"
#include "xenia/kernel/xam/xam_state.h"

DEFINE_string(live_test_party_with, "",
              "Online XUIDs (hex, comma separated) the first signed-in "
              "profile makes a party with: it creates a party and invites "
              "each once they're friends. For tests without the UI.",
              "Live");
DEFINE_string(live_test_party_invite_xuid, "",
              "Online XUID (hex) the first signed-in profile invites to its "
              "party once they're friends, in a party or not (the account "
              "starts one first). For tests without the UI.",
              "Live");
DEFINE_bool(live_party_follow_leader, true,
            "Party members in the leader's title join the leader's session "
            "when it becomes one friends may join.",
            "Live");
DECLARE_bool(live_auto_accept_invites);

namespace xe {
namespace kernel {
namespace xam {

PartyFollowAction DecidePartyFollow(const PartyFollowInput& in) {
  if (!in.party || !in.leader || in.party->leader == in.self ||
      in.leader->xuid != in.party->leader || !in.leader->online ||
      !in.leader->joinable || in.leader->joinable->session_id.empty()) {
    return PartyFollowAction::kNone;
  }
  const std::string& session = in.leader->joinable->session_id;
  if (session == in.followed_session || session == in.own_session) {
    return PartyFollowAction::kNone;
  }
  // Following would leave the session the leader is coming to.
  if (!in.own_session.empty() && in.invited_leader_to == in.own_session) {
    return PartyFollowAction::kWait;
  }
  if (in.running_title && in.running_title == in.leader->title_id &&
      in.leader->joinable->info.size() == sizeof(XSESSION_INFO)) {
    return in.restored ? PartyFollowAction::kAsk : PartyFollowAction::kJoin;
  }
  return PartyFollowAction::kTellApp;
}

void FillPartyUser(const net::live::PartyState& party, size_t member,
                   uint64_t local_xuid, uint32_t local_index,
                   std::span<const uint8_t> own_session,
                   X_PARTY_USER_INFO* out) {
  std::memset(out, 0, sizeof(*out));
  const auto& [xuid, gamertag] = party.members[member];
  out->xuid = xuid;
  std::memcpy(out->gamertag, gamertag.data(),
              std::min(gamertag.size(), sizeof(out->gamertag) - 1));
  const bool local = xuid == local_xuid;
  out->user_index = local ? local_index : XUserIndexNone;
  // TODO(live): flags and the NAT type; their meaning isn't known here.
  std::span<const uint8_t> session;
  if (local) {
    session = own_session;
  }
  for (const auto& presence : party.presence) {
    if (presence.xuid != xuid) {
      continue;
    }
    out->title_id = presence.title_id;
    if (!local && presence.joinable) {
      session = presence.joinable->info;
    }
  }
  if (session.size() == sizeof(XSESSION_INFO)) {
    std::memcpy(&out->session_info, session.data(), sizeof(XSESSION_INFO));
  }
}

namespace {

// How long a member who invited the leader into its game waits for the
// leader before following the leader again.
constexpr auto kLeaderJoiningWindow = std::chrono::seconds(60);

struct UserState {
  std::optional<net::live::Invite> pending;
  std::string followed;  // The leader's session it last followed.
  std::string waited;    // The leader's session it stayed out of (kWait).
  // The party room heard while this app runs, and whether the party came
  // back at sign-in (no event: the lobby restoring it) without anything
  // followed or offered since.
  std::string room;
  bool restored = false;
  std::optional<LivePartyFollow> offered;  // For LivePartyJoinLeader.
  bool test_party_invited = false;
  std::set<uint64_t> test_invited;
};

struct Party {
  std::mutex mutex;
  std::array<UserState, XUserMaxUserCount> users;
  LivePartyEvents events;
};

Party& GetParty() {
  static Party party;
  return party;
}

std::vector<uint64_t> TestPartyXuids() {
  std::vector<uint64_t> xuids;
  std::stringstream list(cvars::live_test_party_with);
  std::string item;
  while (std::getline(list, item, ',')) {
    if (!item.empty()) {
      xuids.push_back(std::strtoull(item.c_str(), nullptr, 16));
    }
  }
  return xuids;
}

uint32_t RunningTitle() {
  auto* state = kernel_state();
  return state ? state->title_id() : 0;
}

// The members' presence: the lobby's, else friends'.
void AddFriendPresence(uint32_t user_index, net::live::PartyState& party) {
  for (const auto& [xuid, gamertag] : party.members) {
    const bool known = std::any_of(
        party.presence.begin(), party.presence.end(),
        [xuid = xuid](const net::live::Presence& p) { return p.xuid == xuid; });
    if (known) {
      continue;
    }
    if (auto presence = LiveFriendPresence(user_index, xuid)) {
      party.presence.push_back(std::move(*presence));
    }
  }
}

void TestParty(uint32_t user_index, net::live::AccountClient& account,
               const std::optional<net::live::PartyState>& party) {
  if (!cvars::live_test_party_invite_xuid.empty() && user_index == 0 &&
      account.signed_in()) {
    const uint64_t xuid = std::strtoull(
        cvars::live_test_party_invite_xuid.c_str(), nullptr, 16);
    bool invite = false;
    if (xuid && account.IsFriend(xuid)) {
      auto& state = GetParty();
      std::lock_guard lock(state.mutex);
      invite = !std::exchange(state.users[user_index].test_party_invited, true);
    }
    if (invite) {
      XELOGI("Live party: invited {:016X} (test, {})", xuid,
             party ? "in a party" : "in no party");
      account.InviteToParty(xuid, RunningTitle());
    }
  }
  if (cvars::live_test_party_with.empty() || user_index != 0 ||
      !account.signed_in()) {
    return;
  }
  // Inviting starts a party when the lobby has none for the account; a
  // relaunched app's party is restored instead (it may not be here yet).
  std::vector<uint64_t> invite;
  {
    auto& state = GetParty();
    std::lock_guard lock(state.mutex);
    auto& user = state.users[user_index];
    if (!party || party->leader == account.xuid()) {
      for (const uint64_t xuid : TestPartyXuids()) {
        const bool member =
            party && std::any_of(party->members.begin(), party->members.end(),
                                 [xuid](const auto& m) { return m.first == xuid; });
        if (!member && account.IsFriend(xuid) &&
            user.test_invited.insert(xuid).second) {
          invite.push_back(xuid);
        }
      }
    }
  }
  for (const uint64_t xuid : invite) {
    XELOGI("Live party: invited {:016X} (test)", xuid);
    account.InviteToParty(xuid, RunningTitle());
  }
}

void Follow(uint32_t user_index, net::live::AccountClient& account,
            const net::live::PartyState& party) {
  if (!cvars::live_party_follow_leader) {
    return;
  }
  std::optional<net::live::Presence> leader;
  for (const auto& presence : party.presence) {
    if (presence.xuid == party.leader) {
      leader = presence;
    }
  }
  const auto own = LiveOwnJoinable(user_index);
  PartyFollowInput in;
  in.self = account.xuid();
  in.party = &party;
  in.leader = leader ? &*leader : nullptr;
  in.own_session = own ? own->session_id : std::string();
  in.running_title = RunningTitle();
  if (leader) {
    in.invited_leader_to =
        account.InvitedTo(leader->xuid, kLeaderJoiningWindow).value_or("");
  }
  PartyFollowAction action;
  LivePartyEvents events;
  {
    auto& state = GetParty();
    std::lock_guard lock(state.mutex);
    auto& user = state.users[user_index];
    if (leader && (!leader->online || !leader->joinable ||
                   leader->joinable->session_id.empty())) {
      // The leader is in no session: the next one is new, and followed.
      user.restored = false;
    }
    in.followed_session = user.followed;
    in.restored = user.restored;
    action = DecidePartyFollow(in);
    if (action == PartyFollowAction::kNone) {
      return;
    }
    if (action == PartyFollowAction::kWait) {
      if (user.waited != leader->joinable->session_id) {
        user.waited = leader->joinable->session_id;
        XELOGI("Live party: user {} stays in session {}: the leader {} was "
               "invited to it",
               user_index, in.own_session, leader->gamertag);
      }
      return;
    }
    user.followed = leader->joinable->session_id;
    const bool restored = user.restored;
    user.restored = false;
    events = state.events;
    if (action != PartyFollowAction::kJoin) {
      LivePartyFollow offer;
      offer.leader = leader->xuid;
      offer.gamertag = leader->gamertag;
      offer.title_id = leader->title_id;
      offer.joinable = *leader->joinable;
      offer.restored = restored;
      user.offered = std::move(offer);
    }
  }
  if (action == PartyFollowAction::kJoin) {
    if (LiveJoinSession(user_index, leader->xuid, leader->gamertag,
                        leader->title_id, *leader->joinable)) {
      XELOGI("Live party: user {} follows the leader {} into session {}",
             user_index, leader->gamertag, leader->joinable->session_id);
    }
    return;
  }
  if (action == PartyFollowAction::kAsk) {
    XELOGI("Live party: user {}: the leader {} was already in session {} when "
           "the party came back; asking before joining",
           user_index, leader->gamertag, leader->joinable->session_id);
  } else {
    XELOGI("Live party: the leader {} is in session {} of title {:08X}; "
           "telling the app",
           leader->gamertag, leader->joinable->session_id, leader->title_id);
  }
  std::optional<LivePartyFollow> follow;
  {
    auto& state = GetParty();
    std::lock_guard lock(state.mutex);
    follow = state.users[user_index].offered;
  }
  if (events.follow && follow) {
    events.follow(user_index, *follow);
  }
}

}  // namespace

bool LivePartyJoinLeader(uint32_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return false;
  }
  std::optional<LivePartyFollow> offer;
  {
    auto& state = GetParty();
    std::lock_guard lock(state.mutex);
    offer = std::move(state.users[user_index].offered);
    state.users[user_index].offered.reset();
  }
  if (!offer || !LiveJoinSession(user_index, offer->leader, offer->gamertag,
                                 offer->title_id, offer->joinable)) {
    return false;
  }
  XELOGI("Live party: user {} joins the leader {} in session {} (asked)",
         user_index, offer->gamertag, offer->joinable.session_id);
  return true;
}

std::optional<net::live::PartyState> LiveParty(uint32_t user_index) {
  auto account = LiveAccount(user_index);
  if (!account) {
    return std::nullopt;
  }
  auto party = account->Party();
  if (party) {
    AddFriendPresence(user_index, *party);
  }
  return party;
}

bool LivePartyCreate(uint32_t user_index) {
  auto account = LiveAccount(user_index);
  if (!account) {
    return false;
  }
  XELOGI("Live party: user {} creates a party", user_index);
  account->CreateParty();
  return true;
}

bool LivePartyLeave(uint32_t user_index) {
  auto account = LiveAccount(user_index);
  if (!account) {
    return false;
  }
  XELOGI("Live party: user {} leaves the party", user_index);
  account->LeaveParty();
  return true;
}

bool LivePartyInvite(uint32_t user_index, uint64_t xuid) {
  auto account = LiveAccount(user_index);
  if (!account) {
    return false;
  }
  XELOGI("Live party: user {} invites {:016X}", user_index, xuid);
  account->InviteToParty(xuid, RunningTitle());
  return true;
}

std::optional<net::live::Invite> LivePartyPendingInvite(uint32_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return std::nullopt;
  }
  auto& state = GetParty();
  std::lock_guard lock(state.mutex);
  return state.users[user_index].pending;
}

bool LivePartyAnswerInvite(uint32_t user_index, bool accept) {
  auto account = LiveAccount(user_index);
  if (!account) {
    return false;
  }
  std::optional<net::live::Invite> invite;
  {
    auto& state = GetParty();
    std::lock_guard lock(state.mutex);
    invite = std::move(state.users[user_index].pending);
    state.users[user_index].pending.reset();
  }
  if (!invite) {
    return false;
  }
  XELOGI("Live party: user {} {} the party invite from {}", user_index,
         accept ? "accepted" : "declined", invite->gamertag);
  account->ReplyInvite(invite->id, accept);
  return true;
}

void SetLivePartyEvents(LivePartyEvents events) {
  auto& state = GetParty();
  std::lock_guard lock(state.mutex);
  state.events = std::move(events);
}

void LivePartyOnInvite(uint32_t user_index, const net::live::Invite& invite) {
  if (user_index >= XUserMaxUserCount) {
    return;
  }
  XELOGI("Live party: an invite from {} to user {}", invite.gamertag,
         user_index);
  LivePartyEvents events;
  {
    auto& state = GetParty();
    std::lock_guard lock(state.mutex);
    state.users[user_index].pending = invite;  // The newest wins.
    events = state.events;
  }
  if (events.invited) {
    events.invited(user_index, invite);
  }
  if (cvars::live_auto_accept_invites) {
    LivePartyAnswerInvite(user_index, true);
  }
}

void LivePartyChanged(uint32_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return;
  }
  auto party = LiveParty(user_index);
  if (party) {
    std::string members;
    for (const auto& [xuid, gamertag] : party->members) {
      members += (members.empty() ? "" : ", ") + gamertag;
    }
    XELOGI("Live party: user {} is in party {} ({} member(s): {}; leader "
           "{:016X})",
           user_index, party->room, party->members.size(), members,
           party->leader);
  } else {
    XELOGI("Live party: user {} is in no party", user_index);
  }
  LivePartyEvents events;
  {
    auto& state = GetParty();
    std::lock_guard lock(state.mutex);
    auto& user = state.users[user_index];
    if (!party) {
      user.followed.clear();
      user.room.clear();
      user.restored = false;
      user.offered.reset();
    } else if (party->room != user.room) {
      // A party with no event is the lobby restoring it at sign-in (the app
      // quit while in it); one this app joined or made says so.
      user.restored = party->event.empty();
      user.room = party->room;
      user.followed.clear();
      user.offered.reset();
      if (user.restored) {
        XELOGI("Live party: user {}: party {} came back at sign-in", user_index,
               party->room);
      }
    }
    events = state.events;
  }
  if (events.changed) {
    events.changed(user_index);
  }
  LivePartyPoll(user_index);
}

void LivePartyPoll(uint32_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return;
  }
  auto account = LiveAccount(user_index);
  if (!account) {
    return;
  }
  auto party = LiveParty(user_index);
  TestParty(user_index, *account, party);
  if (party) {
    Follow(user_index, *account, *party);
  }
}

std::optional<uint32_t> LivePartyUser() {
  for (uint32_t i = 0; i < XUserMaxUserCount; ++i) {
    if (auto account = LiveAccount(i); account && account->Party()) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
