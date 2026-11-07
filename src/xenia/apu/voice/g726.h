/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_G726_H_
#define XENIA_APU_VOICE_G726_H_

#include <cstddef>
#include <cstdint>

namespace xe {
namespace apu {
namespace voice {

// ITU-T G.726 ADPCM at 32 kbit/s (4-bit codes), the format the Xbox 360
// headset exchanges with titles. One instance holds the adaptive state of one
// direction of one stream, so the encoder and decoder of a headset are
// separate instances, and every stream needs its own.
//
// Written from the ITU-T recommendation's block structure (the same structure
// as the public-domain Sun reference implementation), including its reset
// values.
class G726 {
 public:
  G726() { Reset(); }

  void Reset();

  // 16-bit linear PCM in, a 4-bit code out.
  uint8_t Encode(int16_t sample);
  // A 4-bit code in, 16-bit linear PCM out.
  int16_t Decode(uint8_t code);

  // Two codes per byte, the first sample in the low nibble. |pcm| holds
  // 2 * |bytes| samples.
  void EncodePacked(const int16_t* pcm, size_t bytes, uint8_t* out);
  void DecodePacked(const uint8_t* in, size_t bytes, int16_t* pcm);

 private:
  int PredictorZero() const;
  int PredictorPole() const;
  int StepSize() const;
  void Update(int y, int wi, int fi, int dq, int sr, int dqsez);

  int32_t yl_;
  int16_t yu_;
  int16_t dms_;
  int16_t dml_;
  int16_t ap_;
  int16_t a_[2];
  int16_t b_[6];
  int16_t pk_[2];
  int16_t dq_[6];
  int16_t sr_[2];
  bool td_;
};

}  // namespace voice
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_G726_H_
