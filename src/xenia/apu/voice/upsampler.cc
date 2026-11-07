/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice/upsampler.h"

#include <algorithm>
#include <cmath>

namespace xe {
namespace apu {
namespace voice {

namespace {

constexpr size_t kTaps = Upsampler2x::kTaps;

// The taps for the sample halfway between two inputs: a sinc at half-sample
// offsets under a Blackman window, normalized to unity gain.
struct HalfTaps {
  std::array<float, kTaps> taps;
  HalfTaps() {
    constexpr double kPi = 3.14159265358979323846;
    double sum = 0;
    for (size_t k = 0; k < kTaps; ++k) {
      const double x = double(k) + 0.5;
      const double sinc = std::sin(kPi * x) / (kPi * x);
      // Window over the 2 * kTaps span, centred between the two middle inputs.
      const double n = double(kTaps) - 0.5 - double(k);
      const double w = 0.42 - 0.5 * std::cos(2 * kPi * n / (2 * kTaps - 1)) +
                       0.08 * std::cos(4 * kPi * n / (2 * kTaps - 1));
      taps[k] = float(sinc * w);
      sum += 2 * taps[k];
    }
    for (float& t : taps) {
      t = float(t / sum);
    }
  }
};

const HalfTaps& Taps() {
  static const HalfTaps taps;
  return taps;
}

}  // namespace

void Upsampler2x::Reset() { history_.fill(0.0f); }

void Upsampler2x::Process(const int16_t* in, size_t count, int16_t* out) {
  const auto& h = Taps().taps;
  for (size_t i = 0; i < count; ++i) {
    std::copy(history_.begin() + 1, history_.end(), history_.begin());
    history_.back() = float(in[i]);
    // history_[kTaps - 1] and history_[kTaps] are the two inputs the new
    // sample falls between; the original sample is the earlier of them.
    float half = 0;
    for (size_t k = 0; k < kTaps; ++k) {
      half += h[k] * (history_[kTaps - 1 - k] + history_[kTaps + k]);
    }
    out[i * 2] = int16_t(history_[kTaps - 1]);
    out[i * 2 + 1] = int16_t(std::clamp(std::lround(half), -32768L, 32767L));
  }
}

}  // namespace voice
}  // namespace apu
}  // namespace xe
