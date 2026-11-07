/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/live_directory.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/net/testing/fake_lobby.h"

namespace xe::net::live {
namespace {

DirectoryAuth TestAuth() {
  DirectoryAuth auth;
  auth.xuid = [] { return uint64_t(0x0009F00012345678ull); };
  auth.token = []() -> std::optional<std::string> { return "tok1"; };
  return auth;
}

DirectoryAdvert Sample() {
  DirectoryAdvert a;
  a.id = "0123456789abcdef";
  a.info = {0x01, 0x23, 0xAB, 0xFF};
  a.room = "K7QM2X";
  a.flags = 0x3D;
  a.public_slots = 16;
  a.used_public = 1;
  a.playing = true;
  a.join_in_progress = true;
  a.xuid = 0x0009F00012345678ull;
  a.contexts = {{0x8000800A, 0}, {0x8000800B, 13}};
  a.props = {{0x10000015, {0x01, 0x00, 0x00, 0x00}}};
  a.strings = {{0x40008109, "Host Name"}};
  return a;
}

TEST_CASE("Live directory adverts survive the lobby", "[live]") {
  const auto sent = EncodeAdvertise(Sample(), 300);
  CHECK(sent.find(R"("t":"advertise")") != std::string::npos);
  CHECK(sent.find(R"("ttl":300)") != std::string::npos);
  CHECK(sent.find(R"("xuid":"0009f00012345678")") != std::string::npos);

  // The lobby hands an advert back in results as it was sent, without t and
  // ttl.
  auto reply = ParseDirectoryReply(
      fmt::format(R"({{"t":"results","seq":7,"sessions":[{}]}})", sent));
  REQUIRE(reply);
  CHECK(reply->kind == DirectoryReply::Kind::kResults);
  CHECK(reply->seq == 7);
  REQUIRE(reply->sessions.size() == 1);
  const auto& got = reply->sessions[0];
  const auto want = Sample();
  CHECK(got.id == want.id);
  CHECK(got.info == want.info);
  CHECK(got.room == want.room);
  CHECK(got.flags == want.flags);
  CHECK(got.public_slots == 16);
  CHECK(got.used_public == 1);
  CHECK(got.playing);
  CHECK(got.join_in_progress);
  CHECK(got.xuid == want.xuid);
  CHECK(got.contexts == want.contexts);
  CHECK(got.props == want.props);
  CHECK(got.strings == want.strings);
}

TEST_CASE("Live directory replies leave out bad sessions", "[live]") {
  auto reply = ParseDirectoryReply(
      R"({"t":"results","seq":2,"sessions":[)"
      R"({"id":"short","info":"00","room":"A"},)"
      R"({"id":"0123456789abcdef","info":"0g","room":"A"},)"
      R"({"id":"0123456789abcdef","info":"00","room":"A","contexts":[[1]]},)"
      R"({"id":"0123456789abcdef","info":"00","room":"ABC123"},)"
      R"({"id":"0123456789abcdef","info":"00","room":""}]})");
  REQUIRE(reply);
  REQUIRE(reply->sessions.size() == 2);
  CHECK(reply->sessions[0].room == "ABC123");
  CHECK(reply->sessions[1].room.empty());

  CHECK_FALSE(ParseDirectoryReply("not json"));
  CHECK_FALSE(ParseDirectoryReply(R"({"seq":1})"));
  auto other = ParseDirectoryReply(R"({"t":"pong"})");
  REQUIRE(other);
  CHECK(other->kind == DirectoryReply::Kind::kOther);
}

TEST_CASE("Live directory queries", "[live]") {
  CHECK(EncodeSearch({{1, 2}}, 99, true, 5) ==
        R"({"t":"search","contexts":[[1,2]],"inProgress":true,"max":50,"seq":5})");
  CHECK(EncodeLookup({"0123456789abcdef"}, 8) ==
        R"({"t":"lookup","ids":["0123456789abcdef"],"seq":8})");
  CHECK(EncodeWithdraw("0123456789abcdef") ==
        R"({"t":"withdraw","id":"0123456789abcdef"})");
  auto refreshed =
      ParseDirectoryReply(R"({"t":"refreshed","ids":["0123456789abcdef"]})");
  REQUIRE(refreshed);
  CHECK(refreshed->kind == DirectoryReply::Kind::kRefreshed);
  CHECK(refreshed->ids == std::vector<std::string>{"0123456789abcdef"});
}


TEST_CASE("Live directory adverts fit the lobby's limits", "[live]") {
  DirectoryAdvert a;
  a.id = "0123456789abcdef";
  a.public_slots = 5000;
  a.used_public = 4000;
  a.private_slots = 2;
  for (uint32_t i = 0; i < 40; ++i) {
    a.contexts.emplace_back(i, i);
    a.props.emplace_back(i, std::vector<uint8_t>{1});
  }
  a.contexts.insert(a.contexts.begin() + 1, {0, 9});  // A repeated id.
  const DirectoryAdvert fit = FitDirectoryLimits(a);
  CHECK(fit.public_slots == kDirectoryMaxSlots);
  CHECK(fit.used_public == kDirectoryMaxSlots);
  CHECK(fit.private_slots == 2);
  REQUIRE(fit.contexts.size() == kDirectoryMaxPairs);
  CHECK(fit.contexts[0] == std::pair<uint32_t, uint32_t>{0, 0});
  CHECK(fit.contexts[1] == std::pair<uint32_t, uint32_t>{1, 1});
  CHECK(fit.props.size() == kDirectoryMaxPairs);
  // What goes out is what fits.
  const std::string text = EncodeAdvertise(a, 300);
  CHECK(text.find("\"public\":1024") != std::string::npos);
  CHECK(text.find("[32,32]") == std::string::npos);

  auto reject = ParseDirectoryReply(R"({"t":"reject","reason":"bad_request"})");
  REQUIRE(reject);
  CHECK(reject->kind == DirectoryReply::Kind::kError);
  CHECK(reject->reason == "bad_request");
}

// An account with a token of its own, and a count of the tokens asked for.
struct FakeAccount {
  std::mutex mutex;
  std::optional<std::string> token = "tok1";
  int requests = 0;
  DirectoryAuth Auth() {
    DirectoryAuth auth;
    auth.xuid = [] { return uint64_t(0x0009F00012345678ull); };
    auth.token = [this] {
      std::lock_guard lock(mutex);
      return token;
    };
    auth.request_token = [this] {
      std::lock_guard lock(mutex);
      ++requests;
    };
    return auth;
  }
  void Give(std::string t) {
    std::lock_guard lock(mutex);
    token = std::move(t);
  }
};

constexpr char kAuthed[] =
    R"({"t":"authed","xuid":"0009f00012345678","gamertag":"T"})";

DirectoryAdvert TestAdvert() {
  DirectoryAdvert a;
  a.id = "0123456789abcdef";
  a.info = {1, 2, 3};
  a.public_slots = 4;
  return a;
}

TEST_CASE("Live directory messages for signing in and updating", "[live]") {
  CHECK(EncodeDirectoryAuth(0x0009F00012345678ull, "ab12") ==
        R"({"t":"auth","xuid":"0009f00012345678","token":"ab12"})");
  CHECK(EncodeUpdate("0123456789abcdef", 3, 1, 300) ==
        R"({"t":"update","id":"0123456789abcdef","slots":{"usedPublic":3,)"
        R"("usedPrivate":1},"ttl":300})");
  auto authed = ParseDirectoryReply(kAuthed);
  REQUIRE(authed);
  CHECK(authed->kind == DirectoryReply::Kind::kAuthed);
  CHECK(authed->xuid == 0x0009F00012345678ull);
  CHECK(authed->gamertag == "T");
  auto updated =
      ParseDirectoryReply(R"({"t":"updated","id":"0123456789abcdef"})");
  REQUIRE(updated);
  CHECK(updated->kind == DirectoryReply::Kind::kUpdated);
  auto banned = ParseDirectoryReply(
      R"({"t":"reject","reason":"banned","appeal":"https://x.test/a"})");
  REQUIRE(banned);
  CHECK(banned->kind == DirectoryReply::Kind::kError);
  CHECK(banned->rejected);
  CHECK(banned->appeal == "https://x.test/a");
  auto needs = ParseDirectoryReply(
      R"({"t":"error","for":"search","reason":"auth_required"})");
  REQUIRE(needs);
  CHECK_FALSE(needs->rejected);
  CHECK(needs->for_request == "search");

  // Mode and map go out, cut to 64 bytes.
  DirectoryAdvert a = TestAdvert();
  a.mode = "Slayer";
  a.map = std::string(100, 'm');
  const std::string text = EncodeAdvertise(a, 300);
  CHECK(text.find(R"("mode":"Slayer")") != std::string::npos);
  CHECK(text.find("\"map\":\"" + std::string(64, 'm') + "\"") !=
        std::string::npos);
}

TEST_CASE("Live directory signs in before it advertises", "[live]") {
  testing::FakeLobby lobby;
  FakeAccount account;
  SessionDirectory directory("ws://lobby.test", 0x4D5307E6, account.Auth());
  directory.Advertise(TestAdvert());
  auto socket = lobby.WaitForSocket();
  CHECK(lobby.Urls().front() == "ws://lobby.test/v2/sessions/4D5307E6");
  socket.opened();
  // Only the sign-in goes first; the advert waits for the answer.
  auto sent = lobby.Sent();
  REQUIRE(sent.size() == 1);
  CHECK(sent[0] == EncodeDirectoryAuth(0x0009F00012345678ull, "tok1"));
  // Changes before the answer are kept, not sent.
  CHECK(directory.Update("0123456789abcdef", 2, 0));
  CHECK_FALSE(directory.Update("ffffffffffffffff", 2, 0));
  CHECK(lobby.Sent().size() == 1);
  socket.message(kAuthed);
  sent = lobby.Sent();
  REQUIRE(sent.size() == 2);
  CHECK(sent[1].find(R"("t":"advertise")") != std::string::npos);
  CHECK(sent[1].find(R"("usedPublic":2)") != std::string::npos);
  // Now an update goes straight out.
  CHECK(directory.Update("0123456789abcdef", 3, 0));
  CHECK(lobby.Sent().back() ==
        EncodeUpdate("0123456789abcdef", 3, 0, 300));
  directory.Withdraw("0123456789abcdef");
  CHECK(lobby.Sent().back() == EncodeWithdraw("0123456789abcdef"));
  CHECK(account.requests == 0);
}

TEST_CASE("Live directory asks the account for a token it lacks", "[live]") {
  testing::FakeLobby lobby;
  FakeAccount account;
  account.token.reset();
  SessionDirectory directory("ws://lobby.test", 0x4D5307E6, account.Auth());
  directory.Advertise(TestAdvert());
  auto socket = lobby.WaitForSocket();
  socket.opened();
  CHECK(lobby.Sent().empty());
  CHECK(account.requests == 1);
  // The token arrives: sign in, then advertise.
  account.Give("tok2");
  directory.TokenReady();
  auto sent = lobby.Sent();
  REQUIRE(sent.size() == 1);
  CHECK(sent[0] == EncodeDirectoryAuth(0x0009F00012345678ull, "tok2"));
  // Not again for the same socket.
  directory.TokenReady();
  CHECK(lobby.Sent().size() == 1);
  socket.message(kAuthed);
  CHECK(lobby.Sent().size() == 2);
  CHECK(account.requests == 1);
}

TEST_CASE("Live directory searches wait for the sign-in", "[live]") {
  using namespace std::chrono_literals;
  testing::FakeLobby lobby;
  FakeAccount account;
  SessionDirectory directory("ws://lobby.test", 0x4D5307E6, account.Auth());
  std::thread lobby_thread([&] {
    auto socket = lobby.WaitForSocket();
    socket.opened();
    std::this_thread::sleep_for(50ms);
    // The search is held back until the lobby answers the sign-in.
    CHECK(lobby.Sent().size() == 1);
    socket.message(kAuthed);
    while (lobby.Sent().size() < 2) {
      std::this_thread::sleep_for(1ms);
    }
    CHECK(lobby.Sent()[1].find(R"("t":"search")") != std::string::npos);
    socket.message(R"({"t":"results","seq":1,"sessions":[]})");
  });
  CHECK(directory.Search({}, 10, false, 5000).empty());
  lobby_thread.join();
  CHECK(lobby.Sent().size() == 2);
}

TEST_CASE("Live directory gets a new token once after a denial", "[live]") {
  testing::FakeLobby lobby;
  FakeAccount account;
  SessionDirectory directory("ws://lobby.test", 0x4D5307E6, account.Auth());
  directory.Advertise(TestAdvert());
  auto socket = lobby.WaitForSocket();
  socket.opened();
  REQUIRE(lobby.Sent().size() == 1);
  socket.message(R"({"t":"reject","reason":"denied"})");
  CHECK(account.requests == 1);
  // The old token isn't sent again, even if asked to sign in.
  directory.TokenReady();
  CHECK(lobby.Sent().size() == 1);
  account.Give("tok2");
  directory.TokenReady();
  REQUIRE(lobby.Sent().size() == 2);
  CHECK(lobby.Sent()[1] == EncodeDirectoryAuth(0x0009F00012345678ull, "tok2"));
  // Denied again: no more requests.
  socket.message(R"({"t":"reject","reason":"denied"})");
  CHECK(account.requests == 1);
  account.Give("tok3");
  directory.TokenReady();
  CHECK(lobby.Sent().size() == 2);
  CHECK(directory.Search({}, 10, false, 50).empty());
}

TEST_CASE("Live directory stops when the account is banned", "[live]") {
  testing::FakeLobby lobby;
  FakeAccount account;
  SessionDirectory directory("ws://lobby.test", 0x4D5307E6, account.Auth());
  directory.Advertise(TestAdvert());
  auto socket = lobby.WaitForSocket();
  socket.opened();
  socket.message(
      R"({"t":"reject","reason":"banned","appeal":"https://x.test/a"})");
  CHECK(account.requests == 0);
  directory.TokenReady();
  CHECK(lobby.Sent().size() == 1);
  CHECK(directory.Search({}, 10, false, 50).empty());
  CHECK(directory.Update("0123456789abcdef", 2, 0));
  CHECK(lobby.Sent().size() == 1);
}

TEST_CASE("Live directory advertises again what the lobby dropped",
          "[live]") {
  using namespace std::chrono_literals;
  testing::FakeLobby lobby;
  SessionDirectory directory("ws://lobby.test", 0x4D5307E6, TestAuth());
  DirectoryAdvert a;
  a.id = "0123456789abcdef";
  a.info = {1, 2, 3};
  a.public_slots = 4;
  directory.Advertise(a);
  auto socket = lobby.WaitForSocket();
  socket.opened();
  socket.message(R"({"t":"authed","xuid":"0009f00012345678","gamertag":"T"})");
  auto adverts = [&] {
    const auto sent = lobby.Sent();
    return std::count_if(sent.begin(), sent.end(), [](const std::string& t) {
      return t.find(R"("t":"advertise")") != std::string::npos;
    });
  };
  CHECK(adverts() == 1);
  // Still listed: nothing more.
  socket.message(R"({"t":"refreshed","ids":["0123456789abcdef"]})");
  CHECK(adverts() == 1);
  // Dropped: advertised again.
  socket.message(R"({"t":"refreshed","ids":[]})");
  CHECK(adverts() == 2);
}

TEST_CASE("Live directory searches end when the socket closes", "[live]") {
  using namespace std::chrono;
  testing::FakeLobby lobby;
  SessionDirectory directory("ws://lobby.test", 0x4D5307E6, TestAuth());
  std::thread lobby_thread([&] {
    auto socket = lobby.WaitForSocket();
    socket.opened();
    socket.message(R"({"t":"authed","xuid":"0009f00012345678","gamertag":"T"})");
    // The search is sent, then the socket closes without an answer.
    while (lobby.Sent().size() < 2) {
      std::this_thread::sleep_for(1ms);
    }
    socket.closed("gone");
  });
  const auto start = steady_clock::now();
  CHECK(directory.Search({}, 10, false, 5000).empty());
  CHECK(steady_clock::now() - start < seconds(4));
  lobby_thread.join();
}

}  // namespace
}  // namespace xe::net::live
