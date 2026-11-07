/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_XNET_ASSOCIATION_H_
#define XENIA_NET_XNET_ASSOCIATION_H_

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "xenia/net/xnet_secure.h"

namespace xe::net::secure {

struct AssociationConfig {
  Address identity{};
  ProtectionKeys broadcast;
  Metadata metadata{};
  // Reason bytes carried by teardown packets.
  std::array<uint8_t, 4> teardown_reason{};
  std::function<std::optional<Key>(const KeyId&)> credential;
  std::function<uint64_t()> freshness_time;
  uint32_t mtu = 1500;
  size_t peer_limit = 32;
  // Local timing policy, not values taken from the console.
  uint64_t retry_ms = 1000, idle_ms = 60000, keepalive_ms = 10000;
  unsigned retries = 5;
  // Starts sequences at zero and accepts zero from peers.
  bool sequence_zero = false;
  // Set for titles with cross-platform System Link.
  std::optional<CrossPlatform> cross_platform;
};

enum class AssociationStatus { Missing, Pending, Established };

struct AssociationEvent {
  enum class Type { Datagram, Connected, Retired } type = Type::Datagram;
  Address peer{};
  KeyId id{};
  Mac source{};
  Packet packet;
};

// Process-wide: told when an association this side started connects, or
// retires without having connected, with the key (the session's XNKID) it was
// for. Joining a session reports why it failed by it. Called with the owner's
// lock held: keep it short, and call nothing back into System Link.
using AssociationObserver =
    std::function<void(const KeyId& id, bool connected)>;
void SetAssociationObserver(AssociationObserver observer);

// The owner serializes calls. State changes only after a packet authenticates.
class Associations {
 public:
  explicit Associations(AssociationConfig config);
  ~Associations();

  bool Connect(const Address& peer, const KeyId& id, uint64_t now_ms);
  bool Send(const Address& peer, const KeyId& id, Packet packet,
            uint64_t now_ms);
  bool Discover(Packet packet);
  void Receive(const Frame& frame, uint64_t now_ms);
  void Tick(uint64_t now_ms);
  void Unregister(const KeyId& id, uint64_t now_ms);
  // Sends |reason| in place of the configured teardown reason when set.
  // |report| queues a Retired event.
  void Close(const Address& peer, const KeyId& id, uint64_t now_ms,
             std::optional<std::array<uint8_t, 4>> reason = std::nullopt,
             bool report = true);
  void Reset();
  AssociationStatus Status(const Address& peer, const KeyId& id) const;
  std::vector<Frame> TakeOutbound();
  std::vector<AssociationEvent> TakeEvents();
  // The kBroadcastVersion* bits of broadcasts that didn't authenticate since
  // the last |reset|.
  uint32_t BroadcastVersionStatus(bool reset);

 private:
  struct State;
  using PeerKey = std::pair<Address, KeyId>;

  Mac MachineOf(const Address& address) const;
  bool SystemLinkKey(const KeyId& id) const;
  bool SameMachine(const Address& a, const Address& b) const;
  // The state of the MAC |peer| names, keyed by the XNADDR it was first named
  // by.
  State* Find(const Address& peer, const KeyId& id) const;
  uint32_t NewSpi();
  uint64_t NewFreshness();
  bool Emit(Mac destination, std::vector<uint8_t> payload,
            bool broadcast = false);
  bool ExchangePacket(State& state, uint64_t now_ms);
  bool Protected(State& state, const Packet& packet, uint64_t now_ms);
  void Established(State& state, uint64_t now_ms);
  // Drops |key| and the output it queued. |report| also drops its events and
  // tells the owner it retired.
  void Retired(PeerKey key, bool report = true);

  AssociationConfig config_;
  Mac local_{};
  std::map<PeerKey, std::unique_ptr<State>> states_;
  std::vector<Frame> outbound_;
  std::vector<AssociationEvent> events_;
  uint16_t discovery_sequence_ = 0;
  uint64_t freshness_ = 0;
  uint32_t broadcast_version_status_ = 0;
};

}  // namespace xe::net::secure

#endif  // XENIA_NET_XNET_ASSOCIATION_H_
