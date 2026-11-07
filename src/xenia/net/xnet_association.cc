/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/xnet_association.h"

#include <algorithm>
#include <cstdint>
#include <mutex>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"

#include "third_party/mbedtls/include/mbedtls/platform_util.h"

namespace xe::net::secure {
namespace {

// Frames waiting for the owner to send them. A TCP segment stops short of the
// limit so a window's worth of them leaves room for everything else.
constexpr size_t kOutboundLimit = 512, kTcpOutboundLimit = 384;

uint32_t Spi(std::span<const uint8_t> payload) {
  return (uint32_t(payload[1]) << 16) | (uint32_t(payload[2]) << 8) |
         payload[3];
}

struct Observer {
  std::mutex mutex;
  AssociationObserver observer;
};

Observer& GetObserver() {
  static Observer observer;
  return observer;
}

void Observe(const KeyId& id, bool connected) {
  auto& observer = GetObserver();
  std::lock_guard lock(observer.mutex);
  if (observer.observer) {
    observer.observer(id, connected);
  }
}

}  // namespace

void SetAssociationObserver(AssociationObserver observer) {
  auto& state = GetObserver();
  std::lock_guard lock(state.mutex);
  state.observer = std::move(observer);
}

struct Associations::State {
  explicit State(bool sequence_zero) : replay(sequence_zero) {}
  ~State() {
    mbedtls_platform_zeroize(&registration, sizeof(registration));
    mbedtls_platform_zeroize(&transmit, sizeof(transmit));
    mbedtls_platform_zeroize(&receive, sizeof(receive));
  }

  PeerKey key;
  RegistrationKeys registration;
  std::optional<DhKeyPair> dh;
  Exchange request, exchange;
  ProtectionKeys transmit, receive;
  ReplayWindow replay;
  std::deque<Packet> pending;
  AssociationStatus status = AssociationStatus::Pending;
  uint32_t local_spi = 0, remote_spi = 0, sequence = 0;
  uint64_t sent = 0, received = 0, attempted = 0, remote_freshness = 0;
  unsigned attempts = 0;
  bool initiator = false, have_freshness = false, have_sent = false;
};

Associations::Associations(AssociationConfig config)
    : config_(std::move(config)) {
  local_ = MachineOf(config_.identity);
  config_.peer_limit = std::clamp<size_t>(config_.peer_limit, 1, 32);
  config_.retry_ms = std::max<uint64_t>(config_.retry_ms, 100);
  config_.idle_ms = std::max<uint64_t>(config_.idle_ms, 1000);
  config_.keepalive_ms = std::max<uint64_t>(config_.keepalive_ms, 100);
  config_.retries = std::clamp(config_.retries, 1u, 32u);
}

Associations::~Associations() = default;

Mac Associations::MachineOf(const Address& address) const {
  return Machine(address, config_.cross_platform.has_value());
}

bool Associations::SystemLinkKey(const KeyId& id) const {
  return SystemLinkKeyId(id, config_.cross_platform.has_value());
}

bool Associations::SameMachine(const Address& a, const Address& b) const {
  return secure::SameMachine(a, b, config_.cross_platform.has_value());
}

Associations::State* Associations::Find(const Address& peer,
                                        const KeyId& id) const {
  // xam keeps one association per MAC and key.
  for (const auto& [key, state] : states_) {
    if (key.second == id && Machine(key.first, false) == Machine(peer, false)) {
      return state.get();
    }
  }
  return nullptr;
}

uint32_t Associations::NewSpi() {
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    std::array<uint8_t, 3> random;
    Random(random);
    uint32_t value =
        (uint32_t(random[0]) << 16) | (uint32_t(random[1]) << 8) | random[2];
    if (!value) {
      continue;
    }
    bool used = false;
    for (const auto& [key, state] : states_) {
      used |= state->local_spi == value;
    }
    if (!used) {
      return value;
    }
  }
  return 0;
}

uint64_t Associations::NewFreshness() {
  // FILETIME ticks, kept strictly increasing so retries always look newer.
  uint64_t current = config_.freshness_time ? config_.freshness_time()
                                            : Clock::QueryHostSystemTime();
  freshness_ = std::max(current, freshness_ + 1);
  return freshness_;
}

bool Associations::Emit(Mac destination, std::vector<uint8_t> payload,
                        bool broadcast) {
  if (outbound_.size() >= kOutboundLimit || payload.size() + 28 > config_.mtu) {
    return false;
  }
  outbound_.push_back({local_, destination, broadcast, std::move(payload)});
  return true;
}

bool Associations::ExchangePacket(State& state, uint64_t now) {
  state.exchange.freshness = NewFreshness();
  Random(state.exchange.address_iv);
  auto payload = EncodeExchange(state.registration, state.exchange);
  if (!payload ||
      !Emit(MachineOf(state.key.first), {payload->begin(), payload->end()})) {
    return false;
  }
  state.attempted = now;
  ++state.attempts;
  XELOGD("XNet exchange sent: response={}, attempt={}", state.exchange.response,
         state.attempts);
  return true;
}

bool Associations::Connect(const Address& peer, const KeyId& id, uint64_t now) {
  if (Find(peer, id)) {
    return true;
  }
  if (peer == config_.identity || MachineOf(peer) == local_ ||
      !SystemLinkKey(id) || states_.size() >= config_.peer_limit ||
      events_.size() >= 96 || !config_.credential ||
      (config_.cross_platform && !SubnetPeer(peer, *config_.cross_platform))) {
    return false;
  }
  auto credential = config_.credential(id);
  if (!credential) {
    return false;
  }
  auto state = std::make_unique<State>(config_.sequence_zero);
  state->key = {peer, id};
  state->local_spi = NewSpi();
  state->dh = DhKeyPair::Generate();
  if (!state->local_spi || !state->dh) {
    return false;
  }
  state->registration =
      DeriveRegistration(config_.broadcast.authentication, id, *credential);
  state->initiator = true;
  state->received = now;
  auto& request = state->exchange;
  request.id = id;
  request.initiator_spi = state->local_spi;
  Random(request.initiator_challenge);
  request.initiator_address = config_.identity;
  request.responder_address = peer;
  request.public_value = state->dh->public_value();
  state->request = request;
  if (!ExchangePacket(*state, now)) {
    return false;
  }
  states_.emplace(state->key, std::move(state));
  XELOGI("XNet association pending");
  return true;
}

bool Associations::Protected(State& state, const Packet& packet, uint64_t now) {
  if (state.status != AssociationStatus::Established ||
      state.sequence == UINT32_MAX) {
    return false;
  }
  std::vector<uint8_t> payload;
  const uint32_t sequence =
      !state.have_sent && config_.sequence_zero ? 0 : state.sequence + 1;
  if (!Encode(state.transmit, state.remote_spi, sequence, packet, payload) ||
      !Emit(MachineOf(state.key.first), std::move(payload))) {
    return false;
  }
  state.sequence = sequence;
  state.have_sent = true;
  state.sent = now;
  return true;
}

bool Associations::Send(const Address& peer, const KeyId& id, Packet packet,
                        uint64_t now) {
  if (packet.kind == Kind::Control || packet.kind == Kind::Discovery ||
      (packet.kind == Kind::Tcp && outbound_.size() >= kTcpOutboundLimit) ||
      packet.encrypted.size() + packet.clear.size() > config_.mtu ||
      !Connect(peer, id, now)) {
    return false;
  }
  auto& state = *Find(peer, id);
  if (state.status == AssociationStatus::Established) {
    return Protected(state, packet, now);
  }
  if (state.pending.size() >= 32) {
    return false;
  }
  // Encode once with placeholder keys to check the packet fits before queuing.
  std::vector<uint8_t> sizing;
  if (!Encode(ProtectionKeys{}, 1, 1, packet, sizing) ||
      sizing.size() + 28 > config_.mtu) {
    return false;
  }
  state.pending.push_back(std::move(packet));
  return true;
}

bool Associations::Discover(Packet packet) {
  packet.kind = Kind::Discovery;
  packet.metadata = config_.metadata;
  std::vector<uint8_t> payload;
  uint16_t sequence = discovery_sequence_ + 1;
  if (!Encode(config_.broadcast, 0, sequence, packet, payload)) {
    return false;
  }
  Mac broadcast;
  broadcast.fill(255);
  if (!Emit(broadcast, std::move(payload), true)) {
    return false;
  }
  discovery_sequence_ = sequence;
  return true;
}

void Associations::Established(State& state, uint64_t now) {
  if (state.status == AssociationStatus::Established) {
    return;
  }
  state.status = AssociationStatus::Established;
  XELOGI("XNet association established");
  if (state.initiator) {
    Observe(state.key.second, true);
  }
  state.received = now;
  state.attempts = 0;
  events_.push_back({AssociationEvent::Type::Connected,
                     state.key.first,
                     state.key.second,
                     MachineOf(state.key.first),
                     {}});
  Packet keepalive;
  keepalive.kind = Kind::Control;
  keepalive.control_type = 0x0153;
  Protected(state, keepalive, now);
  while (!state.pending.empty()) {
    if (!Protected(state, state.pending.front(), now)) {
      break;
    }
    state.pending.pop_front();
  }
}

void Associations::Retired(PeerKey key, bool report) {
  auto found = states_.find(key);
  if (found == states_.end()) {
    return;
  }
  const uint32_t remote_spi = found->second->remote_spi;
  const bool established =
      found->second->status == AssociationStatus::Established;
  XELOGI("XNet association retired: established={}, attempts={}", established,
         found->second->attempts);
  if (found->second->initiator && !established) {
    Observe(key.second, false);
  }
  states_.erase(found);
  // Drop this generation's queued output and events so a rejoin starts clean.
  auto mac = MachineOf(key.first);
  std::erase_if(outbound_, [&](const Frame& frame) {
    if (frame.destination != mac || frame.payload.size() < 20) {
      return false;
    }
    if (Spi(frame.payload)) {
      return Spi(frame.payload) == remote_spi;
    }
    return std::equal(key.second.begin(), key.second.end(),
                      frame.payload.begin() + 12);
  });
  if (!report) {
    return;
  }
  std::erase_if(events_, [&](const AssociationEvent& event) {
    return event.peer == key.first && event.id == key.second;
  });
  events_.push_back(
      {AssociationEvent::Type::Retired, key.first, key.second, mac, {}});
}

void Associations::Receive(const Frame& frame, uint64_t now) {
  if (frame.source == local_ ||
      (!frame.broadcast && frame.destination != local_) ||
      frame.payload.size() < 16 || frame.payload.size() + 28 > config_.mtu) {
    return;
  }
  const auto& wire = frame.payload;
  uint16_t low =
      wire[wire.size() - 12] | (uint16_t(wire[wire.size() - 11]) << 8);
  if (frame.broadcast) {
    if (auto packet = Decode(config_.broadcast, 0, low, wire, true)) {
      if (events_.size() < 96) {
        events_.push_back({AssociationEvent::Type::Datagram,
                           {},
                           {},
                           frame.source,
                           std::move(*packet)});
      }
    } else if (auto metadata = PeekMetadata(wire)) {
      broadcast_version_status_ |=
          BroadcastVersionDifference(config_.metadata, *metadata);
    }
    return;
  }
  if (!wire[0] && !Spi(wire)) {
    XELOGD("XNet exchange received: bytes={}", wire.size());
    if (wire.size() != 256 || !config_.credential) {
      return;
    }
    KeyId id;
    std::copy_n(wire.begin() + 12, id.size(), id.begin());
    if (!SystemLinkKey(id)) {
      return;
    }
    auto credential = config_.credential(id);
    if (!credential) {
      XELOGD("XNet exchange rejected: credential unavailable");
      return;
    }
    auto registration =
        DeriveRegistration(config_.broadcast.authentication, id, *credential);
    auto exchange = DecodeExchange(registration, id, wire);
    if (!exchange) {
      XELOGD("XNet exchange rejected: decoding failed");
      return;
    }
    if (exchange->response) {
      // The responder may name both machines by XNADDRs other than the
      // request's.
      auto found =
          std::find_if(states_.begin(), states_.end(), [&](const auto& item) {
            return item.first.second == id &&
                   item.second->local_spi == exchange->initiator_spi;
          });
      if (found == states_.end()) {
        XELOGD("XNet exchange rejected: response peer unavailable");
        return;
      }
      auto& state = *found->second;
      const bool machines =
          SameMachine(exchange->initiator_address, config_.identity) &&
          SameMachine(state.request.responder_address,
                      exchange->responder_address);
      const bool source_matches = frame.source == MachineOf(state.key.first);
      if (!state.initiator || !ResponseMatches(state.request, *exchange) ||
          !machines || !source_matches ||
          (state.have_freshness &&
           !FreshnessNewer(exchange->freshness, state.remote_freshness,
                           true))) {
        XELOGD(
            "XNet exchange rejected: initiator={}, request matches={}, "
            "machines match={}, source matches={}",
            state.initiator, ResponseMatches(state.request, *exchange),
            machines, source_matches);
        return;
      }
      // A retried response may confirm an established association but never
      // rekey it.
      if (state.status == AssociationStatus::Established) {
        if (state.remote_spi != exchange->responder_spi ||
            state.exchange.responder_challenge !=
                exchange->responder_challenge ||
            state.exchange.public_value != exchange->public_value) {
          return;
        }
      } else {
        auto shared = state.dh->Shared(exchange->public_value);
        if (!shared) {
          return;
        }
        auto keys = DeriveTraffic(registration.authentication, *shared,
                                  exchange->initiator_challenge,
                                  exchange->responder_challenge);
        state.transmit = keys.a;
        state.receive = keys.b;
        state.remote_spi = exchange->responder_spi;
        state.exchange = *exchange;
      }
      state.have_freshness = true;
      state.remote_freshness = exchange->freshness;
      state.received = now;
      if (state.status == AssociationStatus::Established) {
        Packet keepalive;
        keepalive.kind = Kind::Control;
        keepalive.control_type = 0x0153;
        Protected(state, keepalive, now);
      } else {
        Established(state, now);
      }
      return;
    }
    if (!SameMachine(exchange->responder_address, config_.identity) ||
        exchange->initiator_address == config_.identity ||
        frame.source != MachineOf(exchange->initiator_address)) {
      return;
    }
    State* existing = Find(exchange->initiator_address, id);
    // A simultaneous connect is answered in the association the title asked
    // for. Any other new challenge means the peer started over, which xam
    // answers in a new association.
    bool crossed = false;
    if (existing) {
      auto& state = *existing;
      if (state.have_freshness &&
          !FreshnessNewer(exchange->freshness, state.remote_freshness, false)) {
        return;
      }
      if (state.initiator && state.status == AssociationStatus::Pending) {
        // On a simultaneous connect the larger XNADDR MAC stays the initiator.
        if (!(Machine(config_.identity, false) <
              Machine(exchange->initiator_address, false))) {
          return;
        }
        crossed = true;
      } else if (state.request.initiator_challenge ==
                 exchange->initiator_challenge) {
        // A retry only gets the answer again while it is pending.
        state.have_freshness = true;
        state.remote_freshness = exchange->freshness;
        if (state.status == AssociationStatus::Pending) {
          ExchangePacket(state, now);
        }
        return;
      }
    }
    // A cross-platform peer is answered at the IP it came from.
    const PeerKey key =
        crossed && MachineOf(existing->key.first) == frame.source
            ? existing->key
            : PeerKey{exchange->initiator_address, id};
    // xam frees the association it replaces first.
    if (states_.size() - (existing ? 1 : 0) >= config_.peer_limit ||
        events_.size() >= 96) {
      return;
    }
    auto state = std::make_unique<State>(config_.sequence_zero);
    state->key = key;
    state->local_spi = NewSpi();
    state->dh = DhKeyPair::Generate();
    if (!state->local_spi || !state->dh) {
      return;
    }
    auto shared = state->dh->Shared(exchange->public_value);
    if (!shared) {
      return;
    }
    state->registration = registration;
    state->request = *exchange;
    state->exchange = *exchange;
    state->exchange.response = true;
    state->exchange.initiator_address = key.first;
    state->exchange.responder_address = config_.identity;
    state->exchange.responder_spi = state->local_spi;
    Random(state->exchange.responder_challenge);
    state->exchange.public_value = state->dh->public_value();
    auto keys = DeriveTraffic(registration.authentication, *shared,
                              exchange->initiator_challenge,
                              state->exchange.responder_challenge);
    state->receive = keys.a;
    state->transmit = keys.b;
    state->remote_spi = exchange->initiator_spi;
    state->received = now;
    state->have_freshness = true;
    state->remote_freshness = exchange->freshness;
    if (existing) {
      if (crossed) {
        state->pending = std::move(existing->pending);
      }
      // xam sends no teardown for the association it drops.
      Retired(existing->key, !crossed);
    }
    if (!ExchangePacket(*state, now)) {
      return;
    }
    states_.emplace(key, std::move(state));
    return;
  }
  auto found =
      std::find_if(states_.begin(), states_.end(), [&](const auto& item) {
        return item.second->local_spi == Spi(wire) &&
               MachineOf(item.first.first) == frame.source;
      });
  if (found == states_.end()) {
    XELOGD("XNet protected packet rejected: association unavailable");
    return;
  }
  auto& state = *found->second;
  if (state.initiator && state.status != AssociationStatus::Established) {
    return;
  }
  for (uint32_t sequence : state.replay.Candidates(low)) {
    auto packet = Decode(state.receive, state.local_spi, sequence, wire);
    if (!packet || !state.replay.Commit(sequence)) {
      continue;
    }
    state.received = now;
    if (packet->kind == Kind::Control && packet->control_type == 0x0053) {
      Retired(found->first);
      events_.back().packet = std::move(*packet);
      return;
    }
    Established(state, now);
    if (packet->kind != Kind::Control && events_.size() < 96) {
      events_.push_back({AssociationEvent::Type::Datagram, state.key.first,
                         state.key.second, frame.source, std::move(*packet)});
    }
    return;
  }
  XELOGD(
      "XNet protected packet rejected: authentication or replay check failed");
}

void Associations::Tick(uint64_t now) {
  std::vector<PeerKey> expired;
  for (auto& [key, state] : states_) {
    if (!config_.credential || !config_.credential(key.second)) {
      expired.push_back(key);
      continue;
    }
    if (now - state->received >= config_.idle_ms ||
        state->sequence == UINT32_MAX) {
      expired.push_back(key);
      continue;
    }
    if (state->status == AssociationStatus::Pending &&
        now - state->attempted >= config_.retry_ms) {
      if (state->attempts >= config_.retries) {
        expired.push_back(key);
      } else {
        ExchangePacket(*state, now);
      }
    } else if (state->status == AssociationStatus::Established &&
               now - state->sent >= config_.keepalive_ms) {
      Packet keepalive;
      keepalive.kind = Kind::Control;
      keepalive.control_type = 0x0153;
      Protected(*state, keepalive, now);
    }
    while (state->status == AssociationStatus::Established &&
           !state->pending.empty()) {
      if (!Protected(*state, state->pending.front(), now)) {
        break;
      }
      state->pending.pop_front();
    }
  }
  for (const auto& key : expired) {
    Retired(key);
  }
}

void Associations::Close(const Address& peer, const KeyId& id, uint64_t now,
                         std::optional<std::array<uint8_t, 4>> reason,
                         bool report) {
  State* state = Find(peer, id);
  if (!state) {
    return;
  }
  Packet teardown;
  teardown.kind = Kind::Control;
  teardown.control_type = 0x0053;
  const auto& bytes = reason ? *reason : config_.teardown_reason;
  teardown.encrypted.assign(bytes.begin(), bytes.end());
  bool sent = Protected(*state, teardown, now);
  // Retired() drops queued output for this peer so set the teardown aside.
  std::optional<Frame> final;
  if (sent) {
    final = std::move(outbound_.back());
    outbound_.pop_back();
  }
  Retired(state->key, report);
  if (final) {
    outbound_.push_back(std::move(*final));
  }
}

void Associations::Unregister(const KeyId& id, uint64_t now) {
  std::vector<Address> peers;
  for (const auto& [key, state] : states_) {
    if (key.second == id) {
      peers.push_back(key.first);
    }
  }
  for (const auto& peer : peers) {
    Close(peer, id, now);
  }
}

void Associations::Reset() {
  states_.clear();
  outbound_.clear();
  events_.clear();
  broadcast_version_status_ = 0;
}

uint32_t Associations::BroadcastVersionStatus(bool reset) {
  const uint32_t status = broadcast_version_status_;
  if (reset) {
    broadcast_version_status_ = 0;
  }
  return status;
}

AssociationStatus Associations::Status(const Address& peer,
                                       const KeyId& id) const {
  const State* state = Find(peer, id);
  return state ? state->status : AssociationStatus::Missing;
}

std::vector<Frame> Associations::TakeOutbound() {
  return std::exchange(outbound_, {});
}

std::vector<AssociationEvent> Associations::TakeEvents() {
  return std::exchange(events_, {});
}

}  // namespace xe::net::secure
