/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/stun.h"

#include <algorithm>
#include <cstring>

// This header has no C++ linkage guard of its own.
extern "C" {
#include "third_party/mbedtls/include/mbedtls/constant_time.h"
}
#include "third_party/mbedtls/include/mbedtls/md.h"
#include "xenia/net/xnet_secure.h"

namespace xe::net::stun {
namespace {

constexpr size_t kIntegritySize = 20;
constexpr uint8_t kFamilyV4 = 0x01;
constexpr uint8_t kFamilyV6 = 0x02;

uint16_t Read16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

uint32_t Read32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         p[3];
}

void Put16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  Put16(out, uint16_t(value >> 16));
  Put16(out, uint16_t(value));
}

// The method's 12 bits with the class's 2 bits between them.
uint16_t MessageType(Method method, Class kind) {
  const uint16_t m = uint16_t(method);
  const uint16_t c = uint16_t(kind);
  return uint16_t((m & 0x000F) | (m & 0x0070) << 1 | (m & 0x0F80) << 2 |
                  (c & 1) << 4 | (c & 2) << 7);
}

// The magic cookie, then the transaction: what XOR-*-ADDRESS are XORed with.
std::array<uint8_t, 16> XorMask(const TransactionId& transaction) {
  std::array<uint8_t, 16> mask{};
  mask[0] = uint8_t(kMagicCookie >> 24);
  mask[1] = uint8_t(kMagicCookie >> 16);
  mask[2] = uint8_t(kMagicCookie >> 8);
  mask[3] = uint8_t(kMagicCookie);
  std::copy(transaction.begin(), transaction.end(), mask.begin() + 4);
  return mask;
}

std::optional<Endpoint> ReadAddress(std::span<const uint8_t> value,
                                    const TransactionId& transaction) {
  if (value.size() < 4) {
    return std::nullopt;
  }
  const auto mask = XorMask(transaction);
  Endpoint endpoint;
  endpoint.port = uint16_t(Read16(&value[2]) ^ (kMagicCookie >> 16));
  const size_t size = value[1] == kFamilyV4   ? 4
                      : value[1] == kFamilyV6 ? 16
                                              : 0;
  if (!size || value.size() != 4 + size) {
    return std::nullopt;
  }
  endpoint.v6 = size == 16;
  for (size_t i = 0; i < size; ++i) {
    endpoint.address[i] = value[4 + i] ^ mask[i];
  }
  return endpoint;
}

std::array<uint8_t, kIntegritySize> Hmac(std::span<const uint8_t> key,
                                         std::span<const uint8_t> data) {
  std::array<uint8_t, kIntegritySize> mac{};
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key.data(),
                  key.size(), data.data(), data.size(), mac.data());
  return mac;
}

}  // namespace

TransactionId NewTransactionId() {
  TransactionId transaction;
  Random(transaction);
  return transaction;
}

Writer::Writer(Method method, Class kind, const TransactionId& transaction)
    : transaction_(transaction) {
  Put16(data_, MessageType(method, kind));
  Put16(data_, 0);
  Put32(data_, kMagicCookie);
  data_.insert(data_.end(), transaction.begin(), transaction.end());
}

Writer& Writer::Add(Attribute type, std::span<const uint8_t> value) {
  Put16(data_, uint16_t(type));
  Put16(data_, uint16_t(value.size()));
  data_.insert(data_.end(), value.begin(), value.end());
  data_.resize((data_.size() + 3) & ~size_t(3));
  return *this;
}

Writer& Writer::Add(Attribute type, std::string_view value) {
  return Add(type, std::span(reinterpret_cast<const uint8_t*>(value.data()),
                             value.size()));
}

Writer& Writer::AddUint32(Attribute type, uint32_t value) {
  const uint8_t bytes[] = {uint8_t(value >> 24), uint8_t(value >> 16),
                           uint8_t(value >> 8), uint8_t(value)};
  return Add(type, bytes);
}

Writer& Writer::AddAddress(Attribute type, const Endpoint& endpoint) {
  const auto mask = XorMask(transaction_);
  const size_t size = endpoint.v6 ? 16 : 4;
  std::vector<uint8_t> value;
  value.push_back(0);
  value.push_back(endpoint.v6 ? kFamilyV6 : kFamilyV4);
  Put16(value, uint16_t(endpoint.port ^ (kMagicCookie >> 16)));
  for (size_t i = 0; i < size; ++i) {
    value.push_back(endpoint.address[i] ^ mask[i]);
  }
  return Add(type, value);
}

void Writer::SetLength(size_t length) {
  data_[2] = uint8_t(length >> 8);
  data_[3] = uint8_t(length);
}

std::vector<uint8_t> Writer::Finish(std::span<const uint8_t> key) {
  if (!key.empty()) {
    // The length covers MESSAGE-INTEGRITY itself, though the HMAC doesn't.
    SetLength(data_.size() - kHeaderSize + 4 + kIntegritySize);
    const auto mac = Hmac(key, data_);
    Add(Attribute::kMessageIntegrity, mac);
  }
  SetLength(data_.size() - kHeaderSize);
  return std::move(data_);
}

std::optional<Message> Read(std::span<const uint8_t> datagram) {
  if (datagram.size() < kHeaderSize || datagram[0] & 0xC0 ||
      Read32(&datagram[4]) != kMagicCookie ||
      Read16(&datagram[2]) != datagram.size() - kHeaderSize ||
      datagram.size() % 4) {
    return std::nullopt;
  }
  const uint16_t type = Read16(&datagram[0]);
  Message message;
  message.method =
      Method((type & 0x000F) | (type & 0x00E0) >> 1 | (type & 0x3E00) >> 2);
  message.kind = Class((type >> 4 & 1) | (type >> 7 & 2));
  std::copy_n(&datagram[8], message.transaction.size(),
              message.transaction.begin());
  size_t offset = kHeaderSize;
  while (offset + 4 <= datagram.size()) {
    const auto attribute = Attribute(Read16(&datagram[offset]));
    const size_t size = Read16(&datagram[offset + 2]);
    if (offset + 4 + size > datagram.size()) {
      return std::nullopt;
    }
    const auto value = datagram.subspan(offset + 4, size);
    switch (attribute) {
      case Attribute::kXorMappedAddress:
        message.mapped = ReadAddress(value, message.transaction);
        break;
      case Attribute::kXorRelayedAddress:
        message.relayed = ReadAddress(value, message.transaction);
        break;
      case Attribute::kXorPeerAddress:
        message.peer = ReadAddress(value, message.transaction);
        break;
      case Attribute::kLifetime:
        if (size == 4) {
          message.lifetime = Read32(value.data());
        }
        break;
      case Attribute::kErrorCode:
        if (size >= 4) {
          message.error = uint16_t((value[2] & 7) * 100 + value[3]);
        }
        break;
      case Attribute::kUsername:
        message.username.assign(value.begin(), value.end());
        break;
      case Attribute::kRealm:
        message.realm.assign(value.begin(), value.end());
        break;
      case Attribute::kNonce:
        message.nonce.assign(value.begin(), value.end());
        break;
      case Attribute::kData:
        message.data.assign(value.begin(), value.end());
        break;
      case Attribute::kMessageIntegrity:
        if (size != kIntegritySize) {
          return std::nullopt;
        }
        message.integrity = offset;
        return message;
      default:
        break;
    }
    offset += 4 + ((size + 3) & ~size_t(3));
  }
  return message;
}

bool CheckIntegrity(std::span<const uint8_t> datagram, const Message& message,
                    std::span<const uint8_t> key) {
  const size_t at = message.integrity;
  if (!at || at + 4 + kIntegritySize > datagram.size()) {
    return false;
  }
  std::vector<uint8_t> covered(datagram.begin(), datagram.begin() + at);
  const size_t length = at - kHeaderSize + 4 + kIntegritySize;
  covered[2] = uint8_t(length >> 8);
  covered[3] = uint8_t(length);
  const auto mac = Hmac(key, covered);
  return !mbedtls_ct_memcmp(mac.data(), &datagram[at + 4], mac.size());
}

std::array<uint8_t, 16> LongTermKey(std::string_view username,
                                    std::string_view realm,
                                    std::string_view password) {
  std::string text;
  text.reserve(username.size() + realm.size() + password.size() + 2);
  text.append(username).append(":").append(realm).append(":").append(password);
  std::array<uint8_t, 16> key{};
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_MD5),
             reinterpret_cast<const uint8_t*>(text.data()), text.size(),
             key.data());
  return key;
}

}  // namespace xe::net::stun
