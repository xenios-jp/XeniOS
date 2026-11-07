/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_APPLE_APPLE_VOICE_CODEC_H_
#define XENIA_APU_VOICE_APPLE_APPLE_VOICE_CODEC_H_

#include "xenia/apu/voice/voice_codec.h"

namespace xe {
namespace apu {
namespace voice {

// Apple's Opus (AVAudioConverter, kAudioFormatOpus) at 16 kHz mono in 20 ms
// packets, about 16 kbit/s.
VoiceCodecFactory AppleOpusCodecFactory();

}  // namespace voice
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_APPLE_APPLE_VOICE_CODEC_H_
