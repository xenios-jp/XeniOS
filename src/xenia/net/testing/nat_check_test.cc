/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <utility>
#include <vector>

#include "xenia/net/nat_check.h"
#include "xenia/net/stun.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::net::relay::test {
namespace {

using stun::Attribute;
using stun::Class;
using stun::Method;

Endpoint V4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
  Endpoint endpoint;
  endpoint.address = {a, b, c, d};
  endpoint.port = port;
  return endpoint;
}

const Endpoint kLocal = V4(192, 168, 1, 20, 40000);
const Endpoint kServerA = V4(203, 0, 113, 1, 3478);
const Endpoint kServerB = V4(203, 0, 113, 2, 3478);

// A STUN server's answer to |request|, saying it came from |mapped|.
std::vector<uint8_t> Answer(std::span<const uint8_t> request,
                            const Endpoint& mapped) {
  auto message = stun::Read(request);
  REQUIRE(message);
  return stun::Writer(Method::kBinding, Class::kSuccess, message->transaction)
      .AddAddress(Attribute::kXorMappedAddress, mapped)
      .Finish();
}

}  // namespace

TEST_CASE("NAT types follow from what two servers saw", "[nat]") {
  const std::vector<Endpoint> local{kLocal};
  const Endpoint mapped = V4(198, 51, 100, 7, 50000);
  // The same address and port to both: a NAT that keeps one mapping.
  CHECK(ClassifyNat(mapped, mapped, local) == NatType::kModerate);
  // The socket's own address: no NAT.
  CHECK(ClassifyNat(kLocal, kLocal, local) == NatType::kOpen);
  // Another port, or another address, per server.
  CHECK(ClassifyNat(mapped, V4(198, 51, 100, 7, 50001), local) ==
        NatType::kStrict);
  CHECK(ClassifyNat(mapped, V4(198, 51, 100, 8, 50000), local) ==
        NatType::kStrict);
  // Nobody answered: UDP doesn't get out.
  CHECK(ClassifyNat(std::nullopt, std::nullopt, local) == NatType::kBlocked);
  // One answer can only show there's no NAT.
  CHECK(ClassifyNat(mapped, std::nullopt, local) == NatType::kUnknown);
  CHECK(ClassifyNat(std::nullopt, mapped, local) == NatType::kUnknown);
  CHECK(ClassifyNat(std::nullopt, kLocal, local) == NatType::kOpen);
  CHECK(ClassifyNat(mapped, mapped, {}) == NatType::kModerate);
}

TEST_CASE("NAT types read and write", "[nat]") {
  for (NatType type : {NatType::kUnknown, NatType::kOpen, NatType::kModerate,
                       NatType::kStrict, NatType::kBlocked}) {
    CHECK(ParseNatType(ToString(type)) == type);
  }
  CHECK(!ParseNatType(""));
  CHECK(!ParseNatType("Strict"));
  CHECK(BlocksDirect(NatType::kStrict));
  CHECK(BlocksDirect(NatType::kBlocked));
  CHECK(!BlocksDirect(NatType::kModerate));
  CHECK(!BlocksDirect(NatType::kUnknown));
}

TEST_CASE("NAT checks ask two hosts when there are two", "[nat]") {
  const Endpoint a_other_port = V4(203, 0, 113, 1, 19302);
  CHECK(PickNatServers(std::vector{kServerA, kServerB}) ==
        std::vector{kServerA, kServerB});
  // Another host beats another port of the first.
  CHECK(PickNatServers(std::vector{kServerA, a_other_port, kServerB}) ==
        std::vector{kServerA, kServerB});
  CHECK(PickNatServers(std::vector{kServerA, kServerA, a_other_port}) ==
        std::vector{kServerA, a_other_port});
  // A lone server is asked on the other usual port too.
  CHECK(PickNatServers(std::vector{kServerA}) ==
        std::vector{kServerA, a_other_port});
  CHECK(PickNatServers(std::vector{a_other_port}) ==
        std::vector{a_other_port, kServerA});
  CHECK(PickNatServers(std::vector<Endpoint>{}).empty());
}

TEST_CASE("NAT checks send two requests from one socket", "[nat]") {
  NatCheck check({kServerA, kServerB}, {kLocal});
  std::vector<Datagram> out;
  uint64_t now = 1000;
  check.Tick(now, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0].to == kServerA);
  CHECK(out[1].to == kServerB);
  CHECK(!check.done());

  // Unanswered requests go again, half a second later, then a second.
  std::vector<Datagram> again;
  check.Tick(now + 400, again);
  CHECK(again.empty());
  check.Tick(now + 500, again);
  CHECK(again.size() == 2);

  const Endpoint mapped = V4(198, 51, 100, 7, 50000);
  // Answers from elsewhere, or to other requests, are someone else's.
  CHECK(!check.Receive(Answer(out[0].data, mapped), kServerB));
  CHECK(!check.Receive(
      stun::Writer(Method::kBinding, Class::kSuccess, stun::NewTransactionId())
          .AddAddress(Attribute::kXorMappedAddress, mapped)
          .Finish(),
      kServerA));
  CHECK(!check.Receive(std::vector<uint8_t>{1, 2, 3}, kServerA));

  CHECK(check.Receive(Answer(out[0].data, mapped), kServerA));
  CHECK(!check.done());
  CHECK(check.result() == NatType::kUnknown);
  CHECK(
      check.Receive(Answer(out[1].data, V4(198, 51, 100, 7, 50123)), kServerB));
  CHECK(check.done());
  CHECK(check.result() == NatType::kStrict);

  // Nothing more goes out once it's done.
  std::vector<Datagram> after;
  check.Tick(now + 5000, after);
  CHECK(after.empty());
}

TEST_CASE("NAT checks that hear nothing call UDP blocked", "[nat]") {
  NatCheck check({kServerA, kServerB}, {kLocal});
  std::vector<Datagram> out;
  size_t sent = 0;
  for (uint64_t now = 0; now <= 10000 && !check.done(); now += 25) {
    check.Tick(now, out);
    sent += std::exchange(out, {}).size();
  }
  CHECK(check.done());
  CHECK(check.result() == NatType::kBlocked);
  // A few requests each, not a stream.
  CHECK(sent <= 8);
}

TEST_CASE("NAT checks with one answer don't guess", "[nat]") {
  NatCheck check({kServerA, kServerB}, {kLocal});
  std::vector<Datagram> out;
  check.Tick(0, out);
  REQUIRE(out.size() == 2);
  const Endpoint mapped = V4(198, 51, 100, 7, 50000);
  CHECK(check.Receive(Answer(out[0].data, mapped), kServerA));
  for (uint64_t now = 0; now <= 10000 && !check.done(); now += 25) {
    check.Tick(now, out);
  }
  CHECK(check.done());
  CHECK(check.result() == NatType::kUnknown);
}

TEST_CASE("NAT results are remembered per network", "[nat]") {
  RememberNat("test-network-1", NatType::kStrict);
  CHECK(RememberedNat("test-network-1") == NatType::kStrict);
  CHECK(RememberedNat("test-network-2") == NatType::kUnknown);
  RememberNat("test-network-1", NatType::kUnknown);
  CHECK(RememberedNat("test-network-1") == NatType::kUnknown);
  CHECK(NetworkKey().size() == 16);
  CHECK(NetworkKey() == NetworkKey());
}

}  // namespace xe::net::relay::test
