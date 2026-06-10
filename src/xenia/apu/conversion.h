/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_CONVERSION_H_
#define XENIA_APU_CONVERSION_H_

#include <cstdint>

#include "xenia/base/byte_order.h"
#include "xenia/base/platform.h"

#if XE_ARCH_ARM64
#include <arm_neon.h>
#endif

namespace xe {
namespace apu {
namespace conversion {

#if XE_ARCH_AMD64

XE_NOINLINE
static void _generic_sequential_6_BE_to_interleaved_6_LE(
    float* XE_RESTRICT output, const float* XE_RESTRICT input,
    unsigned ch_sample_count) {
  for (unsigned sample = 0; sample < ch_sample_count; sample++) {
    for (unsigned channel = 0; channel < 6; channel++) {
      unsigned int value = *reinterpret_cast<const unsigned int*>(
          &input[channel * ch_sample_count + sample]);

      *reinterpret_cast<unsigned int*>(&output[sample * 6 + channel]) =
          xe::byte_swap(value);
    }
  }
}
#if XE_COMPILER_CLANG_CL != 1 && !XE_PLATFORM_LINUX && !XE_PLATFORM_APPLE
// load_be_u32 unavailable on clang-cl, clang on Linux, or clang on Apple
XE_NOINLINE
static void _movbe_sequential_6_BE_to_interleaved_6_LE(
    float* XE_RESTRICT output, const float* XE_RESTRICT input,
    unsigned ch_sample_count) {
  for (unsigned sample = 0; sample < ch_sample_count; sample++) {
    for (unsigned channel = 0; channel < 6; channel++) {
      *reinterpret_cast<unsigned int*>(&output[sample * 6 + channel]) =
          _load_be_u32(reinterpret_cast<const unsigned int*>(
              &input[channel * ch_sample_count + sample]));
    }
  }
}

inline static void sequential_6_BE_to_interleaved_6_LE(
    float* output, const float* input, unsigned ch_sample_count) {
  if (amd64::GetFeatureFlags() & amd64::kX64EmitMovbe) {
    _movbe_sequential_6_BE_to_interleaved_6_LE(output, input, ch_sample_count);
  } else {
    _generic_sequential_6_BE_to_interleaved_6_LE(output, input,
                                                 ch_sample_count);
  }
}
#else
inline static void sequential_6_BE_to_interleaved_6_LE(
    float* output, const float* input, unsigned ch_sample_count) {
  _generic_sequential_6_BE_to_interleaved_6_LE(output, input, ch_sample_count);
}
#endif

inline void sequential_6_BE_to_interleaved_2_LE(float* output,
                                                const float* input,
                                                size_t ch_sample_count) {
  assert_true(ch_sample_count % 4 == 0);
  const __m128i byte_swap_shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  // XAudio2-default-style downmix: front pair at unity, C/surround at -3dB,
  // LFE at -6dB. Matches the loudness of XAudio2's default output matrix.
  const __m128 surround_gain = _mm_set1_ps(0.707106781f);
  const __m128 lfe_gain = _mm_set1_ps(0.5f);

  for (size_t sample = 0; sample < ch_sample_count; sample += 4) {
    __m128 fl = _mm_loadu_ps(&input[0 * ch_sample_count + sample]);
    __m128 fr = _mm_loadu_ps(&input[1 * ch_sample_count + sample]);
    __m128 fc = _mm_loadu_ps(&input[2 * ch_sample_count + sample]);
    __m128 lfe = _mm_loadu_ps(&input[3 * ch_sample_count + sample]);
    __m128 bl = _mm_loadu_ps(&input[4 * ch_sample_count + sample]);
    __m128 br = _mm_loadu_ps(&input[5 * ch_sample_count + sample]);
    fl = _mm_castsi128_ps(
        _mm_shuffle_epi8(_mm_castps_si128(fl), byte_swap_shuffle));
    fr = _mm_castsi128_ps(
        _mm_shuffle_epi8(_mm_castps_si128(fr), byte_swap_shuffle));
    fc = _mm_castsi128_ps(
        _mm_shuffle_epi8(_mm_castps_si128(fc), byte_swap_shuffle));
    lfe = _mm_castsi128_ps(
        _mm_shuffle_epi8(_mm_castps_si128(lfe), byte_swap_shuffle));
    bl = _mm_castsi128_ps(
        _mm_shuffle_epi8(_mm_castps_si128(bl), byte_swap_shuffle));
    br = _mm_castsi128_ps(
        _mm_shuffle_epi8(_mm_castps_si128(br), byte_swap_shuffle));

    __m128 fc_mix = _mm_mul_ps(fc, surround_gain);
    __m128 lfe_mix = _mm_mul_ps(lfe, lfe_gain);
    __m128 left = _mm_add_ps(
        fl,
        _mm_add_ps(fc_mix, _mm_add_ps(_mm_mul_ps(bl, surround_gain), lfe_mix)));
    __m128 right = _mm_add_ps(
        fr,
        _mm_add_ps(fc_mix, _mm_add_ps(_mm_mul_ps(br, surround_gain), lfe_mix)));
    _mm_storeu_ps(&output[sample * 2], _mm_unpacklo_ps(left, right));
    _mm_storeu_ps(&output[(sample + 2) * 2], _mm_unpackhi_ps(left, right));
  }
}
#elif XE_ARCH_ARM64
inline void sequential_6_BE_to_interleaved_6_LE(float* output,
                                                const float* input,
                                                size_t ch_sample_count) {
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    for (size_t channel = 0; channel < 6; channel++) {
      output[sample * 6 + channel] =
          xe::byte_swap(input[channel * ch_sample_count + sample]);
    }
  }
}
inline void sequential_6_BE_to_interleaved_2_LE(float* output,
                                                const float* input,
                                                size_t ch_sample_count) {
  assert_true(ch_sample_count % 4 == 0);
  // NEON mirror of the SSE2 path above. Byte swap is done in the integer
  // domain (rev32 within each 32-bit lane) so NaN-pattern floats round-trip
  // bit-exactly. The adds keep the same association as the SSE2 version so
  // results match x86 builds bit-for-bit.
  const float32x4_t surround_gain = vdupq_n_f32(0.707106781f);
  const float32x4_t lfe_gain = vdupq_n_f32(0.5f);
  for (size_t sample = 0; sample < ch_sample_count; sample += 4) {
    const uint8_t* in_bytes = reinterpret_cast<const uint8_t*>(input);
    auto load_swap = [&](size_t channel) {
      const uint8x16_t raw = vld1q_u8(
          &in_bytes[(channel * ch_sample_count + sample) * sizeof(float)]);
      return vreinterpretq_f32_u8(vrev32q_u8(raw));
    };
    const float32x4_t fl = load_swap(0);
    const float32x4_t fr = load_swap(1);
    const float32x4_t fc = load_swap(2);
    const float32x4_t lfe = load_swap(3);
    const float32x4_t bl = load_swap(4);
    const float32x4_t br = load_swap(5);

    const float32x4_t fc_mix = vmulq_f32(fc, surround_gain);
    const float32x4_t lfe_mix = vmulq_f32(lfe, lfe_gain);
    const float32x4_t left = vaddq_f32(
        fl, vaddq_f32(fc_mix,
                      vaddq_f32(vmulq_f32(bl, surround_gain), lfe_mix)));
    const float32x4_t right = vaddq_f32(
        fr, vaddq_f32(fc_mix,
                      vaddq_f32(vmulq_f32(br, surround_gain), lfe_mix)));
    // Interleave L/R pairs (same lane order as unpacklo/unpackhi).
    const float32x4x2_t zipped = vzipq_f32(left, right);
    vst1q_f32(&output[sample * 2], zipped.val[0]);
    vst1q_f32(&output[(sample + 2) * 2], zipped.val[1]);
  }
}
#else
inline void sequential_6_BE_to_interleaved_6_LE(float* output,
                                                const float* input,
                                                size_t ch_sample_count) {
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    for (size_t channel = 0; channel < 6; channel++) {
      output[sample * 6 + channel] =
          xe::byte_swap(input[channel * ch_sample_count + sample]);
    }
  }
}
inline void sequential_6_BE_to_interleaved_2_LE(float* output,
                                                const float* input,
                                                size_t ch_sample_count) {
  // 5.1 mapping: fl, fr, fc, lfe, bl, br. XAudio2-default-style downmix:
  // front pair at unity, C/surround at -3dB, LFE at -6dB.
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    float fl = xe::byte_swap(input[0 * ch_sample_count + sample]);
    float fr = xe::byte_swap(input[1 * ch_sample_count + sample]);
    float fc = xe::byte_swap(input[2 * ch_sample_count + sample]);
    float lfe = xe::byte_swap(input[3 * ch_sample_count + sample]);
    float bl = xe::byte_swap(input[4 * ch_sample_count + sample]);
    float br = xe::byte_swap(input[5 * ch_sample_count + sample]);
    output[sample * 2] =
        fl + 0.707106781f * fc + 0.707106781f * bl + 0.5f * lfe;
    output[sample * 2 + 1] =
        fr + 0.707106781f * fc + 0.707106781f * br + 0.5f * lfe;
  }
}
#endif

}  // namespace conversion
}  // namespace apu
}  // namespace xe

#endif
