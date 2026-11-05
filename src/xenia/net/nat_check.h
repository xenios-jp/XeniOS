/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_NAT_CHECK_H_
#define XENIA_NET_NAT_CHECK_H_

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/net/relay_protocol.h"
#include "xenia/net/stun.h"

namespace xe::net::relay {

// How the network in front of this socket treats UDP, as far as players
// reaching it directly go.
enum class NatType : uint8_t {
  // Not checked, or the check was inconclusive.
  kUnknown,
  // No NAT: the servers see the socket's own address.
  kOpen,
  // A NAT that shows every server the same address and port, so a peer can
  // usually reach this socket at the address the lobby lists.
  kModerate,
  // A NAT that shows each server another port or address. Direct connections
  // only work when the other player's network is open or moderate.
  kStrict,
  // No STUN server answered: UDP doesn't get out at all.
  kBlocked,
};

std::string_view ToString(NatType type);
std::optional<NatType> ParseNatType(std::string_view text);

// Strict or blocked: this network itself keeps most direct connections out.
inline bool BlocksDirect(NatType type) {
  return type == NatType::kStrict || type == NatType::kBlocked;
}

// What two STUN servers saw: where each says the socket's requests came
// from, or none when it never answered. |local| holds the socket's own
// addresses on the host's networks.
NatType ClassifyNat(const std::optional<Endpoint>& first,
                    const std::optional<Endpoint>& second,
                    std::span<const Endpoint> local);

// The two servers to ask, from the addresses the lobby's STUN servers resolve
// to, in the lobby's order: the first two different ones, preferring
// different hosts. With a single address, the second asks it on the other
// usual STUN port (3478 or 19302).
std::vector<Endpoint> PickNatServers(std::span<const Endpoint> resolved);

// A name for the network the host is on now, from its interfaces' names and
// addresses, to remember a check's result by. Not the addresses themselves.
std::string NetworkKey();

// Results by NetworkKey(), for this process. Thread-safe.
void RememberNat(const std::string& network, NatType type);
NatType RememberedNat(const std::string& network);

// Sends one binding request to each of two STUN servers from one socket and
// classifies the answers. Without a socket or a clock, like TurnClient.
class NatCheck {
 public:
  NatCheck(std::vector<Endpoint> servers, std::vector<Endpoint> local);

  // Sends the requests that are due. Call every few tens of ms.
  void Tick(uint64_t now_ms, std::vector<Datagram>& out);
  // Whether |datagram| answered one of this check's requests. Anything else
  // is left for the caller.
  bool Receive(std::span<const uint8_t> datagram, const Endpoint& from);

  bool done() const;
  NatType result() const;
  const std::vector<Endpoint>& servers() const { return servers_; }

 private:
  // RFC 8489's 500 ms, doubling, given up after about 8 s: the check only
  // decides what to tell the player.
  static constexpr uint64_t kFirstRetryMs = 500;
  static constexpr uint32_t kSends = 4;
  static constexpr uint64_t kGiveUpMs = 8000;

  struct Ask {
    Endpoint server;
    stun::TransactionId id{};
    std::vector<uint8_t> request;
    uint64_t next_ms = 0;
    uint32_t sends = 0;
    std::optional<Endpoint> mapped;
  };

  std::vector<Endpoint> servers_;
  std::vector<Endpoint> local_;
  std::vector<Ask> asks_;
  std::optional<uint64_t> started_ms_;
  bool timed_out_ = false;
};

}  // namespace xe::net::relay

#endif  // XENIA_NET_NAT_CHECK_H_
