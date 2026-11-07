/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_live_invites.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/net/relay_protocol.h"
#include "xenia/base/cvar.h"

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "third_party/catch/include/catch.hpp"

DECLARE_string(systemlink_room);

namespace xe {
namespace kernel {
namespace xam {
namespace {

void U32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(value >> shift));
  }
}

void U64(std::vector<uint8_t>& out, uint64_t value) {
  U32(out, uint32_t(value >> 32));
  U32(out, uint32_t(value));
}

std::vector<uint8_t> Request(uint32_t user, std::vector<uint64_t> xuids,
                             std::u16string text, uint32_t length,
                             uint32_t handle) {
  std::vector<uint8_t> out;
  U32(out, user);
  U32(out, uint32_t(xuids.size()));
  for (const uint64_t xuid : xuids) {
    U64(out, xuid);
  }
  U32(out, length);
  for (uint32_t i = 0; i < length; ++i) {
    const char16_t c = i < text.size() ? text[i] : 0;
    out.push_back(uint8_t(c >> 8));
    out.push_back(uint8_t(c));
  }
  U32(out, handle);
  return out;
}

TEST_CASE("XInviteSend request parses", "[live_invites]") {
  const auto bytes =
      Request(1, {0x0009000011FBEF5Dull, 0x0009000044F158E9ull}, u"Join me",
              7, 0xF8000123);
  const auto request = ParseInviteSendRequest(bytes);
  REQUIRE(request);
  REQUIRE(request->user_index == 1);
  REQUIRE(request->invitees.size() == 2);
  REQUIRE(request->invitees[0] == 0x0009000011FBEF5Dull);
  REQUIRE(request->invitees[1] == 0x0009000044F158E9ull);
  REQUIRE(request->text == u"Join me");
  REQUIRE(request->message_handle == 0xF8000123);
}

TEST_CASE("XInviteSend request with a terminated string", "[live_invites]") {
  // A length that counts the terminator and padding after it.
  const auto bytes = Request(0, {0x0009000011FBEF5Dull}, u"Hi", 5, 7);
  const auto request = ParseInviteSendRequest(bytes);
  REQUIRE(request);
  REQUIRE(request->text == u"Hi");
  REQUIRE(request->message_handle == 7);
}

TEST_CASE("XInviteSend request without text", "[live_invites]") {
  const auto request =
      ParseInviteSendRequest(Request(0, {0x0009000011FBEF5Dull}, u"", 0, 0));
  REQUIRE(request);
  REQUIRE(request->text.empty());
  REQUIRE(request->invitees.size() == 1);
}

TEST_CASE("XInviteSend request rejects bad sizes", "[live_invites]") {
  auto bytes = Request(0, {1, 2}, u"abc", 3, 9);
  // Cut anywhere: no request.
  for (size_t size = 0; size < bytes.size(); ++size) {
    REQUIRE_FALSE(ParseInviteSendRequest({bytes.data(), size}));
  }
  // Too many invitees.
  REQUIRE_FALSE(ParseInviteSendRequest(
      Request(0, std::vector<uint64_t>(kInviteMaxInvitees + 1, 1), u"", 0, 0)));
  // A count larger than the bytes.
  bytes = Request(0, {1}, u"", 0, 0);
  bytes[7] = 50;
  REQUIRE_FALSE(ParseInviteSendRequest(bytes));
  // Text longer than allowed.
  REQUIRE_FALSE(ParseInviteSendRequest(
      Request(0, {1}, std::u16string(kInviteMaxText + 1, u'a'),
              kInviteMaxText + 1, 0)));
}

TEST_CASE("X_INVITE_INFO layout and fill", "[live_invites]") {
  REQUIRE(sizeof(X_INVITE_INFO) == 0x54);

  AcceptedInvite invite;
  invite.invitee = 0x0009000011FBEF5Dull;
  invite.inviter = 0x0009000044F158E9ull;
  invite.title_id = 0x4D5307E6;
  for (size_t i = 0; i < sizeof(XSESSION_INFO); ++i) {
    invite.info.push_back(uint8_t(i + 1));
  }
  X_INVITE_INFO info;
  REQUIRE(FillInviteInfo(invite, &info));
  const auto* raw = reinterpret_cast<const uint8_t*>(&info);
  // Big-endian fields.
  const uint8_t invitee[8] = {0x00, 0x09, 0x00, 0x00, 0x11, 0xFB, 0xEF, 0x5D};
  const uint8_t inviter[8] = {0x00, 0x09, 0x00, 0x00, 0x44, 0xF1, 0x58, 0xE9};
  const uint8_t title[4] = {0x4D, 0x53, 0x07, 0xE6};
  const uint8_t from_game[4] = {0, 0, 0, 1};
  REQUIRE(!std::memcmp(raw, invitee, 8));
  REQUIRE(!std::memcmp(raw + 0x8, inviter, 8));
  REQUIRE(!std::memcmp(raw + 0x10, title, 4));
  REQUIRE(!std::memcmp(raw + 0x14, invite.info.data(), 0x3C));
  REQUIRE(!std::memcmp(raw + 0x50, from_game, 4));
  REQUIRE(info.host_info.session_id[0] == 1);
  REQUIRE(info.host_info.key_exchange_key[15] == 0x3C);
}

TEST_CASE("X_INVITE_INFO needs the session", "[live_invites]") {
  AcceptedInvite invite;
  invite.inviter = 5;
  invite.info.resize(0x20, 0xFF);
  X_INVITE_INFO info;
  std::memset(&info, 0xAA, sizeof(info));
  REQUIRE_FALSE(FillInviteInfo(invite, &info));
  const auto* raw = reinterpret_cast<const uint8_t*>(&info);
  for (size_t i = 0; i < sizeof(info); ++i) {
    REQUIRE(raw[i] == 0);
  }
}

AcceptedInvite LaunchInvite(uint8_t session) {
  AcceptedInvite invite;
  invite.inviter = 0x0009000044F158E9ull;
  invite.title_id = 0x4D5307E6;
  invite.info.assign(sizeof(XSESSION_INFO), 0);
  invite.info[0] = session;
  return invite;
}

// Stand-ins for running titles' kernels: the invites only compare them.
KernelState* FakeKernel(uintptr_t n) {
  return reinterpret_cast<KernelState*>(n * 0x1000);
}

TEST_CASE("An accepted invite is read once", "[live_invites]") {
  ClearAcceptedInvites();
  SetAcceptedInvite(0, LaunchInvite(1));
  InstallLiveInvites(FakeKernel(1));
  CHECK(UsersWithAcceptedInvite(0x4D5307E6) == std::vector<uint32_t>{0});
  const auto taken = TakeAcceptedInvite(0);
  REQUIRE(taken);
  CHECK(taken->info[0] == 1);
  CHECK_FALSE(TakeAcceptedInvite(0));
  CHECK(UsersWithAcceptedInvite(0x4D5307E6).empty());
  ShutdownLiveInvites();
  InstallLiveInvites(FakeKernel(2));
  CHECK_FALSE(TakeAcceptedInvite(0));
  ShutdownLiveInvites();
}

TEST_CASE("An accepted invite the title didn't read ends with it",
          "[live_invites]") {
  ClearAcceptedInvites();
  SetAcceptedInvite(0, LaunchInvite(2));
  InstallLiveInvites(FakeKernel(3));
  // The title stops (or is started again) without reading it.
  ShutdownLiveInvites();
  InstallLiveInvites(FakeKernel(4));
  CHECK(UsersWithAcceptedInvite(0x4D5307E6).empty());
  CHECK_FALSE(TakeAcceptedInvite(0));
  ShutdownLiveInvites();
}

TEST_CASE("An invite accepted for the next title outlives the running one",
          "[live_invites]") {
  ClearAcceptedInvites();
  InstallLiveInvites(FakeKernel(5));
  // Accepted in the library while a game runs: that game stops for it.
  SetAcceptedInvite(0, LaunchInvite(3));
  ShutdownLiveInvites();
  InstallLiveInvites(FakeKernel(6));
  const auto taken = TakeAcceptedInvite(0);
  REQUIRE(taken);
  CHECK(taken->info[0] == 3);
  ShutdownLiveInvites();
  // Not read by the title it was for: it doesn't reach a third.
  SetAcceptedInvite(1, LaunchInvite(4));
  InstallLiveInvites(FakeKernel(7));
  ShutdownLiveInvites();
  InstallLiveInvites(FakeKernel(8));
  CHECK_FALSE(TakeAcceptedInvite(1));
  ShutdownLiveInvites();
}

TEST_CASE("A join that can't reach the host says why", "[live_invites]") {
  std::mutex mutex;
  std::condition_variable told;
  std::vector<LiveJoinFailure> reasons;
  SetLiveJoinFailedHandler(
      [&](uint32_t, const std::string&, LiveJoinFailure reason) {
        std::lock_guard lock(mutex);
        reasons.push_back(reason);
        told.notify_all();
      },
      std::chrono::milliseconds(10));
  ClearAcceptedInvites();
  SetAcceptedInvite(0, LaunchInvite(9));
  InstallLiveInvites(FakeKernel(9));
  REQUIRE(TakeAcceptedInvite(0));
  // Another session's association isn't this join's.
  LiveJoinAssociation({7}, false);
  LiveJoinAssociation({9}, false);
  {
    std::unique_lock lock(mutex);
    told.wait_for(lock, std::chrono::seconds(2),
                  [&] { return !reasons.empty(); });
  }
  // No account here, so no presence: as if the host left.
  CHECK(reasons == std::vector<LiveJoinFailure>{LiveJoinFailure::kHostLeft});
  // Once per join.
  LiveJoinAssociation({9}, false);
  // A retry that connects before the settle time says nothing.
  SetAcceptedInvite(0, LaunchInvite(10));
  REQUIRE(TakeAcceptedInvite(0));
  LiveJoinAssociation({10}, false);
  LiveJoinAssociation({10}, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  {
    std::lock_guard lock(mutex);
    CHECK(reasons.size() == 1);
  }
  SetLiveJoinFailedHandler(nullptr);
  ShutdownLiveInvites();
}

TEST_CASE("An invite to another game carries to its launch",
          "[live_invites]") {
  AcceptedInvite invite = LaunchInvite(7);
  invite.invitee = 0x0009000011FBEF5Dull;
  invite.info[0x3B] = 0xAB;
  const std::string room = net::relay::NewRoomCode();
  const std::string text = EncodeAcceptedInvite(2, invite, room);
  const auto launch = DecodeAcceptedInvite(text);
  REQUIRE(launch);
  CHECK(launch->user_index == 2);
  CHECK(launch->room == room);
  CHECK(launch->invite.invitee == invite.invitee);
  CHECK(launch->invite.inviter == invite.inviter);
  CHECK(launch->invite.title_id == 0x4D5307E6);
  CHECK(launch->invite.from_game_invite);
  CHECK(launch->invite.info == invite.info);
  // A System Link game's: no session, the public lobby.
  AcceptedInvite system_link;
  system_link.inviter = 5;
  system_link.title_id = 0x415608C3;
  system_link.from_game_invite = false;
  const auto lobby = DecodeAcceptedInvite(EncodeAcceptedInvite(0, system_link, ""));
  REQUIRE(lobby);
  CHECK(lobby->invite.info.empty());
  CHECK(lobby->room.empty());
  CHECK_FALSE(lobby->invite.from_game_invite);
  // Anything else isn't one.
  for (const std::string_view bad :
       {"", "x", "0:1:2:4D5307E6:1:", "4:1:2:4D5307E6:1::", "0:1:2:0:1::",
        "0:1:2:4D5307E6:2::", "0:1:2:4D5307E6:1:abcd:",
        "0:1:2:4D5307E6:1::room:more", "0:1g:2:4D5307E6:1::"}) {
    CHECK_FALSE(DecodeAcceptedInvite(bad));
  }
  std::string cut = text;
  // The session one byte short.
  cut.erase(cut.size() - room.size() - 3, 2);
  CHECK_FALSE(DecodeAcceptedInvite(cut));
}

TEST_CASE("A title reads only its own game's invite", "[live_invites]") {
  ClearAcceptedInvites();
  SetAcceptedInvite(0, LaunchInvite(11));
  InstallLiveInvites(FakeKernel(10));
  CHECK_FALSE(TakeAcceptedInvite(0, 0x415608C3));
  const auto taken = TakeAcceptedInvite(0, 0x4D5307E6);
  REQUIRE(taken);
  CHECK(taken->info[0] == 11);
  ShutdownLiveInvites();
}

TEST_CASE("Joining a friend's System Link room", "[live_invites]") {
  const std::string before = cvars::systemlink_room;
  const std::string code = net::relay::NewRoomCode();
  // Without the app's policy, systemlink_room.
  JoinSystemLinkRoom(0x415608C3, code);
  CHECK(cvars::systemlink_room == code);
  JoinSystemLinkRoom(0x415608C3, "");
  CHECK(cvars::systemlink_room.empty());
  cvars::systemlink_room = code;
  JoinSystemLinkRoom(0x415608C3, "not a room");
  CHECK(cvars::systemlink_room == code);
  // The app's, which keeps the room for that launch.
  std::vector<std::pair<uint32_t, std::string>> joined;
  SystemLinkRelayPolicy policy;
  policy.join_room = [&](uint32_t title_id, const std::string& room) {
    joined.emplace_back(title_id, room);
  };
  SetSystemLinkRelayPolicy(std::move(policy));
  JoinSystemLinkRoom(0x415608C3, "");
  CHECK(joined == std::vector<std::pair<uint32_t, std::string>>{
                      {0x415608C3, ""}});
  CHECK(cvars::systemlink_room == code);
  SetSystemLinkRelayPolicy({});
  cvars::systemlink_room = before;
}

}  // namespace
}  // namespace xam
}  // namespace kernel
}  // namespace xe
