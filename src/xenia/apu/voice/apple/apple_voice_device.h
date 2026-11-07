/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_APPLE_APPLE_VOICE_DEVICE_H_
#define XENIA_APU_VOICE_APPLE_APPLE_VOICE_DEVICE_H_

#include <memory>

#include "xenia/apu/voice/voice_device.h"

namespace xe {
namespace apu {
namespace voice {

// The headset on macOS and iOS: Apple's voice processing I/O unit (echo
// cancellation, noise suppression and gain control), which plays the
// headset's speaker itself so its echo is cancelled. Plain audio if voice
// processing can't start; without microphone permission it plays only.
//
// AUVoiceIO and AVAudioEngine's voice processing mode are Apple's two voice
// processing APIs, with the same processing (WWDC23 "What's new in voice
// processing"). The unit is used because it converts to and from the
// headset's 16 kHz mono format itself; the engine delivers voice-processed
// input in the hardware's multichannel format.
std::unique_ptr<VoiceDevice> CreateAppleVoiceDevice();

// iOS: posted when voice chat starts or stops needing the microphone, with
// userInfo @{@"active": @(BOOL)}. The app's audio session code switches the
// category and posts "XeniaAudioSessionRestored", which starts the engine.
extern const char* const kVoiceChatChangedNotification;

}  // namespace voice
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_APPLE_APPLE_VOICE_DEVICE_H_
