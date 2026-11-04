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
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "xenia/base/platform.h"
#include "xenia/net/packet_transport.h"
#include "xenia/net/relay_client.h"
#include "xenia/net/relay_protocol.h"
#include "xenia/net/relay_socket.h"
#include "xenia/net/stun.h"
#include "xenia/net/turn_client.h"

#if XE_PLATFORM_APPLE
#include "xenia/net/lobby_socket_apple.h"
#endif

#include "third_party/catch/include/catch.hpp"

namespace xe::net::relay::test {
namespace {

using stun::Attribute;
using stun::Class;
using stun::Method;

constexpr Mac kMacA{0x00, 0x1D, 0xD8, 1, 2, 3},
    kMacB{0x00, 0x1D, 0xD8, 1, 2, 4}, kMacC{0x00, 0x1D, 0xD8, 1, 2, 5};
constexpr Mac kBroadcast{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint32_t kTitle = 0x4D5307E6;
constexpr uint8_t kFrameType = 1;
constexpr std::string_view kPassword = "secret";

Endpoint V4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
  Endpoint endpoint;
  endpoint.address[0] = a;
  endpoint.address[1] = b;
  endpoint.address[2] = c;
  endpoint.address[3] = d;
  endpoint.port = port;
  return endpoint;
}

bool SameAddress(const Endpoint& a, const Endpoint& b) {
  return a.v6 == b.v6 && a.address == b.address;
}

std::vector<uint8_t> MakeFrame(const Mac& source, const Mac& destination,
                               size_t size, uint8_t fill) {
  std::vector<uint8_t> frame(size);
  for (size_t i = 0; i < size; ++i) {
    frame[i] = uint8_t(fill + i);
  }
  std::copy(destination.begin(), destination.end(), frame.begin());
  std::copy(source.begin(), source.end(), frame.begin() + 6);
  return frame;
}

bool IsFrame(std::span<const uint8_t> datagram) {
  return datagram.size() > 5 && datagram[5] == kFrameType &&
         std::equal(datagram.begin(), datagram.begin() + 4, "XESR");
}

RelayConfig Config(uint32_t version = 1, Room room = PublicLobby(kTitle)) {
  RelayConfig config;
  config.room = room;
  config.title_id = kTitle;
  config.title_version = version;
  return config;
}

std::string Hex(const Mac& mac) {
  return fmt::format("{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", mac[0], mac[1],
                     mac[2], mac[3], mac[4], mac[5]);
}

std::string_view String(const rapidjson::Value& object, const char* name) {
  auto member = object.FindMember(name);
  if (member == object.MemberEnd() || !member->value.IsString()) {
    return {};
  }
  return {member->value.GetString(), member->value.GetStringLength()};
}

// Every XOR-PEER-ADDRESS in a request, where stun::Read keeps the last.
std::vector<Endpoint> PeerAddresses(std::span<const uint8_t> datagram) {
  std::vector<Endpoint> peers;
  for (size_t offset = stun::kHeaderSize; offset + 4 <= datagram.size();) {
    const size_t size = datagram[offset + 2] << 8 | datagram[offset + 3];
    const size_t padded = (size + 3) & ~size_t(3);
    if (offset + 4 + padded > datagram.size()) {
      break;
    }
    if ((datagram[offset] << 8 | datagram[offset + 1]) ==
        uint16_t(Attribute::kXorPeerAddress)) {
      std::vector<uint8_t> one(datagram.begin(),
                               datagram.begin() + stun::kHeaderSize);
      one.insert(one.end(), datagram.begin() + offset,
                 datagram.begin() + offset + 4 + padded);
      one[2] = uint8_t((one.size() - stun::kHeaderSize) >> 8);
      one[3] = uint8_t(one.size() - stun::kHeaderSize);
      if (auto read = stun::Read(one); read && read->peer) {
        peers.push_back(*read->peer);
      }
    }
    offset += 4 + padded;
  }
  return peers;
}

// A datagram and the address it comes from.
struct Sent {
  Endpoint from;
  Datagram datagram;
  // Already held back for the trip between players.
  bool held = false;
};

// A TURN server for UDP with one realm, relaying between its allocations and
// the addresses outside it. Relayed addresses each get their own IPv4 address.
class FakeTurn {
 public:
  explicit FakeTurn(const Endpoint& at) : at_(at) {}

  const Endpoint& at() const { return at_; }
  // Whether |endpoint| is an allocation's relayed address.
  bool Relays(const Endpoint& endpoint) const {
    return std::any_of(
        allocations_.begin(), allocations_.end(),
        [&](const auto& entry) { return entry.second.relayed == endpoint; });
  }
  size_t allocations() const { return allocations_.size(); }
  // The usernames the allocations were made with.
  std::set<std::string> usernames() const {
    std::set<std::string> names;
    for (const auto& [client, allocation] : allocations_) {
      names.insert(allocation.username);
    }
    return names;
  }

  // Passwords by username.
  std::map<std::string, std::string> users;
  // Nothing is answered.
  bool silent = false;
  // The next authenticated request is told its nonce is stale.
  bool stale_nonce = false;
  // Frames carried from one allocation to another.
  size_t relayed_frames = 0;

  // A datagram for the server from |from|.
  void Receive(std::span<const uint8_t> data, const Endpoint& from,
               std::vector<Sent>& out) {
    auto message = stun::Read(data);
    if (!message || silent) {
      return;
    }
    auto allocation = allocations_.find(from);
    if (message->kind == Class::kIndication) {
      if (message->method == Method::kSend && message->peer &&
          allocation != allocations_.end() &&
          allocation->second.Permits(*message->peer)) {
        out.push_back(
            {allocation->second.relayed, {*message->peer, message->data}});
      }
      return;
    }
    if (message->kind != Class::kRequest) {
      return;
    }
    const auto& id = message->transaction;
    if (message->method == Method::kBinding) {
      out.push_back({at_,
                     {from, stun::Writer(Method::kBinding, Class::kSuccess, id)
                                .AddAddress(Attribute::kXorMappedAddress, from)
                                .Finish()}});
      return;
    }
    auto user = users.find(message->username);
    const auto key = user == users.end()
                         ? std::array<uint8_t, 16>{}
                         : stun::LongTermKey(user->first, kRealm, user->second);
    if (user == users.end() || !message->integrity ||
        !stun::CheckIntegrity(data, *message, key)) {
      out.push_back({at_, {from, Error(*message, 401, {}, true)}});
      return;
    }
    if (stale_nonce || message->nonce != nonce_) {
      stale_nonce = false;
      nonce_ = fmt::format("nonce{}", ++nonces_);
      out.push_back({at_, {from, Error(*message, 438, key, true)}});
      return;
    }
    switch (message->method) {
      case Method::kAllocate: {
        if (allocation != allocations_.end()) {
          out.push_back({at_, {from, Error(*message, 437, key)}});
          return;
        }
        Allocation made;
        made.username = user->first;
        made.relayed = V4(203, 0, 113, uint8_t(100 + ++relays_), 49152);
        allocations_[from] = made;
        out.push_back(
            {at_,
             {from, stun::Writer(Method::kAllocate, Class::kSuccess, id)
                        .AddAddress(Attribute::kXorRelayedAddress, made.relayed)
                        .AddAddress(Attribute::kXorMappedAddress, from)
                        .AddUint32(Attribute::kLifetime, 600)
                        .Finish(key)}});
        return;
      }
      case Method::kRefresh:
        if (allocation == allocations_.end() ||
            allocation->second.username != user->first) {
          out.push_back({at_, {from, Error(*message, 437, key)}});
          return;
        }
        if (message->lifetime == 0u) {
          allocations_.erase(allocation);
        }
        out.push_back(
            {at_,
             {from, stun::Writer(Method::kRefresh, Class::kSuccess, id)
                        .AddUint32(Attribute::kLifetime,
                                   message->lifetime.value_or(600))
                        .Finish(key)}});
        return;
      case Method::kCreatePermission:
        if (allocation == allocations_.end()) {
          out.push_back({at_, {from, Error(*message, 437, key)}});
          return;
        }
        for (const Endpoint& peer : PeerAddresses(data)) {
          allocation->second.permitted.push_back(peer);
        }
        out.push_back(
            {at_,
             {from, stun::Writer(Method::kCreatePermission, Class::kSuccess, id)
                        .Finish(key)}});
        return;
      default:
        out.push_back({at_, {from, Error(*message, 400, key)}});
    }
  }

  // A datagram from |from| to the relayed address |to|.
  void ReceiveRelayed(std::span<const uint8_t> data, const Endpoint& from,
                      const Endpoint& to, std::vector<Sent>& out) {
    if (silent) {
      return;
    }
    for (const auto& [client, allocation] : allocations_) {
      if (allocation.relayed == to && allocation.Permits(from)) {
        relayed_frames += IsFrame(data);
        out.push_back(
            {at_,
             {client, stun::Writer(Method::kData, Class::kIndication,
                                   stun::NewTransactionId())
                          .AddAddress(Attribute::kXorPeerAddress, from)
                          .Add(Attribute::kData, data)
                          .Finish()}});
      }
    }
  }

  // Delivers what |out| sends to other allocations, and leaves the rest.
  void RelayBetweenAllocations(std::vector<Sent>& out) {
    for (size_t i = 0; i < out.size();) {
      if (!Relays(out[i].datagram.to)) {
        ++i;
        continue;
      }
      Sent sent = std::move(out[i]);
      out.erase(out.begin() + i);
      ReceiveRelayed(sent.datagram.data, sent.from, sent.datagram.to, out);
    }
  }

 private:
  static constexpr std::string_view kRealm = "fake.test";

  struct Allocation {
    std::string username;
    Endpoint relayed;
    std::vector<Endpoint> permitted;
    bool Permits(const Endpoint& peer) const {
      return std::any_of(
          permitted.begin(), permitted.end(),
          [&](const Endpoint& allowed) { return SameAddress(allowed, peer); });
    }
  };

  std::vector<uint8_t> Error(const stun::Message& request, uint16_t code,
                             std::span<const uint8_t> key,
                             bool with_nonce = false) {
    const uint8_t error[] = {0, 0, uint8_t(code / 100), uint8_t(code % 100)};
    stun::Writer writer(request.method, Class::kError, request.transaction);
    writer.Add(Attribute::kErrorCode, error);
    if (with_nonce) {
      writer.Add(Attribute::kRealm, kRealm).Add(Attribute::kNonce, nonce_);
    }
    return writer.Finish(key);
  }

  Endpoint at_;
  std::string nonce_ = "nonce0";
  uint32_t nonces_ = 0;
  uint32_t relays_ = 0;
  std::map<Endpoint, Allocation> allocations_;
};

// The Worker's rooms (tools/systemlink-lobby/PROTOCOL.md) without its
// timeouts. Used from one thread.
class FakeLobby {
 public:
  struct Socket {
    std::function<void(std::string)> message;
    std::function<void(std::string)> closed;
  };

  size_t capacity = 16;
  // The protocol version it speaks.
  uint32_t version = 1;
  bool up = true;
  // The "ice" object for each welcome and ice reply.
  std::function<std::string()> ice = [] {
    return std::string(R"({"stun":["203.0.113.1:3478"]})");
  };
  // Everything clients sent, in order.
  std::vector<std::string> received;
  size_t connects = 0;
  // Appended to every player's candidates, as a forged presence could, and a
  // relay address that replaces every player's own.
  std::vector<std::string> extra_candidates;
  std::string forged_relay;
  // Joins are neither welcomed nor rejected.
  bool silent = false;

  // The connection's id, or none while the lobby is down.
  // |net| is the network the lobby saw the player on, if it says.
  std::optional<int> Connect(const std::string& path, Socket socket,
                             std::string net = {}) {
    ++connects;
    if (!up) {
      return std::nullopt;
    }
    const int id = next_id_++;
    connections_[id] = {path, std::move(socket)};
    connections_[id].net = std::move(net);
    return id;
  }

  void Message(int id, const std::string& text) {
    auto found = connections_.find(id);
    if (found == connections_.end()) {
      return;
    }
    received.push_back(text);
    Connection& connection = found->second;
    if (text == "ping") {
      connection.socket.message("pong");
      return;
    }
    rapidjson::Document message;
    message.Parse(text.c_str());
    if (message.HasParseError() || !message.IsObject()) {
      return Reject(id, "bad_request");
    }
    const std::string_view type = String(message, "t");
    if (type == "join") {
      if (silent) {
        return;
      }
      auto v = message.FindMember("v");
      if (connection.joined || v == message.MemberEnd() || !v->value.IsUint()) {
        return Reject(id, "bad_request");
      }
      if (v->value.GetUint() != version) {
        return Reject(id, "version");
      }
      connection.mac = String(message, "mac");
      connection.title = String(message, "title");
      connection.version = message["version"].GetUint();
      Place(connection, message);
      if (connection.path.starts_with("lobby/") &&
          connection.path.substr(6) != connection.title) {
        return Reject(id, "bad_request");
      }
      size_t players = 0;
      for (const auto& [other_id, other] : connections_) {
        if (other_id == id || !other.joined || other.path != connection.path) {
          continue;
        }
        if (other.mac == connection.mac) {
          return Reject(id, "duplicate_mac");
        }
        ++players;
      }
      if (players >= capacity) {
        return Reject(id, "full");
      }
      std::string& secret = secrets_[connection.path];
      if (!players) {
        secret = fmt::format("{:016x}", RandomId());
      }
      connection.joined = true;
      connection.socket.message(fmt::format(
          R"({{"t":"welcome","v":1,"secret":"{}",{}"ice":{}}})", secret,
          connection.net.empty()
              ? std::string()
              : fmt::format(R"("net":"{}",)", connection.net),
          ice()));
      Broadcast(connection.path);
    } else if (type == "update" && connection.joined) {
      Place(connection, message);
      Broadcast(connection.path);
    } else if (type == "ice" && connection.joined) {
      connection.socket.message(
          fmt::format(R"({{"t":"ice","ice":{}}})", ice()));
    } else {
      Reject(id, "bad_request");
    }
  }

  // The client closed the connection.
  void Close(int id) {
    auto found = connections_.find(id);
    if (found == connections_.end()) {
      return;
    }
    const std::string path = found->second.path;
    connections_.erase(found);
    Broadcast(path);
  }

  // The lobby restarts: every connection closes, and rooms start over.
  void Restart() {
    auto closing = std::exchange(connections_, {});
    secrets_.clear();
    for (auto& [id, connection] : closing) {
      connection.socket.closed("1001 restarting");
    }
  }

  // The players in |path|.
  size_t players(const std::string& path) const {
    return std::count_if(
        connections_.begin(), connections_.end(), [&](const auto& entry) {
          return entry.second.joined && entry.second.path == path;
        });
  }

 private:
  struct Connection {
    std::string path;
    Socket socket;
    bool joined = false;
    std::string mac;
    std::string title;
    uint32_t version = 0;
    std::vector<std::string> candidates;
    std::string relay;
    std::string net;
  };

  static void Place(Connection& connection, const rapidjson::Value& message) {
    connection.candidates.clear();
    auto candidates = message.FindMember("candidates");
    if (candidates != message.MemberEnd() && candidates->value.IsArray()) {
      for (const auto& candidate : candidates->value.GetArray()) {
        connection.candidates.emplace_back(candidate.GetString());
      }
    }
    connection.relay = String(message, "relay");
  }

  void Reject(int id, const char* reason) {
    auto found = connections_.find(id);
    Connection connection = std::move(found->second);
    connections_.erase(found);
    connection.socket.message(
        fmt::format(R"({{"t":"reject","reason":"{}"}})", reason));
    connection.socket.closed(fmt::format("4000 {}", reason));
    Broadcast(connection.path);
  }

  void Broadcast(const std::string& path) {
    for (auto& [id, to] : connections_) {
      if (!to.joined || to.path != path) {
        continue;
      }
      std::string peers;
      for (const auto& [other_id, other] : connections_) {
        if (other_id == id || !other.joined || other.path != path) {
          continue;
        }
        std::string candidates;
        auto add = [&](const std::string& candidate) {
          candidates +=
              fmt::format("{}\"{}\"", candidates.empty() ? "" : ",", candidate);
        };
        for (const std::string& candidate : other.candidates) {
          add(candidate);
        }
        for (const std::string& candidate : extra_candidates) {
          add(candidate);
        }
        const std::string& relay =
            forged_relay.empty() ? other.relay : forged_relay;
        peers += fmt::format(
            R"({}{{"mac":"{}","title":"{}","version":{},"candidates":[{}]{}{}}})",
            peers.empty() ? "" : ",", other.mac, other.title, other.version,
            candidates,
            relay.empty() ? std::string()
                          : fmt::format(R"(,"relay":"{}")", relay),
            other.net.empty() ? std::string()
                              : fmt::format(R"(,"net":"{}")", other.net));
      }
      to.socket.message(fmt::format(R"({{"t":"peers","peers":[{}]}})", peers));
    }
  }

  int next_id_ = 1;
  std::map<int, Connection> connections_;
  std::map<std::string, std::string> secrets_;
};

// TURN credentials like the Worker's, which |turn| accepts.
std::function<std::string()> IceWithTurn(FakeTurn& turn, const Endpoint& at,
                                         uint32_t ttl_s = 21600) {
  return [&turn, server = ToString(at), ttl_s, issued = 0]() mutable {
    const std::string username = fmt::format("user{}", ++issued);
    turn.users[username] = std::string(kPassword);
    return fmt::format(
        R"({{"stun":["{0}"],"turn":{{"servers":["{0}"],"username":"{1}",)"
        R"("credential":"{2}","ttl":{3}}}}})",
        server, username, kPassword, ttl_s);
  };
}

// Players behind NATs that each map a player's socket to one public address,
// the lobby, and a TURN server. Players on one network share a public IP and
// reach each other's LAN addresses. Direct paths over the internet can be cut.
class Network {
 public:
  struct Node {
    RelayClient client;
    Room room;
    Endpoint at;
    Endpoint lan;
    std::optional<int> lobby;
    std::vector<std::vector<uint8_t>> received;
  };

  Network() { lobby.ice = IceWithTurn(turn, turn.at()); }

  // |network| 0 puts the player on a network of its own.
  Node& Add(const Mac& mac, const RelayConfig& config = Config(),
            uint8_t network = 0) {
    const uint8_t n = uint8_t(nodes_.size() + 1);
    const uint8_t public_ip = network ? network : uint8_t(100 + n);
    nodes_.push_back(std::make_unique<Node>(
        Node{RelayClient(mac, config),
             config.room,
             V4(198, 51, 100, public_ip, uint16_t(50000 + n)),
             V4(192, 168, public_ip, n, uint16_t(40000 + n)),
             std::nullopt,
             {}}));
    Node& node = *nodes_.back();
    Outbox out;
    node.client.SetHostCandidates({node.lan}, out);
    Run(25);
    return node;
  }

  void Send(Node& node, std::span<const uint8_t> frame) {
    Outbox out;
    node.client.Send(frame, now_, out);
    Queue(node, out);
    Pump();
  }

  void Leave(Node& node) {
    Outbox out;
    node.client.Leave(out);
    Queue(node, out);
    if (node.lobby) {
      lobby.Close(*node.lobby);
      node.lobby.reset();
    }
    Pump();
  }

  void Run(uint64_t ms) {
    for (const uint64_t end = now_ + ms; now_ < end;) {
      now_ += 25;
      // Direct datagrams whose trip is over arrive with this step.
      while (!delayed_.empty() && delayed_.front().first <= now_) {
        queue_.push_back(std::move(delayed_.front().second));
        delayed_.pop_front();
      }
      for (auto& node : nodes_) {
        Step(*node);
      }
      Pump();
    }
  }

  // Sends |frame| from |node| every 100 ms for |ms|.
  void Stream(Node& node, std::span<const uint8_t> frame, uint64_t ms) {
    for (uint64_t sent = 0; sent < ms; sent += 100) {
      Send(node, frame);
      Run(100);
    }
  }

  FakeLobby lobby;
  FakeTurn turn{V4(203, 0, 113, 1, 3478)};
  bool block_direct = false;
  // How long a direct datagram takes from one player to another.
  uint64_t direct_delay_ms = 0;
  // The lobby names each player's network, which is its public address.
  bool send_net = false;
  // Where datagrams were sent, but not to the TURN server.
  std::set<Endpoint> sent_to;
  size_t direct_frames = 0;
  // Datagrams sent to a LAN address on another network.
  size_t foreign_lan_sends = 0;

 private:
  enum class Event { kMessage, kClosed };
  struct Pending {
    Node* node;
    Event event;
    std::string text;
  };

  void Step(Node& node) {
    Outbox out;
    if (node.lobby && node.client.LobbyStale(now_)) {
      lobby.Close(*node.lobby);
      node.lobby.reset();
      node.client.LobbyClosed(now_);
    }
    if (!node.lobby && node.client.WantsLobby(now_)) {
      node.client.LobbyConnecting(now_);
      node.lobby = lobby.Connect(
          RoomPath(node.room),
          {[this, n = &node](std::string text) {
             events_.push_back({n, Event::kMessage, std::move(text)});
           },
           [this, n = &node](std::string reason) {
             events_.push_back({n, Event::kClosed, std::move(reason)});
           }},
          send_net ? fmt::format("{:016x}", uint64_t(node.at.address[3]))
                   : std::string());
      if (node.lobby) {
        node.client.LobbyOpened(now_, out);
      } else {
        node.client.LobbyClosed(now_);
      }
    }
    if (auto server = node.client.WantedIceServer(now_)) {
      node.client.IceLookupStarted();
      node.client.UseIceServer(ParseEndpoint(*server), now_, out);
    }
    node.client.Tick(now_, out);
    Queue(node, out);
  }

  void Queue(Node& node, Outbox& out) {
    for (auto& datagram : out.datagrams) {
      queue_.push_back({node.at, std::move(datagram)});
    }
    for (std::string& text : out.lobby) {
      if (node.lobby) {
        lobby.Message(*node.lobby, text);
      }
    }
    out = {};
  }

  Node* At(const Endpoint& endpoint) {
    for (auto& node : nodes_) {
      if (node->at == endpoint || node->lan == endpoint) {
        return node.get();
      }
    }
    return nullptr;
  }

  void Pump() {
    while (!queue_.empty() || !events_.empty()) {
      while (!events_.empty()) {
        Pending pending = std::move(events_.front());
        events_.pop_front();
        Outbox out;
        if (pending.event == Event::kMessage) {
          pending.node->client.LobbyMessage(pending.text, now_, out);
        } else {
          pending.node->lobby.reset();
          pending.node->client.LobbyClosed(now_);
        }
        Queue(*pending.node, out);
      }
      if (queue_.empty()) {
        break;
      }
      Sent sent = std::move(queue_.front());
      queue_.pop_front();
      const Endpoint to = sent.datagram.to;
      if (to != turn.at() && !turn.Relays(to)) {
        sent_to.insert(to);
      }
      if (to == turn.at() || turn.Relays(to)) {
        std::vector<Sent> out;
        if (to == turn.at()) {
          turn.Receive(sent.datagram.data, sent.from, out);
        } else {
          turn.ReceiveRelayed(sent.datagram.data, sent.from, to, out);
        }
        turn.RelayBetweenAllocations(out);
        for (Sent& reply : out) {
          queue_.push_back(std::move(reply));
        }
        continue;
      }
      Node* receiver = At(to);
      Node* sender = At(sent.from);
      if (!receiver) {
        continue;
      }
      Endpoint from = sent.from;
      if (to == receiver->lan) {
        // The sender's socket is the same on its LAN, just not translated.
        if (!sender || !SameAddress(sender->at, receiver->at)) {
          ++foreign_lan_sends;
          continue;
        }
        from = sender->lan;
      } else if (sender && block_direct) {
        continue;
      }
      if (sender && direct_delay_ms && !sent.held) {
        sent.held = true;
        delayed_.push_back({now_ + direct_delay_ms, std::move(sent)});
        continue;
      }
      if (sender) {
        direct_frames += IsFrame(sent.datagram.data);
      }
      Outbox out;
      if (auto frame =
              receiver->client.Receive(sent.datagram.data, from, now_, out)) {
        receiver->received.push_back(std::move(*frame));
      }
      Queue(*receiver, out);
    }
  }

  uint64_t now_ = 100000;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::deque<Sent> queue_;
  // Direct datagrams on their way, by when they arrive.
  std::deque<std::pair<uint64_t, Sent>> delayed_;
  std::deque<Pending> events_;
};

using State = RelayStatus::State;

}  // namespace

TEST_CASE("Relay messages survive the wire", "[relay]") {
  Frame frame;
  frame.key = 0x0123456789ABCDEF;
  frame.source = kMacA;
  frame.destination = kMacB;
  frame.index = 1;
  frame.count = 2;
  frame.id = 0xBEEF;
  frame.payload = {1, 2, 3};
  auto encoded = Encode(frame);
  CHECK(IsFrame(encoded));
  auto decoded = Decode(encoded);
  REQUIRE(decoded);
  const auto& f = std::get<Frame>(*decoded);
  CHECK(f.key == frame.key);
  CHECK(f.source == kMacA);
  CHECK(f.destination == kMacB);
  CHECK(f.index == 1);
  CHECK(f.count == 2);
  CHECK(f.id == 0xBEEF);
  CHECK(f.payload == frame.payload);

  auto probe = Encode(Probe{5, kMacA, kMacB, 99, true});
  decoded = Decode(probe);
  REQUIRE(decoded);
  const auto& p = std::get<Probe>(*decoded);
  CHECK(p.room_secret == 5);
  CHECK(p.from == kMacA);
  CHECK(p.to == kMacB);
  CHECK(p.nonce == 99);
  CHECK(p.reply);

  // Truncated, padded, foreign and future datagrams are dropped.
  probe.pop_back();
  CHECK(!Decode(probe));
  auto padded = Encode(Probe{5, kMacA, kMacB, 99, false});
  padded.push_back(0);
  CHECK(!Decode(padded));
  CHECK(!Decode(std::vector<uint8_t>{'X', 'E', 'S', 'L', 1, 1}));
  auto future = Encode(Probe{5, kMacA, kMacB, 99, false});
  future[4] = kVersion + 1;
  CHECK(!Decode(future));
}

TEST_CASE("Relay addresses read and write", "[relay]") {
  Endpoint v6;
  v6.v6 = true;
  v6.address = {0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  v6.port = 4000;
  CHECK(ToString(V4(192, 0, 2, 1, 1234)) == "192.0.2.1:1234");
  CHECK(ParseEndpoint("192.0.2.1:1234") == V4(192, 0, 2, 1, 1234));
  CHECK(ParseEndpoint(ToString(v6)) == v6);
  CHECK(ParseEndpoint("[2001:db8::1]:4000") == v6);
  Endpoint zero;
  zero.v6 = true;
  zero.port = 1;
  CHECK(ParseEndpoint("[::]:1") == zero);
  CHECK(!ParseEndpoint("192.0.2.1"));
  CHECK(!ParseEndpoint("192.0.2.256:1"));
  CHECK(!ParseEndpoint("192.0.2.1:65536"));
  CHECK(!ParseEndpoint("[2001:db8::1:4000"));
  CHECK(!ParseEndpoint("[1::2::3]:4"));
  CHECK(!ParseEndpoint("host:4000"));

  std::string host;
  uint16_t port = 0;
  REQUIRE(ParseServer("turn.cloudflare.com", 3478, host, port));
  CHECK(host == "turn.cloudflare.com");
  CHECK(port == 3478);
  REQUIRE(ParseServer("192.0.2.1:4000", 3478, host, port));
  CHECK(host == "192.0.2.1");
  CHECK(port == 4000);
  REQUIRE(ParseServer("[2001:db8::1]:4001", 3478, host, port));
  CHECK(host == "2001:db8::1");
  CHECK(port == 4001);
  REQUIRE(ParseServer("2001:db8::1", 3478, host, port));
  CHECK(host == "2001:db8::1");
  CHECK(port == 3478);
  CHECK(!ParseServer("", 3478, host, port));
  CHECK(!ParseServer("host:", 3478, host, port));
  CHECK(!ParseServer("host:70000", 3478, host, port));
  CHECK(!ParseServer("host:0", 3478, host, port));
  CHECK(!ParseServer("[::1", 3478, host, port));
}

TEST_CASE("Relay room codes are easy to read out", "[relay]") {
  for (int i = 0; i < 100; ++i) {
    const std::string code = NewRoomCode();
    REQUIRE(code.size() == kRoomCodeLength);
    CHECK(NormalizeRoomCode(code) == code);
  }
  CHECK(NormalizeRoomCode("ab-c 0ol") == "ABC001");
  CHECK(NormalizeRoomCode("ilIL9z") == "11119Z");
  CHECK(!NormalizeRoomCode("ABCDE"));
  CHECK(!NormalizeRoomCode("ABCDEFG"));
  CHECK(!NormalizeRoomCode("ABCDEU"));
  CHECK(!NormalizeRoomCode("ABC!EF"));
  CHECK(RoomName(PublicLobby(kTitle)) == "lobby:4D5307E6");
  CHECK(RoomName(PrivateRoom("ABC001")) == "room:ABC001");
  CHECK(RoomPath(PublicLobby(kTitle)) == "lobby/4D5307E6");
  CHECK(RoomPath(PrivateRoom("ABC001")) == "room/ABC001");
  CHECK(PrivateRoom("ABC001") != PublicLobby(kTitle));
  CHECK(ParseReject("duplicate_mac") == Reject::kDuplicateMac);
  CHECK(ParseReject("full") == Reject::kFull);
  CHECK(!ParseReject("nope"));
}

TEST_CASE("Relay frames split and come back together", "[relay]") {
  const auto ethernet = MakeFrame(kMacA, kMacB, 1514, 7);
  auto fragments = Fragment(ethernet, 1, 9);
  REQUIRE(fragments.size() == 2);
  CHECK(fragments[0].payload.size() == kFragmentPayload);
  CHECK(fragments[0].source == kMacA);
  CHECK(fragments[1].destination == kMacB);

  Reassembler reassembler;
  CHECK(!reassembler.Add(Frame(fragments[1]), 0));
  // A repeat doesn't count twice.
  CHECK(!reassembler.Add(Frame(fragments[1]), 0));
  auto whole = reassembler.Add(Frame(fragments[0]), 10);
  REQUIRE(whole);
  CHECK(*whole == ethernet);

  // An incomplete frame expires.
  CHECK(!reassembler.Add(Frame(fragments[0]), 1000));
  CHECK(!reassembler.Add(Frame(fragments[1]),
                         1000 + Reassembler::kTimeoutMs + 1));

  CHECK(Fragment(MakeFrame(kMacA, kMacB, 60, 0), 1, 1).size() == 1);
  CHECK(Fragment(std::vector<uint8_t>(10), 1, 1).empty());
  CHECK(
      Fragment(std::vector<uint8_t>(kFragmentPayload * kMaxFragments + 1), 1, 1)
          .empty());
}

TEST_CASE("STUN messages read like RFC 5769's", "[relay]") {
  // RFC 5769 2.2, a Binding response with a short-term credential.
  const std::vector<uint8_t> response = {
      0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01,
      0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b,
      0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
      0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
      0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3,
      0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7,
      0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96};
  auto message = stun::Read(response);
  REQUIRE(message);
  CHECK(message->method == Method::kBinding);
  CHECK(message->kind == Class::kSuccess);
  CHECK(message->mapped == V4(192, 0, 2, 1, 32853));
  const std::string_view password = "VOkJxbRl1RmTxUk/WvJxBt";
  const auto short_term = std::span(
      reinterpret_cast<const uint8_t*>(password.data()), password.size());
  CHECK(stun::CheckIntegrity(response, *message, short_term));
  auto tampered = response;
  tampered[47] ^= 1;
  auto read = stun::Read(tampered);
  REQUIRE(read);
  CHECK(!stun::CheckIntegrity(tampered, *read, short_term));

  // RFC 5769 2.4, a request with a long-term credential. The password is
  // "TheMatrIX" after SASLprep.
  const std::vector<uint8_t> request = {
      0x00, 0x01, 0x00, 0x60, 0x21, 0x12, 0xa4, 0x42, 0x78, 0xad, 0x34, 0x33,
      0xc6, 0xad, 0x72, 0xc0, 0x29, 0xda, 0x41, 0x2e, 0x00, 0x06, 0x00, 0x12,
      0xe3, 0x83, 0x9e, 0xe3, 0x83, 0x88, 0xe3, 0x83, 0xaa, 0xe3, 0x83, 0x83,
      0xe3, 0x82, 0xaf, 0xe3, 0x82, 0xb9, 0x00, 0x00, 0x00, 0x15, 0x00, 0x1c,
      0x66, 0x2f, 0x2f, 0x34, 0x39, 0x39, 0x6b, 0x39, 0x35, 0x34, 0x64, 0x36,
      0x4f, 0x4c, 0x33, 0x34, 0x6f, 0x4c, 0x39, 0x46, 0x53, 0x54, 0x76, 0x79,
      0x36, 0x34, 0x73, 0x41, 0x00, 0x14, 0x00, 0x0b, 0x65, 0x78, 0x61, 0x6d,
      0x70, 0x6c, 0x65, 0x2e, 0x6f, 0x72, 0x67, 0x00, 0x00, 0x08, 0x00, 0x14,
      0xf6, 0x70, 0x24, 0x65, 0x6d, 0xd6, 0x4a, 0x3e, 0x02, 0xb8, 0xe0, 0x71,
      0x2e, 0x85, 0xc9, 0xa2, 0x8c, 0xa8, 0x96, 0x66};
  message = stun::Read(request);
  REQUIRE(message);
  CHECK(message->kind == Class::kRequest);
  CHECK(message->username ==
        "\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa\xe3\x83\x83\xe3\x82\xaf\xe3\x82"
        "\xb9");
  CHECK(message->realm == "example.org");
  CHECK(message->nonce == "f//499k954d6OL34oL9FSTvy64sA");
  CHECK(stun::CheckIntegrity(
      request, *message,
      stun::LongTermKey(message->username, message->realm, "TheMatrIX")));
  CHECK(!stun::CheckIntegrity(
      request, *message,
      stun::LongTermKey(message->username, message->realm, "TheMatrix")));

  // Malformed messages are dropped.
  auto short_length = response;
  short_length[3] -= 4;
  CHECK(!stun::Read(short_length));
  auto not_stun = response;
  not_stun[0] |= 0x80;
  CHECK(!stun::Read(not_stun));
  CHECK(!stun::Read(std::span(response).first(19)));
}

TEST_CASE("STUN messages survive the wire", "[relay]") {
  Endpoint v6;
  v6.v6 = true;
  v6.address = {0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  v6.port = 4000;
  const auto key = stun::LongTermKey("user", "realm", "pass");
  const std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  const auto id = stun::NewTransactionId();
  auto written =
      stun::Writer(Method::kAllocate, Class::kSuccess, id)
          .AddAddress(Attribute::kXorRelayedAddress, v6)
          .AddAddress(Attribute::kXorMappedAddress, V4(198, 51, 100, 7, 50001))
          .AddUint32(Attribute::kLifetime, 600)
          .Add(Attribute::kData, data)
          .Finish(key);
  CHECK(written.size() % 4 == 0);
  auto message = stun::Read(written);
  REQUIRE(message);
  CHECK(message->method == Method::kAllocate);
  CHECK(message->kind == Class::kSuccess);
  CHECK(message->transaction == id);
  CHECK(message->relayed == v6);
  CHECK(message->mapped == V4(198, 51, 100, 7, 50001));
  CHECK(message->lifetime == 600u);
  CHECK(message->data == data);
  CHECK(stun::CheckIntegrity(written, *message, key));
  CHECK(!stun::CheckIntegrity(written, *message,
                              stun::LongTermKey("user", "realm", "other")));
  written[30] ^= 1;
  message = stun::Read(written);
  REQUIRE(message);
  CHECK(!stun::CheckIntegrity(written, *message, key));

  const uint8_t error[] = {0, 0, 4, 38};
  message = stun::Read(stun::Writer(Method::kRefresh, Class::kError, id)
                           .Add(Attribute::kErrorCode, error)
                           .Add(Attribute::kNonce, "abc")
                           .Finish());
  REQUIRE(message);
  CHECK(message->kind == Class::kError);
  CHECK(message->method == Method::kRefresh);
  CHECK(message->error == 438);
  CHECK(message->nonce == "abc");
  CHECK(!message->integrity);
  CHECK(stun::NewTransactionId() != stun::NewTransactionId());
}

namespace {

// One TurnClient talking to a FakeTurn.
struct TurnLink {
  TurnLink() {
    turn.users["user1"] = std::string(kPassword);
    client.Tick(now, out);
    Pump();
  }

  void Pump() {
    while (!out.empty()) {
      for (const Datagram& datagram : std::exchange(out, {})) {
        if (datagram.to != turn.at()) {
          continue;
        }
        std::vector<Sent> sent;
        turn.Receive(datagram.data, at, sent);
        for (const Sent& reply : sent) {
          if (reply.datagram.to == at) {
            client.Receive(reply.datagram.data, now, out);
          } else {
            outside.push_back(reply);
          }
        }
      }
    }
  }

  void Run(uint64_t ms) {
    for (const uint64_t end = now + ms; now < end;) {
      now += 25;
      client.Tick(now, out);
      Pump();
    }
  }

  // Another client on the same socket, as after the last one gave up.
  void Replace(TurnClient::Credentials credentials) {
    client = TurnClient{turn.at(), std::move(credentials)};
    client.Tick(now, out);
    Pump();
  }

  FakeTurn turn{V4(203, 0, 113, 1, 3478)};
  Endpoint at = V4(198, 51, 100, 7, 50001);
  TurnClient client{turn.at(),
                    TurnClient::Credentials{"user1", std::string(kPassword)}};
  uint64_t now = 1000;
  std::vector<Datagram> out;
  // What the server relayed to addresses outside it.
  std::vector<Sent> outside;
};

}  // namespace

TEST_CASE("TURN allocations relay to permitted peers", "[relay]") {
  TurnLink link;
  REQUIRE(link.client.relayed());
  CHECK(link.client.mapped() == link.at);
  CHECK(link.turn.allocations() == 1);
  CHECK(!link.client.failed());

  const Endpoint peer = V4(192, 0, 2, 9, 7000);
  const std::vector<uint8_t> data = {9, 8, 7};
  CHECK(!link.client.Send(peer, data, link.out));
  link.client.Permit({peer}, link.now, link.out);
  link.Pump();
  REQUIRE(link.client.Send(peer, data, link.out));
  link.Pump();
  REQUIRE(link.outside.size() == 1);
  CHECK(link.outside[0].from == *link.client.relayed());
  CHECK(link.outside[0].datagram.to == peer);
  CHECK(link.outside[0].datagram.data == data);
  // A permission covers the address, whatever the port.
  CHECK(link.client.Send(V4(192, 0, 2, 9, 7001), data, link.out));
  CHECK(!link.client.Send(V4(192, 0, 2, 10, 7000), data, link.out));
  link.out.clear();

  std::vector<Sent> in;
  link.turn.ReceiveRelayed(data, peer, *link.client.relayed(), in);
  REQUIRE(in.size() == 1);
  auto relayed = link.client.Receive(in[0].datagram.data, link.now, link.out);
  REQUIRE(relayed);
  CHECK(relayed->peer == peer);
  CHECK(relayed->data == data);
  // Strangers can't reach the allocation.
  in.clear();
  link.turn.ReceiveRelayed(data, V4(192, 0, 2, 10, 7000),
                           *link.client.relayed(), in);
  CHECK(in.empty());

  // The allocation and permissions are refreshed before they lapse.
  link.Run(700000);
  CHECK(!link.client.failed());
  CHECK(link.turn.allocations() == 1);
  CHECK(link.client.Send(peer, data, link.out));
  link.out.clear();

  link.client.Release(link.out);
  link.Pump();
  CHECK(link.turn.allocations() == 0);
  CHECK(link.client.failed());
}

TEST_CASE("TURN clients ask again with a fresh nonce", "[relay]") {
  TurnLink link;
  REQUIRE(link.client.relayed());
  link.turn.stale_nonce = true;
  link.client.Permit({V4(192, 0, 2, 9, 7000)}, link.now, link.out);
  link.Pump();
  CHECK(!link.client.Send(V4(192, 0, 2, 9, 7000), {}, link.out));
  link.Run(25);
  CHECK(link.client.Send(V4(192, 0, 2, 9, 7000), {}, link.out));
  CHECK(!link.client.failed());
}

TEST_CASE("TURN clients give up on refusals and silence", "[relay]") {
  {
    // Wrong credentials.
    TurnLink link;
    link.turn.users["user1"] = "other";
    TurnClient client{link.turn.at(),
                      TurnClient::Credentials{"user1", "wrong"}};
    std::vector<Datagram> out;
    client.Tick(link.now, out);
    for (int i = 0; i < 4 && !out.empty(); ++i) {
      for (const Datagram& datagram : std::exchange(out, {})) {
        std::vector<Sent> sent;
        link.turn.Receive(datagram.data, V4(198, 51, 100, 8, 1), sent);
        for (const Sent& reply : sent) {
          client.Receive(reply.datagram.data, link.now, out);
        }
      }
    }
    CHECK(client.failed());
    CHECK(!client.relayed());
  }
  {
    TurnLink link;
    REQUIRE(link.client.relayed());
    // A forged response is ignored.
    link.client.Permit({V4(192, 0, 2, 9, 7000)}, link.now, link.out);
    REQUIRE(link.out.size() == 1);
    auto request = stun::Read(link.out[0].data);
    REQUIRE(request);
    auto forged = stun::Writer(Method::kCreatePermission, Class::kSuccess,
                               request->transaction)
                      .Finish(stun::LongTermKey("user1", "fake.test", "nope"));
    link.client.Receive(forged, link.now, link.out);
    CHECK(!link.client.Send(V4(192, 0, 2, 9, 7000), {}, link.out));
    // So is an unsigned one.
    auto bare = stun::Writer(Method::kCreatePermission, Class::kSuccess,
                             request->transaction)
                    .Finish();
    link.client.Receive(bare, link.now, link.out);
    CHECK(!link.client.Send(V4(192, 0, 2, 9, 7000), {}, link.out));
    link.out.clear();

    // The keepalive 15 s in goes unanswered for another 15 s.
    link.turn.silent = true;
    link.Run(40000);
    CHECK(link.client.failed());
    CHECK(!link.client.relayed());
  }
  {
    // STUN only: a Binding finds the mapped address, and keepalives notice
    // when the server goes.
    FakeTurn turn{V4(203, 0, 113, 1, 3478)};
    TurnClient client{turn.at(), std::nullopt};
    std::vector<Datagram> out;
    client.Tick(0, out);
    REQUIRE(out.size() == 1);
    std::vector<Sent> sent;
    turn.Receive(out[0].data, V4(198, 51, 100, 8, 1), sent);
    REQUIRE(sent.size() == 1);
    out.clear();
    client.Receive(sent[0].datagram.data, 10, out);
    CHECK(client.mapped() == V4(198, 51, 100, 8, 1));
    CHECK(!client.relayed());
    CHECK(!client.Send(V4(192, 0, 2, 9, 7000), {}, out));
    for (uint64_t now = 0; now < 60000; now += 25) {
      client.Tick(now, out);
    }
    CHECK(client.failed());
  }
}

TEST_CASE("TURN clients notice when the allocation stops being theirs",
          "[relay]") {
  {
    // A NAT that maps the socket to another port ends the allocation.
    TurnLink link;
    REQUIRE(link.client.relayed());
    link.at = V4(198, 51, 100, 7, 50002);
    link.Run(16000);
    CHECK(link.client.failure() == TurnClient::Failure::kLost);
  }
  {
    // The last client on this socket gave up while the server was quiet, so
    // its allocation is freed for the next.
    TurnLink link;
    REQUIRE(link.client.relayed());
    const Endpoint before = *link.client.relayed();
    link.Replace({"user1", std::string(kPassword)});
    REQUIRE(link.client.relayed());
    CHECK(*link.client.relayed() != before);
    CHECK(link.turn.allocations() == 1);
    CHECK(!link.client.failed());
  }
  {
    // One made with other credentials can't be.
    TurnLink link;
    REQUIRE(link.client.relayed());
    link.turn.users["user2"] = std::string(kPassword);
    link.Replace({"user2", std::string(kPassword)});
    CHECK(link.client.failure() == TurnClient::Failure::kInUse);
    CHECK(link.turn.allocations() == 1);
  }
}

TEST_CASE("Relay players only trust answers to their own probes", "[relay]") {
  RelayClient client(kMacA, Config());
  Outbox out;
  uint64_t now = 100000;
  client.LobbyConnecting(now);
  client.LobbyOpened(now, out);
  const uint64_t secret = 0x0123456789ABCDEF;
  client.LobbyMessage(
      fmt::format(R"({{"t":"welcome","v":1,"secret":"{:016x}","ice":{{}}}})",
                  secret),
      now, out);
  // Only addresses someone could be reached at count, and only 8 of them.
  std::string candidates =
      R"("0.0.0.0:5000","198.51.100.9:0","224.0.0.1:5000",)"
      R"("255.255.255.255:5000","[::]:5000","[ff02::1]:5000")";
  for (int i = 0; i < 10; ++i) {
    candidates += fmt::format(R"(,"198.51.100.{}:5000")", 20 + i);
  }
  client.LobbyMessage(
      fmt::format(R"({{"t":"peers","peers":[{{"mac":"{}","title":"{:08X}",)"
                  R"("version":1,"candidates":[{}]}}]}})",
                  Hex(kMacB), kTitle, candidates),
      now, out);
  std::set<Endpoint> probed;
  uint64_t nonce = 0;
  for (int i = 0; i < 40; ++i, now += 25) {
    client.Tick(now, out);
    for (const Datagram& datagram : std::exchange(out.datagrams, {})) {
      auto decoded = Decode(datagram.data);
      if (decoded && std::holds_alternative<Probe>(*decoded)) {
        probed.insert(datagram.to);
        nonce = std::get<Probe>(*decoded).nonce;
      }
    }
  }
  CHECK(probed.size() == 8);
  CHECK(probed.count(V4(198, 51, 100, 20, 5000)));
  CHECK(!probed.count(V4(198, 51, 100, 28, 5000)));
  REQUIRE(nonce);

  const Endpoint b = V4(198, 51, 100, 20, 5000);
  auto reply = [&](uint64_t with, const Endpoint& from) {
    client.Receive(Encode(Probe{secret, kMacB, kMacA, with, true}), from, now,
                   out);
    client.Tick(now, out);
    return client.status().direct;
  };
  // Not a nonce it sent, or not from where it probed.
  CHECK(reply(nonce + 1, b) == 0);
  CHECK(reply(nonce, V4(192, 0, 2, 66, 5000)) == 0);
  CHECK(reply(nonce, b) == 1);
}

TEST_CASE("Relay players find each other and go direct", "[relay]") {
  Network network;
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  network.Run(3000);
  CHECK(a.client.status().state == State::kOnline);
  CHECK(a.client.status().players == 1);
  CHECK(a.client.status().direct == 1);
  CHECK(b.client.status().direct == 1);
  CHECK(!a.client.status().version_mismatch);

  const auto hello = MakeFrame(kMacA, kBroadcast, 60, 1);
  network.Send(a, hello);
  REQUIRE(b.received.size() == 1);
  CHECK(b.received[0] == hello);
  const auto reply = MakeFrame(kMacB, kMacA, 1514, 2);
  network.Send(b, reply);
  REQUIRE(a.received.size() == 1);
  CHECK(a.received[0] == reply);
  CHECK(network.direct_frames == 3);
  CHECK(network.turn.relayed_frames == 0);
  // Each network's LAN addresses are only tried from that network.
  CHECK(network.foreign_lan_sends == 0);
  // Frames for nobody in the room go nowhere.
  network.Send(a, MakeFrame(kMacA, kMacC, 60, 3));
  CHECK(network.direct_frames == 3);
}

TEST_CASE("Relay players time the trip to each other", "[relay]") {
  Network network;
  // 50 ms each way, so a keepalive comes back about 100 ms after it left.
  network.direct_delay_ms = 50;
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  network.Run(3000);
  REQUIRE(a.client.status().direct == 1);
  network.Run(10000);
  CHECK(a.client.status().ping_ms >= 95);
  CHECK(a.client.status().ping_ms <= 130);
  CHECK(a.client.status().ping_ms % 5 == 0);
  CHECK(b.client.status().ping_ms >= 95);
}

TEST_CASE("Relay players reached only through the relay have no ping", "[relay]") {
  Network network;
  network.block_direct = true;
  auto& a = network.Add(kMacA);
  network.Add(kMacB);
  network.Run(10000);
  CHECK(a.client.status().relayed == 1);
  CHECK(a.client.status().ping_ms == 0);
}

TEST_CASE("Relay carries what NATs won't let through", "[relay]") {
  Network network;
  network.block_direct = true;
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  network.Run(3000);
  CHECK(a.client.status().state == State::kOnline);
  CHECK(a.client.status().direct == 0);
  CHECK(a.client.status().relayed == 1);
  CHECK(b.client.status().relayed == 1);

  const auto hello = MakeFrame(kMacA, kBroadcast, 1514, 1);
  network.Send(a, hello);
  REQUIRE(b.received.size() == 1);
  CHECK(b.received[0] == hello);
  network.Send(b, MakeFrame(kMacB, kMacA, 60, 2));
  CHECK(a.received.size() == 1);
  CHECK(network.direct_frames == 0);
  CHECK(network.turn.relayed_frames == 3);
}

TEST_CASE("Relay takes over when a direct path stops", "[relay]") {
  Network network;
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  network.Run(3000);
  REQUIRE(a.client.status().direct == 1);

  network.block_direct = true;
  const auto frame = MakeFrame(kMacA, kMacB, 60, 1);
  network.Stream(a, frame, 10000);
  CHECK(a.client.status().direct == 0);
  CHECK(a.client.status().relayed == 1);
  b.received.clear();
  network.Send(a, frame);
  CHECK(b.received.size() == 1);

  // And gives the path back once it works again.
  network.block_direct = false;
  network.Stream(a, frame, 15000);
  CHECK(a.client.status().direct == 1);
}

TEST_CASE("Relay players on one network meet on it", "[relay]") {
  Network network;
  // The router won't hairpin, so only the LAN addresses work.
  network.block_direct = true;
  auto& a = network.Add(kMacA, Config(), 7);
  auto& b = network.Add(kMacB, Config(), 7);
  network.Run(3000);
  CHECK(a.client.status().direct == 1);
  CHECK(b.client.status().direct == 1);
  network.Send(a, MakeFrame(kMacA, kMacB, 60, 1));
  CHECK(b.received.size() == 1);
  CHECK(network.turn.relayed_frames == 0);
}

TEST_CASE("Relay players keep playing while the lobby is away", "[relay]") {
  Network network;
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  network.Run(3000);
  REQUIRE(a.client.status().direct == 1);

  network.lobby.up = false;
  network.lobby.Restart();
  network.Run(15000);
  CHECK(a.client.status().state == State::kOffline);
  network.Send(a, MakeFrame(kMacA, kMacB, 60, 1));
  CHECK(b.received.size() == 1);

  network.lobby.up = true;
  network.Run(20000);
  CHECK(a.client.status().state == State::kOnline);
  CHECK(a.client.status().players == 1);
  CHECK(b.client.status().players == 1);
  network.Send(a, MakeFrame(kMacA, kMacB, 60, 2));
  CHECK(b.received.size() == 2);
}

TEST_CASE("Relay rooms keep their players apart", "[relay]") {
  Network network;
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  const std::string code = NewRoomCode();
  auto& c = network.Add(kMacC, Config(1, PrivateRoom(code)));
  network.Run(3000);
  CHECK(a.client.status().players == 1);
  CHECK(c.client.status().state == State::kOnline);
  CHECK(c.client.status().players == 0);
  network.Send(a, MakeFrame(kMacA, kBroadcast, 60, 1));
  CHECK(b.received.size() == 1);
  CHECK(c.received.empty());
  CHECK(network.lobby.players("room/" + code) == 1);

  // A player that leaves is gone for the others.
  network.Leave(b);
  network.Run(1000);
  CHECK(a.client.status().players == 0);
  CHECK(network.turn.allocations() == 2);
}

TEST_CASE("Relay lobby turns players away", "[relay]") {
  Network network;
  network.lobby.capacity = 2;
  auto& a = network.Add(kMacA);
  auto& twin = network.Add(kMacA);
  network.Run(1000);
  CHECK(a.client.status().state == State::kOnline);
  CHECK(twin.client.status().state == State::kRejected);
  CHECK(twin.client.status().reject == Reject::kDuplicateMac);

  network.Add(kMacB);
  auto& c = network.Add(kMacC);
  network.Run(1000);
  CHECK(c.client.status().state == State::kRejected);
  CHECK(c.client.status().reject == Reject::kFull);
  // Turned away players try again now and then.
  const size_t connects = network.lobby.connects;
  network.Run(25000);
  CHECK(network.lobby.connects > connects);
  CHECK(network.lobby.connects < connects + 10);

  // A lobby for another version of the protocol isn't asked again.
  Network newer;
  newer.lobby.version = 2;
  auto& old = newer.Add(kMacA);
  newer.Run(60000);
  CHECK(old.client.status().state == State::kRejected);
  CHECK(old.client.status().reject == Reject::kVersion);
  CHECK(newer.lobby.connects == 1);
}

TEST_CASE("Relay players never probe addresses a peer made up", "[relay]") {
  // Everyone lists the others' public addresses and local ones of its own
  // choosing, as a hostile player could.
  const std::vector<std::string> forged = {
      "198.51.100.101:9", "198.51.100.102:9", "192.168.77.5:4000",
      "127.0.0.1:4000",   "169.254.9.9:4000", "[fe80::9]:4000"};
  const Endpoint lan = V4(192, 168, 77, 5, 4000);
  const Endpoint loopback = V4(127, 0, 0, 1, 4000);
  const Endpoint link_local = V4(169, 254, 9, 9, 4000);
  const Endpoint link_local_v6 = *ParseEndpoint("[fe80::9]:4000");
  auto run = [&](bool send_net, uint8_t network) {
    auto network_ = std::make_unique<Network>();
    network_->send_net = send_net;
    network_->lobby.extra_candidates = forged;
    network_->Add(kMacA, Config(), network);
    network_->Add(kMacB, Config(), network);
    network_->Run(3000);
    return network_;
  };
  auto never_loopback_or_link_local = [&](const Network& network) {
    return !network.sent_to.count(loopback) &&
           !network.sent_to.count(link_local) &&
           !network.sent_to.count(link_local_v6);
  };

  // The lobby says they're on different networks, whatever they list.
  auto apart = run(true, 0);
  CHECK(!apart->sent_to.count(lan));
  CHECK(never_loopback_or_link_local(*apart));
  // On one network, private addresses are worth trying.
  auto together = run(true, 7);
  CHECK(together->sent_to.count(lan));
  CHECK(never_loopback_or_link_local(*together));
  // An old lobby doesn't say, so the listed addresses decide as before.
  auto unsaid = run(false, 0);
  CHECK(unsaid->sent_to.count(lan));
  CHECK(never_loopback_or_link_local(*unsaid));
}

TEST_CASE("Relay players only use a peer's public relay address", "[relay]") {
  for (const char* relay : {"192.168.1.5:49152", "127.0.0.1:49152",
                            "169.254.1.1:49152", "10.0.0.7:49152",
                            "100.64.0.9:49152", "[fe80::1]:49152"}) {
    Network network;
    network.block_direct = true;
    network.lobby.forged_relay = relay;
    auto& a = network.Add(kMacA);
    network.Add(kMacB);
    network.Run(3000);
    CHECK(a.client.status().players == 1);
    CHECK(a.client.status().relayed == 0);
  }
  Network network;
  network.block_direct = true;
  auto& a = network.Add(kMacA);
  network.Add(kMacB);
  network.Run(3000);
  CHECK(a.client.status().relayed == 1);
}

namespace {

// Opens the lobby connection of |client| and welcomes it, 'net' when given.
void JoinRoom(RelayClient& client, uint64_t now, const char* net = nullptr) {
  Outbox out;
  client.LobbyConnecting(now);
  client.LobbyOpened(now, out);
  client.LobbyMessage(
      fmt::format(
          R"({{"t":"welcome","v":1,"secret":"0123456789abcdef",{}"ice":{{}}}})",
          net ? fmt::format(R"("net":"{}",)", net) : std::string()),
      now, out);
}

}  // namespace

TEST_CASE("Relay players wait for a welcome only so long", "[relay]") {
  RelayClient client(kMacA, Config());
  Outbox out;
  uint64_t now = 100000;
  client.LobbyConnecting(now);
  client.LobbyOpened(now, out);
  // A join that is neither welcomed nor rejected is given up on.
  CHECK(!client.LobbyStale(now + 15000));
  CHECK(client.LobbyStale(now + 15001));
  client.LobbyClosed(now + 15001);
  CHECK(client.WantsLobby(now + 16001));

  // A welcome or a rejection answers it.
  RelayClient welcomed(kMacA, Config());
  JoinRoom(welcomed, now);
  CHECK(!welcomed.LobbyStale(now + 30000));
  RelayClient rejected(kMacA, Config());
  rejected.LobbyConnecting(now);
  rejected.LobbyOpened(now, out);
  rejected.LobbyMessage(R"({"t":"reject","reason":"full"})", now + 100, out);
  CHECK(!rejected.LobbyStale(now + 30000));
}

TEST_CASE("Relay players name their app so the room counts it", "[relay]") {
  RelayConfig config = Config();
  config.app_id = "0123456789abcdef";
  RelayClient client(kMacA, config);
  Outbox out;
  uint64_t now = 100000;
  client.LobbyConnecting(now);
  client.LobbyOpened(now, out);
  REQUIRE(out.lobby.size() == 1);
  CHECK(out.lobby[0].find(R"("here":"0123456789abcdef")") != std::string::npos);
  client.LobbyMessage(
      R"({"t":"welcome","v":1,"secret":"0123456789abcdef","here":true,"ice":{}})",
      now, out);
  CHECK(client.status().app_counted);
  client.LobbyClosed(now + 1000);
  CHECK(!client.status().app_counted);

  // A lobby that doesn't say so doesn't count it.
  RelayClient old(kMacA, config);
  JoinRoom(old, now);
  CHECK(old.status().state == State::kOnline);
  CHECK(!old.status().app_counted);

  // Without an ID, or with one that isn't 16 hex digits, the join names none.
  for (const char* id : {"", "0123456789ABCDEF", "0123"}) {
    RelayConfig other = Config();
    other.app_id = id;
    RelayClient anonymous(kMacA, other);
    Outbox sent;
    anonymous.LobbyConnecting(now);
    anonymous.LobbyOpened(now, sent);
    REQUIRE(sent.lobby.size() == 1);
    CHECK(sent.lobby[0].find("here") == std::string::npos);
  }
}

TEST_CASE("Relay players back off from a lobby that turns them away",
          "[relay]") {
  RelayClient client(kMacA, Config());
  Outbox out;
  uint64_t now = 100000;
  // Turned away again and again: 10 seconds, twice as long each time in a
  // row, and at most 5 minutes.
  for (uint64_t wait : {10000ull, 20000ull, 40000ull, 80000ull, 160000ull,
                        300000ull, 300000ull}) {
    client.LobbyConnecting(now);
    client.LobbyOpened(now, out);
    client.LobbyMessage(R"({"t":"reject","reason":"full"})", now, out);
    client.LobbyClosed(now);
    CHECK(!client.WantsLobby(now + wait - 1));
    CHECK(client.WantsLobby(now + wait));
    now += wait;
  }

  // A welcome starts over, and a dropped connection is tried again soon.
  JoinRoom(client, now);
  client.LobbyClosed(now);
  CHECK(client.WantsLobby(now + 1000));

  // An HTTP error instead of the connection (the lobby is out of its daily
  // quota) backs off the same way.
  now += 1000;
  for (uint64_t wait : {10000ull, 20000ull, 40000ull}) {
    client.LobbyConnecting(now);
    client.LobbyClosed(now, true);
    CHECK(!client.WantsLobby(now + wait - 1));
    CHECK(client.WantsLobby(now + wait));
    now += wait;
  }
}

TEST_CASE("Relay players rejoining their own room are reconnecting",
          "[relay]") {
  constexpr std::string_view kDuplicate =
      R"({"t":"reject","reason":"duplicate_mac"})";
  RelayClient client(kMacA, Config());
  Outbox out;
  uint64_t now = 100000;
  JoinRoom(client, now);
  REQUIRE(client.status().state == State::kOnline);

  // The network changed: the lobby still holds the old connection.
  now += 5000;
  client.LobbyClosed(now);
  const uint64_t left = now;
  now += 1000;
  client.LobbyConnecting(now);
  client.LobbyOpened(now, out);
  client.LobbyMessage(kDuplicate, now, out);
  CHECK(client.status().state == State::kReconnecting);
  client.LobbyClosed(now);
  // Tried again sooner than other turned away players.
  CHECK(!client.WantsLobby(now + 4999));
  CHECK(client.WantsLobby(now + 5000));
  // Past the time the lobby holds a quiet connection, it's someone else.
  client.Tick(left + 70000, out);
  CHECK(client.status().state == State::kReconnecting);
  client.Tick(left + 70001, out);
  CHECK(client.status().state == State::kRejected);
  CHECK(client.status().reject == Reject::kDuplicateMac);

  // Never in the room, the profile is another player's.
  RelayClient stranger(kMacA, Config());
  stranger.LobbyConnecting(now);
  stranger.LobbyOpened(now, out);
  stranger.LobbyMessage(kDuplicate, now, out);
  CHECK(stranger.status().state == State::kRejected);
}

TEST_CASE("Relay players take the lobby's word for who shares a network",
          "[relay]") {
  // Private addresses are probed when the lobby saw both players on one
  // network, even before a public address of their own is known, and never
  // when it saw them apart.
  auto probes_private = [](const char* mine, const char* theirs) {
    RelayClient client(kMacA, Config());
    Outbox out;
    uint64_t now = 100000;
    JoinRoom(client, now, mine);
    client.LobbyMessage(
        fmt::format(
            R"({{"t":"peers","peers":[{{"mac":"{}","title":"{:08X}","version":1,)"
            R"("candidates":["192.168.5.5:4000","198.51.100.9:5000"],)"
            R"("net":"{}"}}]}})",
            Hex(kMacB), kTitle, theirs),
        now, out);
    std::set<Endpoint> probed;
    for (int i = 0; i < 40; ++i, now += 25) {
      client.Tick(now, out);
      for (const Datagram& datagram : std::exchange(out.datagrams, {})) {
        probed.insert(datagram.to);
      }
    }
    return probed.count(V4(192, 168, 5, 5, 4000)) != 0;
  };
  CHECK(probes_private("00000000000000aa", "00000000000000aa"));
  CHECK(!probes_private("00000000000000aa", "00000000000000bb"));
}

TEST_CASE("Relay players see another version of the title", "[relay]") {
  Network network;
  auto& a = network.Add(kMacA, Config(1));
  auto& b = network.Add(kMacB, Config(2));
  network.Run(3000);
  CHECK(a.client.status().version_mismatch);
  CHECK(b.client.status().version_mismatch);
  network.Send(a, MakeFrame(kMacA, kBroadcast, 60, 1));
  CHECK(b.received.size() == 1);
}

TEST_CASE("Relay players without TURN only go direct", "[relay]") {
  Network network;
  network.lobby.ice = [] {
    return std::string(R"({"stun":["203.0.113.1:3478"]})");
  };
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  network.Run(3000);
  CHECK(a.client.status().direct == 1);
  CHECK(network.turn.allocations() == 0);
  network.Send(a, MakeFrame(kMacA, kMacB, 60, 1));
  CHECK(b.received.size() == 1);

  Network blocked;
  blocked.block_direct = true;
  blocked.lobby.ice = network.lobby.ice;
  auto& c = blocked.Add(kMacA);
  blocked.Add(kMacB);
  blocked.Run(3000);
  CHECK(c.client.status().state == State::kOnline);
  CHECK(c.client.status().players == 1);
  CHECK(c.client.status().direct == 0);
  CHECK(c.client.status().relayed == 0);
}

TEST_CASE("Relay players fall back to STUN when TURN refuses", "[relay]") {
  Network network;
  // Credentials the TURN server doesn't know.
  network.lobby.ice = [] {
    return std::string(
        R"({"stun":["203.0.113.1:3478"],"turn":{"servers":["203.0.113.1:3478"],)"
        R"("username":"nobody","credential":"x","ttl":21600}})");
  };
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  network.Run(3000);
  CHECK(a.client.status().direct == 0);
  network.Run(35000);
  CHECK(a.client.status().direct == 1);
  CHECK(b.client.status().direct == 1);
  CHECK(network.turn.allocations() == 0);
}

TEST_CASE("Relay players get the relay back after TURN goes quiet", "[relay]") {
  Network network;
  network.block_direct = true;
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  network.Run(3000);
  REQUIRE(a.client.status().relayed == 1);

  // Long enough for both to give up on it, and on a new allocation too.
  network.turn.silent = true;
  network.Run(40000);
  CHECK(a.client.status().relayed == 0);
  network.turn.silent = false;
  network.Run(20000);
  CHECK(a.client.status().relayed == 1);
  CHECK(b.client.status().relayed == 1);
  CHECK(network.turn.allocations() == 2);
  network.Send(a, MakeFrame(kMacA, kMacB, 60, 1));
  CHECK(b.received.size() == 1);
}

TEST_CASE("Relay players renew their TURN credentials", "[relay]") {
  Network network;
  network.block_direct = true;
  network.lobby.ice = IceWithTurn(network.turn, network.turn.at(), 900);
  auto& a = network.Add(kMacA);
  auto& b = network.Add(kMacB);
  network.Run(3000);
  REQUIRE(a.client.status().relayed == 1);
  CHECK(network.turn.usernames() == std::set<std::string>{"user1", "user2"});

  // Credentials for 900 s are renewed after 300 s.
  network.Run(310000);
  CHECK(std::count(network.lobby.received.begin(), network.lobby.received.end(),
                   R"({"t":"ice"})") == 2);
  CHECK(network.turn.allocations() == 2);
  CHECK(network.turn.usernames() == std::set<std::string>{"user3", "user4"});
  CHECK(a.client.status().relayed == 1);
  network.Send(a, MakeFrame(kMacA, kMacB, 60, 1));
  CHECK(b.received.size() == 1);
  network.Send(b, MakeFrame(kMacB, kMacA, 60, 2));
  CHECK(a.received.size() == 1);
}

namespace {

// Keeps global hooks for one test.
struct Hooks {
  Hooks() {
    // Nothing reaches a real lobby unless a test asks.
    secure::SetLobbySocketFactory(nullptr);
    secure::SetRelayStatusListener([this](const RelayStatus& status) {
      std::lock_guard lock(mutex);
      statuses.push_back(status);
    });
  }
  ~Hooks() {
    secure::SetRelayStatusListener(nullptr);
    secure::SetLobbySocketFactory(nullptr);
  }

  // The first status from |from| on that |match| accepts.
  template <typename Match>
  std::optional<size_t> Reported(size_t from, Match match) {
    std::lock_guard lock(mutex);
    for (size_t i = from; i < statuses.size(); ++i) {
      if (match(statuses[i])) {
        return i;
      }
    }
    return std::nullopt;
  }

  std::mutex mutex;
  std::vector<RelayStatus> statuses;
};

class FakeLobbySocket : public secure::LobbySocket {
 public:
  FakeLobbySocket(FakeLobby& lobby, std::optional<int> id)
      : lobby_(lobby), id_(id) {}
  ~FakeLobbySocket() override {
    if (id_) {
      lobby_.Close(*id_);
    }
  }
  void Send(std::string text) override {
    if (id_) {
      lobby_.Message(*id_, text);
    }
  }

 private:
  FakeLobby& lobby_;
  std::optional<int> id_;
};

constexpr std::string_view kLobbyUrl = "wss://lobby.test";

void UseLobby(FakeLobby& lobby) {
  secure::SetLobbySocketFactory(
      [&lobby](const std::string& url, secure::LobbySocket::Events events)
          -> std::unique_ptr<secure::LobbySocket> {
        if (!url.starts_with(std::string(kLobbyUrl) + "/v1/")) {
          return nullptr;
        }
        auto id = lobby.Connect(url.substr(kLobbyUrl.size() + 4),
                                {events.message, events.closed});
        if (id) {
          events.opened();
        } else {
          events.closed("connection refused");
        }
        return std::make_unique<FakeLobbySocket>(lobby, id);
      });
}

// A FakeTurn on a loopback socket, with its own thread. user1 can allocate.
class IceServer {
 public:
  explicit IceServer(uint16_t port = 0) {
    std::string error;
    REQUIRE(socket_.Open(port, error));
    turn_ = std::make_unique<FakeTurn>(V4(127, 0, 0, 1, socket_.port()));
    turn_->users["user1"] = std::string(kPassword);
    thread_ = std::thread([this] { Serve(); });
  }
  ~IceServer() {
    stop_ = true;
    socket_.Wake();
    thread_.join();
  }

  // An ice object with TURN credentials, or STUN only.
  std::string Ice(bool with_turn) {
    const std::string server = ToString(turn_->at());
    if (!with_turn) {
      return fmt::format(R"({{"stun":["{}"]}})", server);
    }
    return fmt::format(
        R"({{"stun":["{0}"],"turn":{{"servers":["{0}"],"username":"user1",)"
        R"("credential":"{1}","ttl":21600}}}})",
        server, kPassword);
  }

  size_t relayed_frames() {
    std::lock_guard lock(mutex_);
    return turn_->relayed_frames;
  }

 private:
  void Serve() {
    while (!stop_) {
      std::span<const uint8_t> data;
      Endpoint from;
      std::string error;
      if (socket_.ReceiveFor(100, data, from, error) !=
          RelaySocket::Wait::kDatagram) {
        continue;
      }
      std::vector<Sent> out;
      {
        std::lock_guard lock(mutex_);
        turn_->Receive(data, from, out);
        turn_->RelayBetweenAllocations(out);
      }
      for (const Sent& sent : out) {
        if (sent.from == turn_->at()) {
          socket_.SendTo(sent.datagram.data, sent.datagram.to);
        }
      }
    }
  }

  RelaySocket socket_;
  std::mutex mutex_;
  std::unique_ptr<FakeTurn> turn_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace

TEST_CASE("Relay transports meet on loopback", "[relay]") {
  for (const bool direct : {true, false}) {
    Hooks hooks;
    IceServer ice;
    FakeLobby lobby;
    lobby.ice = [&] { return ice.Ice(true); };
    UseLobby(lobby);

    RelayConfig config = Config(1, PrivateRoom(NewRoomCode()));
    config.lobby = std::string(kLobbyUrl);
    config.direct_paths = direct;
    // The fake TURN server and the players share the loopback network.
    config.probe_local_candidates = true;
    std::string error;
    auto a = secure::OpenRelayTransport(kMacA, config, error);
    REQUIRE(a);
    auto b = secure::OpenRelayTransport(kMacB, config, error);
    REQUIRE(b);

    std::vector<std::vector<uint8_t>> at_a, at_b;
    auto pump = [&](auto&& done) {
      const auto until =
          std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!done() && std::chrono::steady_clock::now() < until) {
        std::vector<uint8_t> frame;
        if (a->Receive(frame, 2)) {
          at_a.push_back(frame);
        }
        if (b->Receive(frame, 2)) {
          at_b.push_back(frame);
        }
      }
      return done();
    };
    // Frames sent before a path opens are lost, so A keeps trying.
    const auto hello = MakeFrame(kMacA, kBroadcast, 60, 1);
    REQUIRE(pump([&] {
      a->Send(hello);
      return !at_b.empty();
    }));
    CHECK(at_b[0] == hello);
    const auto reply = MakeFrame(kMacB, kMacA, 1514, 2);
    CHECK(b->Send(reply) == int(reply.size()));
    REQUIRE(pump([&] { return !at_a.empty(); }));
    CHECK(at_a[0] == reply);
    // A wait of 0 still hands over what arrived.
    const auto again = MakeFrame(kMacB, kMacA, 60, 3);
    CHECK(b->Send(again) == int(again.size()));
    bool got = false;
    for (const auto until =
             std::chrono::steady_clock::now() + std::chrono::seconds(5);
         !got && std::chrono::steady_clock::now() < until;) {
      std::vector<uint8_t> frame;
      got = a->Receive(frame, 0) && frame == again;
    }
    CHECK(got);
    REQUIRE(pump([&] {
      return hooks
          .Reported(0,
                    [&](const RelayStatus& status) {
                      return status.state == State::kOnline &&
                             status.players == 1 &&
                             (direct ? status.direct : status.relayed) == 1;
                    })
          .has_value();
    }));
    if (!direct) {
      CHECK(ice.relayed_frames() > 0);
    }
    CHECK(a->healthy());
    CHECK(b->healthy());

    a.reset();
    b.reset();
    std::lock_guard lock(hooks.mutex);
    CHECK(hooks.statuses.back().state == State::kStopped);
  }
}

TEST_CASE("Relay transport leaves while it isn't allowed online", "[relay]") {
  Hooks hooks;
  IceServer ice;
  FakeLobby lobby;
  lobby.ice = [&] { return ice.Ice(false); };
  UseLobby(lobby);

  RelayConfig config = Config(1, PrivateRoom(NewRoomCode()));
  config.lobby = std::string(kLobbyUrl);
  // The fake TURN server and the players share the loopback network.
  config.probe_local_candidates = true;
  std::string error;
  auto b = secure::OpenRelayTransport(kMacB, config, error);
  REQUIRE(b);
  std::atomic<bool> allowed{true};
  config.allowed = [&] { return allowed.load(); };
  auto a = secure::OpenRelayTransport(kMacA, config, error);
  REQUIRE(a);

  std::vector<std::vector<uint8_t>> at_b;
  auto pump = [&](auto&& done, int ms) {
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done() && std::chrono::steady_clock::now() < until) {
      std::vector<uint8_t> frame;
      a->Receive(frame, 2);
      if (b->Receive(frame, 2)) {
        at_b.push_back(frame);
      }
    }
    return done();
  };
  const auto hello = MakeFrame(kMacA, kBroadcast, 60, 1);
  auto reaches_b = [&] {
    a->Send(hello);
    return !at_b.empty();
  };
  REQUIRE(pump(reaches_b, 5000));

  allowed = false;
  std::optional<size_t> stopped;
  REQUIRE(pump(
      [&] {
        stopped = hooks.Reported(0, [](const RelayStatus& status) {
          return status.state == State::kPaused;
        });
        return stopped.has_value();
      },
      5000));
  // B sees A go.
  REQUIRE(pump(
      [&] {
        return hooks
            .Reported(*stopped,
                      [](const RelayStatus& status) {
                        return status.state == State::kOnline &&
                               status.players == 0;
                      })
            .has_value();
      },
      5000));
  at_b.clear();
  CHECK(!pump(reaches_b, 500));
  CHECK(a->healthy());

  allowed = true;
  REQUIRE(pump(reaches_b, 5000));
  CHECK(at_b[0] == hello);
}

TEST_CASE("Relay transport changes rooms while it runs", "[relay]") {
  Hooks hooks;
  IceServer ice;
  FakeLobby lobby;
  lobby.ice = [&] { return ice.Ice(false); };
  UseLobby(lobby);

  const Room room = PrivateRoom(NewRoomCode());
  const Room elsewhere = PublicLobby(kTitle);
  RelayConfig config = Config(1, room);
  config.lobby = std::string(kLobbyUrl);
  // The fake TURN server and the players share the loopback network.
  config.probe_local_candidates = true;
  std::string error;
  auto b = secure::OpenRelayTransport(kMacB, config, error);
  REQUIRE(b);
  std::mutex mutex;
  Room wanted = elsewhere;
  config.room = elsewhere;
  config.current_room = [&]() -> std::optional<Room> {
    std::lock_guard lock(mutex);
    return wanted;
  };
  auto a = secure::OpenRelayTransport(kMacA, config, error);
  REQUIRE(a);
  auto want = [&](const Room& next) {
    std::lock_guard lock(mutex);
    wanted = next;
  };

  std::vector<std::vector<uint8_t>> at_b;
  auto pump = [&](auto&& done, int ms) {
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done() && std::chrono::steady_clock::now() < until) {
      std::vector<uint8_t> frame;
      a->Receive(frame, 2);
      if (b->Receive(frame, 2)) {
        at_b.push_back(frame);
      }
    }
    return done();
  };
  const auto hello = MakeFrame(kMacA, kBroadcast, 60, 1);
  auto reaches_b = [&] {
    a->Send(hello);
    return !at_b.empty();
  };
  // In another room, A can't reach B.
  CHECK(!pump(reaches_b, 1500));

  want(room);
  REQUIRE(pump(reaches_b, 5000));
  CHECK(at_b[0] == hello);

  want(elsewhere);
  REQUIRE(pump(
      [&] {
        at_b.clear();
        return !pump(reaches_b, 300);
      },
      5000));
  CHECK(a->healthy());

  want(room);
  at_b.clear();
  REQUIRE(pump(reaches_b, 5000));
}

TEST_CASE("Relay transport waits out a lobby that isn't there", "[relay]") {
  Hooks hooks;
  RelayConfig config = Config();
  config.lobby = std::string(kLobbyUrl);
  std::string error;
  {
    // No way to reach the lobby at all.
    auto transport = secure::OpenRelayTransport(kMacA, config, error);
    REQUIRE(transport);
    std::vector<uint8_t> frame;
    for (int i = 0; i < 5; ++i) {
      CHECK(!transport->Receive(frame, 50));
      CHECK(transport->Send(MakeFrame(kMacA, kBroadcast, 60, 0)) == 60);
    }
    CHECK(transport->healthy());
  }
  FakeLobby lobby;
  lobby.up = false;
  UseLobby(lobby);
  auto transport = secure::OpenRelayTransport(kMacA, config, error);
  REQUIRE(transport);
  std::vector<uint8_t> frame;
  for (int i = 0; i < 70; ++i) {
    CHECK(!transport->Receive(frame, 50));
    CHECK(transport->Send(MakeFrame(kMacA, kBroadcast, 60, 0)) == 60);
  }
  CHECK(transport->healthy());
  // Backing off: 1 s, then 2 s.
  CHECK(lobby.connects >= 2);
  CHECK(lobby.connects <= 3);
}

TEST_CASE("Relay transport stays out of the lobby until System Link plays",
          "[relay]") {
  Hooks hooks;
  FakeLobby lobby;
  UseLobby(lobby);
  RelayConfig config = Config();
  config.lobby = std::string(kLobbyUrl);
  std::string error;
  std::vector<uint8_t> frame;
  auto wait_for_lobby = [&](secure::PacketTransport& transport) {
    for (int i = 0; i < 100 && !lobby.connects; ++i) {
      transport.Receive(frame, 50);
    }
    return lobby.connects;
  };
  auto quiet = [&](secure::PacketTransport& transport, int ms) {
    for (int i = 0; i < ms / 50; ++i) {
      transport.Receive(frame, 50);
    }
    return lobby.connects == 0;
  };
  {
    // Nothing opens on its own, or says anything.
    auto transport = secure::OpenRelayTransport(kMacA, config, error);
    REQUIRE(transport);
    CHECK(quiet(*transport, 2500));
    {
      std::lock_guard lock(hooks.mutex);
      CHECK(hooks.statuses.empty());
    }
    transport->Activate();
    CHECK(wait_for_lobby(*transport) == 1);
    for (int i = 0; i < 5; ++i) {
      transport->Receive(frame, 50);
    }
    CHECK(hooks
              .Reported(0,
                        [](const RelayStatus& status) {
                          return status.state == State::kOnline;
                        })
              .has_value());
  }
  {
    std::lock_guard lock(hooks.mutex);
    hooks.statuses.clear();
  }

  // A frame to send is System Link play too.
  lobby.connects = 0;
  {
    auto transport = secure::OpenRelayTransport(kMacA, config, error);
    REQUIRE(transport);
    CHECK(transport->Send(MakeFrame(kMacA, kBroadcast, 60, 1)) == 60);
    CHECK(wait_for_lobby(*transport) == 1);
  }

  // A private room the player chose joins at game start.
  lobby.connects = 0;
  {
    RelayConfig chosen = Config(1, PrivateRoom(NewRoomCode()));
    chosen.lobby = std::string(kLobbyUrl);
    auto transport = secure::OpenRelayTransport(kMacA, chosen, error);
    REQUIRE(transport);
    CHECK(wait_for_lobby(*transport) == 1);
  }

  // And so does one chosen while the game runs.
  lobby.connects = 0;
  {
    std::mutex mutex;
    Room wanted = PublicLobby(kTitle);
    RelayConfig later = config;
    later.current_room = [&]() -> std::optional<Room> {
      std::lock_guard lock(mutex);
      return wanted;
    };
    auto transport = secure::OpenRelayTransport(kMacA, later, error);
    REQUIRE(transport);
    CHECK(quiet(*transport, 2500));
    {
      std::lock_guard lock(mutex);
      wanted = PrivateRoom(NewRoomCode());
    }
    CHECK(wait_for_lobby(*transport) == 1);
  }
}

TEST_CASE("Relay transport says when it is paused", "[relay]") {
  Hooks hooks;
  FakeLobby lobby;
  UseLobby(lobby);
  RelayConfig config = Config(1, PrivateRoom(NewRoomCode()));
  config.lobby = std::string(kLobbyUrl);
  std::atomic<bool> allowed{true};
  config.allowed = [&] { return allowed.load(); };
  std::string error;
  auto transport = secure::OpenRelayTransport(kMacA, config, error);
  REQUIRE(transport);
  std::vector<uint8_t> frame;
  auto pump = [&](State state) {
    for (int i = 0; i < 100; ++i) {
      transport->Receive(frame, 50);
      if (hooks.Reported(0, [&](const RelayStatus& status) {
                return status.state == state;
              })) {
        return true;
      }
    }
    return false;
  };
  REQUIRE(pump(State::kOnline));
  allowed = false;
  // Not the Stopped that ends System Link for the game.
  CHECK(pump(State::kPaused));
  CHECK(!hooks.Reported(0, [](const RelayStatus& status) {
    return status.state == State::kStopped;
  }));
}

#if XE_PLATFORM_APPLE
// Against a real lobby over NSURLSession, when XE_SYSTEMLINK_TEST_LOBBY names
// it. With XE_SYSTEMLINK_TEST_ICE_PORT, its TURN credentials are for user1 on a
// fake server at 127.0.0.1 on that port, as from the Worker under
// `wrangler dev`; without, the lobby's own servers are used.
// XE_SYSTEMLINK_TEST_PATHS is "direct" or "relay" for one of the two runs.
TEST_CASE("Relay transports meet in a real lobby", "[.lobby]") {
  const char* lobby = std::getenv("XE_SYSTEMLINK_TEST_LOBBY");
  const char* ice_port = std::getenv("XE_SYSTEMLINK_TEST_ICE_PORT");
  const char* paths = std::getenv("XE_SYSTEMLINK_TEST_PATHS");
  if (!lobby) {
    WARN("XE_SYSTEMLINK_TEST_LOBBY isn't set");
    return;
  }
  for (const bool direct : {true, false}) {
    if (paths && std::string_view(paths) != (direct ? "direct" : "relay")) {
      continue;
    }
    Hooks hooks;
    secure::SetLobbySocketFactory(secure::OpenAppleLobbySocket);
    std::optional<IceServer> ice;
    if (ice_port) {
      ice.emplace(uint16_t(std::atoi(ice_port)));
    }
    RelayConfig config = Config(1, PrivateRoom(NewRoomCode()));
    config.lobby = lobby;
    config.direct_paths = direct;
    config.probe_local_candidates = true;
    std::string error;
    auto a = secure::OpenRelayTransport(kMacA, config, error);
    REQUIRE(a);
    auto b = secure::OpenRelayTransport(kMacB, config, error);
    REQUIRE(b);

    auto online = [&] {
      return hooks
          .Reported(0,
                    [&](const RelayStatus& status) {
                      return status.state == State::kOnline &&
                             status.players == 1 &&
                             (direct ? status.direct : status.relayed) == 1;
                    })
          .has_value();
    };
    // Frames sent before a path opens are lost, so A keeps trying.
    const auto hello = MakeFrame(kMacA, kBroadcast, 60, 1);
    bool got = false;
    for (const auto until =
             std::chrono::steady_clock::now() + std::chrono::seconds(20);
         !(got && online()) && std::chrono::steady_clock::now() < until;) {
      a->Send(hello);
      std::vector<uint8_t> frame;
      a->Receive(frame, 10);
      if (b->Receive(frame, 10) && frame == hello) {
        got = true;
      }
    }
    CHECK(got);
    CHECK(online());
    if (!direct && ice) {
      CHECK(ice->relayed_frames() > 0);
    }
  }
}
#endif  // XE_PLATFORM_APPLE

}  // namespace xe::net::relay::test
