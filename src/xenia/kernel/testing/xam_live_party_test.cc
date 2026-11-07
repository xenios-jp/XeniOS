/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_live_party.h"

#include <cstring>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace {

constexpr uint64_t kLeader = 0x0009F00000000001ull;
constexpr uint64_t kMember = 0x0009F00000000002ull;
constexpr uint32_t kTitle = 0x4D5307E6;

net::live::PartyState Party() {
  net::live::PartyState party;
  party.room = "ABC123";
  party.leader = kLeader;
  party.members = {{kLeader, "Leader"}, {kMember, "Member"}};
  return party;
}

net::live::Presence LeaderIn(std::string session, uint32_t title = kTitle) {
  net::live::Presence p;
  p.xuid = kLeader;
  p.gamertag = "Leader";
  p.online = true;
  p.title_id = title;
  net::live::Joinable j;
  j.session_id = std::move(session);
  j.info.assign(sizeof(XSESSION_INFO), 0x5A);
  p.joinable = std::move(j);
  return p;
}

TEST_CASE("Party members follow the leader into its session",
          "[live_party]") {
  const auto party = Party();
  auto leader = LeaderIn("00000000000000aa");
  PartyFollowInput in;
  in.self = kMember;
  in.party = &party;
  in.leader = &leader;
  in.running_title = kTitle;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kJoin);

  // Once per session.
  in.followed_session = "00000000000000aa";
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kNone);
  // A new session of the leader's: again.
  auto moved = LeaderIn("00000000000000bb");
  in.leader = &moved;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kJoin);
  // Already in it.
  in.own_session = "00000000000000bb";
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kNone);
}

TEST_CASE("A member who invited the leader waits for it", "[live_party]") {
  const auto party = Party();
  // The leader's presence still shows the session it is coming from.
  auto leader = LeaderIn("00000000000000aa");
  PartyFollowInput in;
  in.self = kMember;
  in.party = &party;
  in.leader = &leader;
  in.running_title = kTitle;
  in.own_session = "00000000000000cc";
  in.invited_leader_to = "00000000000000cc";
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kWait);
  // It arrived.
  auto arrived = LeaderIn("00000000000000cc");
  in.leader = &arrived;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kNone);
  // An invite to a session the member has since left doesn't hold it back.
  in.leader = &leader;
  in.own_session = "00000000000000dd";
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kJoin);
  // Nor does one with no session of its own.
  in.own_session.clear();
  in.invited_leader_to.clear();
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kJoin);
}

// 2026-11-07 QA: a relaunched member followed the leader into a Live session
// with no prompt.
TEST_CASE("A party restored at sign-in asks before following",
          "[live_party]") {
  const auto party = Party();
  auto leader = LeaderIn("00000000000000aa");
  PartyFollowInput in;
  in.self = kMember;
  in.party = &party;
  in.leader = &leader;
  in.running_title = kTitle;
  in.restored = true;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kAsk);
  // In another title the app is told, as always (it asks there anyway).
  in.running_title = 0x41560817;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kTellApp);
  // Already in it: nothing to ask.
  in.running_title = kTitle;
  in.own_session = "00000000000000aa";
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kNone);
}

TEST_CASE("Party members in another title tell the app", "[live_party]") {
  const auto party = Party();
  auto leader = LeaderIn("00000000000000aa");
  PartyFollowInput in;
  in.self = kMember;
  in.party = &party;
  in.leader = &leader;
  in.running_title = 0x41560817;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kTellApp);
  // No title running (the app's home screen).
  in.running_title = 0;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kTellApp);
  // A session without its XSESSION_INFO can't be joined from the title.
  leader.joinable->info.clear();
  in.running_title = kTitle;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kTellApp);
}

TEST_CASE("Nothing to follow", "[live_party]") {
  const auto party = Party();
  PartyFollowInput in;
  in.self = kMember;
  in.party = &party;
  in.running_title = kTitle;
  // The leader's presence unknown.
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kNone);
  // Not joinable.
  auto leader = LeaderIn("00000000000000aa");
  leader.joinable.reset();
  in.leader = &leader;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kNone);
  // Offline.
  leader = LeaderIn("00000000000000aa");
  leader.online = false;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kNone);
  // Someone else's presence.
  leader = LeaderIn("00000000000000aa");
  leader.xuid = kMember;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kNone);
  // The leader follows nobody.
  leader = LeaderIn("00000000000000aa");
  in.self = kLeader;
  CHECK(DecidePartyFollow(in) == PartyFollowAction::kNone);
}

TEST_CASE("XamPartyGetUserList entries", "[live_party]") {
  auto party = Party();
  party.members[1].second = "AVeryLongGamertagIndeed";
  party.presence.push_back(LeaderIn("00000000000000aa"));
  std::vector<uint8_t> own(sizeof(XSESSION_INFO), 0x11);

  X_PARTY_USER_INFO leader;
  FillPartyUser(party, 0, kMember, 2, own, &leader);
  CHECK(uint64_t(leader.xuid) == kLeader);
  CHECK(std::string(leader.gamertag) == "Leader");
  CHECK(uint32_t(leader.user_index) == XUserIndexNone);
  CHECK(uint32_t(leader.title_id) == kTitle);
  CHECK(reinterpret_cast<const uint8_t*>(&leader.session_info)[0] == 0x5A);

  X_PARTY_USER_INFO self;
  FillPartyUser(party, 1, kMember, 2, own, &self);
  CHECK(uint64_t(self.xuid) == kMember);
  // Cut to 15 characters and a terminator.
  CHECK(std::string(self.gamertag) == "AVeryLongGamert");
  CHECK(uint32_t(self.user_index) == 2);
  CHECK(reinterpret_cast<const uint8_t*>(&self.session_info)[0] == 0x11);

  CHECK(offsetof(X_PARTY_USER_LIST, users) == 8);
  CHECK(offsetof(X_PARTY_USER_INFO, session_info) == 0x28);
  CHECK(offsetof(X_PARTY_USER_INFO, custom_data) == 0x68);
  CHECK(offsetof(X_PARTY_USER_LIST_INTERNAL, users) == 8);
}

}  // namespace
}  // namespace xam
}  // namespace kernel
}  // namespace xe
