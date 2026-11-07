/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice/voice_codec.h"

#include <mutex>

namespace xe {
namespace apu {
namespace voice {

namespace {
std::mutex codec_mutex;
VoiceCodecFactory codec_factory;
}  // namespace

void SetVoiceCodecFactory(VoiceCodecFactory factory) {
  std::lock_guard lock(codec_mutex);
  codec_factory = std::move(factory);
}

VoiceCodecFactory GetVoiceCodecFactory() {
  std::lock_guard lock(codec_mutex);
  return codec_factory;
}

}  // namespace voice
}  // namespace apu
}  // namespace xe
