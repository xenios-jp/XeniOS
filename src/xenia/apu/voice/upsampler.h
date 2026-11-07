/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_UPSAMPLER_H_
#define XENIA_APU_VOICE_UPSAMPLER_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace xe {
namespace apu {
namespace voice {

// Doubles the sample rate of a stream (the headset's 8 kHz speaker to the
// 16 kHz device rate) with a windowed-sinc interpolator. Holds the stream's
// history, so one instance per stream. Delays by kTaps input samples.
class Upsampler2x {
 public:
  static constexpr size_t kTaps = 8;

  Upsampler2x() { Reset(); }
  void Reset();

  // Writes 2 * |count| samples to |out|.
  void Process(const int16_t* in, size_t count, int16_t* out);

 private:
  // The last 2 * kTaps input samples, oldest first.
  std::array<float, 2 * kTaps> history_;
};

}  // namespace voice
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_UPSAMPLER_H_
