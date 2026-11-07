/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/live_directory.h"

#include "third_party/catch/include/catch.hpp"
#include "third_party/fmt/include/fmt/format.h"

namespace xe::net::live {
namespace {

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

}  // namespace
}  // namespace xe::net::live
