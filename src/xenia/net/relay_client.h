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

#include "xenia/net/nat_check.h"
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
  // Probes loopback and link-local candidates too, for tests on one machine.
  bool probe_local_candidates = false;
  // The app's ID for counting open apps (16 hex digits, as in
  // GET /v1/online?here=), sent with the join so the room counts the app while
  // it plays. Empty sends none.
  std::string app_id;
  // Asked about once a second when set. False leaves the room, and the
  // transport sends nothing until it's true again.
  std::function<bool()> allowed;
  // Asked about once a second when set: a room other than |room| leaves it and
  // joins that one. Nothing keeps the transport where it is.
  std::function<std::optional<Room>()> current_room;
  // Reported as this network's NAT type in place of checking it, for testing
  // what the player is told.
  std::optional<NatType> nat_override;
};

struct RelayStatus {
  enum class State : uint8_t {
    kConnecting,
    kOnline,
    // The lobby can't be reached. Still trying.
    kOffline,
    kRejected,
    // Turned away as a duplicate soon after this player was in the room, so the
    // lobby most likely still holds its old connection. Still trying.
    kReconnecting,
    // Left the room because the app doesn't allow it online any more, and
    // waits to be allowed again.
    kPaused,
    // System Link closed.
    kStopped,
  };
  State state = State::kConnecting;
  // The other players in the room, and how many of them are reached directly
  // and how many through the relay.
  size_t players = 0;
  size_t direct = 0;
  size_t relayed = 0;
  // Players reached neither way for kUnreachableMs or longer.
  size_t unreachable = 0;
  // What this instance's network does to direct connections, once checked.
  NatType nat = NatType::kUnknown;
  // The lobby names a TURN server, so players may be reached through it.
  bool turn = false;
  Reject reject = Reject::kFull;
  // Another player runs another title or another version of it.
  bool version_mismatch = false;
  // The round trip to the slowest player reached directly, in ms, rounded to
  // 5 ms above 10 so it doesn't change with every probe. 0 until measured.
  uint32_t ping_ms = 0;
  // The room counts this app as open (see RelayConfig::app_id), so it needn't
  // check in with the lobby while it's in the room.
  bool app_counted = false;
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
  // A WebSocket ping for the lobby, or the text "ping" where there's none.
  bool lobby_ping = false;
};

// One instance's side of a room, without sockets or a clock. The lobby names
// the other players and where they may be reached, frames go straight to the
// ones that answered a probe, and through this instance's TURN allocation to
// the others' allocations otherwise.
// A routable unicast address: where a STUN or TURN server the lobby names may
// be. Loopback, private, link-local and reserved ones are not.
bool IsPublicServer(const Endpoint& endpoint);

class RelayClient {
 public:
  RelayClient(const Mac& mac, const RelayConfig& config);

  // Whether to open a connection to the lobby now.
  bool WantsLobby(uint64_t now_ms) const;
  void LobbyConnecting(uint64_t now_ms);
  void LobbyOpened(uint64_t now_ms, Outbox& out);
  void LobbyMessage(std::string_view text, uint64_t now_ms, Outbox& out);
  // |refused|: the lobby answered with an HTTP error instead of the connection,
  // as it does while it's overloaded or out of its daily quota.
  void LobbyClosed(uint64_t now_ms, bool refused = false);
  // An open lobby connection that has gone quiet, to be closed.
  bool LobbyStale(uint64_t now_ms) const;

  // The STUN or TURN server to look up, as "host:port", when one is needed.
  std::optional<std::string> WantedIceServer(uint64_t now_ms) const;
  void IceLookupStarted() { looking_up_ice_ = true; }
  // Its address, or none when the lookup failed.
  void UseIceServer(std::optional<Endpoint> server, uint64_t now_ms,
                    Outbox& out);

  // The addresses this instance's socket has on the host's own networks. A
  // change means the network changed (Wi-Fi to cellular, say): direct
  // paths, the public address and the lobby connection are found again at
  // once, so play carries on before the game times the other players out.
  void SetHostCandidates(std::vector<Endpoint> candidates, uint64_t now_ms,
                         Outbox& out);

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
  // The STUN and TURN servers the lobby named, once it has.
  const std::optional<IceServers>& ice() const { return ice_; }
  // Reported in the status from now on.
  void SetNat(NatType nat);

  // How long a player goes without a path before it counts as unreachable.
  static constexpr uint64_t kUnreachableMs = 10000;

  // A player as the lobby lists it.
  struct Listed {
    Mac mac{};
    uint32_t title_id = 0;
    uint32_t title_version = 0;
    std::vector<Endpoint> candidates;
    std::optional<Endpoint> relay;
    // Which network the lobby saw the player on, when it says.
    std::optional<uint64_t> net;
    bool operator==(const Listed&) const = default;
  };

 private:
  // A WebSocket ping keeps the connection up and shows it's alive, two in the
  // time this client waits before calling it lost. They cost the lobby
  // nothing, but the room doesn't see them, so it's told the player is there
  // with the text "ping" every kLobbyTextPingMs, which counts against its
  // daily requests.
  static constexpr uint64_t kLobbyPingMs = 30000;
  static constexpr uint64_t kLobbyTextPingMs = 60000;
  static constexpr uint64_t kLobbyLostMs = 60000;
  // A player turned away, or refused, tries again after this long, twice as
  // long each time in a row, up to kMaxRefusedBackoffMs, so a lobby at its
  // limits isn't kept there by retries.
  static constexpr uint64_t kRejectRetryMs = 10000;
  static constexpr uint64_t kMaxRefusedBackoffMs = 300000;
  // The lobby drops a connection that went quiet after this long, so a
  // duplicate this soon after leaving is this player's own.
  static constexpr uint64_t kRejoinGraceMs = 70000;
  static constexpr uint64_t kRejoinRetryMs = 5000;
  // A joined lobby connection is welcomed or rejected within this long.
  static constexpr uint64_t kWelcomeMs = 15000;
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
    // The direct path that stopped answering. Frames still go there while
    // nothing else reaches the peer: with no relay, a path that only dropped
    // a few answers is better than none, and the peer may still hear them.
    std::optional<Endpoint> quiet;
    uint64_t heard_ms = 0;
    uint64_t sent_ms = 0;
    uint64_t next_probe_ms = 0;
    uint32_t probes = 0;
    // When the latest keepalive left for the direct path, until it is
    // answered, and the round trip the answers average to.
    uint64_t keepalive_sent_ms = 0;
    uint32_t rtt_ms = 0;
    // What the latest probes carry, and the ones before, which a reply may
    // still answer.
    std::array<uint64_t, 2> nonces{};
    // Since when neither a direct path nor the relay reaches it, if neither
    // does.
    std::optional<uint64_t> pathless_since_ms;
  };

  void OnWelcome(uint64_t room_secret, std::optional<IceServers> ice,
                 uint64_t now_ms);
  void OnIce(IceServers ice, uint64_t now_ms);
  // Whether the next STUN or TURN client should hold a TURN allocation.
  bool WantsTurn() const;
  void RenewIce(uint64_t now_ms, Outbox& out);
  void OnPeers(std::vector<Listed> listed, uint64_t now_ms, Outbox& out);
  void OnNetworkChanged(uint64_t now_ms, Outbox& out);
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
  // Whether the peer's local addresses are worth probing: the lobby saw both
  // on one network, or without its word, both show one public address.
  bool SameNetwork(const Peer& peer) const;
  // A duplicate join that is most likely this player's own old connection.
  bool Reconnecting(uint64_t now_ms) const;
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
  // Connections in a row that were turned away or refused.
  uint32_t refusals_ = 0;
  std::optional<uint64_t> down_since_ms_;
  uint64_t lobby_heard_ms_ = 0;
  uint64_t join_sent_ms_ = 0;
  // The lobby answered this connection's join.
  bool answered_ = false;
  // When this player was last in the room, if it was.
  std::optional<uint64_t> last_joined_ms_;
  // The lobby's name for this player's network, when it gives one.
  std::optional<uint64_t> net_;
  uint64_t next_lobby_ping_ms_ = 0;
  uint64_t next_lobby_text_ping_ms_ = 0;
  bool joined_ = false;
  // The welcome said the room counts the app.
  bool app_counted_ = false;
  bool rejected_ = false;
  bool left_ = false;
  Reject reject_ = Reject::kFull;
  uint64_t room_secret_ = 0;
  // From the welcome: shown on the next join, it lets the lobby replace this
  // player's old connection from another network.
  std::string resume_;
  // The network changed under the open lobby connection, which is closed and
  // opened again straight away.
  bool restart_lobby_ = false;

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
  bool host_candidates_read_ = false;
  std::vector<Endpoint> sent_candidates_;
  std::optional<Endpoint> sent_relay_;

  uint16_t next_frame_id_ = 0;
  std::map<Mac, Peer> peers_;
  Reassembler reassembler_;
  NatType nat_ = NatType::kUnknown;
  RelayStatus status_;
};

}  // namespace xe::net::relay

#endif  // XENIA_NET_RELAY_CLIENT_H_
