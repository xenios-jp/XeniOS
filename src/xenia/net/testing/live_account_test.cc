/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/live_account.h"
#include "xenia/net/live_account_hub.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/net/testing/fake_lobby.h"

namespace xe::net::live {
namespace {

TEST_CASE("Live account keys sign what the lobby verifies", "[live]") {
  const DeviceKey key = DeviceKey::Generate();
  const std::string pub = key.PublicHex();
  REQUIRE(pub.size() == 130);
  CHECK(pub.substr(0, 2) == "04");
  const std::string sig = key.Sign("xenios-auth\n0009f00012345678\nabcd");
  REQUIRE(sig.size() == 128);
  CHECK(VerifySignature(pub, "xenios-auth\n0009f00012345678\nabcd", sig));
  CHECK_FALSE(VerifySignature(pub, "xenios-auth\n0009f00012345678\nabce", sig));
  CHECK_FALSE(
      VerifySignature(DeviceKey::Generate().PublicHex(),
                      "xenios-auth\n0009f00012345678\nabcd", sig));

  // The same secret is the same key.
  auto again = DeviceKey::FromSecret(key.secret());
  REQUIRE(again);
  CHECK(again->PublicHex() == pub);
  CHECK_FALSE(DeviceKey::FromSecret({}));  // 0 isn't a private key.
}

TEST_CASE("Live account keys are kept in a file", "[live]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-live-key-test-" +
       DeviceKey::Generate().PublicHex().substr(2, 16));
  std::filesystem::remove(path);
  auto made = DeviceKey::LoadOrCreate(path);
  REQUIRE(made);
  auto read = DeviceKey::LoadOrCreate(path);
  REQUIRE(read);
  CHECK(read->PublicHex() == made->PublicHex());
  std::filesystem::remove(path);
}

TEST_CASE("Live account key files are private and never half written",
          "[live]") {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-live-key-test-" +
       DeviceKey::Generate().PublicHex().substr(2, 16));
  std::filesystem::remove(path);
  // An empty file, left by a write that never finished, holds no key.
  { std::ofstream empty(path); }
  auto made = DeviceKey::LoadOrCreate(path);
  REQUIRE(made);
  CHECK(std::filesystem::file_size(path) == 65);
#if !defined(_WIN32)
  CHECK((std::filesystem::status(path).permissions() &
         std::filesystem::perms::all) == (std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_write));
#endif
  auto read = DeviceKey::LoadOrCreate(path);
  REQUIRE(read);
  CHECK(read->PublicHex() == made->PublicHex());
  // Something that isn't a key is left alone.
  { std::ofstream bad(path, std::ios::trunc); bad << "not a key\n"; }
  CHECK_FALSE(DeviceKey::LoadOrCreate(path));
  {
    std::ifstream bad(path);
    std::string text;
    std::getline(bad, text);
    CHECK(text == "not a key");
  }
  std::filesystem::remove(path);
}

TEST_CASE("Live account messages that are too deep or long are ignored",
          "[live]") {
  CHECK_FALSE(ParseAccountMessage(std::string(100000, '[') +
                                  std::string(100000, ']')));
  CHECK_FALSE(ParseAccountMessage(
      R"({"t":"error","reason":")" + std::string(kMaxLobbyMessage, 'x') +
      R"("})"));
}

TEST_CASE("Live account keeps only what a lobby may say", "[live]") {
  // XUIDs are 16 hex digits: no sign, spaces or 0x.
  for (const char* xuid : {" 009f00000000001", "+009f00000000001",
                           "-009f00000000001", "0x09f00000000001"}) {
    auto m = ParseAccountMessage(
        std::string(R"({"t":"presence","online":true,"xuid":")") + xuid +
        R"("})");
    CHECK_FALSE(m);
  }
  // A welcome or move to an XUID that isn't an online one is refused.
  CHECK_FALSE(ParseAccountMessage(
      R"({"t":"welcome","xuid":"0000000000000001","friends":[]})"));
  CHECK_FALSE(
      ParseAccountMessage(R"({"t":"moved","xuid":"0000000000000001"})"));
  CHECK(ParseAccountMessage(R"({"t":"moved","xuid":"0009f00000000002"})"));

  // Session info is an XSESSION_INFO or nothing.
  const std::string info(2 * kSessionInfoSize, 'a');
  auto ok = ParseAccountMessage(
      R"({"t":"presence","xuid":"0009f00000000004","online":true,)"
      R"("joinable":{"id":"x","room":"ABC123","info":")" +
      info + R"("}})");
  REQUIRE(ok);
  REQUIRE(ok->presence.joinable);
  CHECK(ok->presence.joinable->info.size() == kSessionInfoSize);
  auto big = ParseAccountMessage(
      R"({"t":"presence","xuid":"0009f00000000004","online":true,)"
      R"("joinable":{"id":"x","info":")" +
      std::string(200000, 'a') + R"("}})");
  REQUIRE(big);
  REQUIRE(big->presence.joinable);
  CHECK(big->presence.joinable->info.empty());

  // Text shown to the player loses control and direction characters and is
  // one line's worth.
  auto invite = ParseAccountMessage(
      R"({"t":"invite","id":"0123456789abcdef","kind":"game",)"
      R"("from":"0009f00000000004","gamertag":"Ann","title":"4D5307E6",)"
      R"("message":"\u202Eevil\u0000\nline)" +
      std::string(5000, 'x') + R"("})");
  REQUIRE(invite);
  CHECK(invite->invite.message.find("\u202E") == std::string::npos);
  CHECK(invite->invite.message.find('\n') == std::string::npos);
  CHECK(invite->invite.message.size() <= kMaxRemoteText);
  auto reject = ParseAccountMessage(
      R"({"t":"reject","reason":"elsewhere","device":"\u202EiPad\n)" +
      std::string(500, 'y') + R"("})");
  REQUIRE(reject);
  CHECK(reject->device_name.find("\u202E") == std::string::npos);
  CHECK(reject->device_name.size() <= 40);
}

TEST_CASE("Live account sign-in messages", "[live]") {
  const DeviceKey key = DeviceKey::Generate();
  const std::string reg = EncodeRegister(key, "127.0.0.1:8787",
                                         0x0009F00012345678ull, "XeniOSLiveA",
                                         "n0nce");
  CHECK(reg.find(R"("t":"register")") != std::string::npos);
  CHECK(reg.find(R"("xuid":"0009f00012345678")") != std::string::npos);
  const std::string sig = reg.substr(reg.find(R"("sig":")") + 7, 128);
  CHECK(VerifySignature(key.PublicHex(),
                        "xenios-register\n127.0.0.1:8787\n0009f00012345678\n"
                        "XeniOSLiveA\n" +
                            key.PublicHex() + "\nn0nce",
                        sig));
  const std::string auth =
      EncodeAuth(key, "lobby.example", 0x0009F00012345678ull, "n0nce");
  CHECK(auth.find(R"("t":"auth")") != std::string::npos);
  CHECK(VerifySignature(
      key.PublicHex(), "xenios-auth\nlobby.example\n0009f00012345678\nn0nce",
      auth.substr(auth.find(R"("sig":")") + 7, 128)));

  // The host as the lobby sees it in the request.
  CHECK(LobbyHost("ws://127.0.0.1:8787") == "127.0.0.1:8787");
  CHECK(LobbyHost("wss://Lobby.Example.dev") == "lobby.example.dev");
  CHECK(LobbyHost("wss://lobby.example:443/") == "lobby.example");
  CHECK(LobbyHost("ws://lobby.example:80") == "lobby.example");
  CHECK(LobbyHost("wss://lobby.example:8443") == "lobby.example:8443");

  // The lobby's web address.
  CHECK(LobbyWebRoot("ws://127.0.0.1:8787") == "http://127.0.0.1:8787");
  CHECK(LobbyWebRoot("wss://Lobby.Example.dev/") ==
        "https://lobby.example.dev");

  auto challenge = ParseAccountMessage(R"({"t":"challenge","v":2,"nonce":"ab"})");
  REQUIRE(challenge);
  CHECK(challenge->kind == AccountMessage::Kind::kChallenge);
  CHECK(challenge->nonce == "ab");

  auto welcome = ParseAccountMessage(
      R"({"t":"welcome","v":2,"xuid":"0009f00000000001","gamertag":"A",)"
      R"("tag":"0427","code":"K7Q2M9XD","assigned":true,"friends":[)"
      R"({"xuid":"0009f00000000002","gamertag":"B","state":"friend"},)"
      R"({"xuid":"0009f00000000003","gamertag":"C","state":"incoming"},)"
      R"({"xuid":"bad","gamertag":"D","state":"friend"}]})");
  REQUIRE(welcome);
  CHECK(welcome->kind == AccountMessage::Kind::kWelcome);
  CHECK(welcome->xuid == 0x0009F00000000001ull);
  CHECK(welcome->code == "K7Q2M9XD");
  CHECK(welcome->assigned);
  REQUIRE(welcome->friends.size() == 2);
  CHECK(welcome->friends[1].state == Friend::State::kIncoming);
}

TEST_CASE("Live account names from the lobby keep to the gamertag rule",
          "[live]") {
  CHECK(CleanGamertag("Master Chief") == "Master Chief");
  CHECK(CleanGamertag(" Spaced Out ") == "Spaced Out");
  CHECK(CleanGamertag("ABCDEFGHIJKLMNOPQRS") == "ABCDEFGHIJKLMNO");
  // Right-to-left overrides, zero-width joiners and newlines are dropped.
  CHECK(CleanGamertag("Evil\xE2\x80\xAE" "etiS\n\xE2\x80\x8D") == "EviletiS");
  CHECK(CleanGamertag("\xF0\x9F\x98\x80").empty());

  CHECK(CleanText("Will's iPhone", 40) == "Will's iPhone");
  CHECK(CleanText("caf\xC3\xA9\xE2\x80\xAE\t", 40) == "caf\xC3\xA9");
  CHECK(CleanText("abcdef", 3) == "abc");
  CHECK(CleanText("ok\xFF", 40) == "ok");

  auto friend_message = ParseAccountMessage(
      R"({"t":"friend","xuid":"0009f00000000002","gamertag":"B\u202Eob",)"
      R"("state":"friend"})");
  REQUIRE(friend_message);
  REQUIRE(friend_message->friends.size() == 1);
  CHECK(friend_message->friends[0].gamertag == "Bob");
}

TEST_CASE("Live account friends, presence and invites", "[live]") {
  auto gone = ParseAccountMessage(
      R"({"t":"friend","xuid":"0009f00000000002","gamertag":"B","state":"none"})");
  REQUIRE(gone);
  CHECK(gone->removed);

  auto presence = ParseAccountMessage(
      R"({"t":"presence","xuid":"0009f00000000002","gamertag":"B",)"
      R"("online":true,"title":"4D5307E6","context":3,"rich":"Matchmaking",)"
      R"("joinable":{"room":"","id":"0123456789abcdef","info":"00ff"}})");
  REQUIRE(presence);
  CHECK(presence->presence.online);
  CHECK(presence->presence.title_id == 0x4D5307E6);
  CHECK(presence->presence.context == 3);
  REQUIRE(presence->presence.joinable);
  CHECK(presence->presence.joinable->session_id == "0123456789abcdef");
  CHECK(presence->presence.joinable->info == std::vector<uint8_t>{0x00, 0xFF});

  auto invite = ParseAccountMessage(
      R"({"t":"invite","id":"00112233445566aa","kind":"game",)"
      R"("from":"0009f00000000002","gamertag":"B","title":"4D5307E6",)"
      R"("room":"","info":"0a0b","message":"Come play"})");
  REQUIRE(invite);
  CHECK(invite->kind == AccountMessage::Kind::kInvite);
  CHECK(invite->invite.from == 0x0009F00000000002ull);
  CHECK(invite->invite.info == std::vector<uint8_t>{0x0A, 0x0B});
  CHECK_FALSE(invite->invite.party);

  CHECK(EncodeInvite(0x0009F00000000002ull, 0x4D5307E6,
                     {"", "0123456789abcdef", {0x0A}}, "") ==
        R"({"t":"invite","kind":"game","to":"0009f00000000002",)"
        R"("title":"4D5307E6","room":"","id":"0123456789abcdef","info":"0a"})");
  CHECK(EncodePresence(0x4D5307E6, 3, "", std::nullopt) ==
        R"({"t":"presence","title":"4D5307E6","context":3})");
  CHECK(EncodeFriendAdd(0x0009F00000000002ull) ==
        R"({"t":"friend_add","xuid":"0009f00000000002"})");
  CHECK(EncodeFriendEdit("friend_remove", 0x0009F00000000002ull) ==
        R"({"t":"friend_remove","xuid":"0009f00000000002"})");
  CHECK(EncodeFriendEdit("friend_decline", 0x0009F00000000003ull) ==
        R"({"t":"friend_decline","xuid":"0009f00000000003"})");

  auto party = ParseAccountMessage(
      R"({"t":"party","room":"ABC123","leader":"0009f00000000002",)"
      R"("members":[{"xuid":"0009f00000000002","gamertag":"B"}]})");
  REQUIRE(party);
  REQUIRE(party->party);
  CHECK(party->party->members.size() == 1);
  auto left = ParseAccountMessage(R"({"t":"party","room":"ABC123","left":true})");
  REQUIRE(left);
  CHECK_FALSE(left->party);
  CHECK_FALSE(ParseAccountMessage("pong"));
  auto moved = ParseAccountMessage(R"({"t":"moved","xuid":"0009f000000000aa"})");
  REQUIRE(moved);
  CHECK(moved->kind == AccountMessage::Kind::kMoved);
  CHECK(moved->xuid == 0x0009F000000000AAull);
}


using testing::FakeLobby;

constexpr char kWelcome[] =
    R"({"t":"welcome","xuid":"0009f00000000001","code":"ABCD-EFGH",)"
    R"("friends":[]})";

TEST_CASE("Live account blocked messages", "[live]") {
  auto welcome = ParseAccountMessage(
      R"({"t":"welcome","xuid":"0009f00000000001","friends":[],)"
      R"("blocked":["0009f00000000004","bad","0009f00000000005"]})");
  REQUIRE(welcome);
  CHECK(welcome->blocked ==
        std::vector<uint64_t>{0x0009F00000000004ull, 0x0009F00000000005ull});
  auto profile = ParseAccountMessage(
      R"({"t":"profile","xuid":"0009f00000000001","blocked":["0009f00000000004"]})");
  REQUIRE(profile);
  CHECK(profile->blocked == std::vector<uint64_t>{0x0009F00000000004ull});
  auto bare = ParseAccountMessage(R"({"t":"welcome","xuid":"0009f00000000001"})");
  REQUIRE(bare);
  CHECK(bare->blocked.empty());

  auto pushed = ParseAccountMessage(
      R"({"t":"blocked","xuids":["0009f00000000006"]})");
  REQUIRE(pushed);
  CHECK(pushed->kind == AccountMessage::Kind::kBlocked);
  CHECK(pushed->blocked == std::vector<uint64_t>{0x0009F00000000006ull});
  auto emptied = ParseAccountMessage(R"({"t":"blocked","xuids":[]})");
  REQUIRE(emptied);
  CHECK(emptied->kind == AccountMessage::Kind::kBlocked);
  CHECK(emptied->blocked.empty());

  CHECK(EncodeBlock(0x0009F00000000004ull) ==
        R"({"t":"block","xuid":"0009f00000000004"})");
  CHECK(EncodeUnblock(0x0009F00000000004ull) ==
        R"({"t":"unblock","xuid":"0009f00000000004"})");
}

TEST_CASE("Live account keeps the blocked list", "[live]") {
  FakeLobby lobby;
  std::atomic<int> changes = 0;
  AccountClient::Events events;
  events.blocked_changed = [&] { ++changes; };
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", std::move(events));
  auto socket = lobby.WaitForSocket();
  // Asked before signing in, so sent after.
  client.Block(0x0009F00000000004ull);
  CHECK_FALSE(client.IsBlocked(0x0009F00000000004ull));

  socket.message(
      R"({"t":"welcome","xuid":"0009f00000000001","friends":[],)"
      R"("blocked":["0009f00000000005"]})");
  CHECK(changes == 1);
  CHECK(client.IsBlocked(0x0009F00000000005ull));
  CHECK_FALSE(client.IsBlocked(0x0009F00000000004ull));
  const auto sent = lobby.Sent();
  CHECK(std::find(sent.begin(), sent.end(), EncodeBlock(0x0009F00000000004ull)) !=
        sent.end());

  client.Unblock(0x0009F00000000005ull);
  CHECK(lobby.Sent().back() == EncodeUnblock(0x0009F00000000005ull));
  // The list is the lobby's to change.
  CHECK(client.IsBlocked(0x0009F00000000005ull));

  socket.message(
      R"({"t":"blocked","xuids":["0009f00000000004","0009f00000000006"]})");
  CHECK(changes == 2);
  CHECK_FALSE(client.IsBlocked(0x0009F00000000005ull));
  CHECK(client.Blocked() ==
        std::vector<uint64_t>{0x0009F00000000004ull, 0x0009F00000000006ull});

  // A new sign-in starts from what the lobby says then.
  socket.message(kWelcome);
  CHECK(client.Blocked().empty());
  CHECK(changes == 3);
}

TEST_CASE("Live account starts a party for a party invite without one",
          "[live][party]") {
  FakeLobby lobby;
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", {});
  auto socket = lobby.WaitForSocket();
  socket.message(kWelcome);
  const auto count = [&](const std::string& text) {
    const auto sent = lobby.Sent();
    return std::count(sent.begin(), sent.end(), text);
  };
  const std::string invite_b = EncodePartyInvite(0x0009F00000000002ull, 0);
  const std::string invite_c =
      EncodePartyInvite(0x0009F00000000003ull, 0x4D5307E6);

  // No party heard of: the invites go (into the party the lobby may still
  // have for the account); the lobby has none and says so.
  client.InviteToParty(0x0009F00000000002ull, 0);
  client.InviteToParty(0x0009F00000000003ull, 0x4D5307E6);
  CHECK(count(EncodePartyCreate()) == 0);
  CHECK(count(invite_b) == 1);
  CHECK(count(invite_c) == 1);
  socket.message(R"({"t":"error","for":"invite","reason":"no_party",)"
                 R"("to":"0009f00000000002"})");
  socket.message(R"({"t":"error","for":"invite","reason":"no_party",)"
                 R"("to":"0009f00000000003"})");
  // One party for both.
  CHECK(count(EncodePartyCreate()) == 1);

  // The lobby made it: both go again, once.
  socket.message(
      R"({"t":"party","room":"ABC123","leader":"0009f00000000001",)"
      R"("members":[{"xuid":"0009f00000000001","gamertag":"Tester"}],)"
      R"("event":{"kind":"created","xuid":"0009f00000000001"}})");
  CHECK(count(invite_b) == 2);
  CHECK(count(invite_c) == 2);
  socket.message(
      R"({"t":"party","room":"ABC123","leader":"0009f00000000001",)"
      R"("members":[{"xuid":"0009f00000000001","gamertag":"Tester"}]})");
  CHECK(count(invite_b) == 2);

  // In a party: straight away, without another party.
  client.InviteToParty(0x0009F00000000002ull, 0);
  CHECK(count(invite_b) == 3);
  CHECK(count(EncodePartyCreate()) == 1);
}

TEST_CASE("Live account answers an invite the lobby still has, once",
          "[live]") {
  FakeLobby lobby;
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", {});
  auto socket = lobby.WaitForSocket();
  socket.message(kWelcome);
  const auto count = [&](const std::string& text) {
    const auto sent = lobby.Sent();
    return std::count(sent.begin(), sent.end(), text);
  };
  const auto invite = [&](const char* id, const char* from) {
    socket.message(std::string(R"({"t":"invite","id":")") + id +
                   R"(","kind":"game","from":")" + from +
                   R"(","gamertag":"B","title":"415608C3","room":""})");
  };

  invite("00000000000000a1", "0009f00000000002");
  client.ReplyInvite("00000000000000a1", true);
  client.ReplyInvite("00000000000000a1", true);
  CHECK(count(EncodeInviteReply("00000000000000a1", true)) == 1);

  // Another of the account's apps answered it.
  invite("00000000000000a2", "0009f00000000003");
  socket.message(R"({"t":"invite_answered","id":"00000000000000a2",)"
                 R"("accept":true})");
  client.ReplyInvite("00000000000000a2", true);
  CHECK(count(EncodeInviteReply("00000000000000a2", true)) == 0);

  // A newer one from the same friend replaced it.
  invite("00000000000000a3", "0009f00000000004");
  invite("00000000000000a4", "0009f00000000004");
  client.ReplyInvite("00000000000000a3", false);
  CHECK(count(EncodeInviteReply("00000000000000a3", false)) == 0);
  client.ReplyInvite("00000000000000a4", false);
  CHECK(count(EncodeInviteReply("00000000000000a4", false)) == 1);

  // A party invite isn't held, but is answered.
  client.ReplyInvite("00000000000000b1", true);
  CHECK(count(EncodeInviteReply("00000000000000b1", true)) == 1);
}

TEST_CASE("Live account drops party invites when the party can't be made",
          "[live][party]") {
  FakeLobby lobby;
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", {});
  auto socket = lobby.WaitForSocket();
  socket.message(kWelcome);
  client.InviteToParty(0x0009F00000000002ull, 0);
  socket.message(R"({"t":"error","for":"invite","reason":"no_party",)"
                 R"("to":"0009f00000000002"})");
  socket.message(R"({"t":"error","for":"party_create","reason":"limit"})");
  socket.message(
      R"({"t":"party","room":"XYZ789","leader":"0009f00000000009",)"
      R"("members":[{"xuid":"0009f00000000009","gamertag":"Other"}]})");
  const auto sent = lobby.Sent();
  // Only the first try, which the lobby refused.
  CHECK(std::count(sent.begin(), sent.end(),
                   EncodePartyInvite(0x0009F00000000002ull, 0)) == 1);
}

TEST_CASE("Live account report messages", "[live]") {
  CHECK(IsReportReason("cheating"));
  CHECK(IsReportReason("offensive_name"));
  CHECK(IsReportReason("other"));
  CHECK_FALSE(IsReportReason(""));
  CHECK_FALSE(IsReportReason("Cheating"));
  CHECK_FALSE(IsReportReason("griefing"));

  CHECK(EncodeReport(0x0009F00000000004ull, "cheating", "Aimbot in Guardian",
                     0x4D5307E6, "ABC123") ==
        R"({"t":"report","xuid":"0009f00000000004","reason":"cheating",)"
        R"("note":"Aimbot in Guardian","title":"4D5307E6","room":"ABC123"})");
  // No note, no game: only what's there.
  CHECK(EncodeReport(0x0009F00000000004ull, "spam", "", 0, "") ==
        R"({"t":"report","xuid":"0009f00000000004","reason":"spam"})");
  // A room that isn't a room code (the public lobby's "", a typo, the letters
  // the alphabet leaves out) is left out rather than refused.
  CHECK(EncodeReport(0x0009F00000000004ull, "spam", " \n ", 0x4D5307E6,
                     "abc123") ==
        R"({"t":"report","xuid":"0009f00000000004","reason":"spam",)"
        R"("title":"4D5307E6"})");
  CHECK(EncodeReport(0x0009F00000000004ull, "spam", "", 0, "ABCDE0I")
            .find("room") == std::string::npos);
  CHECK(EncodeReport(0x0009F00000000004ull, "spam", "", 0, "ABCDEU")
            .find("room") == std::string::npos);

  // Notes: line breaks and tabs stay, the lobby's unprintable characters go,
  // the ends are trimmed (JavaScript's trim, with its Unicode spaces).
  CHECK(CleanReportNote("  line one\n\tline two  ") == "line one\n\tline two");
  CHECK(CleanReportNote("a\r\nb") == "a\nb");
  CHECK(CleanReportNote("x\u200By\u202Ez\u00AD\u2066!") == "xyz!");
  CHECK(CleanReportNote("\u3000\u00A0 \n") == "");
  CHECK(CleanReportNote("") == "");
  // 500 UTF-16 units at most: an emoji is two.
  CHECK(CleanReportNote(std::string(600, 'a')) == std::string(500, 'a'));
  std::string emoji;
  for (int i = 0; i < 300; ++i) {
    emoji += "\U0001F600";
  }
  CHECK(CleanReportNote(emoji).size() == 250 * 4);
  std::string almost(499, 'a');
  CHECK(CleanReportNote(almost + "\U0001F600") == almost);
  CHECK(CleanReportNote(almost + "\u00E9") == almost + "\u00E9");
  // Broken UTF-8 ends the note where it breaks.
  CHECK(CleanReportNote("ok\xFF more") == "ok");
  // The longest note still fits in a lobby message (2048 bytes).
  std::string wide;
  for (int i = 0; i < 600; ++i) {
    wide += "\u754C";
  }
  CHECK(EncodeReport(0x0009F00000000004ull, "harassment", wide, 0x4D5307E6,
                     "ABC123")
            .size() < 2048);

  auto reported = ParseAccountMessage(
      R"({"t":"reported","id":"0a1b2c3d","xuid":"0009f00000000004"})");
  REQUIRE(reported);
  CHECK(reported->kind == AccountMessage::Kind::kReported);
  CHECK(reported->report_id == "0a1b2c3d");
  CHECK(reported->xuid == 0x0009F00000000004ull);
  auto refused = ParseAccountMessage(
      R"({"t":"error","for":"report","xuid":"0009f00000000004",)"
      R"("reason":"duplicate"})");
  REQUIRE(refused);
  CHECK(refused->kind == AccountMessage::Kind::kError);
  CHECK(refused->for_request == "report");
  CHECK(refused->reason == "duplicate");
  CHECK(refused->xuid == 0x0009F00000000004ull);
}

TEST_CASE("Live account reports players", "[live]") {
  FakeLobby lobby;
  std::mutex mutex;
  std::vector<std::string> heard;
  auto add = [&](std::string line) {
    std::lock_guard lock(mutex);
    heard.push_back(std::move(line));
  };
  AccountClient::Events events;
  events.reported = [&](const std::string& id, uint64_t xuid) {
    add("reported " + id + " " + XuidHex(xuid));
  };
  events.report_failed = [&](uint64_t xuid, const std::string& reason) {
    add("report_failed " + XuidHex(xuid) + " " + reason);
  };
  events.request_failed = [&](const std::string& request,
                              const std::string& reason) {
    add("failed " + request + " " + reason);
  };
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", std::move(events));
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  socket.message(kWelcome);
  const size_t before = lobby.Sent().size();

  // A reason the lobby doesn't know isn't sent: it would close the socket.
  CHECK_FALSE(client.Report(0x0009F00000000004ull, "griefing", "", 0, ""));
  CHECK(lobby.Sent().size() == before);

  CHECK(client.Report(0x0009F00000000004ull, "offensive_name", " Rude name ",
                      0x4D5307E6, "ABC123"));
  CHECK(lobby.Sent().back() ==
        EncodeReport(0x0009F00000000004ull, "offensive_name", "Rude name",
                     0x4D5307E6, "ABC123"));
  socket.message(R"({"t":"reported","id":"0a1b2c3d","xuid":"0009f00000000004"})");
  CHECK(client.Report(0x0009F00000000004ull, "other", "", 0, ""));
  socket.message(
      R"({"t":"error","for":"report","xuid":"0009f00000000004",)"
      R"("reason":"duplicate"})");
  socket.message(
      R"({"t":"error","for":"report","xuid":"0009f00000000005",)"
      R"("reason":"limit"})");
  // Other refusals keep going where they went.
  socket.message(R"({"t":"error","for":"friend_add","reason":"blocked"})");
  std::lock_guard lock(mutex);
  CHECK(heard == std::vector<std::string>{
                     "reported 0a1b2c3d 0009f00000000004",
                     "report_failed 0009f00000000004 duplicate",
                     "report_failed 0009f00000000005 limit",
                     "failed friend_add blocked"});
}

TEST_CASE("Live account player search", "[live]") {
  CHECK(EncodeSearch(" Real ") == R"({"t":"search","prefix":"Real"})");
  // Too short or too long for the lobby.
  CHECK(EncodeSearch("Re").empty());
  CHECK(EncodeSearch("  Re  ").empty());
  CHECK(EncodeSearch(std::string(16, 'a')).empty());
  auto m = ParseAccountMessage(
      R"({"t":"results","prefix":"Real","users":[)"
      R"({"xuid":"0009f00000000004","gamertag":"RealityJP","state":"none"},)"
      R"({"xuid":"0009f00000000005","gamertag":"Realm","state":"friend"},)"
      R"({"gamertag":"NoXuid"}]})");
  REQUIRE(m);
  CHECK(m->kind == AccountMessage::Kind::kResults);
  CHECK(m->prefix == "Real");
  REQUIRE(m->users.size() == 2);
  CHECK(m->users[0] == std::pair<uint64_t, std::string>{0x0009F00000000004ull,
                                                        "RealityJP"});
}

TEST_CASE("Live account names and settings messages", "[live]") {
  CHECK(EncodeNames({0x0009F00000000004ull, 0x0009F00000000005ull}) ==
        R"({"t":"names","xuids":["0009f00000000004","0009f00000000005"]})");
  // What the lobby would close the socket for isn't made.
  CHECK(EncodeNames({}).empty());
  CHECK(EncodeNames(std::vector<uint64_t>(17, 0x0009F00000000004ull)).empty());
  CHECK_FALSE(
      EncodeNames(std::vector<uint64_t>(16, 0x0009F00000000004ull)).empty());
  CHECK(EncodeNames({0x0009F00000000004ull, 0xE000000000000001ull}).empty());
  CHECK(EncodeNames({0}).empty());

  CHECK(EncodeSettings(FriendRequests::kCode) ==
        R"({"t":"settings","requests":"code"})");
  CHECK(EncodeSettings(FriendRequests::kAnyone) ==
        R"({"t":"settings","requests":"anyone"})");
  CHECK(EncodeSettings(std::nullopt) == R"({"t":"settings"})");

  auto names = ParseAccountMessage(
      R"({"t":"names","users":[{"xuid":"0009f00000000004","gamertag":"Ann"},)"
      R"({"xuid":"bad","gamertag":"Bo"},{"xuid":"0009f00000000006"},)"
      R"({"xuid":"0009f00000000007","gamertag":"<!>"},)"
      R"({"xuid":"0009f00000000008","gamertag":"Cy Two"}]})");
  REQUIRE(names);
  CHECK(names->kind == AccountMessage::Kind::kNames);
  CHECK(names->users ==
        std::vector<std::pair<uint64_t, std::string>>{
            {0x0009F00000000004ull, "Ann"}, {0x0009F00000000008ull, "Cy Two"}});
  auto found = ParseAccountMessage(
      R"({"t":"found","users":[{"xuid":"0009f00000000004","gamertag":"Ann"}]})");
  REQUIRE(found);
  CHECK(found->kind == AccountMessage::Kind::kFound);
  CHECK(found->users.size() == 1);

  auto settings = ParseAccountMessage(R"({"t":"settings","requests":"code"})");
  REQUIRE(settings);
  CHECK(settings->kind == AccountMessage::Kind::kSettings);
  CHECK(settings->requests == FriendRequests::kCode);
  CHECK_FALSE(ParseAccountMessage(R"({"t":"settings","requests":"friends"})"));

  auto welcome = ParseAccountMessage(
      R"({"t":"welcome","xuid":"0009f00000000001","friends":[],)"
      R"("settings":{"requests":"anyone"}})");
  REQUIRE(welcome);
  CHECK(welcome->settings);
  CHECK(welcome->requests == FriendRequests::kAnyone);
  // A lobby from before settings says none.
  auto old = ParseAccountMessage(R"({"t":"welcome","xuid":"0009f00000000001"})");
  REQUIRE(old);
  CHECK_FALSE(old->settings);
  CHECK_FALSE(old->requests);

  auto refused = ParseAccountMessage(
      R"({"t":"error","for":"friend_add","reason":"not_allowed",)"
      R"("xuid":"0009f00000000004"})");
  REQUIRE(refused);
  CHECK(refused->for_request == "friend_add");
  CHECK(refused->reason == "not_allowed");
  CHECK(refused->xuid == 0x0009F00000000004ull);
}

constexpr char kWelcomeWithSettings[] =
    R"({"t":"welcome","xuid":"0009f00000000001","code":"ABCD-EFGH",)"
    R"("friends":[{"xuid":"0009f00000000002","gamertag":"Pal",)"
    R"("state":"friend"}],"settings":{"requests":"anyone"}})";

// The `names` requests among what the client sent.
std::vector<std::string> NamesSent(FakeLobby& lobby) {
  std::vector<std::string> names;
  for (const auto& text : lobby.Sent()) {
    if (text.rfind(R"({"t":"names")", 0) == 0) {
      names.push_back(text);
    }
  }
  return names;
}

TEST_CASE("Live account asks the lobby for names", "[live]") {
  FakeLobby lobby;
  std::mutex mutex;
  std::vector<std::pair<uint64_t, std::string>> heard;
  AccountClient::Events events;
  events.names = [&](const std::vector<std::pair<uint64_t, std::string>>& n) {
    std::lock_guard lock(mutex);
    heard.insert(heard.end(), n.begin(), n.end());
  };
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", std::move(events));
  auto socket = lobby.WaitForSocket();
  // Asked before signing in: sent once signed in. Itself, a friend, guests
  // and offline XUIDs aren't asked for.
  client.RequestNames({0x0009F00000000004ull, 0x0009F00000000001ull,
                       0xE000000000000004ull, 0, 0x0009F00000000004ull});
  CHECK(NamesSent(lobby).empty());
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  socket.message(kWelcomeWithSettings);
  CHECK(client.GamertagOf(0x0009F00000000002ull) == "Pal");
  client.RequestNames({0x0009F00000000002ull});
  REQUIRE(NamesSent(lobby) ==
          std::vector<std::string>{EncodeNames({0x0009F00000000004ull})});

  socket.message(
      R"({"t":"names","users":[{"xuid":"0009f00000000004","gamertag":"Ann"}]})");
  CHECK(client.GamertagOf(0x0009F00000000004ull) == "Ann");
  CHECK(client.GamertagOf(0x0009F00000000009ull).empty());
  {
    std::lock_guard lock(mutex);
    CHECK(heard == std::vector<std::pair<uint64_t, std::string>>{
                       {0x0009F00000000004ull, "Ann"}});
  }
  // Known now, or asked for already (the lobby left it out): not again.
  client.RequestNames({0x0009F00000000004ull});
  CHECK(NamesSent(lobby).size() == 1);

  // Names heard elsewhere count too: invites, presence and the party.
  socket.message(
      R"({"t":"party","room":"ABC123","leader":"0009f00000000010",)"
      R"("members":[{"xuid":"0009f00000000010","gamertag":"Lead"}]})");
  socket.message(
      R"({"t":"invite","id":"0123456789abcdef","kind":"game",)"
      R"("from":"0009f00000000011","gamertag":"Sender","title":"4D5307E6"})");
  CHECK(client.GamertagOf(0x0009F00000000010ull) == "Lead");
  CHECK(client.GamertagOf(0x0009F00000000011ull) == "Sender");
  client.RequestNames({0x0009F00000000010ull, 0x0009F00000000011ull});
  CHECK(NamesSent(lobby).size() == 1);

  // 40 players: batches of 16.
  std::vector<uint64_t> many;
  for (uint64_t i = 0; i < 40; ++i) {
    many.push_back(0x0009F00000001000ull + i);
  }
  client.RequestNames(many);
  const auto sent = NamesSent(lobby);
  REQUIRE(sent.size() == 4);
  CHECK(sent[1] == EncodeNames({many.begin(), many.begin() + 16}));
  CHECK(sent[3] == EncodeNames({many.begin() + 32, many.end()}));
}

TEST_CASE("Live account keeps under the lobby's limit for names", "[live]") {
  FakeLobby lobby;
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", {});
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  socket.message(kWelcomeWithSettings);
  // More than a minute's requests: the rest wait.
  std::vector<uint64_t> many;
  for (uint64_t i = 0;
       i < AccountClient::kNamesRequestsPerMinute * kMaxNamesPerRequest + 5;
       ++i) {
    many.push_back(0x0009F00000002000ull + i);
  }
  client.RequestNames(many);
  CHECK(NamesSent(lobby).size() == AccountClient::kNamesRequestsPerMinute);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(NamesSent(lobby).size() == AccountClient::kNamesRequestsPerMinute);
}

TEST_CASE("Live account names wait after the lobby's limit", "[live]") {
  FakeLobby lobby;
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", {});
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  socket.message(kWelcomeWithSettings);
  client.RequestNames({0x0009F00000000004ull});
  REQUIRE(NamesSent(lobby).size() == 1);
  // Over the limit (friend requests by gamertag count too): nothing more for
  // now, and the refused players are asked for again later, not forgotten.
  socket.message(R"({"t":"error","for":"names","reason":"limit"})");
  client.RequestNames({0x0009F00000000005ull});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(NamesSent(lobby).size() == 1);
}

TEST_CASE("Live account doesn't send names or settings to an older lobby",
          "[live]") {
  FakeLobby lobby;
  std::atomic<int> settings_changes = 0;
  AccountClient::Events events;
  events.settings_changed = [&] { ++settings_changes; };
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", std::move(events));
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  // A welcome without settings: the lobby doesn't know names or settings, and
  // would close the socket for them.
  socket.message(kWelcome);
  REQUIRE(client.signed_in());
  const size_t before = lobby.Sent().size();
  client.RequestNames({0x0009F00000000004ull});
  CHECK_FALSE(client.SetFriendRequests(FriendRequests::kCode));
  CHECK_FALSE(client.FriendRequestsSetting());
  CHECK(lobby.Sent().size() == before);
  CHECK(settings_changes == 0);
}

TEST_CASE("Live account friend request settings", "[live]") {
  FakeLobby lobby;
  std::atomic<int> settings_changes = 0;
  std::mutex mutex;
  std::vector<std::string> refused;
  AccountClient::Events events;
  events.settings_changed = [&] { ++settings_changes; };
  events.friend_add_refused = [&](uint64_t xuid, const std::string& gamertag,
                                  const std::string& reason) {
    std::lock_guard lock(mutex);
    refused.push_back(XuidHex(xuid) + " " + gamertag + " " + reason);
  };
  std::vector<std::string> failed;
  events.request_failed = [&](const std::string& request,
                              const std::string& reason) {
    std::lock_guard lock(mutex);
    failed.push_back(request + " " + reason);
  };
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", std::move(events));
  auto socket = lobby.WaitForSocket();
  // Not signed in: nothing to set yet.
  CHECK_FALSE(client.SetFriendRequests(FriendRequests::kCode));
  CHECK_FALSE(client.FriendRequestsSetting());
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  socket.message(kWelcomeWithSettings);
  CHECK(settings_changes == 1);
  CHECK(client.FriendRequestsSetting() == FriendRequests::kAnyone);

  CHECK(client.SetFriendRequests(FriendRequests::kCode));
  CHECK(lobby.Sent().back() == R"({"t":"settings","requests":"code"})");
  // The lobby's answer (to every app of the account) is what changes it.
  CHECK(client.FriendRequestsSetting() == FriendRequests::kAnyone);
  socket.message(R"({"t":"settings","requests":"code"})");
  CHECK(settings_changes == 2);
  CHECK(client.FriendRequestsSetting() == FriendRequests::kCode);

  // A request to a player who takes them by code only, named when known.
  socket.message(
      R"({"t":"names","users":[{"xuid":"0009f00000000004","gamertag":"Ann"}]})");
  client.AddFriend(0x0009F00000000004ull);
  socket.message(
      R"({"t":"error","for":"friend_add","reason":"not_allowed",)"
      R"("xuid":"0009f00000000004"})");
  socket.message(
      R"({"t":"error","for":"friend_add","reason":"not_allowed",)"
      R"("xuid":"0009f00000000005"})");
  socket.message(R"({"t":"error","for":"friend_add","reason":"full"})");
  {
    std::lock_guard lock(mutex);
    CHECK(refused == std::vector<std::string>{
                         "0009f00000000004 Ann not_allowed",
                         "0009f00000000005  not_allowed"});
    CHECK(failed == std::vector<std::string>{"friend_add full"});
  }

  // A new sign-in starts from the lobby's welcome.
  socket.message(kWelcome);
  CHECK_FALSE(client.FriendRequestsSetting());
}

TEST_CASE("Live account appears offline", "[live]") {
  CHECK(EncodePresence(0x4D5307E6, 3, "", std::nullopt, true) ==
        R"({"t":"presence","title":"4D5307E6","context":3,"offline":true})");
  CHECK(EncodePresence(0, 0, "", std::nullopt, true) ==
        R"({"t":"presence","offline":true})");
  const DeviceKey key = DeviceKey::Generate();
  const std::string auth =
      EncodeAuth(key, "lobby.test", 0x0009F00000000001ull, "ab", {}, true);
  CHECK(auth.find(R"("presence":{"offline":true})") != std::string::npos);
  CHECK(EncodeAuth(key, "lobby.test", 0x0009F00000000001ull, "ab", {})
            .find("presence") == std::string::npos);
  CHECK(EncodeRegister(key, "lobby.test", 0x0009F00000000001ull, "A", "ab", {},
                       true)
            .find(R"("presence":{"offline":true})") != std::string::npos);

  FakeLobby lobby;
  AccountClient client("ws://lobby.test", key, 0x0009F00000000001ull, "Tester",
                       {});
  CHECK_FALSE(client.appear_offline());
  // Set before the challenge: the sign-in itself says so, so friends never see
  // the account online.
  client.SetAppearOffline(true);
  CHECK(client.appear_offline());
  client.SetPresence(0, 0, {}, std::nullopt);
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  REQUIRE(lobby.Sent().size() == 1);
  CHECK(lobby.Sent()[0].find(R"("t":"register")") != std::string::npos);
  CHECK(lobby.Sent()[0].find(R"("presence":{"offline":true})") !=
        std::string::npos);
  socket.message(kWelcome);
  CHECK(lobby.Sent().back() == R"({"t":"presence","offline":true})");

  // A title's presence keeps the flag.
  client.SetPresence(0x4D5307E6, 3, "Matchmaking", std::nullopt);
  CHECK(lobby.Sent().back() ==
        EncodePresence(0x4D5307E6, 3, "Matchmaking", std::nullopt, true));
  // The same again sends nothing; so does setting the flag it has.
  const size_t count = lobby.Sent().size();
  client.SetPresence(0x4D5307E6, 3, "Matchmaking", std::nullopt);
  client.SetAppearOffline(true);
  CHECK(lobby.Sent().size() == count);

  // Back online, with the presence it had.
  client.SetAppearOffline(false);
  CHECK(lobby.Sent().back() ==
        EncodePresence(0x4D5307E6, 3, "Matchmaking", std::nullopt));
  client.SetAppearOffline(true);
  CHECK(lobby.Sent().back() ==
        EncodePresence(0x4D5307E6, 3, "Matchmaking", std::nullopt, true));

  // Connecting again (here: the lobby moved the account) still appears
  // offline from the first message on.
  socket.message(R"({"t":"moved","xuid":"0009f000000000aa"})");
  socket.closed("closed 4007 moved");
  auto second = lobby.WaitForSocket(2);
  second.message(R"({"t":"challenge","v":2,"nonce":"cd"})");
  const std::string again = lobby.Sent().back();
  CHECK(again.find(R"("t":"register")") != std::string::npos);
  CHECK(again.find(R"("presence":{"offline":true})") != std::string::npos);
  second.message(
      R"({"t":"welcome","xuid":"0009f000000000aa","code":"ABCD-EFGH",)"
      R"("friends":[]})");
  CHECK(lobby.Sent().back() ==
        EncodePresence(0x4D5307E6, 3, "Matchmaking", std::nullopt, true));
}

TEST_CASE("Live account hub opens a connection appearing offline", "[live]") {
  // A title can open the account's connection before the app sets the flag:
  // the hub asks the app's setting, so the very first sign-in is offline.
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("xe-live-hub-offline-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  std::vector<uint64_t> asked;
  SetAppearOfflineProvider([&asked](uint64_t online_xuid) {
    asked.push_back(online_xuid);
    return online_xuid == 0x0009F00000000001ull;
  });
  FakeLobby lobby;
  {
    auto share = ShareAccount("ws://lobby.test", dir / "device_key",
                              0x0009F00000000001ull, "Tester", {});
    REQUIRE(share);
    CHECK((*share)->appear_offline());
    auto socket = lobby.WaitForSocket();
    socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
    REQUIRE(lobby.Sent().size() == 1);
    CHECK(lobby.Sent()[0].find(R"("presence":{"offline":true})") !=
          std::string::npos);
  }
  {
    auto share = ShareAccount("ws://lobby.test", dir / "device_key",
                              0x0009F00000000002ull, "Other", {});
    REQUIRE(share);
    CHECK_FALSE((*share)->appear_offline());
  }
  SetAppearOfflineProvider({});
  CHECK(asked ==
        std::vector<uint64_t>{0x0009F00000000001ull, 0x0009F00000000002ull});
  std::filesystem::remove_all(dir);
}

std::string SigOf(const std::string& message) {
  return message.substr(message.find(R"("sig":")") + 7, 128);
}

// The message with its signature blanked (signatures differ every time).
std::string NoSig(std::string message) {
  const size_t at = message.find(R"("sig":")");
  if (at != std::string::npos) {
    message.replace(at + 7, 128, "");
  }
  return message;
}

TEST_CASE("Live account that only signs in never registers", "[live]") {
  FakeLobby lobby;
  std::atomic<int> refused = 0;
  std::atomic<int> moved = 0;
  AccountClient::Events events;
  events.request_failed = [&](const std::string& request,
                              const std::string& reason) {
    if (request == "auth" && reason == "denied") {
      ++refused;
    }
  };
  events.moved = [&](uint64_t) { ++moved; };
  const DeviceKey key = DeviceKey::Generate();
  AccountOptions options;
  options.appear_offline = true;
  options.sign_in_only = true;
  AccountClient client("ws://lobby.test", key, 0x0009F00000000001ull, "Tester",
                       std::move(events), options);
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  REQUIRE(lobby.Sent().size() == 1);
  // A sign-in with the key, appearing offline: never a register, which could
  // make an account or move this one.
  CHECK(NoSig(lobby.Sent()[0]) ==
        NoSig(EncodeAuth(key, "lobby.test", 0x0009F00000000001ull, "ab",
                         options.device, true, true)));
  // Passive: it doesn't end the account's session on another device.
  CHECK(lobby.Sent()[0].find(R"("passive":true)") != std::string::npos);
  client.RetryRegister("Other");
  CHECK(lobby.Sent().size() == 1);
  // The key isn't the account's any more: refused, and nothing else is tried.
  socket.message(R"({"t":"error","for":"auth","reason":"denied"})");
  CHECK(refused == 1);
  CHECK(moved == 0);
  CHECK(lobby.Sent().size() == 1);
}

TEST_CASE("Live account gamertag, recovery and device messages", "[live]") {
  auto welcome = ParseAccountMessage(
      R"({"t":"welcome","v":2,"xuid":"0009f00000000001","gamertag":"A",)"
      R"("friends":[],"recovery":"ABCD-EFGH-JKMN-PQRS-TVWX",)"
      R"("nextRename":1700000000000,"rename":"taken",)"
      R"("refused":{"gamertag":"B","reason":"too_soon","next":1700000000000}})");
  REQUIRE(welcome);
  CHECK(welcome->recovery == "ABCD-EFGH-JKMN-PQRS-TVWX");
  CHECK(welcome->rename == "taken");
  CHECK(welcome->next_rename_ms == 1700000000000ull);
  REQUIRE(welcome->refused);
  CHECK(welcome->refused->gamertag == "B");
  CHECK(welcome->refused->reason == "too_soon");
  CHECK(welcome->refused->next_ms == 1700000000000ull);
  auto plain = ParseAccountMessage(R"({"t":"welcome","xuid":"0009f00000000001"})");
  REQUIRE(plain);
  CHECK(plain->recovery.empty());
  CHECK(plain->rename.empty());
  CHECK_FALSE(plain->refused);

  auto profile = ParseAccountMessage(
      R"({"t":"profile","gamertag":"New","tag":"0427","code":"K7Q2M9XD",)"
      R"("nextRename":1700000000000})");
  REQUIRE(profile);
  CHECK(profile->kind == AccountMessage::Kind::kProfile);
  CHECK(profile->gamertag == "New");
  CHECK(profile->rename.empty());
  CHECK(profile->next_rename_ms == 1700000000000ull);

  auto checked = ParseAccountMessage(
      R"({"t":"checked","gamertag":"Chief","status":"taken",)"
      R"("suggestions":["TheChief","Chief47",5]})");
  REQUIRE(checked);
  CHECK(checked->kind == AccountMessage::Kind::kChecked);
  CHECK(checked->checked.gamertag == "Chief");
  CHECK(checked->checked.status == "taken");
  CHECK(checked->checked.suggestions ==
        std::vector<std::string>{"TheChief", "Chief47"});
  CHECK_FALSE(ParseAccountMessage(R"({"t":"checked","gamertag":"Chief"})")
                  ->checked.status.size());

  auto recovery = ParseAccountMessage(R"({"t":"recovery","code":"AAAA-BBBB"})");
  REQUIRE(recovery);
  CHECK(recovery->kind == AccountMessage::Kind::kRecovery);
  CHECK(recovery->recovery == "AAAA-BBBB");

  auto link = ParseAccountMessage(
      R"({"t":"link_code","code":"ABCD2345","ttl":600})");
  REQUIRE(link);
  CHECK(link->kind == AccountMessage::Kind::kLinkCode);
  CHECK(link->code == "ABCD2345");
  CHECK(link->link_ttl_s == 600);

  auto devices = ParseAccountMessage(
      R"({"t":"devices","keys":["k1","k2"],"list":[)"
      R"({"key":"k1","name":"Will's iPhone","platform":"ios",)"
      R"("added":1700000000000,"seen":1700000100000,"current":true},)"
      R"({"key":"k2","added":5,"seen":6,"current":false},{"name":"no key"}]})");
  REQUIRE(devices);
  CHECK(devices->kind == AccountMessage::Kind::kDevices);
  REQUIRE(devices->devices.size() == 2);
  CHECK(devices->devices[0].name == "Will's iPhone");
  CHECK(devices->devices[0].platform == "ios");
  CHECK(devices->devices[0].added_ms == 1700000000000ull);
  CHECK(devices->devices[0].seen_ms == 1700000100000ull);
  CHECK(devices->devices[0].current);
  CHECK(devices->devices[1].name.empty());
  CHECK_FALSE(devices->devices[1].current);

  auto moved = ParseAccountMessage(
      R"({"t":"moved","xuid":"0009f000000000aa","recovery":"NEW-CODE"})");
  REQUIRE(moved);
  CHECK(moved->recovery == "NEW-CODE");

  auto error = ParseAccountMessage(
      R"({"t":"error","for":"rename","reason":"too_soon","gamertag":"X",)"
      R"("next":1700000000000})");
  REQUIRE(error);
  CHECK(error->for_request == "rename");
  CHECK(error->reason == "too_soon");
  CHECK(error->gamertag == "X");
  CHECK(error->next_ms == 1700000000000ull);
}

TEST_CASE("Live account encoders for the new requests", "[live]") {
  CHECK(EncodeCheckName("Master Chief") ==
        R"({"t":"check","gamertag":"Master Chief"})");
  CHECK(EncodeRename("New Name") == R"({"t":"rename","gamertag":"New Name"})");
  CHECK(EncodeRecoveryNew() == R"({"t":"recovery_new"})");
  CHECK(EncodeLinkCode() == R"({"t":"link_code"})");
  CHECK(EncodeDevices() == R"({"t":"devices"})");
  CHECK(EncodeRemoveDevice("04ab") == R"({"t":"remove_device","key":"04ab"})");

  CHECK(NormalizeRecoveryCode("abcd-efgh-jkmn-pqrs-tvwx") ==
        "ABCDEFGHJKMNPQRSTVWX");
  CHECK(NormalizeRecoveryCode(" ABCD EFGH-JKMN-PQRS-TVWX ") ==
        "ABCDEFGHJKMNPQRSTVWX");
  CHECK(NormalizeRecoveryCode("ABCD-EFGH").empty());
  CHECK(NormalizeRecoveryCode("ABCD-EFGH-JKMN-PQRS-TVW!").empty());
  CHECK(NormalizeLinkCode("abcd 2345") == "ABCD2345");
  CHECK(NormalizeLinkCode("abc").empty());

  CHECK(NameCheckUrl("wss://lobby.example") ==
        "https://lobby.example/v2/names/check");
  CHECK(NameCheckUrl("ws://127.0.0.1:8787/") ==
        "http://127.0.0.1:8787/v2/names/check");
  CHECK(EncodeNameCheckBody("master chief") ==
        R"({"gamertag":"master chief"})");
  auto response = ParseNameCheckResponse(
      "master chief", R"({"status":"taken","suggestions":["MasterChief"]})");
  REQUIRE(response);
  CHECK(response->gamertag == "master chief");
  CHECK(response->status == "taken");
  CHECK(response->suggestions == std::vector<std::string>{"MasterChief"});
  CHECK_FALSE(ParseNameCheckResponse("x", R"({"error":"rate"})"));
  CHECK_FALSE(ParseNameCheckResponse("x", "not json"));
}

TEST_CASE("Live account link and recover signatures", "[live]") {
  const DeviceKey key = DeviceKey::Generate();
  const std::string pub = key.PublicHex();
  CHECK(JoinSignedText("recover", "lobby.example", "ABCDEFGHJKMNPQRSTVWX", "04",
                       "n0nce") ==
        "xenios-recover\nlobby.example\nABCDEFGHJKMNPQRSTVWX\n04\nn0nce");

  const DeviceInfo device{"Will's iPhone", "ios"};
  const std::string recover = EncodeRecover(
      key, "lobby.example", "ABCDEFGHJKMNPQRSTVWX", "n0nce", device);
  CHECK(recover.find(R"("t":"recover","v":2,"code":"ABCDEFGHJKMNPQRSTVWX")") !=
        std::string::npos);
  CHECK(recover.find(R"("device":{"name":"Will's iPhone","platform":"ios"})") !=
        std::string::npos);
  CHECK(VerifySignature(pub,
                        "xenios-recover\nlobby.example\nABCDEFGHJKMNPQRSTVWX\n" +
                            pub + "\nn0nce",
                        SigOf(recover)));

  const std::string link = EncodeLink(key, "127.0.0.1:8787", "ABCD2345", "ab");
  CHECK(link.find(R"("t":"link")") != std::string::npos);
  CHECK(link.find(R"("device")") == std::string::npos);
  CHECK(VerifySignature(
      pub, "xenios-link\n127.0.0.1:8787\nABCD2345\n" + pub + "\nab",
      SigOf(link)));

  // Device values the lobby would close the socket for are cleaned up.
  const std::string odd = EncodeAuth(
      key, "h", 1, "n", {std::string(40, 'a') + "\n", "iOS Beta!"});
  CHECK(odd.find(R"("name":")" + std::string(32, 'a') + R"(")") !=
        std::string::npos);
  CHECK(odd.find(R"("platform":"iosbeta")") != std::string::npos);
  // Cut at a character, not inside one.
  const std::string utf = EncodeAuth(key, "h", 1, "n",
                                     {std::string(31, 'a') + "\xC3\xA9", ""});
  CHECK(utf.find(R"("name":")" + std::string(31, 'a') + R"(")") !=
        std::string::npos);

  const DeviceInfo defaults = DefaultDeviceInfo();
  CHECK_FALSE(defaults.platform.empty());
  CHECK(defaults.name.size() <= 32);
  const std::string auth =
      EncodeAuth(key, "h", 0x0009F00000000001ull, "n", defaults);
  CHECK(auth.find(R"("platform":")" + defaults.platform + R"(")") !=
        std::string::npos);
}

TEST_CASE("Live account name check, link code, recovery and devices",
          "[live]") {
  FakeLobby lobby;
  struct Heard {
    std::mutex mutex;
    std::vector<std::string> log;
    void Add(std::string line) {
      std::lock_guard lock(mutex);
      log.push_back(std::move(line));
    }
    std::vector<std::string> Get() {
      std::lock_guard lock(mutex);
      return log;
    }
  } heard;
  AccountClient::Events events;
  events.name_checked = [&](const std::string& name, const std::string& status,
                            const std::vector<std::string>& suggestions) {
    heard.Add("checked " + name + " " + status + " " +
              std::to_string(suggestions.size()));
  };
  events.recovery_code = [&](const std::string& code) {
    heard.Add("recovery " + code);
  };
  events.link_code = [&](const std::string& code, uint32_t ttl) {
    heard.Add("link " + code + " " + std::to_string(ttl));
  };
  events.devices_changed = [&] { heard.Add("devices"); };
  events.profile_changed = [&] { heard.Add("profile"); };
  events.rename_failed = [&](const std::string& reason, uint64_t next) {
    heard.Add("rename_failed " + reason + " " + std::to_string(next));
  };
  events.request_failed = [&](const std::string& request,
                              const std::string& reason) {
    heard.Add("failed " + request + " " + reason);
  };
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", std::move(events));
  auto socket = lobby.WaitForSocket();
  CHECK(lobby.Urls().front() == "ws://lobby.test/v2/account/0009f00000000001");

  // Not signed in: a check isn't kept for later.
  CHECK_FALSE(client.CheckName("Chief"));
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  const auto first = lobby.Sent();
  REQUIRE(first.size() == 1);
  CHECK(first[0].find(R"("t":"register")") != std::string::npos);
  CHECK(first[0].find(R"("device":{)") != std::string::npos);

  socket.message(
      R"({"t":"welcome","xuid":"0009f00000000001","gamertag":"Tester",)"
      R"("friends":[],"recovery":"AAAA-BBBB","rename":"reserved",)"
      R"("nextRename":42})");
  CHECK(client.gamertag() == "Tester");
  CHECK(client.rename_prompt() == "reserved");
  CHECK(client.next_rename_ms() == 42);
  CHECK(heard.Get() == std::vector<std::string>{"recovery AAAA-BBBB"});

  CHECK(client.CheckName("Chief"));
  CHECK(lobby.Sent().back() == EncodeCheckName("Chief"));
  socket.message(
      R"({"t":"checked","gamertag":"Chief","status":"available",)"
      R"("suggestions":["a","b","c"]})");
  client.RequestLinkCode();
  CHECK(lobby.Sent().back() == EncodeLinkCode());
  socket.message(R"({"t":"link_code","code":"ABCD2345","ttl":600})");
  // Session tokens: kept while they have 5 minutes left.
  CHECK(EncodeSessionToken() == R"({"t":"session_token"})");
  client.RequestSessionToken();
  CHECK(lobby.Sent().back() == EncodeSessionToken());
  CHECK_FALSE(client.SessionToken());
  socket.message(
      R"({"t":"session_token","token":"00112233445566778899aabbccddeeff",)"
      R"("ttl":3600})");
  REQUIRE(client.SessionToken());
  CHECK(*client.SessionToken() == "00112233445566778899aabbccddeeff");
  socket.message(R"({"t":"session_token","token":"ffeeddcc","ttl":200})");
  CHECK_FALSE(client.SessionToken());
  client.NewRecoveryCode();
  CHECK(lobby.Sent().back() == EncodeRecoveryNew());
  socket.message(R"({"t":"recovery","code":"CCCC-DDDD"})");

  client.RequestDevices();
  CHECK(lobby.Sent().back() == EncodeDevices());
  CHECK(client.Devices().empty());
  socket.message(
      R"({"t":"devices","keys":["k1"],"list":[{"key":"k1","name":"Mac",)"
      R"("platform":"macos","added":1,"seen":2,"current":true}]})");
  REQUIRE(client.Devices().size() == 1);
  CHECK(client.Devices()[0].current);
  client.RemoveDevice("k1");
  CHECK(lobby.Sent().back() == EncodeRemoveDevice("k1"));
  socket.message(R"({"t":"error","for":"remove_device","reason":"last_device"})");

  // Renaming: the lobby's profile is the answer; the prompt goes with it.
  client.Rename("Chief");
  CHECK(lobby.Sent().back() == EncodeRename("Chief"));
  socket.message(R"({"t":"error","for":"rename","reason":"too_soon","next":99})");
  CHECK(client.rename_prompt() == "reserved");
  socket.message(
      R"({"t":"profile","gamertag":"Chief","tag":"0427","code":"K7Q2M9XD",)"
      R"("nextRename":1000})");
  CHECK(client.rename_prompt().empty());
  CHECK(client.gamertag() == "Chief");
  CHECK(client.friend_code() == "K7Q2M9XD");
  CHECK(client.next_rename_ms() == 1000);

  CHECK(heard.Get() ==
        std::vector<std::string>{
            "recovery AAAA-BBBB", "checked Chief available 3",
            "link ABCD2345 600", "recovery CCCC-DDDD", "devices",
            "failed remove_device last_device", "rename_failed too_soon 99",
            "profile"});
}

TEST_CASE("Live account refused name at sign-in", "[live]") {
  FakeLobby lobby;
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Wanted", {});
  auto socket = lobby.WaitForSocket();
  socket.message(
      R"({"t":"welcome","xuid":"0009f00000000001","gamertag":"Kept",)"
      R"("friends":[],"refused":{"gamertag":"Wanted","reason":"too_soon",)"
      R"("next":77}})");
  REQUIRE(client.refused());
  CHECK(client.refused()->gamertag == "Wanted");
  CHECK(client.refused()->next_ms == 77);
  CHECK(client.gamertag() == "Kept");
  socket.message(kWelcome);
  CHECK_FALSE(client.refused());
}

TEST_CASE("Live account joins an account with a link code", "[live]") {
  using namespace std::chrono_literals;
  FakeLobby lobby;
  std::atomic<uint64_t> moved = 0;
  std::atomic<uint64_t> signed_in = 0;
  std::atomic<int> failed = 0;
  AccountClient::Events events;
  events.moved = [&](uint64_t xuid) { moved = xuid; };
  events.signed_in = [&](uint64_t xuid, const std::string&) { signed_in = xuid; };
  events.join_failed = [&](const std::string&) { ++failed; };
  const DeviceKey key = DeviceKey::Generate();
  AccountOptions options;
  options.join = AccountJoin{AccountJoin::Mode::kLink, "abcd-2345"};
  options.device = {"Test Mac", "macos"};
  AccountClient client("ws://lobby.test", key, 0, "Tester", std::move(events),
                       options);
  auto socket = lobby.WaitForSocket();
  // A device with no account yet asks for a XUID of the lobby's making.
  CHECK(lobby.Urls().back() == "ws://lobby.test/v2/account");
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  REQUIRE(lobby.Sent().size() == 1);
  CHECK(NoSig(lobby.Sent()[0]) ==
        NoSig(EncodeLink(key, "lobby.test", "ABCD2345", "ab", options.device)));
  CHECK(VerifySignature(key.PublicHex(),
                        JoinSignedText("link", "lobby.test", "ABCD2345",
                                       key.PublicHex(), "ab"),
                        SigOf(lobby.Sent()[0])));

  socket.message(R"({"t":"moved","xuid":"0009f000000000aa"})");
  CHECK(moved == 0x0009F000000000AAull);
  CHECK(client.xuid() == 0x0009F000000000AAull);
  socket.closed("closed 4007 moved");

  // Back on the account's own socket, signing in with the key.
  auto second = lobby.WaitForSocket(2);
  CHECK(lobby.Urls().back() == "ws://lobby.test/v2/account/0009f000000000aa");
  second.message(R"({"t":"challenge","v":2,"nonce":"cd"})");
  REQUIRE(lobby.Sent().size() == 2);
  CHECK(NoSig(lobby.Sent()[1]) ==
        NoSig(EncodeAuth(key, "lobby.test", 0x0009F000000000AAull, "cd",
                         options.device)));
  second.message(
      R"({"t":"welcome","xuid":"0009f000000000aa","gamertag":"Real",)"
      R"("friends":[]})");
  CHECK(signed_in == 0x0009F000000000AAull);
  CHECK(client.signed_in());
  CHECK(failed == 0);
}

TEST_CASE("Live account recovers with a recovery code", "[live]") {
  FakeLobby lobby;
  std::string new_code;
  std::atomic<uint64_t> moved = 0;
  AccountClient::Events events;
  events.moved = [&](uint64_t xuid) { moved = xuid; };
  events.recovery_code = [&](const std::string& code) { new_code = code; };
  const DeviceKey key = DeviceKey::Generate();
  AccountOptions options;
  options.join =
      AccountJoin{AccountJoin::Mode::kRecover, "abcd-efgh-jkmn-pqrs-tvwx"};
  AccountClient client("wss://Lobby.Test", key, 0, "Tester", std::move(events),
                       options);
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  REQUIRE(lobby.Sent().size() == 1);
  CHECK(NoSig(lobby.Sent()[0]) ==
        NoSig(EncodeRecover(key, "lobby.test", "ABCDEFGHJKMNPQRSTVWX", "ab",
                            options.device)));
  socket.message(
      R"({"t":"moved","xuid":"0009f000000000bb","recovery":"NEW-CODE"})");
  CHECK(moved == 0x0009F000000000BBull);
  CHECK(new_code == "NEW-CODE");
}

TEST_CASE("Live account that is refused a join doesn't try again", "[live]") {
  using namespace std::chrono_literals;
  FakeLobby lobby;
  std::string reason;
  std::atomic<int> failed = 0;
  AccountClient::Events events;
  events.join_failed = [&](const std::string& r) {
    reason = r;
    ++failed;
  };
  AccountOptions options;
  options.join = AccountJoin{AccountJoin::Mode::kLink, "ABCD2345"};
  AccountClient client("ws://lobby.test", DeviceKey::Generate(), 0, "Tester",
                       std::move(events), options);
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  socket.message(R"({"t":"reject","reason":"not_found"})");
  CHECK(failed == 1);
  CHECK(reason == "not_found");
  socket.closed("closed 4006 not_found");
  std::this_thread::sleep_for(300ms);
  CHECK(lobby.Urls().size() == 1);

  // A code that can't be a code is refused without asking the lobby.
  FakeLobby other;
  std::atomic<int> bad = 0;
  AccountClient::Events bad_events;
  bad_events.join_failed = [&](const std::string&) { ++bad; };
  AccountOptions bad_options;
  bad_options.join = AccountJoin{AccountJoin::Mode::kRecover, "short"};
  AccountClient bad_client("ws://lobby.test", DeviceKey::Generate(), 0,
                           "Tester", std::move(bad_events), bad_options);
  auto bad_socket = other.WaitForSocket();
  bad_socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  CHECK(bad == 1);
  CHECK(other.Sent().empty());
}

TEST_CASE("Live account join request encoding and signatures", "[live]") {
  const DeviceKey key = DeviceKey::Generate();
  const std::string pub = key.PublicHex();
  const DeviceInfo device{"Will's iPad", "ios"};
  const std::string named =
      EncodeJoinRequest(key, "lobby.test", "Master Chief", "ab", device);
  CHECK(named.find(R"("t":"join_request","v":2,"gamertag":"Master Chief")") !=
        std::string::npos);
  CHECK(named.find(R"("device":{"name":"Will's iPad","platform":"ios"})") !=
        std::string::npos);
  CHECK(VerifySignature(
      pub, "xenios-join\nlobby.test\nMaster Chief\n" + pub + "\nab",
      SigOf(named)));

  // A request for scanning has no gamertag, and signs an empty line.
  const std::string scan = EncodeJoinRequest(key, "lobby.test", "", "ab");
  CHECK(scan.find("gamertag") == std::string::npos);
  CHECK(VerifySignature(pub, "xenios-join\nlobby.test\n\n" + pub + "\nab",
                        SigOf(scan)));

  CHECK(ApproveSignedText("lobby.test", "0123", "ab") ==
        "xenios-approve\nlobby.test\n0123\nab");
  const std::string approve =
      EncodeJoinApprove(key, "lobby.test", "0123", "ab");
  CHECK(approve.find(R"("t":"join_approve","id":"0123")") != std::string::npos);
  CHECK(VerifySignature(pub, "xenios-approve\nlobby.test\n0123\nab",
                        SigOf(approve)));
  CHECK(EncodeJoinDeny("0123") == R"({"t":"join_deny","id":"0123"})");
  CHECK(EncodeJoinPeek("0123") == R"({"t":"join_peek","id":"0123"})");

  const std::string id(32, 'a');
  CHECK(JoinQrPayload(id) == "xenios-join:" + id);
  CHECK(ParseJoinQrPayload("xenios-join:" + id) == id);
  CHECK(ParseJoinQrPayload("xenios-join:abc").empty());
  CHECK(ParseJoinQrPayload("https://example.com/" + id).empty());

  auto pending = ParseAccountMessage(
      R"({"t":"join_pending","id":"abc","number":"42","ttl":120})");
  REQUIRE(pending);
  CHECK(pending->kind == AccountMessage::Kind::kJoinPending);
  CHECK(pending->join_id == "abc");
  CHECK(pending->join_number == "42");
  CHECK(pending->join_ttl_s == 120);
  auto request = ParseAccountMessage(
      R"({"t":"join_request","id":"abc","device":{"n":"iPad","p":"ios"},)"
      R"("ttl":90,"scanned":true})");
  REQUIRE(request);
  CHECK(request->kind == AccountMessage::Kind::kJoinRequest);
  CHECK(request->join_device.name == "iPad");
  CHECK(request->join_device.platform == "ios");
  CHECK(request->join_number.empty());
  CHECK(request->join_scanned);
  auto closed = ParseAccountMessage(
      R"({"t":"join_closed","id":"abc","reason":"approved"})");
  REQUIRE(closed);
  CHECK(closed->kind == AccountMessage::Kind::kJoinClosed);
  CHECK(closed->reason == "approved");
  CHECK_FALSE(ParseAccountMessage(R"({"t":"join_closed"})"));
}

TEST_CASE("Live account joins by asking another device", "[live]") {
  FakeLobby lobby;
  std::mutex mutex;
  std::string pending;
  std::atomic<uint64_t> moved = 0;
  std::atomic<int> failed = 0;
  AccountClient::Events events;
  events.join_pending = [&](const std::string& id, const std::string& number,
                            uint32_t ttl) {
    std::lock_guard lock(mutex);
    pending = id + " " + number + " " + std::to_string(ttl);
  };
  events.moved = [&](uint64_t xuid) { moved = xuid; };
  events.join_failed = [&](const std::string&) { ++failed; };
  const DeviceKey key = DeviceKey::Generate();
  AccountOptions options;
  options.join = AccountJoin{AccountJoin::Mode::kRequest, "Master Chief"};
  options.device = {"Test iPad", "ios"};
  AccountClient client("ws://lobby.test", key, 0, "Tester", std::move(events),
                       options);
  auto socket = lobby.WaitForSocket();
  CHECK(lobby.Urls().back() == "ws://lobby.test/v2/account");
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  // Only the request goes out: no auth or register before an approval.
  REQUIRE(lobby.Sent().size() == 1);
  CHECK(NoSig(lobby.Sent()[0]) ==
        NoSig(EncodeJoinRequest(key, "lobby.test", "Master Chief", "ab",
                                options.device)));
  CHECK(VerifySignature(key.PublicHex(),
                        JoinSignedText("join", "lobby.test", "Master Chief",
                                       key.PublicHex(), "ab"),
                        SigOf(lobby.Sent()[0])));

  socket.message(R"({"t":"join_pending","id":"abc","number":"42","ttl":120})");
  {
    std::lock_guard lock(mutex);
    CHECK(pending == "abc 42 120");
  }
  CHECK(lobby.Sent().size() == 1);

  socket.message(R"({"t":"moved","xuid":"0009f000000000aa"})");
  CHECK(moved == 0x0009F000000000AAull);
  socket.closed("closed 4007 moved");

  auto second = lobby.WaitForSocket(2);
  CHECK(lobby.Urls().back() == "ws://lobby.test/v2/account/0009f000000000aa");
  second.message(R"({"t":"challenge","v":2,"nonce":"cd"})");
  REQUIRE(lobby.Sent().size() == 2);
  CHECK(NoSig(lobby.Sent()[1]) ==
        NoSig(EncodeAuth(key, "lobby.test", 0x0009F000000000AAull, "cd",
                         options.device)));
  CHECK(failed == 0);
}

TEST_CASE("Live account request for scanning has no gamertag", "[live]") {
  FakeLobby lobby;
  std::mutex mutex;
  std::string pending;
  AccountClient::Events events;
  events.join_pending = [&](const std::string& id, const std::string& number,
                            uint32_t) {
    std::lock_guard lock(mutex);
    pending = id + "|" + number;
  };
  const DeviceKey key = DeviceKey::Generate();
  AccountOptions options;
  options.join = AccountJoin{AccountJoin::Mode::kRequest, ""};
  AccountClient client("ws://lobby.test", key, 0, "Tester", std::move(events),
                       options);
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  REQUIRE(lobby.Sent().size() == 1);
  CHECK(NoSig(lobby.Sent()[0]) ==
        NoSig(EncodeJoinRequest(key, "lobby.test", "", "ab", options.device)));
  socket.message(R"({"t":"join_pending","id":"abc","ttl":120})");
  std::lock_guard lock(mutex);
  CHECK(pending == "abc|");
}

TEST_CASE("Live account join request refusals stop the client", "[live]") {
  using namespace std::chrono_literals;
  for (const char* reason :
       {"declined", "expired", "full", "limit", "denied"}) {
    FakeLobby lobby;
    std::mutex mutex;
    std::vector<std::string> failures;
    std::atomic<uint64_t> moved = 0;
    AccountClient::Events events;
    events.join_failed = [&](const std::string& r) {
      std::lock_guard lock(mutex);
      failures.push_back(r);
    };
    events.moved = [&](uint64_t xuid) { moved = xuid; };
    AccountOptions options;
    options.join = AccountJoin{AccountJoin::Mode::kRequest, "Master Chief"};
    AccountClient client("ws://lobby.test", DeviceKey::Generate(), 0, "Tester",
                         std::move(events), options);
    auto socket = lobby.WaitForSocket();
    socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
    socket.message(
        R"({"t":"join_pending","id":"abc","number":"42","ttl":120})");
    socket.message(std::string(R"({"t":"reject","reason":")") + reason +
                   R"("})");
    socket.closed(std::string("closed ") + reason);
    {
      std::lock_guard lock(mutex);
      CHECK(failures == std::vector<std::string>{reason});
    }
    CHECK(moved == 0);
    // No second request, and nothing but the request was ever sent.
    std::this_thread::sleep_for(300ms);
    CHECK(lobby.Urls().size() == 1);
    CHECK(lobby.Sent().size() == 1);
  }
}

TEST_CASE("Live account approves, denies and peeks at join requests",
          "[live]") {
  FakeLobby lobby;
  std::mutex mutex;
  std::vector<JoinRequest> requests;
  std::vector<std::string> closed;
  std::vector<std::string> failed;
  AccountClient::Events events;
  events.join_requested = [&](const JoinRequest& r) {
    std::lock_guard lock(mutex);
    requests.push_back(r);
  };
  events.join_closed = [&](const std::string& id, const std::string& reason) {
    std::lock_guard lock(mutex);
    closed.push_back(id + " " + reason);
  };
  events.request_failed = [&](const std::string& request,
                              const std::string& reason) {
    std::lock_guard lock(mutex);
    failed.push_back(request + " " + reason);
  };
  const DeviceKey key = DeviceKey::Generate();
  AccountClient client("ws://lobby.test", key, 0x0009F00000000001ull, "Old",
                       std::move(events));
  auto socket = lobby.WaitForSocket();

  // Not signed in: an approval is signed for a socket, so it isn't kept.
  client.ApproveJoin("early");
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  REQUIRE(lobby.Sent().size() == 1);
  client.ApproveJoin("early");
  CHECK(lobby.Sent().size() == 1);
  socket.message(R"({"t":"welcome","xuid":"0009f00000000001","gamertag":"Old",)"
                 R"("friends":[]})");

  socket.message(
      R"({"t":"join_request","id":"abc","device":{"n":"iPad","p":"ios"},)"
      R"("number":"42","ttl":120})");
  {
    std::lock_guard lock(mutex);
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].id == "abc");
    CHECK(requests[0].device.name == "iPad");
    CHECK(requests[0].device.platform == "ios");
    CHECK(requests[0].number == "42");
    CHECK(requests[0].ttl_s == 120);
    CHECK_FALSE(requests[0].scanned);
  }

  const size_t before = lobby.Sent().size();
  client.ApproveJoin("abc");
  REQUIRE(lobby.Sent().size() == before + 1);
  CHECK(NoSig(lobby.Sent().back()) ==
        NoSig(EncodeJoinApprove(key, "lobby.test", "abc", "ab")));
  CHECK(VerifySignature(key.PublicHex(),
                        ApproveSignedText("lobby.test", "abc", "ab"),
                        SigOf(lobby.Sent().back())));
  client.DenyJoin("def");
  CHECK(lobby.Sent().back() == EncodeJoinDeny("def"));
  client.PeekJoin("ghi");
  CHECK(lobby.Sent().back() == EncodeJoinPeek("ghi"));

  socket.message(R"({"t":"join_request","id":"ghi","device":{"n":"Phone",)"
                 R"("p":"ios"},"ttl":60,"scanned":true})");
  socket.message(R"({"t":"join_closed","id":"abc","reason":"approved"})");
  socket.message(R"({"t":"join_closed","id":"def","reason":"left"})");
  socket.message(
      R"({"t":"error","for":"join_approve","reason":"not_found","id":"x"})");
  std::lock_guard lock(mutex);
  REQUIRE(requests.size() == 2);
  CHECK(requests[1].scanned);
  CHECK(requests[1].number.empty());
  CHECK(closed == std::vector<std::string>{"abc approved", "def left"});
  CHECK(failed == std::vector<std::string>{"join_approve not_found"});
}

TEST_CASE("Live account registers again after a refused gamertag", "[live]") {
  FakeLobby lobby;
  std::vector<std::string> refused;
  std::mutex mutex;
  std::string failed;
  AccountClient::Events events;
  events.register_refused = [&](const std::string& name,
                                const std::string& reason) {
    std::lock_guard lock(mutex);
    refused.push_back(name + " " + reason);
  };
  events.request_failed = [&](const std::string& request,
                              const std::string& reason) {
    std::lock_guard lock(mutex);
    failed = request + " " + reason;
  };
  const DeviceKey key = DeviceKey::Generate();
  AccountClient client("ws://lobby.test", key, 0x0009F00000000001ull, "Taken",
                       std::move(events));
  auto socket = lobby.WaitForSocket();
  // Before the challenge there is nothing to answer.
  client.RetryRegister("Early");
  CHECK(lobby.Sent().empty());
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  REQUIRE(lobby.Sent().size() == 1);

  socket.message(
      R"({"t":"error","for":"register","reason":"taken","gamertag":"Taken"})");
  {
    std::lock_guard lock(mutex);
    CHECK(refused == std::vector<std::string>{"Taken taken"});
  }
  client.RetryRegister("Free Name");
  REQUIRE(lobby.Sent().size() == 2);
  const std::string again = lobby.Sent()[1];
  CHECK(again.find(R"("gamertag":"Free Name")") != std::string::npos);
  CHECK(VerifySignature(
      key.PublicHex(),
      "xenios-register\nlobby.test\n0009f00000000001\nFree Name\n" +
          key.PublicHex() + "\nab",
      SigOf(again)));

  socket.message(
      R"({"t":"error","for":"register","reason":"reserved","gamertag":"Free Name"})");
  CHECK(client.signed_in() == false);
  // The 5th refusal closes the socket with limit: the client stops.
  socket.message(R"({"t":"reject","reason":"limit"})");
  {
    std::lock_guard lock(mutex);
    CHECK(refused.size() == 2);
    CHECK(failed == "register limit");
  }
  socket.closed("closed 4008 limit");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(lobby.Urls().size() == 1);

  // Signed in, a retry does nothing.
  FakeLobby other;
  AccountClient signed_client("ws://lobby.test", key, 0x0009F00000000001ull,
                              "A", {});
  auto s2 = other.WaitForSocket();
  s2.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  s2.message(kWelcome);
  signed_client.RetryRegister("B");
  CHECK(other.Sent().size() == 1);
}

TEST_CASE("Live account removed from the account stops connecting", "[live]") {
  FakeLobby lobby;
  std::atomic<int> removed = 0;
  AccountClient::Events events;
  events.removed = [&] { ++removed; };
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", std::move(events));
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  socket.message(R"({"t":"reject","reason":"removed"})");
  CHECK(removed == 1);
  socket.closed("closed 4005 removed");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(lobby.Urls().size() == 1);
}

TEST_CASE("Live account signed in on another device stops, and the hub "
          "opens a new connection",
          "[live]") {
  // One device at a time, as on the console: coming back on its own would end
  // the other device's session, so the client stops; signing in again is a
  // new client, not the stopped one shared.
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("xe-live-elsewhere-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  FakeLobby lobby;
  std::mutex mutex;
  std::vector<std::string> elsewhere;
  AccountClient::Events events;
  events.signed_in_elsewhere = [&](const std::string& device) {
    std::lock_guard lock(mutex);
    elsewhere.push_back(device);
  };
  auto first = ShareAccount("ws://lobby.test", dir / "device_key",
                            0x0009F00000000001ull, "Tester", std::move(events));
  REQUIRE(first);
  auto socket = lobby.WaitForSocket();
  socket.message(R"({"t":"challenge","v":2,"nonce":"ab"})");
  socket.message(kWelcome);
  CHECK((*first)->signed_in());
  CHECK_FALSE((*first)->halted());
  socket.message(
      R"({"t":"reject","reason":"elsewhere","device":"Will's iPad"})");
  {
    std::lock_guard lock(mutex);
    CHECK(elsewhere == std::vector<std::string>{"Will's iPad"});
  }
  socket.closed("closed 4012 elsewhere");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(lobby.Urls().size() == 1);
  CHECK((*first)->halted());
  // Signing in again while the stopped one is still held.
  auto again = ShareAccount("ws://lobby.test", dir / "device_key",
                            0x0009F00000000001ull, "Tester", {});
  REQUIRE(again);
  lobby.WaitForSocket(2);
  CHECK(lobby.Urls().size() == 2);
  CHECK_FALSE((*again)->halted());
  first.reset();
  again.reset();
  std::filesystem::remove_all(dir);
}

TEST_CASE("Live account waits for a running event before going away",
          "[live]") {
  using namespace std::chrono_literals;
  FakeLobby lobby;
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> event_done = false;
  AccountClient::Events events;
  events.friends_changed = [&] {
    entered.set_value();
    released.wait();
    event_done = true;
  };
  auto client = std::make_unique<AccountClient>(
      "ws://lobby.test", DeviceKey::Generate(), 0x0009F00000000001ull,
      "Tester", std::move(events));
  auto socket = lobby.WaitForSocket();
  std::thread lobby_thread([&] { socket.message(kWelcome); });
  entered.get_future().wait();

  std::atomic<bool> destroyed = false;
  std::thread destroyer([&] {
    client.reset();
    destroyed = true;
  });
  std::this_thread::sleep_for(100ms);
  CHECK_FALSE(destroyed);
  release.set_value();
  destroyer.join();
  lobby_thread.join();
  CHECK(event_done);
  CHECK(destroyed);
  // Events after that do nothing.
  socket.message(kWelcome);
  socket.closed("gone");
}

TEST_CASE("Live account events may drop the client's last reference",
          "[live]") {
  FakeLobby lobby;
  std::shared_ptr<AccountClient> client;
  std::mutex mutex;
  std::atomic<bool> dropped = false;
  AccountClient::Events events;
  events.friends_changed = [&] {
    std::shared_ptr<AccountClient> last;
    {
      std::lock_guard lock(mutex);
      last = std::move(client);
    }
    last.reset();  // Destroys the client on the socket's thread.
    dropped = true;
  };
  {
    std::lock_guard lock(mutex);
    client = std::make_shared<AccountClient>(
        "ws://lobby.test", DeviceKey::Generate(), 0x0009F00000000001ull,
        "Tester", std::move(events));
  }
  auto socket = lobby.WaitForSocket();
  std::thread lobby_thread([&] { socket.message(kWelcome); });
  lobby_thread.join();
  CHECK(dropped);
}


TEST_CASE("Live account holds a bounded number of invites", "[live]") {
  FakeLobby lobby;
  AccountClient client("ws://lobby.test", DeviceKey::Generate(),
                       0x0009F00000000001ull, "Tester", {});
  auto socket = lobby.WaitForSocket();
  socket.message(kWelcome);
  for (int i = 0; i < 200; ++i) {
    char text[200];
    std::snprintf(text, sizeof(text),
                  R"({"t":"invite","id":"%016x","kind":"game",)"
                  R"("from":"0009f0000000%04x","gamertag":"P",)"
                  R"("title":"4D5307E6"})",
                  0x1000 + i, 0x100 + i);
    socket.message(text);
  }
  const auto invites = client.Invites();
  CHECK(invites.size() == kMaxHeldInvites);
  // The newest are the ones kept.
  CHECK(invites.back().id == "00000000000010c7");
}

}  // namespace
}  // namespace xe::net::live
