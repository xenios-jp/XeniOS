/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_STUN_H_
#define XENIA_NET_STUN_H_

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/net/relay_protocol.h"

// The STUN (RFC 8489) and TURN (RFC 8656) messages a client sends and reads
// over UDP.
namespace xe::net::stun {

using relay::Endpoint;

inline constexpr uint32_t kMagicCookie = 0x2112A442;
inline constexpr size_t kHeaderSize = 20;

enum class Method : uint16_t {
  kBinding = 0x001,
  kAllocate = 0x003,
  kRefresh = 0x004,
  kSend = 0x006,
  kData = 0x007,
  kCreatePermission = 0x008,
};

enum class Class : uint8_t {
  kRequest = 0,
  kIndication = 1,
  kSuccess = 2,
  kError = 3,
};

enum class Attribute : uint16_t {
  kUsername = 0x0006,
  kMessageIntegrity = 0x0008,
  kErrorCode = 0x0009,
  kLifetime = 0x000D,
  kXorPeerAddress = 0x0012,
  kData = 0x0013,
  kRealm = 0x0014,
  kNonce = 0x0015,
  kXorRelayedAddress = 0x0016,
  kRequestedTransport = 0x0019,
  kXorMappedAddress = 0x0020,
  kFingerprint = 0x8028,
};

inline constexpr uint8_t kTransportUdp = 17;

using TransactionId = std::array<uint8_t, 12>;
TransactionId NewTransactionId();

// Builds a message one attribute at a time.
class Writer {
 public:
  Writer(Method method, Class kind, const TransactionId& transaction);

  Writer& Add(Attribute type, std::span<const uint8_t> value);
  Writer& Add(Attribute type, std::string_view value);
  Writer& AddUint32(Attribute type, uint32_t value);
  // XORed with the magic cookie and the transaction, as XOR-*-ADDRESS are.
  Writer& AddAddress(Attribute type, const Endpoint& endpoint);

  // The message, ending in a MESSAGE-INTEGRITY made with |key| unless it's
  // empty.
  std::vector<uint8_t> Finish(std::span<const uint8_t> key = {});

 private:
  void SetLength(size_t length);

  TransactionId transaction_;
  std::vector<uint8_t> data_;
};

struct Message {
  Method method = Method::kBinding;
  Class kind = Class::kRequest;
  TransactionId transaction{};
  std::optional<Endpoint> mapped;
  std::optional<Endpoint> relayed;
  std::optional<Endpoint> peer;
  std::optional<uint32_t> lifetime;
  // An error response's code, such as 401.
  uint16_t error = 0;
  std::string username;
  std::string realm;
  std::string nonce;
  std::vector<uint8_t> data;
  // Where MESSAGE-INTEGRITY starts, or 0 without one.
  size_t integrity = 0;
};

// Any well-formed message. Attributes it doesn't know are skipped, and so is
// everything after MESSAGE-INTEGRITY.
std::optional<Message> Read(std::span<const uint8_t> datagram);

// Whether the MESSAGE-INTEGRITY of |datagram|, read as |message|, was made
// with |key|.
bool CheckIntegrity(std::span<const uint8_t> datagram, const Message& message,
                    std::span<const uint8_t> key);

// MD5(username ":" realm ":" password), the long-term credential key.
std::array<uint8_t, 16> LongTermKey(std::string_view username,
                                    std::string_view realm,
                                    std::string_view password);

}  // namespace xe::net::stun

#endif  // XENIA_NET_STUN_H_
