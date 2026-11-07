/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/party_voice_wire.h"

#include "third_party/catch/include/catch.hpp"
#include "xenia/net/relay_protocol.h"

namespace xe::net::party_voice {
namespace {

TEST_CASE("Party voice rooms come from the party's code", "[net][party]") {
  const auto code = VoiceRoomCode("7K2M9Q");
  REQUIRE(code);
  CHECK(code->size() == relay::kRoomCodeLength);
  // A room code the lobby takes, other than the party's own.
  CHECK(relay::NormalizeRoomCode(*code) == code);
  CHECK(*code != "7K2M9Q");
  // The same for every member, however they typed it.
  CHECK(VoiceRoomCode("7k2-m9q") == code);
  CHECK(VoiceRoomCode("7K2M9R") != code);
  CHECK_FALSE(VoiceRoomCode("not a code"));
}

TEST_CASE("Party voice packets survive the wire", "[net][party]") {
  const secure::Mac mac = RandomVoiceMac();
  // Locally administered, unicast.
  CHECK((mac[0] & 0x03) == 0x02);

  Packet audio;
  audio.kind = Packet::Kind::kAudio;
  audio.xuid = 0x0009000012345678;
  audio.sequence = 0xFFFE;
  audio.audio = {1, 2, 3, 4, 5};
  const auto frame = EncodeFrame(audio, mac);
  REQUIRE(frame);
  // Broadcast, from |mac|.
  CHECK(std::all_of(frame->begin(), frame->begin() + 6,
                    [](uint8_t b) { return b == 0xFF; }));
  CHECK(std::equal(mac.begin(), mac.end(), frame->begin() + 6));
  const auto decoded = DecodeFrame(*frame);
  REQUIRE(decoded);
  CHECK(decoded->kind == Packet::Kind::kAudio);
  CHECK(decoded->xuid == audio.xuid);
  CHECK(decoded->sequence == audio.sequence);
  CHECK_FALSE(decoded->muted);
  CHECK(decoded->audio == audio.audio);

  Packet keepalive;
  keepalive.xuid = 7;
  keepalive.muted = true;
  const auto quiet = DecodeFrame(*EncodeFrame(keepalive, mac));
  REQUIRE(quiet);
  CHECK(quiet->kind == Packet::Kind::kKeepalive);
  CHECK(quiet->muted);
  CHECK(quiet->audio.empty());

  // Too big to send; not party voice; audio without bytes.
  Packet big = audio;
  big.audio.resize(kMaxAudioBytes + 1);
  CHECK_FALSE(EncodeFrame(big, mac));
  auto other = *frame;
  other[13] ^= 1;
  CHECK_FALSE(DecodeFrame(other));
  auto truncated = *EncodeFrame(audio, mac);
  truncated.resize(truncated.size() - audio.audio.size());
  CHECK_FALSE(DecodeFrame(truncated));
}

}  // namespace
}  // namespace xe::net::party_voice
