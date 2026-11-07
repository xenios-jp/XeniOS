/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_PARTY_VOICE_WIRE_H_
#define XENIA_NET_PARTY_VOICE_WIRE_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/net/xnet_secure.h"

// Party voice travels in a relay room of its own, as Ethernet frames that
// each member broadcasts to the others it reaches directly.
namespace xe::net::party_voice {

// The voice room's code for the party whose room code is |party_code|: only
// the members know the party's code, so only they can find its voice room,
// and System Link play in the party's own room never meets voice. Nothing for
// a code that isn't a room code.
std::optional<std::string> VoiceRoomCode(std::string_view party_code);

// IEEE 802 local experimental EtherType.
constexpr uint16_t kEtherType = 0x88B5;
// A codec packet larger than this isn't sent (Opus at 16 kbit/s makes ~40).
constexpr size_t kMaxAudioBytes = 400;

struct Packet {
  enum class Kind : uint8_t {
    // 20 ms of speech.
    kAudio = 1,
    // Nobody is talking: says the sender is still there (each second).
    kKeepalive = 2,
  };
  Kind kind = Kind::kKeepalive;
  // The sender's online XUID.
  uint64_t xuid = 0;
  // Counts every packet the sender sent, keepalives too.
  uint16_t sequence = 0;
  // The sender muted themselves in the party.
  bool muted = false;
  std::vector<uint8_t> audio;  // kAudio.
};

// The whole frame, broadcast from |source|. Nothing when |packet| is too big.
std::optional<std::vector<uint8_t>> EncodeFrame(const Packet& packet,
                                                const secure::Mac& source);
// Nothing for frames that aren't party voice.
std::optional<Packet> DecodeFrame(std::span<const uint8_t> ethernet);

// A locally administered unicast MAC for this device in the voice room.
secure::Mac RandomVoiceMac();

}  // namespace xe::net::party_voice

#endif  // XENIA_NET_PARTY_VOICE_WIRE_H_
