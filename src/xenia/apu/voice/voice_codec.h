/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_VOICE_CODEC_H_
#define XENIA_APU_VOICE_VOICE_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "xenia/apu/voice/voice_device.h"

namespace xe {
namespace apu {
namespace voice {

// Party voice's packets: 20 ms of the device's 16 kHz mono each.
constexpr size_t kPartyFrameSamples = kSampleRate / 50;

// Compresses one frame of speech at a time. Not thread safe.
class VoiceEncoder {
 public:
  virtual ~VoiceEncoder() = default;
  // One packet for |frame|, or nothing when encoding failed.
  virtual std::vector<uint8_t> Encode(
      std::span<const int16_t, kPartyFrameSamples> frame) = 0;
};

// Expands one sender's packets, in order. Not thread safe.
class VoiceDecoder {
 public:
  virtual ~VoiceDecoder() = default;
  // False when |packet| couldn't be decoded; |frame| is silence then.
  virtual bool Decode(std::span<const uint8_t> packet,
                      std::span<int16_t, kPartyFrameSamples> frame) = 0;
};

struct VoiceCodecFactory {
  std::function<std::unique_ptr<VoiceEncoder>()> encoder;
  std::function<std::unique_ptr<VoiceDecoder>()> decoder;
};

// Installed by the app (Apple's Opus on macOS and iOS). Without one there is
// no party voice.
void SetVoiceCodecFactory(VoiceCodecFactory factory);
VoiceCodecFactory GetVoiceCodecFactory();

}  // namespace voice
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_VOICE_CODEC_H_
