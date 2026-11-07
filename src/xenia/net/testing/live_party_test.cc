/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/net/live_account.h"
#include "xenia/net/packet_transport.h"

namespace xe::net::live {
namespace {

TEST_CASE("Live party messages encode", "[live][party]") {
  CHECK(EncodePartyCreate() == R"({"t":"party_create"})");
  CHECK(EncodePartyLeave() == R"({"t":"party_leave"})");
  CHECK(EncodePartyInvite(0x0009F00000000002ull, 0x4D5307E6) ==
        R"({"t":"invite","kind":"party","to":"0009f00000000002",)"
        R"("title":"4D5307E6"})");
  // Without a title, the dashboard's.
  CHECK(EncodePartyInvite(0x0009F00000000002ull, 0) ==
        R"({"t":"invite","kind":"party","to":"0009f00000000002",)"
        R"("title":"FFFE07D1"})");
}

TEST_CASE("Live party messages parse", "[live][party]") {
  auto m = ParseAccountMessage(
      R"({"t":"party","room":"ABC123","leader":"0009f00000000001",)"
      R"("members":[{"xuid":"0009f00000000001","gamertag":"A"},)"
      R"({"xuid":"0009f00000000002","gamertag":"B"}],)"
      R"("event":{"kind":"joined","xuid":"0009f00000000002"}})");
  REQUIRE(m);
  CHECK(m->kind == AccountMessage::Kind::kParty);
  REQUIRE(m->party);
  CHECK(m->party->room == "ABC123");
  CHECK(m->party->leader == 0x0009F00000000001ull);
  REQUIRE(m->party->members.size() == 2);
  CHECK(m->party->members[1].first == 0x0009F00000000002ull);
  CHECK(m->party->members[1].second == "B");
  CHECK(m->party->event == "joined");
  CHECK(m->party->event_xuid == 0x0009F00000000002ull);
  // Lobbies that don't say members' presence.
  CHECK(m->party->presence.empty());

  // Members' presence, when the lobby says it.
  auto p = ParseAccountMessage(
      R"({"t":"party","room":"ABC123","leader":"0009f00000000001",)"
      R"("members":[{"xuid":"0009f00000000001","gamertag":"A",)"
      R"("title":"4D5307E6","joinable":{"room":"","id":"0123456789abcdef",)"
      R"("info":"0a0b"}},{"xuid":"0009f00000000002","gamertag":"B",)"
      R"("online":false}]})");
  REQUIRE(p);
  REQUIRE(p->party);
  REQUIRE(p->party->presence.size() == 2);
  CHECK(p->party->presence[0].xuid == 0x0009F00000000001ull);
  CHECK(p->party->presence[0].online);
  CHECK(p->party->presence[0].title_id == 0x4D5307E6);
  REQUIRE(p->party->presence[0].joinable);
  CHECK(p->party->presence[0].joinable->session_id == "0123456789abcdef");
  CHECK_FALSE(p->party->presence[1].online);

  auto left = ParseAccountMessage(
      R"({"t":"party","room":"ABC123","left":true,)"
      R"("event":{"kind":"left","xuid":"0009f00000000002"}})");
  REQUIRE(left);
  CHECK(left->kind == AccountMessage::Kind::kParty);
  CHECK_FALSE(left->party);

  auto invite = ParseAccountMessage(
      R"({"t":"invite","id":"00112233445566aa","kind":"party",)"
      R"("from":"0009f00000000001","gamertag":"A","title":"FFFE07D1",)"
      R"("room":"ABC123"})");
  REQUIRE(invite);
  CHECK(invite->invite.party);
  CHECK(invite->invite.room == "ABC123");
}

// A lobby socket the test drives: what the client sends, and messages to it.
struct ScriptedLobby {
  struct Socket : secure::LobbySocket {
    explicit Socket(ScriptedLobby& lobby) : lobby(lobby) {}
    void Send(std::string text) override {
      std::lock_guard lock(lobby.mutex);
      lobby.sent.push_back(std::move(text));
      lobby.changed.notify_all();
    }
    bool Ping() override { return true; }
    ScriptedLobby& lobby;
  };

  ScriptedLobby() {
    secure::SetLobbySocketFactory(
        [this](const std::string& url, secure::LobbySocket::Events events)
            -> std::unique_ptr<secure::LobbySocket> {
          std::lock_guard lock(mutex);
          this->url = url;
          this->events = std::move(events);
          changed.notify_all();
          return std::make_unique<Socket>(*this);
        });
  }
  ~ScriptedLobby() { secure::SetLobbySocketFactory(nullptr); }

  // Waits for the client to have sent |count| messages, or opened a socket.
  bool WaitSent(size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5),
                            [&] { return sent.size() >= count; });
  }
  bool WaitOpen() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5),
                            [&] { return bool(events.message); });
  }
  void Deliver(std::string text) {
    std::function<void(std::string)> message;
    {
      std::lock_guard lock(mutex);
      message = events.message;
    }
    message(std::move(text));
  }
  std::vector<std::string> Sent() {
    std::lock_guard lock(mutex);
    return sent;
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::string url;
  secure::LobbySocket::Events events;
  std::vector<std::string> sent;
};

TEST_CASE("Live account sends party requests and keeps the party",
          "[live][party]") {
  ScriptedLobby lobby;
  std::mutex mutex;
  int party_changes = 0;
  AccountClient::Events events;
  events.party_changed = [&] {
    std::lock_guard lock(mutex);
    ++party_changes;
  };
  AccountClient client("wss://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "A", std::move(events));
  // Before signing in, requests wait.
  client.CreateParty();
  REQUIRE(lobby.WaitOpen());
  CHECK(lobby.url == "wss://lobby.test/v2/account/0009f00000000001");
  lobby.Deliver(R"({"t":"challenge","nonce":"n1"})");
  REQUIRE(lobby.WaitSent(1));
  CHECK(lobby.Sent()[0].find(R"("t":"register")") != std::string::npos);
  lobby.Deliver(
      R"({"t":"welcome","v":2,"xuid":"0009f00000000001","gamertag":"A",)"
      R"("tag":"0001","code":"K7Q2M9XD","friends":[{"xuid":)"
      R"("0009f00000000002","gamertag":"B","state":"friend"}]})");
  REQUIRE(lobby.WaitSent(2));
  CHECK(lobby.Sent()[1] == R"({"t":"party_create"})");

  lobby.Deliver(
      R"({"t":"party","room":"ABC123","leader":"0009f00000000001",)"
      R"("members":[{"xuid":"0009f00000000001","gamertag":"A"}],)"
      R"("event":{"kind":"created","xuid":"0009f00000000001"}})");
  auto party = client.Party();
  REQUIRE(party);
  CHECK(party->room == "ABC123");
  CHECK(party->leader == 0x0009F00000000001ull);

  client.InviteToParty(0x0009F00000000002ull, 0x4D5307E6);
  client.LeaveParty();
  REQUIRE(lobby.WaitSent(4));
  CHECK(lobby.Sent()[2] ==
        R"({"t":"invite","kind":"party","to":"0009f00000000002",)"
        R"("title":"4D5307E6"})");
  CHECK(lobby.Sent()[3] == R"({"t":"party_leave"})");

  lobby.Deliver(R"({"t":"party","room":"ABC123","left":true})");
  CHECK_FALSE(client.Party());
  std::lock_guard lock(mutex);
  CHECK(party_changes == 2);
}

size_t CountSent(ScriptedLobby& lobby, std::string_view needle) {
  size_t n = 0;
  for (const auto& text : lobby.Sent()) {
    n += text.find(needle) != std::string::npos;
  }
  return n;
}

void SignIn(ScriptedLobby& lobby) {
  REQUIRE(lobby.WaitOpen());
  lobby.Deliver(R"({"t":"challenge","nonce":"n1"})");
  REQUIRE(lobby.WaitSent(1));
  lobby.Deliver(
      R"({"t":"welcome","v":2,"xuid":"0009f00000000001","gamertag":"A",)"
      R"("tag":"0001","code":"K7Q2M9XD","friends":[{"xuid":)"
      R"("0009f00000000002","gamertag":"B","state":"friend"}]})");
}

constexpr std::string_view kPartyInviteB =
    R"({"t":"invite","kind":"party","to":"0009f00000000002")";

// 2026-11-07 QA: a relaunched app invited before the lobby had restored its
// party, so it started a second one and left the first to its other member.
TEST_CASE("Live account invites into the party a relaunch restores",
          "[live][party]") {
  ScriptedLobby lobby;
  AccountClient client("wss://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "A", {});
  SignIn(lobby);
  // Signed in, the party not said yet.
  client.InviteToParty(0x0009F00000000002ull, 0x4D5307E6);
  CHECK(CountSent(lobby, kPartyInviteB) == 1);
  CHECK(CountSent(lobby, "party_create") == 0);
  // The lobby restores the party the account was in (no event).
  lobby.Deliver(
      R"({"t":"party","room":"ER2YRF","leader":"0009f00000000001",)"
      R"("members":[{"xuid":"0009f00000000001","gamertag":"A"},)"
      R"({"xuid":"0009f00000000002","gamertag":"B"}]})");
  auto party = client.Party();
  REQUIRE(party);
  CHECK(party->room == "ER2YRF");
  CHECK(party->members.size() == 2);
  CHECK(party->event.empty());
  CHECK(CountSent(lobby, "party_create") == 0);
  // Leaving another party, told late, leaves this one alone.
  lobby.Deliver(R"({"t":"party","room":"OLD111","left":true,)"
                R"("event":{"kind":"left","xuid":"0009f00000000001"}})");
  party = client.Party();
  REQUIRE(party);
  CHECK(party->room == "ER2YRF");
  // Leaving this one doesn't.
  lobby.Deliver(R"({"t":"party","room":"ER2YRF","left":true})");
  CHECK_FALSE(client.Party());
}

TEST_CASE("Live account starts a party when the lobby has none to invite to",
          "[live][party]") {
  ScriptedLobby lobby;
  AccountClient client("wss://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "A", {});
  SignIn(lobby);
  client.InviteToParty(0x0009F00000000002ull, 0x4D5307E6);
  client.InviteToParty(0x0009F00000000003ull, 0x4D5307E6);
  CHECK(CountSent(lobby, "party_create") == 0);
  lobby.Deliver(R"({"t":"error","for":"invite","reason":"no_party",)"
                R"("to":"0009f00000000002"})");
  CHECK(CountSent(lobby, "party_create") == 1);
  // One party for both.
  lobby.Deliver(R"({"t":"error","for":"invite","reason":"no_party",)"
                R"("to":"0009f00000000003"})");
  CHECK(CountSent(lobby, "party_create") == 1);
  lobby.Deliver(
      R"({"t":"party","room":"NEW222","leader":"0009f00000000001",)"
      R"("members":[{"xuid":"0009f00000000001","gamertag":"A"}],)"
      R"("event":{"kind":"created","xuid":"0009f00000000001"}})");
  CHECK(CountSent(lobby, kPartyInviteB) == 2);
  CHECK(CountSent(lobby,
                  R"({"t":"invite","kind":"party","to":"0009f00000000003")") ==
        2);
  // A no_party for an invite it didn't send unsure starts nothing.
  lobby.Deliver(R"({"t":"error","for":"invite","reason":"no_party",)"
                R"("to":"0009f00000000009"})");
  CHECK(CountSent(lobby, "party_create") == 1);
}

}  // namespace
}  // namespace xe::net::live
