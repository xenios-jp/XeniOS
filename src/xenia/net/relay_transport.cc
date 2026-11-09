/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/net/host_interfaces.h"
#include "xenia/net/nat_check.h"
#include "xenia/net/network_path.h"
#include "xenia/net/packet_transport.h"
#include "xenia/net/relay_socket.h"

#if XE_PLATFORM_APPLE
#include "xenia/net/lobby_socket_apple.h"
#endif

namespace xe::net::secure {
namespace {

using relay::Datagram;
using relay::Endpoint;
using relay::NatType;
using relay::RelayStatus;

constexpr uint32_t kMtu = 1500;
constexpr uint64_t kAskAllowedMs = 1000;
// The host's addresses are read once a second, so a change of network is
// noticed well before the game drops the other players (Halo 3 after about
// 7 s without them).
constexpr uint64_t kHostCandidatesMs = 1000;
constexpr uint16_t kStunPort = 3478;

uint64_t Now() { return Clock::QueryHostUptimeMillis(); }

std::mutex listener_mutex;
std::function<void(const RelayStatus&)> status_listener;

void Report(const RelayStatus& status) {
  std::function<void(const RelayStatus&)> listener;
  {
    std::lock_guard lock(listener_mutex);
    listener = status_listener;
  }
  if (listener) {
    listener(status);
  }
}

std::mutex factory_mutex;
#if XE_PLATFORM_APPLE
LobbySocketFactory lobby_factory = OpenAppleLobbySocket;
#else
LobbySocketFactory lobby_factory;
#endif

// What a lobby socket's events leave for the System Link thread.
struct LobbyInbox {
  enum class Kind { kOpened, kMessage, kClosed };
  struct Event {
    Kind kind;
    std::string text;
  };
  void Push(Kind kind, std::string text = {}) {
    std::lock_guard lock(mutex);
    events.push_back({kind, std::move(text)});
  }
  std::vector<Event> Take() {
    std::lock_guard lock(mutex);
    return std::exchange(events, {});
  }
  std::mutex mutex;
  std::vector<Event> events;
};

std::vector<Endpoint> HostCandidates(uint16_t port) {
  std::vector<Endpoint> candidates;
  for (const HostInterface& host : HostInterfaces()) {
    // A self-assigned 169.254 address (a Thunderbolt bridge, a USB network
    // device) reaches nobody in the room, and the lobby turns away a player
    // that offers one.
    if ((host.address >> 16) == 0xA9FE) {
      continue;
    }
    Endpoint endpoint;
    for (int i = 0; i < 4; ++i) {
      endpoint.address[i] = uint8_t(host.address >> (24 - 8 * i));
    }
    endpoint.port = port;
    if (std::find(candidates.begin(), candidates.end(), endpoint) ==
        candidates.end()) {
      candidates.push_back(endpoint);
    }
  }
  return candidates;
}

class RelayTransport final : public PacketTransport {
 public:
  RelayTransport(const Mac& mac, const relay::RelayConfig& config)
      : mac_(mac), config_(config), client_(mac, config) {}

  ~RelayTransport() override {
    // First, so no lobby events come after this.
    CloseLobby();
    client_.Leave(out_);
    Flush();
    // Nothing was reported while it was dormant, so there is nothing to end.
    if (active_ && config_.report_status) {
      RelayStatus stopped;
      stopped.state = RelayStatus::State::kStopped;
      Report(stopped);
    }
  }

  bool Open(std::string& error) {
    if (!socket_.Open(0, error)) {
      return false;
    }
    {
      std::lock_guard lock(factory_mutex);
      factory_ = lobby_factory;
    }
    if (!factory_) {
      XELOGW("System Link: this build has no way to reach the lobby");
    }
    return true;
  }

  Mac identity() const override { return mac_; }
  uint32_t mtu() const override { return kMtu; }
  bool healthy() const override { return healthy_; }

  // Nothing reaches the lobby, STUN or TURN before this. Any frame to send is
  // System Link play too, so Send() activates as well.
  void Activate() override {
    if (!active_.exchange(true)) {
      socket_.Wake();
    }
  }

  // A frame that can't go anywhere yet is lost like on a busy LAN.
  int Send(std::span<const uint8_t> frame) override {
    Activate();
    if (!paused_) {
      client_.Send(frame, Now(), out_);
      Flush();
    }
    return static_cast<int>(frame.size());
  }

  bool Receive(std::vector<uint8_t>& frame, uint32_t timeout_ms) override {
    const uint64_t deadline = Now() + timeout_ms;
    while (healthy_) {
      const uint64_t now = Now();
      Service(now);
      // Always waits a little, so a timeout of 0 still reads the socket.
      const uint64_t wait =
          std::clamp<uint64_t>(deadline > now ? deadline - now : 0, 1, 50);
      std::span<const uint8_t> data;
      Endpoint from;
      std::string error;
      switch (socket_.ReceiveFor(uint32_t(wait), data, from, error)) {
        case relay::RelaySocket::Wait::kWoken:
          return false;
        case relay::RelaySocket::Wait::kTimeout:
          if (Now() >= deadline) {
            return false;
          }
          continue;
        case relay::RelaySocket::Wait::kFailed:
          XELOGE("System Link: receive failed: {}", error);
          healthy_ = false;
          return false;
        case relay::RelaySocket::Wait::kDatagram:
          break;
      }
      if (nat_check_ && nat_check_->Receive(data, from)) {
        if (nat_check_->done()) {
          FinishNatCheck();
        }
      } else if (!paused_) {
        auto whole = client_.Receive(data, from, Now(), out_);
        Flush();
        ReportChanges();
        if (whole) {
          frame = std::move(*whole);
          return true;
        }
      }
      if (Now() >= deadline) {
        return false;
      }
    }
    return false;
  }

  void Wake() override { socket_.Wake(); }

 private:
  void Service(uint64_t now) {
    if ((config_.allowed || config_.current_room) &&
        now - asked_ms_ >= kAskAllowedMs) {
      asked_ms_ = now;
      if (config_.allowed) {
        Allow(config_.allowed());
      }
      if (config_.current_room) {
        if (const auto room = config_.current_room();
            room && *room != config_.room) {
          MoveTo(*room);
        }
      }
    }
    // A room the player chose joins at game start. The public lobby waits for
    // the game to play System Link.
    if (!active_ && config_.room != relay::PublicLobby(config_.title_id)) {
      Activate();
    }
    if (paused_ || !active_) {
      return;
    }
    if (!gathered_ || now - gathered_ms_ >= kHostCandidatesMs) {
      gathered_ = true;
      gathered_ms_ = now;
      client_.SetHostCandidates(HostCandidates(socket_.port()), now, out_);
    }
    DrainLobby(now);
    if (lobby_ && client_.LobbyStale(now)) {
      XELOGW("System Link: the lobby stopped answering");
      CloseLobby();
      client_.LobbyClosed(now);
    }
    // Only the lobby needs the internet: peers on the LAN keep their traffic.
    const bool network = NetworkPathAvailable();
    if (network != had_network_) {
      had_network_ = network;
      if (!network && lobby_) {
        CloseLobby();
        client_.LobbyClosed(now);
      } else if (network) {
        client_.NetworkReturned();
      }
    }
    if (network && !lobby_ && factory_ && client_.WantsLobby(now)) {
      OpenLobby(now);
    }
    if (auto server = client_.WantedIceServer(now)) {
      LookUp(*server);
    }
    client_.Tick(now, out_);
    CheckNat(now);
    Flush();
    ReportChanges();
  }

  // Once per network, as soon as the lobby names its STUN servers: from this
  // socket, as the other players see it, before there's anything to play.
  void CheckNat(uint64_t now) {
    if (nat_check_) {
      nat_check_->Tick(now, out_.datagrams);
      if (nat_check_->done()) {
        FinishNatCheck();
      }
      return;
    }
    const auto& ice = client_.ice();
    if (nat_asked_ || !ice) {
      return;
    }
    nat_asked_ = true;
    nat_network_ = relay::NetworkKey();
    if (config_.nat_override) {
      SetNat(*config_.nat_override);
      return;
    }
    if (NatType known = relay::RememberedNat(nat_network_);
        known != NatType::kUnknown) {
      SetNat(known);
      return;
    }
    std::vector<std::string> names(ice->stun.begin(), ice->stun.end());
    if (names.empty()) {
      names = ice->turn;
    }
    names.resize(std::min<size_t>(names.size(), 2));
    nat_resolved_.assign(names.size(), {});
    nat_lookups_ = names.size();
    for (size_t i = 0; i < names.size(); ++i) {
      std::string host;
      uint16_t port = kStunPort;
      if (!relay::ParseServer(names[i], kStunPort, host, port)) {
        --nat_lookups_;
        continue;
      }
      socket_.Resolve(
          host, port,
          [this, i](std::vector<Endpoint> endpoints, std::string /* error */) {
            DropPrivateServers(endpoints);
            nat_resolved_[i] = std::move(endpoints);
            if (--nat_lookups_ == 0) {
              StartNatCheck();
            }
          });
    }
    if (nat_lookups_ == 0) {
      StartNatCheck();
    }
  }

  void StartNatCheck() {
    if (nat_check_ || nat_resolved_.empty()) {
      return;
    }
    std::vector<Endpoint> resolved;
    for (const auto& endpoints : nat_resolved_) {
      resolved.insert(resolved.end(), endpoints.begin(), endpoints.end());
    }
    nat_resolved_.clear();
    auto servers = relay::PickNatServers(resolved);
    if (servers.size() < 2) {
      XELOGW("System Link: no STUN server to check the network with");
      return;
    }
    nat_check_.emplace(std::move(servers), HostCandidates(socket_.port()));
    nat_check_->Tick(Now(), out_.datagrams);
    Flush();
  }

  void FinishNatCheck() {
    const NatType nat = nat_check_->result();
    nat_check_.reset();
    relay::RememberNat(nat_network_, nat);
    SetNat(nat);
  }

  void SetNat(NatType nat) {
    if (nat != nat_) {
      XELOGI("System Link: this network's NAT is {}", relay::ToString(nat));
    }
    nat_ = nat;
    client_.SetNat(nat);
    ReportChanges();
  }

  void OpenLobby(uint64_t now) {
    auto inbox = std::make_shared<LobbyInbox>();
    LobbySocket::Events events;
    events.opened = [inbox] { inbox->Push(LobbyInbox::Kind::kOpened); };
    events.message = [inbox](std::string text) {
      inbox->Push(LobbyInbox::Kind::kMessage, std::move(text));
    };
    events.closed = [inbox](std::string reason) {
      inbox->Push(LobbyInbox::Kind::kClosed, std::move(reason));
    };
    client_.LobbyConnecting(now);
    inbox_ = inbox;
    lobby_ = factory_(
        fmt::format("{}/v1/{}", config_.lobby, relay::RoomPath(config_.room)),
        std::move(events));
    if (!lobby_) {
      inbox_.reset();
      client_.LobbyClosed(now);
    }
  }

  // Destroying the socket ends its events.
  void CloseLobby() {
    lobby_.reset();
    inbox_.reset();
  }

  void DrainLobby(uint64_t now) {
    if (!inbox_) {
      return;
    }
    for (auto& event : inbox_->Take()) {
      switch (event.kind) {
        case LobbyInbox::Kind::kOpened:
          client_.LobbyOpened(now, out_);
          break;
        case LobbyInbox::Kind::kMessage:
          client_.LobbyMessage(event.text, now, out_);
          break;
        case LobbyInbox::Kind::kClosed:
          // The reason never has the address, which can name a private room.
          if (event.text != last_close_) {
            XELOGW("System Link: the lobby connection closed: {}", event.text);
            last_close_ = event.text;
          }
          CloseLobby();
          client_.LobbyClosed(now, event.text.starts_with("http "));
          return;
      }
    }
  }

  // The lobby names the STUN and TURN servers; none on this device, its
  // network or a reserved range is used (a test's local lobby excepted).
  void DropPrivateServers(std::vector<Endpoint>& endpoints) const {
    if (!config_.probe_local_candidates) {
      std::erase_if(endpoints, [](const Endpoint& endpoint) {
        return !relay::IsPublicServer(endpoint);
      });
    }
  }

  void LookUp(const std::string& server) {
    std::string host;
    uint16_t port = kStunPort;
    if (!relay::ParseServer(server, kStunPort, host, port)) {
      XELOGW("System Link: can't read the server address \"{}\"", server);
      client_.UseIceServer(std::nullopt, Now(), out_);
      return;
    }
    client_.IceLookupStarted();
    socket_.Resolve(
        host, port,
        [this, host](std::vector<Endpoint> endpoints, std::string error) {
          DropPrivateServers(endpoints);
          if (endpoints.empty() && error.empty()) {
            error = "not a public address";
          }
          std::optional<Endpoint> server;
          if (!endpoints.empty()) {
            // Each lookup after a failure tries the next address.
            server = endpoints[lookups_++ % endpoints.size()];
          } else {
            XELOGW("System Link: can't look up {}: {}", host, error);
          }
          client_.UseIceServer(server, Now(), out_);
          Flush();
        });
  }

  void Flush() {
    for (const Datagram& datagram : out_.datagrams) {
      if (!socket_.SendTo(datagram.data, datagram.to)) {
        XELOGE("System Link: the socket failed");
        healthy_ = false;
        break;
      }
    }
    out_.datagrams.clear();
    for (std::string& text : out_.lobby) {
      if (lobby_) {
        lobby_->Send(std::move(text));
      }
    }
    out_.lobby.clear();
    if (out_.lobby_ping && lobby_ && !lobby_->Ping()) {
      lobby_->Send("ping");
    }
    out_.lobby_ping = false;
  }

  // Online features can be turned off while a game runs: the transport
  // leaves the room and stays quiet until they're back on.
  void Allow(bool allowed) {
    if (allowed == !paused_) {
      return;
    }
    paused_ = !allowed;
    if (paused_) {
      XELOGI("System Link: no longer allowed online, leaving the room");
      CloseLobby();
      client_.Leave(out_);
      Flush();
    } else {
      XELOGI("System Link: allowed online again");
      client_ = relay::RelayClient(mac_, config_);
      client_.SetNat(nat_);
      gathered_ = false;
    }
    ReportChanges();
  }

  // The player chose another room while the game runs: the transport leaves
  // this one and joins that one, as if it had just opened there.
  void MoveTo(const relay::Room& room) {
    XELOGI("System Link: changing rooms");
    if (!paused_) {
      CloseLobby();
      client_.Leave(out_);
      Flush();
    }
    config_.room = room;
    client_ = relay::RelayClient(mac_, config_);
    client_.SetNat(nat_);
    gathered_ = false;
    ReportChanges();
  }

  void ReportChanges() {
    if (!active_) {
      return;
    }
    RelayStatus status;
    if (paused_) {
      status.state = RelayStatus::State::kPaused;
    } else {
      status = client_.status();
    }
    if (status != reported_) {
      reported_ = status;
      if (config_.report_status) {
        Report(status);
      }
    }
  }

  Mac mac_;
  relay::RelayConfig config_;
  relay::RelayClient client_;
  relay::RelaySocket socket_;
  LobbySocketFactory factory_;
  bool had_network_ = true;
  std::unique_ptr<LobbySocket> lobby_;
  std::shared_ptr<LobbyInbox> inbox_;
  std::string last_close_;
  relay::Outbox out_;
  size_t lookups_ = 0;
  bool gathered_ = false;
  uint64_t gathered_ms_ = 0;
  uint64_t asked_ms_ = 0;
  bool paused_ = false;
  std::optional<RelayStatus> reported_;
  std::optional<relay::NatCheck> nat_check_;
  bool nat_asked_ = false;
  std::string nat_network_;
  std::vector<std::vector<Endpoint>> nat_resolved_;
  size_t nat_lookups_ = 0;
  NatType nat_ = NatType::kUnknown;
  std::atomic<bool> healthy_{true};
  // Set by the first System Link activity, from any thread.
  std::atomic<bool> active_{false};
};

}  // namespace

std::unique_ptr<PacketTransport> OpenRelayTransport(
    const Mac& mac, const relay::RelayConfig& config, std::string& error) {
  auto transport = std::make_unique<RelayTransport>(mac, config);
  if (!transport->Open(error)) {
    return nullptr;
  }
  return transport;
}

void SetRelayStatusListener(std::function<void(const RelayStatus&)> listener) {
  std::lock_guard lock(listener_mutex);
  status_listener = std::move(listener);
}

void SetLobbySocketFactory(LobbySocketFactory factory) {
  std::lock_guard lock(factory_mutex);
  lobby_factory = std::move(factory);
}

LobbySocketFactory GetLobbySocketFactory() {
  std::lock_guard lock(factory_mutex);
  return lobby_factory;
}

}  // namespace xe::net::secure
