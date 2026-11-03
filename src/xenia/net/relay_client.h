/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_RELAY_CLIENT_H_
#define XENIA_NET_RELAY_CLIENT_H_

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/net/relay_protocol.h"
#include "xenia/net/turn_client.h"

namespace xe::net::relay {

struct RelayConfig {
  // The lobby's wss:// or ws:// address, without the room's path.
  std::string lobby = std::string(kDefaultLobby);
  Room room{};
  uint32_t title_id = 0;
  uint32_t title_version = 0;
  // Off sends every frame through the TURN relay.
  bool direct_paths = true;
  // Asked about once a second when set. False leaves the room, and the
  // transport sends nothing until it's true again.
  std::function<bool()> allowed;
  // Asked about once a second when set: a room other than |room| leaves it and
  // joins that one. Nothing keeps the transport where it is.
  std::function<std::optional<Room>()> current_room;
};

struct RelayStatus {
  enum class State : uint8_t {
    kConnecting,
    kOnline,
    // The lobby can't be reached. Still trying.
    kOffline,
    kRejected,
    // System Link closed.
    kStopped,
  };
  State state = State::kConnecting;
  // The other players in the room, and how many of them are reached directly
  // and how many through the relay.
  size_t players = 0;
  size_t direct = 0;
  size_t relayed = 0;
  Reject reject = Reject::kFull;
  // Another player runs another title or another version of it.
  bool version_mismatch = false;
  bool operator==(const RelayStatus&) const = default;
};

// The STUN and TURN servers the lobby names, as "host:port".
struct IceServers {
  std::vector<std::string> stun;
  std::vector<std::string> turn;
  std::string username;
  std::string credential;
  // How long the credentials last, or 0 when the lobby didn't say.
  uint32_t ttl_s = 0;
};

// What a RelayClient wants sent: datagrams, and text for the lobby.
struct Outbox {
  std::vector<Datagram> datagrams;
  std::vector<std::string> lobby;
};

// One instance's side of a room, without sockets or a clock. The lobby names
// the other players and where they may be reached, frames go straight to the
// ones that answered a probe, and through this instance's TURN allocation to
// the others' allocations otherwise.
class RelayClient {
 public:
  RelayClient(const Mac& mac, const RelayConfig& config);

  // Whether to open a connection to the lobby now.
  bool WantsLobby(uint64_t now_ms) const;
  void LobbyConnecting(uint64_t now_ms);
  void LobbyOpened(uint64_t now_ms, Outbox& out);
  void LobbyMessage(std::string_view text, uint64_t now_ms, Outbox& out);
  void LobbyClosed(uint64_t now_ms);
  // An open lobby connection that has gone quiet, to be closed.
  bool LobbyStale(uint64_t now_ms) const;

  // The STUN or TURN server to look up, as "host:port", when one is needed.
  std::optional<std::string> WantedIceServer(uint64_t now_ms) const;
  void IceLookupStarted() { looking_up_ice_ = true; }
  // Its address, or none when the lookup failed.
  void UseIceServer(std::optional<Endpoint> server, uint64_t now_ms,
                    Outbox& out);

  // The addresses this instance's socket has on the host's own networks.
  void SetHostCandidates(std::vector<Endpoint> candidates, Outbox& out);

  // False when the frame can't go anywhere yet.
  bool Send(std::span<const uint8_t> ethernet, uint64_t now_ms, Outbox& out);
  // The whole Ethernet frame for this instance that |data| completes.
  std::optional<std::vector<uint8_t>> Receive(std::span<const uint8_t> data,
                                              const Endpoint& from,
                                              uint64_t now_ms, Outbox& out);
  // Pings, probes and refreshes when they are due. Call every few tens of ms.
  void Tick(uint64_t now_ms, Outbox& out);
  // Leaves the room for good. Closing the lobby connection is up to the
  // caller.
  void Leave(Outbox& out);

  const RelayStatus& status() const { return status_; }

  // A player as the lobby lists it.
  struct Listed {
    Mac mac{};
    uint32_t title_id = 0;
    uint32_t title_version = 0;
    std::vector<Endpoint> candidates;
    std::optional<Endpoint> relay;
    bool operator==(const Listed&) const = default;
  };

 private:
  static constexpr uint64_t kLobbyPingMs = 20000;
  static constexpr uint64_t kLobbyLostMs = 60000;
  static constexpr uint64_t kRejectRetryMs = 10000;
  static constexpr uint64_t kMaxBackoffMs = 15000;
  static constexpr uint64_t kOfflineMs = 10000;
  static constexpr uint64_t kIceRetryMs = 30000;
  // TURN credentials are renewed this long before they expire.
  static constexpr uint64_t kIceRenewMs = 600000;
  // Longer than the lobby's minute between asks.
  static constexpr uint64_t kIceAskMs = 90000;
  // After the relay turned the credentials down.
  static constexpr uint64_t kRefusedAskMs = 300000;
  static constexpr uint64_t kReallocateMs = 1000;
  static constexpr size_t kMaxCandidates = 8;

  struct Peer {
    Listed listed;
    // Where the peer answered a probe from.
    std::optional<Endpoint> direct;
    // Where its probes come from, when that isn't one of its candidates.
    std::optional<Endpoint> reflexive;
    uint64_t heard_ms = 0;
    uint64_t sent_ms = 0;
    uint64_t next_probe_ms = 0;
    uint32_t probes = 0;
    // What the latest probes carry, and the ones before, which a reply may
    // still answer.
    std::array<uint64_t, 2> nonces{};
  };

  void OnWelcome(uint64_t room_secret, std::optional<IceServers> ice,
                 uint64_t now_ms);
  void OnIce(IceServers ice, uint64_t now_ms);
  // Whether the next STUN or TURN client should hold a TURN allocation.
  bool WantsTurn() const;
  void RenewIce(uint64_t now_ms, Outbox& out);
  void OnPeers(std::vector<Listed> listed, uint64_t now_ms, Outbox& out);
  // Lets the other players' relayed addresses reach this one's allocation.
  void PermitRelays(uint64_t now_ms, Outbox& out);
  std::optional<std::vector<uint8_t>> OnPeerDatagram(
      std::span<const uint8_t> data, const Endpoint& from, bool relayed,
      uint64_t now_ms, Outbox& out);
  void OnProbe(const Probe& probe, const Endpoint& from, uint64_t now_ms,
               Outbox& out);
  bool SendTo(Peer& peer, const std::vector<std::vector<uint8_t>>& fragments,
              Outbox& out);
  void TickPeers(uint64_t now_ms, Outbox& out);
  // Starts a round of probes with a new nonce.
  static void NewProbeRound(Peer& peer);
  void SendProbe(const Peer& peer, const Endpoint& to, Outbox& out);
  // Whether |from| is one of the addresses |peer| is probed at.
  static bool Probed(const Peer& peer, const Endpoint& from);
  // Whether the peer is behind the public address this instance is.
  bool SharesPublicAddress(const Peer& peer) const;
  std::vector<Endpoint> Candidates() const;
  std::optional<Endpoint> RelayAddress() const;
  std::string Presence(bool join) const;
  void UpdatePresence(Outbox& out);
  void UpdateStatus(uint64_t now_ms);
  std::string DescribeRoom() const;

  Mac mac_;
  RelayConfig config_;

  enum class Lobby { kClosed, kConnecting, kOpen };
  Lobby lobby_ = Lobby::kClosed;
  uint64_t next_connect_ms_ = 0;
  uint32_t failures_ = 0;
  std::optional<uint64_t> down_since_ms_;
  uint64_t lobby_heard_ms_ = 0;
  uint64_t next_lobby_ping_ms_ = 0;
  bool joined_ = false;
  bool rejected_ = false;
  bool left_ = false;
  Reject reject_ = Reject::kFull;
  uint64_t room_secret_ = 0;

  std::optional<IceServers> ice_;
  std::unique_ptr<TurnClient> turn_;
  // |turn_| holds an allocation rather than only learning the mapped address.
  bool using_turn_ = false;
  // The last allocation failed, so the next client only uses STUN.
  bool turn_refused_ = false;
  bool looking_up_ice_ = false;
  uint64_t next_ice_ms_ = 0;
  uint64_t ice_expires_ms_ = 0;
  // When the credentials |turn_| uses expire, or 0.
  uint64_t turn_expires_ms_ = 0;
  uint64_t next_ice_ask_ms_ = 0;

  std::vector<Endpoint> host_candidates_;
  std::vector<Endpoint> sent_candidates_;
  std::optional<Endpoint> sent_relay_;

  uint16_t next_frame_id_ = 0;
  std::map<Mac, Peer> peers_;
  Reassembler reassembler_;
  RelayStatus status_;
};

}  // namespace xe::net::relay

#endif  // XENIA_NET_RELAY_CLIENT_H_
