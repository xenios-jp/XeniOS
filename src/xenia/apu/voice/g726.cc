/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice/g726.h"

#include <algorithm>
#include <cstdlib>

namespace xe {
namespace apu {
namespace voice {

namespace {

// 32 kbit/s tables: log-domain reconstruction levels, scale factor multipliers
// and the transition-detect weights, indexed by code.
constexpr int16_t kDqlnTab[16] = {-2048, 4,   135, 213, 273, 323, 373, 425,
                                  425,   373, 323, 273, 213, 135, 4,   -2048};
constexpr int16_t kWiTab[16] = {-12,  18,  41,  64,  112, 198, 355, 1122,
                                1122, 355, 198, 112, 64,  41,  18,  -12};
constexpr int16_t kFiTab[16] = {0,     0,     0,     0x200, 0x200, 0x200,
                                0x600, 0xE00, 0xE00, 0x600, 0x200, 0x200,
                                0x200, 0,     0,     0};
// Quantizer decision levels.
constexpr int16_t kQTab[7] = {-124, 80, 178, 246, 300, 349, 400};

constexpr int16_t kPower2[15] = {1,     2,     4,     8,     0x10,
                                 0x20,  0x40,  0x80,  0x100, 0x200,
                                 0x400, 0x800, 0x1000, 0x2000, 0x4000};

// Index of the first table entry above |value|, or |size|.
int Quan(int value, const int16_t* table, int size) {
  int i = 0;
  while (i < size && value >= table[i]) {
    ++i;
  }
  return i;
}

// Multiplies a predictor coefficient by a value held in the codec's
// floating-point form (4-bit exponent, 6-bit mantissa, sign in bit 10).
int FMult(int an, int srn) {
  const int anmag = an > 0 ? an : ((-an) & 0x1FFF);
  const int anexp = Quan(anmag, kPower2, 15) - 6;
  const int anmant =
      anmag == 0 ? 32 : (anexp >= 0 ? anmag >> anexp : anmag << -anexp);
  const int wanexp = anexp + ((srn >> 6) & 0xF) - 13;
  const int wanmant = (anmant * (srn & 0x3F) + 0x30) >> 4;
  const int retval =
      wanexp >= 0 ? ((wanmant << wanexp) & 0x7FFF) : (wanmant >> -wanexp);
  return ((an ^ srn) < 0) ? -retval : retval;
}

// Quantizes the prediction difference |d| with step size |y|.
int Quantize(int d, int y) {
  const int dqm = std::abs(d);
  const int exp = Quan(dqm >> 1, kPower2, 15);
  const int mant = ((dqm << 7) >> exp) & 0x7F;
  const int dl = (exp << 7) + mant;
  const int dln = dl - (y >> 2);
  const int i = Quan(dln, kQTab, 7);
  if (d < 0) {
    return 15 - i;
  }
  return i == 0 ? 15 : i;
}

// The quantized difference for a code, in sign-magnitude form: negative
// values are the magnitude minus 0x8000.
int Reconstruct(bool sign, int dqln, int y) {
  const int dql = dqln + (y >> 2);
  if (dql < 0) {
    return sign ? -0x8000 : 0;
  }
  const int dex = (dql >> 7) & 15;
  const int dqt = 128 + (dql & 127);
  const int dq = (dqt << 7) >> (14 - dex);
  return sign ? dq - 0x8000 : dq;
}

// A positive or negative magnitude in the codec's floating-point form.
int16_t ToFloat(int magnitude, bool negative) {
  const int exp = Quan(magnitude, kPower2, 15);
  const int value = (exp << 6) + ((magnitude << 6) >> exp);
  return static_cast<int16_t>(negative ? value - 0x400 : value);
}

}  // namespace

void G726::Reset() {
  yl_ = 34816;
  yu_ = 544;
  dms_ = 0;
  dml_ = 0;
  ap_ = 0;
  td_ = false;
  for (int i = 0; i < 2; ++i) {
    a_[i] = 0;
    pk_[i] = 0;
    sr_[i] = 32;
  }
  for (int i = 0; i < 6; ++i) {
    b_[i] = 0;
    dq_[i] = 32;
  }
}

int G726::PredictorZero() const {
  int sezi = 0;
  for (int i = 0; i < 6; ++i) {
    sezi += FMult(b_[i] >> 2, dq_[i]);
  }
  return sezi;
}

int G726::PredictorPole() const {
  return FMult(a_[1] >> 2, sr_[1]) + FMult(a_[0] >> 2, sr_[0]);
}

int G726::StepSize() const {
  if (ap_ >= 256) {
    return yu_;
  }
  int y = yl_ >> 6;
  const int dif = yu_ - y;
  const int al = ap_ >> 2;
  if (dif > 0) {
    y += (dif * al) >> 6;
  } else if (dif < 0) {
    y += (dif * al + 0x3F) >> 6;
  }
  return y;
}

void G726::Update(int y, int wi, int fi, int dq, int sr, int dqsez) {
  const int pk0 = dqsez < 0 ? 1 : 0;
  const int mag = dq & 0x7FFF;

  // Transition detection: a tone that suddenly ends resets the predictor.
  const int ylint = yl_ >> 15;
  const int ylfrac = (yl_ >> 10) & 0x1F;
  const int thr1 = (32 + ylfrac) << ylint;
  const int thr2 = ylint > 9 ? 31 << 10 : thr1;
  const int dqthr = (thr2 + (thr2 >> 1)) >> 1;
  const bool tr = td_ && mag > dqthr;

  // Quantizer scale factor adaptation.
  yu_ = static_cast<int16_t>(std::clamp(y + ((wi - y) >> 5), 544, 5120));
  yl_ += yu_ + ((-yl_) >> 6);

  // Adaptive predictor coefficients.
  int a2p = 0;
  if (tr) {
    a_[0] = 0;
    a_[1] = 0;
    for (int i = 0; i < 6; ++i) {
      b_[i] = 0;
    }
  } else {
    const int pks1 = pk0 ^ pk_[0];
    a2p = a_[1] - (a_[1] >> 7);
    if (dqsez != 0) {
      const int fa1 = pks1 ? a_[0] : -a_[0];
      if (fa1 < -8191) {
        a2p -= 0x100;
      } else if (fa1 > 8191) {
        a2p += 0xFF;
      } else {
        a2p += fa1 >> 5;
      }
      if (pk0 ^ pk_[1]) {
        if (a2p <= -12160) {
          a2p = -12288;
        } else if (a2p >= 12416) {
          a2p = 12288;
        } else {
          a2p -= 0x80;
        }
      } else if (a2p <= -12416) {
        a2p = -12288;
      } else if (a2p >= 12160) {
        a2p = 12288;
      } else {
        a2p += 0x80;
      }
    }
    a_[1] = static_cast<int16_t>(a2p);

    int a1 = a_[0] - (a_[0] >> 8);
    if (dqsez != 0) {
      a1 += pks1 == 0 ? 192 : -192;
    }
    const int a1ul = 15360 - a2p;
    a_[0] = static_cast<int16_t>(std::clamp(a1, -a1ul, a1ul));

    for (int i = 0; i < 6; ++i) {
      int b = b_[i] - (b_[i] >> 8);
      if (mag) {
        b += ((dq ^ dq_[i]) >= 0) ? 128 : -128;
      }
      b_[i] = static_cast<int16_t>(b);
    }
  }

  for (int i = 5; i > 0; --i) {
    dq_[i] = dq_[i - 1];
  }
  if (mag == 0) {
    dq_[0] = static_cast<int16_t>(dq >= 0 ? 0x20 : 0xFC20);
  } else {
    dq_[0] = ToFloat(mag, dq < 0);
  }

  sr_[1] = sr_[0];
  if (sr == 0) {
    sr_[0] = 0x20;
  } else if (sr > 0) {
    sr_[0] = ToFloat(sr, false);
  } else if (sr > -32768) {
    sr_[0] = ToFloat(-sr, true);
  } else {
    sr_[0] = static_cast<int16_t>(0xFC20);
  }

  pk_[1] = pk_[0];
  pk_[0] = static_cast<int16_t>(pk0);

  // Tone detection.
  td_ = !tr && a2p < -11776;

  // Adaptation speed control.
  dms_ = static_cast<int16_t>(dms_ + ((fi - dms_) >> 5));
  dml_ = static_cast<int16_t>(dml_ + (((fi << 2) - dml_) >> 7));
  if (tr) {
    ap_ = 256;
  } else if (y < 1536 || td_ ||
             std::abs((dms_ << 2) - dml_) >= (dml_ >> 3)) {
    ap_ = static_cast<int16_t>(ap_ + ((0x200 - ap_) >> 4));
  } else {
    ap_ = static_cast<int16_t>(ap_ + ((-ap_) >> 4));
  }
}

uint8_t G726::Encode(int16_t sample) {
  // The codec works on 14-bit samples.
  const int sl = sample >> 2;
  const int sezi = PredictorZero();
  const int sez = sezi >> 1;
  const int se = (sezi + PredictorPole()) >> 1;
  const int d = sl - se;
  const int y = StepSize();
  const int i = Quantize(d, y);
  const int dq = Reconstruct((i & 8) != 0, kDqlnTab[i], y);
  const int sr = dq < 0 ? se - (dq & 0x3FFF) : se + dq;
  const int dqsez = sr + sez - se;
  Update(y, kWiTab[i] << 5, kFiTab[i], dq, sr, dqsez);
  return static_cast<uint8_t>(i);
}

int16_t G726::Decode(uint8_t code) {
  const int i = code & 0x0F;
  const int sezi = PredictorZero();
  const int sez = sezi >> 1;
  const int se = (sezi + PredictorPole()) >> 1;
  const int y = StepSize();
  const int dq = Reconstruct((i & 8) != 0, kDqlnTab[i], y);
  const int sr = dq < 0 ? se - (dq & 0x3FFF) : se + dq;
  const int dqsez = sr - se + sez;
  Update(y, kWiTab[i] << 5, kFiTab[i], dq, sr, dqsez);
  return static_cast<int16_t>(std::clamp(sr << 2, -32768, 32767));
}

void G726::EncodePacked(const int16_t* pcm, size_t bytes, uint8_t* out) {
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t lo = Encode(pcm[i * 2]);
    const uint8_t hi = Encode(pcm[i * 2 + 1]);
    out[i] = static_cast<uint8_t>(lo | (hi << 4));
  }
}

void G726::DecodePacked(const uint8_t* in, size_t bytes, int16_t* pcm) {
  for (size_t i = 0; i < bytes; ++i) {
    pcm[i * 2] = Decode(in[i] & 0x0F);
    pcm[i * 2 + 1] = Decode(in[i] >> 4);
  }
}

}  // namespace voice
}  // namespace apu
}  // namespace xe
