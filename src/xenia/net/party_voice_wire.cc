/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/party_voice_wire.h"

#include <random>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/net/relay_protocol.h"

namespace xe::net::party_voice {

namespace {

constexpr size_t kEthernetHeader = 14;
constexpr uint8_t kMagic[3] = {'X', 'P', 'V'};
constexpr uint8_t kVersion = 1;
// Magic, version, kind, flags, XUID, sequence.
constexpr size_t kHeader = 3 + 1 + 1 + 1 + 8 + 2;
constexpr uint8_t kFlagMuted = 0x01;
// Crockford base32, as the lobby's room codes.
constexpr std::string_view kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

}  // namespace

std::optional<std::string> VoiceRoomCode(std::string_view party_code) {
  const auto normalized = relay::NormalizeRoomCode(party_code);
  if (!normalized) {
    return std::nullopt;
  }
  const std::string text = "party-voice:" + *normalized;
  unsigned char digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(text.data()),
                 text.size(), digest, 0);
  uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) {
    bits = (bits << 8) | digest[i];
  }
  std::string code;
  for (size_t i = 0; i < relay::kRoomCodeLength; ++i) {
    code += kAlphabet[bits & 31];
    bits >>= 5;
  }
  return code;
}

std::optional<std::vector<uint8_t>> EncodeFrame(const Packet& packet,
                                                const secure::Mac& source) {
  if (packet.audio.size() > kMaxAudioBytes) {
    return std::nullopt;
  }
  std::vector<uint8_t> frame;
  frame.reserve(kEthernetHeader + kHeader + packet.audio.size());
  frame.insert(frame.end(), 6, 0xFF);
  frame.insert(frame.end(), source.begin(), source.end());
  frame.push_back(uint8_t(kEtherType >> 8));
  frame.push_back(uint8_t(kEtherType));
  frame.insert(frame.end(), std::begin(kMagic), std::end(kMagic));
  frame.push_back(kVersion);
  frame.push_back(uint8_t(packet.kind));
  frame.push_back(packet.muted ? kFlagMuted : 0);
  for (int shift = 56; shift >= 0; shift -= 8) {
    frame.push_back(uint8_t(packet.xuid >> shift));
  }
  frame.push_back(uint8_t(packet.sequence >> 8));
  frame.push_back(uint8_t(packet.sequence));
  frame.insert(frame.end(), packet.audio.begin(), packet.audio.end());
  return frame;
}

std::optional<Packet> DecodeFrame(std::span<const uint8_t> ethernet) {
  if (ethernet.size() < kEthernetHeader + kHeader ||
      ((ethernet[12] << 8) | ethernet[13]) != kEtherType) {
    return std::nullopt;
  }
  const auto body = ethernet.subspan(kEthernetHeader);
  if (!std::equal(std::begin(kMagic), std::end(kMagic), body.begin()) ||
      body[3] != kVersion) {
    return std::nullopt;
  }
  Packet packet;
  switch (body[4]) {
    case uint8_t(Packet::Kind::kAudio):
      packet.kind = Packet::Kind::kAudio;
      break;
    case uint8_t(Packet::Kind::kKeepalive):
      packet.kind = Packet::Kind::kKeepalive;
      break;
    default:
      return std::nullopt;
  }
  packet.muted = (body[5] & kFlagMuted) != 0;
  for (size_t i = 0; i < 8; ++i) {
    packet.xuid = (packet.xuid << 8) | body[6 + i];
  }
  packet.sequence = uint16_t((body[14] << 8) | body[15]);
  const auto audio = body.subspan(kHeader);
  if (audio.size() > kMaxAudioBytes ||
      (packet.kind == Packet::Kind::kAudio) == audio.empty()) {
    return std::nullopt;
  }
  packet.audio.assign(audio.begin(), audio.end());
  return packet;
}

secure::Mac RandomVoiceMac() {
  std::random_device random;
  secure::Mac mac;
  for (auto& byte : mac) {
    byte = uint8_t(random());
  }
  mac[0] = uint8_t((mac[0] & 0xFC) | 0x02);
  return mac;
}

}  // namespace xe::net::party_voice
