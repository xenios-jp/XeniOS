/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_RELAY_PROTOCOL_H_
#define XENIA_NET_RELAY_PROTOCOL_H_

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "xenia/net/xnet_secure.h"

// System Link over the internet: the instances in one room meet in a lobby
// and exchange their Ethernet frames directly where
// their NATs let them, and through a TURN relay otherwise. These are the
// messages they send each other.
namespace xe::net::relay {

using secure::Mac;

inline constexpr uint8_t kVersion = 1;
inline constexpr std::string_view kDefaultLobby =
    "wss://xenios-systemlink.xenios.workers.dev";

// Frames longer than this are split so no datagram is larger than an IPv6
// path is guaranteed to carry unfragmented (1280 minus the IPv6 and UDP
// headers, less ours).
inline constexpr size_t kFragmentPayload = 1200;
inline constexpr size_t kMaxFragments = 4;

// Host byte order port, address in network order. IPv4 uses the first 4 bytes.
struct Endpoint {
  std::array<uint8_t, 16> address{};
  bool v6 = false;
  uint16_t port = 0;
  auto operator<=>(const Endpoint&) const = default;
};
// "a.b.c.d:port" or "[v6]:port".
std::string ToString(const Endpoint& endpoint);
std::optional<Endpoint> ParseEndpoint(std::string_view text);

struct Datagram {
  Endpoint to;
  std::vector<uint8_t> data;
};

// Unpredictable, never 0.
uint64_t RandomId();

// Up to 16 bytes, zero padded.
using Room = std::array<uint8_t, 16>;
// The room everyone playing |title_id| meets in.
Room PublicLobby(uint32_t title_id);
// A private room named by a code from NewRoomCode.
Room PrivateRoom(std::string_view code);
std::string RoomName(const Room& room);
// "lobby/<title ID>" or "room/<code>", the room's path in the lobby.
std::string RoomPath(const Room& room);

inline constexpr size_t kRoomCodeLength = 6;
// Crockford base32, so a code read aloud or typed can't be mistaken.
std::string NewRoomCode();
// Uppercases and drops spaces and dashes, reads O as 0 and I or L as 1, and
// rejects anything that isn't a code.
std::optional<std::string> NormalizeRoomCode(std::string_view text);

// Why the lobby turned a player away.
enum class Reject : uint8_t {
  kFull = 1,
  kDuplicateMac = 2,
  kVersion = 3,
  kBadRequest = 4,
};
std::string_view ToString(Reject reason);
// The lobby's name for a reason, such as "full".
std::optional<Reject> ParseReject(std::string_view name);

// |key| is the room's secret. Fragments of one frame share |id|.
struct Frame {
  uint64_t key = 0;
  Mac source{};
  Mac destination{};
  uint8_t index = 0;
  uint8_t count = 1;
  uint16_t id = 0;
  std::vector<uint8_t> payload;
};
// Between members, to open and keep a direct path. A reply carries the
// probe's |nonce|.
struct Probe {
  uint64_t room_secret = 0;
  Mac from{};
  Mac to{};
  uint64_t nonce = 0;
  bool reply = false;
};

// The wire type of each is its index plus one.
using Message = std::variant<Frame, Probe>;

std::vector<uint8_t> Encode(const Message& message);
std::optional<Message> Decode(std::span<const uint8_t> datagram);

// Splits an Ethernet frame into Frame messages of at most kFragmentPayload.
// Empty for a frame too long to send.
std::vector<Frame> Fragment(std::span<const uint8_t> ethernet, uint64_t key,
                            uint16_t id);

// Puts fragments back together. Incomplete frames are dropped after
// kTimeoutMs or when kLimit frames are pending.
class Reassembler {
 public:
  static constexpr uint64_t kTimeoutMs = 250;
  static constexpr size_t kLimit = 64;

  // The whole Ethernet frame once its last fragment has arrived.
  std::optional<std::vector<uint8_t>> Add(Frame&& fragment, uint64_t now_ms);

 private:
  struct Pending {
    Mac source{};
    uint16_t id = 0;
    uint8_t count = 0;
    uint8_t received = 0;
    uint64_t started_ms = 0;
    std::array<std::vector<uint8_t>, kMaxFragments> parts;
  };
  std::vector<Pending> pending_;
};

}  // namespace xe::net::relay

#endif  // XENIA_NET_RELAY_PROTOCOL_H_
