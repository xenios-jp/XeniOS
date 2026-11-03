/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/relay_client.h"

#include <algorithm>
#include <charconv>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "xenia/base/logging.h"

namespace xe::net::relay {
namespace {

constexpr size_t kEthernetHeaderSize = 14;
// Probes go out quickly while two NATs open to each other, then slowly for a
// player that may never be reachable.
constexpr uint32_t kFastProbes = 10;
constexpr uint64_t kFastProbeMs = 1000;
constexpr uint64_t kSlowProbeMs = 10000;
// A direct path carrying frames is checked often so a player that moved falls
// back to the relay before games give up on it.
constexpr uint64_t kActiveMs = 10000;
constexpr uint64_t kActiveKeepaliveMs = 2000;
constexpr uint64_t kActiveLostMs = 6000;
constexpr uint64_t kIdleKeepaliveMs = 10000;
constexpr uint64_t kIdleLostMs = 30000;
// The lobby is waited for this long to open.
constexpr uint64_t kLobbyOpenMs = 20000;
// As many as the lobby lets a player list.
constexpr size_t kMaxListedCandidates = 8;

bool IsGroup(const Mac& mac) { return mac[0] & 1; }

// An address on a private, link-local or loopback network.
bool IsLocal(const Endpoint& endpoint) {
  const auto& a = endpoint.address;
  if (endpoint.v6) {
    const bool loopback =
        std::all_of(a.begin(), a.end() - 1, [](uint8_t b) { return !b; }) &&
        a[15] == 1;
    return loopback || (a[0] & 0xFE) == 0xFC ||
           (a[0] == 0xFE && (a[1] & 0xC0) == 0x80);
  }
  return a[0] == 10 || a[0] == 127 || (a[0] == 172 && (a[1] & 0xF0) == 16) ||
         (a[0] == 192 && a[1] == 168) || (a[0] == 169 && a[1] == 254);
}

// An address worth sending to: not unspecified, multicast or broadcast, and
// with a port.
bool Usable(const Endpoint& endpoint) {
  const auto& a = endpoint.address;
  if (!endpoint.port) {
    return false;
  }
  if (endpoint.v6) {
    return a[0] != 0xFF &&
           std::any_of(a.begin(), a.end(), [](uint8_t b) { return b != 0; });
  }
  return a[0] != 0 && a[0] < 224;
}

bool SameAddress(const Endpoint& a, const Endpoint& b) {
  return a.v6 == b.v6 && a.address == b.address;
}

std::string MacText(const Mac& mac) {
  return fmt::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", mac[0],
                     mac[1], mac[2], mac[3], mac[4], mac[5]);
}

template <typename T>
std::optional<T> ParseHex(std::string_view text, size_t digits) {
  T value = 0;
  if (text.size() != digits) {
    return std::nullopt;
  }
  auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (ec != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::string_view String(const rapidjson::Value& object, const char* name) {
  auto member = object.FindMember(name);
  if (member == object.MemberEnd() || !member->value.IsString()) {
    return {};
  }
  return {member->value.GetString(), member->value.GetStringLength()};
}

std::vector<std::string> Strings(const rapidjson::Value& object,
                                 const char* name) {
  std::vector<std::string> strings;
  auto member = object.FindMember(name);
  if (member == object.MemberEnd() || !member->value.IsArray()) {
    return strings;
  }
  for (const auto& item : member->value.GetArray()) {
    if (item.IsString()) {
      strings.emplace_back(item.GetString(), item.GetStringLength());
    }
  }
  return strings;
}

std::optional<IceServers> ReadIce(const rapidjson::Value& welcome) {
  auto ice = welcome.FindMember("ice");
  if (ice == welcome.MemberEnd() || !ice->value.IsObject()) {
    return std::nullopt;
  }
  IceServers servers;
  servers.stun = Strings(ice->value, "stun");
  auto turn = ice->value.FindMember("turn");
  if (turn != ice->value.MemberEnd() && turn->value.IsObject()) {
    servers.turn = Strings(turn->value, "servers");
    servers.username = std::string(String(turn->value, "username"));
    servers.credential = std::string(String(turn->value, "credential"));
    auto ttl = turn->value.FindMember("ttl");
    if (ttl != turn->value.MemberEnd() && ttl->value.IsUint()) {
      servers.ttl_s = ttl->value.GetUint();
    }
  }
  return servers;
}

std::optional<RelayClient::Listed> ReadListed(const rapidjson::Value& peer) {
  if (!peer.IsObject()) {
    return std::nullopt;
  }
  RelayClient::Listed listed;
  const auto mac = ParseHex<uint64_t>(String(peer, "mac"), 12);
  const auto title = ParseHex<uint32_t>(String(peer, "title"), 8);
  auto version = peer.FindMember("version");
  if (!mac || !title || version == peer.MemberEnd() ||
      !version->value.IsUint()) {
    return std::nullopt;
  }
  for (size_t i = 0; i < listed.mac.size(); ++i) {
    listed.mac[i] = uint8_t(*mac >> (8 * (5 - i)));
  }
  listed.title_id = *title;
  listed.title_version = version->value.GetUint();
  for (const std::string& text : Strings(peer, "candidates")) {
    auto endpoint = ParseEndpoint(text);
    if (endpoint && Usable(*endpoint) &&
        listed.candidates.size() < kMaxListedCandidates) {
      listed.candidates.push_back(*endpoint);
    }
  }
  if (auto relay = ParseEndpoint(String(peer, "relay"));
      relay && Usable(*relay)) {
    listed.relay = *relay;
  }
  return listed;
}

}  // namespace

RelayClient::RelayClient(const Mac& mac, const RelayConfig& config)
    : mac_(mac), config_(config) {
  UpdateStatus(0);
}

bool RelayClient::WantsLobby(uint64_t now_ms) const {
  return !left_ && lobby_ == Lobby::kClosed &&
         !(rejected_ && reject_ == Reject::kVersion) &&
         now_ms >= next_connect_ms_;
}

void RelayClient::LobbyConnecting(uint64_t now_ms) {
  lobby_ = Lobby::kConnecting;
  lobby_heard_ms_ = now_ms;
  if (!down_since_ms_) {
    down_since_ms_ = now_ms;
  }
}

void RelayClient::LobbyOpened(uint64_t now_ms, Outbox& out) {
  lobby_ = Lobby::kOpen;
  lobby_heard_ms_ = now_ms;
  next_lobby_ping_ms_ = now_ms + kLobbyPingMs;
  out.lobby.push_back(Presence(true));
  sent_candidates_ = Candidates();
  sent_relay_ = RelayAddress();
}

void RelayClient::LobbyMessage(std::string_view text, uint64_t now_ms,
                               Outbox& out) {
  if (lobby_ != Lobby::kOpen) {
    return;
  }
  lobby_heard_ms_ = now_ms;
  if (text == "pong") {
    return;
  }
  rapidjson::Document message;
  message.Parse(text.data(), text.size());
  if (message.HasParseError() || !message.IsObject()) {
    return;
  }
  const std::string_view type = String(message, "t");
  if (type == "welcome") {
    if (auto secret = ParseHex<uint64_t>(String(message, "secret"), 16)) {
      OnWelcome(*secret, ReadIce(message), now_ms);
    }
  } else if (type == "peers") {
    auto peers = message.FindMember("peers");
    if (joined_ && peers != message.MemberEnd() && peers->value.IsArray()) {
      std::vector<Listed> listed;
      for (const auto& peer : peers->value.GetArray()) {
        if (auto read = ReadListed(peer)) {
          listed.push_back(std::move(*read));
        }
      }
      OnPeers(std::move(listed), now_ms, out);
    }
  } else if (type == "ice") {
    if (auto ice = ReadIce(message); ice && joined_) {
      OnIce(std::move(*ice), now_ms);
    }
  } else if (type == "reject") {
    const Reject reason =
        ParseReject(String(message, "reason")).value_or(Reject::kBadRequest);
    if (!rejected_ || reject_ != reason) {
      XELOGW("System Link: can't join {}: {}", DescribeRoom(),
             ToString(reason));
    }
    joined_ = false;
    rejected_ = true;
    reject_ = reason;
  }
  UpdateStatus(now_ms);
}

void RelayClient::OnWelcome(uint64_t room_secret, std::optional<IceServers> ice,
                            uint64_t now_ms) {
  // A room that emptied since is another room, with its own secret.
  if (room_secret != room_secret_) {
    peers_.clear();
  }
  room_secret_ = room_secret;
  joined_ = true;
  rejected_ = false;
  failures_ = 0;
  down_since_ms_.reset();
  if (ice) {
    // An allocation that still works keeps its credentials until they're
    // about to expire.
    OnIce(std::move(*ice), now_ms);
  }
  XELOGI("System Link: joined {}", DescribeRoom());
}

void RelayClient::OnIce(IceServers ice, uint64_t now_ms) {
  ice_ = std::move(ice);
  ice_expires_ms_ = ice_->ttl_s ? now_ms + uint64_t(ice_->ttl_s) * 1000 : 0;
  // New credentials are worth another try at the relay.
  turn_refused_ = false;
}

bool RelayClient::WantsTurn() const {
  return ice_ && !ice_->turn.empty() && !ice_->username.empty() &&
         !turn_refused_;
}

void RelayClient::OnPeers(std::vector<Listed> listed, uint64_t now_ms,
                          Outbox& out) {
  std::map<Mac, Peer> updated;
  for (Listed& entry : listed) {
    if (entry.mac == mac_) {
      continue;
    }
    Peer peer;
    auto old = peers_.find(entry.mac);
    if (old != peers_.end()) {
      peer = old->second;
      // New addresses are probed straight away. A direct path that works
      // keeps working until it doesn't.
      if (peer.listed.candidates != entry.candidates) {
        peer.probes = 0;
        peer.next_probe_ms = now_ms;
      }
    } else {
      peer.next_probe_ms = now_ms;
    }
    peer.listed = std::move(entry);
    updated[peer.listed.mac] = std::move(peer);
  }
  peers_ = std::move(updated);
  PermitRelays(now_ms, out);
}

void RelayClient::PermitRelays(uint64_t now_ms, Outbox& out) {
  if (!turn_) {
    return;
  }
  std::vector<Endpoint> relays;
  for (const auto& [mac, peer] : peers_) {
    if (peer.listed.relay) {
      relays.push_back(*peer.listed.relay);
    }
  }
  turn_->Permit(relays, now_ms, out.datagrams);
}

void RelayClient::LobbyClosed(uint64_t now_ms) {
  lobby_ = Lobby::kClosed;
  joined_ = false;
  if (!down_since_ms_) {
    down_since_ms_ = now_ms;
  }
  next_connect_ms_ =
      now_ms + (rejected_
                    ? kRejectRetryMs
                    : std::min<uint64_t>(kMaxBackoffMs,
                                         1000ull << std::min(failures_, 4u)));
  ++failures_;
  UpdateStatus(now_ms);
}

bool RelayClient::LobbyStale(uint64_t now_ms) const {
  return (lobby_ == Lobby::kOpen && now_ms - lobby_heard_ms_ > kLobbyLostMs) ||
         (lobby_ == Lobby::kConnecting &&
          now_ms - lobby_heard_ms_ > kLobbyOpenMs);
}

std::optional<std::string> RelayClient::WantedIceServer(uint64_t now_ms) const {
  if (!ice_ || left_ || looking_up_ice_ || turn_ || now_ms < next_ice_ms_) {
    return std::nullopt;
  }
  if (WantsTurn()) {
    return ice_->turn.front();
  }
  if (!ice_->stun.empty()) {
    return ice_->stun.front();
  }
  // TURN servers answer STUN too.
  if (!ice_->turn.empty()) {
    return ice_->turn.front();
  }
  return std::nullopt;
}

void RelayClient::UseIceServer(std::optional<Endpoint> server, uint64_t now_ms,
                               Outbox& out) {
  looking_up_ice_ = false;
  if (!server || !ice_ || left_) {
    next_ice_ms_ = now_ms + kIceRetryMs;
    return;
  }
  std::optional<TurnClient::Credentials> credentials;
  using_turn_ = WantsTurn();
  turn_expires_ms_ = 0;
  if (using_turn_) {
    credentials = TurnClient::Credentials{ice_->username, ice_->credential};
    turn_expires_ms_ = ice_expires_ms_;
  }
  turn_ = std::make_unique<TurnClient>(*server, std::move(credentials));
  turn_->Tick(now_ms, out.datagrams);
  PermitRelays(now_ms, out);
}

void RelayClient::SetHostCandidates(std::vector<Endpoint> candidates,
                                    Outbox& out) {
  host_candidates_ = std::move(candidates);
  UpdatePresence(out);
}

std::vector<Endpoint> RelayClient::Candidates() const {
  std::vector<Endpoint> candidates;
  auto add = [&](const Endpoint& endpoint) {
    if (Usable(endpoint) && candidates.size() < kMaxCandidates &&
        std::find(candidates.begin(), candidates.end(), endpoint) ==
            candidates.end()) {
      candidates.push_back(endpoint);
    }
  };
  if (turn_ && turn_->mapped()) {
    add(*turn_->mapped());
  }
  for (const Endpoint& endpoint : host_candidates_) {
    add(endpoint);
  }
  return candidates;
}

std::optional<Endpoint> RelayClient::RelayAddress() const {
  return turn_ ? turn_->relayed() : std::nullopt;
}

std::string RelayClient::Presence(bool join) const {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> json(buffer);
  json.StartObject();
  json.Key("t");
  json.String(join ? "join" : "update");
  if (join) {
    json.Key("v");
    json.Uint(1);
    json.Key("mac");
    json.String(fmt::format("{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", mac_[0],
                            mac_[1], mac_[2], mac_[3], mac_[4], mac_[5])
                    .c_str());
    json.Key("title");
    json.String(fmt::format("{:08X}", config_.title_id).c_str());
    json.Key("version");
    json.Uint(config_.title_version);
  }
  json.Key("candidates");
  json.StartArray();
  for (const Endpoint& candidate : Candidates()) {
    json.String(ToString(candidate).c_str());
  }
  json.EndArray();
  if (auto relay = RelayAddress()) {
    json.Key("relay");
    json.String(ToString(*relay).c_str());
  }
  json.EndObject();
  return buffer.GetString();
}

void RelayClient::UpdatePresence(Outbox& out) {
  if (!joined_ || lobby_ != Lobby::kOpen) {
    return;
  }
  auto candidates = Candidates();
  auto relay = RelayAddress();
  if (candidates != sent_candidates_ || relay != sent_relay_) {
    out.lobby.push_back(Presence(false));
    sent_candidates_ = std::move(candidates);
    sent_relay_ = relay;
  }
}

bool RelayClient::Send(std::span<const uint8_t> ethernet, uint64_t now_ms,
                       Outbox& out) {
  if (!room_secret_ || left_ || ethernet.size() < kEthernetHeaderSize) {
    return false;
  }
  Mac destination;
  std::copy_n(ethernet.begin(), destination.size(), destination.begin());
  std::vector<std::vector<uint8_t>> fragments;
  for (const Frame& fragment :
       Fragment(ethernet, room_secret_, next_frame_id_++)) {
    fragments.push_back(Encode(fragment));
  }
  if (fragments.empty()) {
    return false;
  }
  if (IsGroup(destination)) {
    bool sent = false;
    for (auto& [mac, peer] : peers_) {
      sent |= SendTo(peer, fragments, out);
    }
    return sent;
  }
  auto peer = peers_.find(destination);
  if (peer == peers_.end()) {
    return false;
  }
  peer->second.sent_ms = now_ms;
  return SendTo(peer->second, fragments, out);
}

bool RelayClient::SendTo(Peer& peer,
                         const std::vector<std::vector<uint8_t>>& fragments,
                         Outbox& out) {
  if (config_.direct_paths && peer.direct) {
    for (const auto& fragment : fragments) {
      out.datagrams.push_back({*peer.direct, fragment});
    }
    return true;
  }
  if (!turn_ || !peer.listed.relay) {
    return false;
  }
  bool sent = true;
  for (const auto& fragment : fragments) {
    sent &= turn_->Send(*peer.listed.relay, fragment, out.datagrams);
  }
  return sent;
}

std::optional<std::vector<uint8_t>> RelayClient::Receive(
    std::span<const uint8_t> data, const Endpoint& from, uint64_t now_ms,
    Outbox& out) {
  if (left_) {
    return std::nullopt;
  }
  if (turn_ && from == turn_->server()) {
    auto relayed = turn_->Receive(data, now_ms, out.datagrams);
    if (!relayed) {
      return std::nullopt;
    }
    return OnPeerDatagram(relayed->data, relayed->peer, true, now_ms, out);
  }
  return OnPeerDatagram(data, from, false, now_ms, out);
}

std::optional<std::vector<uint8_t>> RelayClient::OnPeerDatagram(
    std::span<const uint8_t> data, const Endpoint& from, bool relayed,
    uint64_t now_ms, Outbox& out) {
  // Players keep reaching each other while the lobby is away.
  if (!room_secret_) {
    return std::nullopt;
  }
  auto message = Decode(data);
  if (!message) {
    return std::nullopt;
  }
  if (auto* probe = std::get_if<Probe>(&*message)) {
    if (!relayed && config_.direct_paths) {
      OnProbe(*probe, from, now_ms, out);
    }
    return std::nullopt;
  }
  auto& frame = std::get<Frame>(*message);
  if (frame.key != room_secret_) {
    return std::nullopt;
  }
  auto peer = peers_.find(frame.source);
  if (peer == peers_.end()) {
    return std::nullopt;
  }
  if (!relayed && peer->second.direct == from) {
    peer->second.heard_ms = now_ms;
  }
  const Mac source = frame.source;
  auto whole = reassembler_.Add(std::move(frame), now_ms);
  if (!whole || whole->size() < kEthernetHeaderSize ||
      !std::equal(source.begin(), source.end(), whole->begin() + 6) ||
      source == mac_) {
    return std::nullopt;
  }
  return whole;
}

void RelayClient::OnProbe(const Probe& probe, const Endpoint& from,
                          uint64_t now_ms, Outbox& out) {
  if (probe.room_secret != room_secret_ || probe.to != mac_ ||
      probe.from == mac_) {
    return;
  }
  auto found = peers_.find(probe.from);
  if (!probe.reply) {
    // Answered even before the lobby names the player, so its side of the
    // path opens as soon as it can.
    out.datagrams.push_back({from, Encode(Probe{room_secret_, mac_, probe.from,
                                                probe.nonce, true})});
    if (found != peers_.end()) {
      Peer& peer = found->second;
      const auto& candidates = peer.listed.candidates;
      if (peer.direct == from) {
        peer.heard_ms = now_ms;
      } else if (std::find(candidates.begin(), candidates.end(), from) ==
                     candidates.end() &&
                 std::any_of(candidates.begin(), candidates.end(),
                             [&](const Endpoint& candidate) {
                               return SameAddress(candidate, from);
                             })) {
        // A NAT that maps each destination to its own port. Only another port
        // of a listed address, so no one else can name where the peer is.
        peer.reflexive = from;
      }
    }
    return;
  }
  if (found == peers_.end()) {
    return;
  }
  Peer& peer = found->second;
  if (!probe.nonce ||
      std::find(peer.nonces.begin(), peer.nonces.end(), probe.nonce) ==
          peer.nonces.end() ||
      !Probed(peer, from)) {
    return;
  }
  if (peer.direct != from) {
    // A path that works is kept over another that answers, unless the other
    // stays on the LAN.
    const bool working =
        peer.direct && now_ms - peer.heard_ms < 2 * kActiveKeepaliveMs;
    if (working && (!IsLocal(from) || IsLocal(*peer.direct))) {
      return;
    }
    XELOGI("System Link: reaching {} directly", MacText(peer.listed.mac));
    peer.direct = from;
    peer.next_probe_ms = now_ms + kActiveKeepaliveMs;
  }
  peer.heard_ms = now_ms;
  UpdateStatus(now_ms);
}

bool RelayClient::Probed(const Peer& peer, const Endpoint& from) {
  const auto& candidates = peer.listed.candidates;
  return peer.direct == from || peer.reflexive == from ||
         std::find(candidates.begin(), candidates.end(), from) !=
             candidates.end();
}

void RelayClient::NewProbeRound(Peer& peer) {
  peer.nonces = {RandomId(), peer.nonces[0]};
}

bool RelayClient::SharesPublicAddress(const Peer& peer) const {
  if (!turn_ || !turn_->mapped()) {
    return false;
  }
  const Endpoint& mine = *turn_->mapped();
  return std::any_of(peer.listed.candidates.begin(),
                     peer.listed.candidates.end(), [&](const Endpoint& theirs) {
                       return theirs.v6 == mine.v6 &&
                              theirs.address == mine.address;
                     });
}

void RelayClient::SendProbe(const Peer& peer, const Endpoint& to, Outbox& out) {
  out.datagrams.push_back({to, Encode(Probe{room_secret_, mac_, peer.listed.mac,
                                            peer.nonces[0], false})});
}

void RelayClient::Tick(uint64_t now_ms, Outbox& out) {
  if (left_) {
    return;
  }
  if (lobby_ != Lobby::kOpen && !down_since_ms_) {
    down_since_ms_ = now_ms;
  }
  if (lobby_ == Lobby::kOpen && now_ms >= next_lobby_ping_ms_) {
    out.lobby.push_back("ping");
    next_lobby_ping_ms_ = now_ms + kLobbyPingMs;
  }
  if (turn_) {
    turn_->Tick(now_ms, out.datagrams);
    switch (turn_->failure()) {
      case TurnClient::Failure::kNone:
        RenewIce(now_ms, out);
        break;
      case TurnClient::Failure::kRefused:
        // A relay that refused is passed over for plain STUN, which still
        // finds the public address for direct paths, until there are new
        // credentials, and the other way around.
        turn_refused_ = using_turn_;
        next_ice_ask_ms_ = std::max(next_ice_ask_ms_, now_ms + kRefusedAskMs);
        next_ice_ms_ = now_ms + kIceRetryMs;
        turn_.reset();
        break;
      case TurnClient::Failure::kInUse:
        next_ice_ms_ = now_ms + kIceRetryMs;
        turn_.reset();
        break;
      default:
        // Most likely the network changed, so a new allocation is made now.
        next_ice_ms_ = now_ms + kReallocateMs;
        turn_.reset();
        break;
    }
  }
  UpdatePresence(out);
  if (room_secret_ && config_.direct_paths) {
    TickPeers(now_ms, out);
  }
  UpdateStatus(now_ms);
}

void RelayClient::RenewIce(uint64_t now_ms, Outbox& out) {
  const bool asks =
      lobby_ == Lobby::kOpen && joined_ && now_ms >= next_ice_ask_ms_;
  if (!using_turn_) {
    // A STUN client gives way once there are credentials worth trying, and
    // asks for some now and then after the relay refused.
    if (WantsTurn()) {
      turn_.reset();
      next_ice_ms_ = now_ms;
    } else if (turn_refused_ && asks) {
      out.lobby.push_back(R"({"t":"ice"})");
      next_ice_ask_ms_ = now_ms + kRefusedAskMs;
    }
    return;
  }
  if (!turn_expires_ms_ || now_ms + kIceRenewMs < turn_expires_ms_) {
    return;
  }
  // An allocation keeps the credentials it was made with, so newer ones need
  // a new allocation, made before the old one stops refreshing.
  if (ice_expires_ms_ > turn_expires_ms_) {
    turn_->Release(out.datagrams);
    turn_.reset();
    next_ice_ms_ = now_ms + kReallocateMs;
  } else if (asks) {
    out.lobby.push_back(R"({"t":"ice"})");
    next_ice_ask_ms_ = now_ms + kIceAskMs;
  }
}

void RelayClient::TickPeers(uint64_t now_ms, Outbox& out) {
  for (auto& [mac, peer] : peers_) {
    if (peer.direct) {
      const bool active = now_ms - peer.sent_ms < kActiveMs;
      if (now_ms - peer.heard_ms <= (active ? kActiveLostMs : kIdleLostMs)) {
        if (now_ms >= peer.next_probe_ms) {
          NewProbeRound(peer);
          SendProbe(peer, *peer.direct, out);
          peer.next_probe_ms =
              now_ms + (active ? kActiveKeepaliveMs : kIdleKeepaliveMs);
        }
        continue;
      }
      XELOGI("System Link: lost the direct path to {}; using the relay",
             MacText(mac));
      peer.direct.reset();
      peer.probes = 0;
      peer.next_probe_ms = now_ms;
    }
    if (now_ms < peer.next_probe_ms) {
      continue;
    }
    // Local addresses are only worth trying behind the same public address,
    // and trying them makes iOS ask for local network access.
    const bool same_network = SharesPublicAddress(peer);
    NewProbeRound(peer);
    for (const Endpoint& candidate : peer.listed.candidates) {
      if (same_network || !IsLocal(candidate)) {
        SendProbe(peer, candidate, out);
      }
    }
    const auto& candidates = peer.listed.candidates;
    if (peer.reflexive && std::find(candidates.begin(), candidates.end(),
                                    *peer.reflexive) == candidates.end()) {
      SendProbe(peer, *peer.reflexive, out);
    }
    ++peer.probes;
    peer.next_probe_ms =
        now_ms + (peer.probes < kFastProbes ? kFastProbeMs : kSlowProbeMs);
  }
}

void RelayClient::Leave(Outbox& out) {
  if (turn_) {
    turn_->Release(out.datagrams);
    turn_.reset();
  }
  left_ = true;
  joined_ = false;
  lobby_ = Lobby::kClosed;
  peers_.clear();
}

void RelayClient::UpdateStatus(uint64_t now_ms) {
  RelayStatus status;
  if (joined_) {
    status.state = RelayStatus::State::kOnline;
  } else if (rejected_) {
    status.state = RelayStatus::State::kRejected;
    status.reject = reject_;
  } else if (down_since_ms_ && now_ms - *down_since_ms_ > kOfflineMs) {
    status.state = RelayStatus::State::kOffline;
  }
  if (joined_) {
    const bool relaying = turn_ && turn_->relayed();
    status.players = peers_.size();
    for (const auto& [mac, peer] : peers_) {
      status.direct += peer.direct.has_value();
      status.relayed += !peer.direct && peer.listed.relay && relaying;
      status.version_mismatch |=
          peer.listed.title_id != config_.title_id ||
          peer.listed.title_version != config_.title_version;
    }
  }
  status_ = status;
}

std::string RelayClient::DescribeRoom() const {
  const std::string name = RoomName(config_.room);
  // A private room's code lets anyone in, so it stays out of logs.
  return name.starts_with("lobby:")
             ? fmt::format("the public lobby for {:08X}", config_.title_id)
             : std::string("a private room");
}

}  // namespace xe::net::relay
