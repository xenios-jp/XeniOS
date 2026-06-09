/*
 *
 * TinySHA1 - a header only implementation of the SHA1 algorithm in C++. Based
 * on the implementation in boost::uuid::details.
 *
 * SHA1 Wikipedia Page: http://en.wikipedia.org/wiki/SHA-1
 *
 * Copyright (c) 2012-22 SAURAV MOHAPATRA <mohaps@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Taken from https://github.com/mohaps/TinySHA1
 * Modified for use by Xenia
 */
#ifndef _TINY_SHA1_HPP_
#define _TINY_SHA1_HPP_

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#if defined(__aarch64__) && \
    (defined(__ARM_FEATURE_CRYPTO) || defined(__ARM_FEATURE_SHA1))
#include <arm_neon.h>
#define XE_TINYSHA1_ARM64_HW 1
#endif

namespace sha1 {

inline uint32_t SHA1Rotl(uint32_t value, uint32_t count) {
  return (value << count) ^ (value >> (32 - count));
}

// Reference (scalar) block compression: reads big-endian message bytes in
// `block`, updates the five SHA1 state words in `digest`.
inline void SHA1ProcessBlockSoft(uint32_t digest[5], const uint8_t block[64]) {
  uint32_t w[80];
  for (size_t i = 0; i < 16; i++) {
    w[i] = (uint32_t(block[i * 4 + 0]) << 24) |
           (uint32_t(block[i * 4 + 1]) << 16) |
           (uint32_t(block[i * 4 + 2]) << 8) | (uint32_t(block[i * 4 + 3]));
  }
  for (size_t i = 16; i < 80; i++) {
    w[i] = SHA1Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  uint32_t a = digest[0], b = digest[1], c = digest[2], d = digest[3],
           e = digest[4];
  for (size_t i = 0; i < 80; ++i) {
    uint32_t f = 0, k = 0;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t temp = SHA1Rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = SHA1Rotl(b, 30);
    b = a;
    a = temp;
  }
  digest[0] += a;
  digest[1] += b;
  digest[2] += c;
  digest[3] += d;
  digest[4] += e;
}

#if XE_TINYSHA1_ARM64_HW
// ARMv8 SHA1 crypto-extension block compression (canonical unrolled form).
// Always validated against SHA1ProcessBlockSoft at runtime before use, so a
// transcription error here can only cost the speedup, never corrupt a hash.
inline void SHA1ProcessBlockHW(uint32_t state[5], const uint8_t block[64]) {
  uint32x4_t ABCD = vld1q_u32(&state[0]);
  uint32_t E0 = state[4];
  const uint32x4_t ABCD_SAVED = ABCD;
  const uint32_t E0_SAVED = E0;
  uint32_t E1;
  uint32x4_t MSG0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 0)));
  uint32x4_t MSG1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16)));
  uint32x4_t MSG2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 32)));
  uint32x4_t MSG3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 48)));
  const uint32x4_t K0 = vdupq_n_u32(0x5A827999), K1 = vdupq_n_u32(0x6ED9EBA1),
                   K2 = vdupq_n_u32(0x8F1BBCDC), K3 = vdupq_n_u32(0xCA62C1D6);
  uint32x4_t TMP0 = vaddq_u32(MSG0, K0), TMP1 = vaddq_u32(MSG1, K0);
  // 0-3
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1cq_u32(ABCD, E0, TMP0);
  TMP0 = vaddq_u32(MSG2, K0);
  MSG0 = vsha1su0q_u32(MSG0, MSG1, MSG2);
  // 4-7
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1cq_u32(ABCD, E1, TMP1);
  TMP1 = vaddq_u32(MSG3, K0);
  MSG0 = vsha1su1q_u32(MSG0, MSG3);
  MSG1 = vsha1su0q_u32(MSG1, MSG2, MSG3);
  // 8-11
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1cq_u32(ABCD, E0, TMP0);
  TMP0 = vaddq_u32(MSG0, K0);
  MSG1 = vsha1su1q_u32(MSG1, MSG0);
  MSG2 = vsha1su0q_u32(MSG2, MSG3, MSG0);
  // 12-15
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1cq_u32(ABCD, E1, TMP1);
  TMP1 = vaddq_u32(MSG1, K1);
  MSG2 = vsha1su1q_u32(MSG2, MSG1);
  MSG3 = vsha1su0q_u32(MSG3, MSG0, MSG1);
  // 16-19
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1cq_u32(ABCD, E0, TMP0);
  TMP0 = vaddq_u32(MSG2, K1);
  MSG3 = vsha1su1q_u32(MSG3, MSG2);
  MSG0 = vsha1su0q_u32(MSG0, MSG1, MSG2);
  // 20-23
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E1, TMP1);
  TMP1 = vaddq_u32(MSG3, K1);
  MSG0 = vsha1su1q_u32(MSG0, MSG3);
  MSG1 = vsha1su0q_u32(MSG1, MSG2, MSG3);
  // 24-27
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E0, TMP0);
  TMP0 = vaddq_u32(MSG0, K1);
  MSG1 = vsha1su1q_u32(MSG1, MSG0);
  MSG2 = vsha1su0q_u32(MSG2, MSG3, MSG0);
  // 28-31
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E1, TMP1);
  TMP1 = vaddq_u32(MSG1, K1);
  MSG2 = vsha1su1q_u32(MSG2, MSG1);
  MSG3 = vsha1su0q_u32(MSG3, MSG0, MSG1);
  // 32-35
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E0, TMP0);
  TMP0 = vaddq_u32(MSG2, K2);
  MSG3 = vsha1su1q_u32(MSG3, MSG2);
  MSG0 = vsha1su0q_u32(MSG0, MSG1, MSG2);
  // 36-39
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E1, TMP1);
  TMP1 = vaddq_u32(MSG3, K2);
  MSG0 = vsha1su1q_u32(MSG0, MSG3);
  MSG1 = vsha1su0q_u32(MSG1, MSG2, MSG3);
  // 40-43
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1mq_u32(ABCD, E0, TMP0);
  TMP0 = vaddq_u32(MSG0, K2);
  MSG1 = vsha1su1q_u32(MSG1, MSG0);
  MSG2 = vsha1su0q_u32(MSG2, MSG3, MSG0);
  // 44-47
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1mq_u32(ABCD, E1, TMP1);
  TMP1 = vaddq_u32(MSG1, K2);
  MSG2 = vsha1su1q_u32(MSG2, MSG1);
  MSG3 = vsha1su0q_u32(MSG3, MSG0, MSG1);
  // 48-51
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1mq_u32(ABCD, E0, TMP0);
  TMP0 = vaddq_u32(MSG2, K2);
  MSG3 = vsha1su1q_u32(MSG3, MSG2);
  MSG0 = vsha1su0q_u32(MSG0, MSG1, MSG2);
  // 52-55
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1mq_u32(ABCD, E1, TMP1);
  TMP1 = vaddq_u32(MSG3, K3);
  MSG0 = vsha1su1q_u32(MSG0, MSG3);
  MSG1 = vsha1su0q_u32(MSG1, MSG2, MSG3);
  // 56-59
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1mq_u32(ABCD, E0, TMP0);
  TMP0 = vaddq_u32(MSG0, K3);
  MSG1 = vsha1su1q_u32(MSG1, MSG0);
  MSG2 = vsha1su0q_u32(MSG2, MSG3, MSG0);
  // 60-63
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E1, TMP1);
  TMP1 = vaddq_u32(MSG1, K3);
  MSG2 = vsha1su1q_u32(MSG2, MSG1);
  MSG3 = vsha1su0q_u32(MSG3, MSG0, MSG1);
  // 64-67
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E0, TMP0);
  TMP0 = vaddq_u32(MSG2, K3);
  MSG3 = vsha1su1q_u32(MSG3, MSG2);
  MSG0 = vsha1su0q_u32(MSG0, MSG1, MSG2);
  // 68-71
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E1, TMP1);
  TMP1 = vaddq_u32(MSG3, K3);
  MSG0 = vsha1su1q_u32(MSG0, MSG3);
  // 72-75
  E1 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E0, TMP0);
  // 76-79
  E0 = vsha1h_u32(vgetq_lane_u32(ABCD, 0));
  ABCD = vsha1pq_u32(ABCD, E1, TMP1);
  E0 += E0_SAVED;
  ABCD = vaddq_u32(ABCD_SAVED, ABCD);
  vst1q_u32(&state[0], ABCD);
  state[4] = E0;
}

// One-time self-test: hash an arbitrary block with both paths and compare.
// Caches the result; if the hardware path ever disagrees, it is never used.
inline bool SHA1HardwareUsable() {
  static const bool ok = [] {
    uint32_t hw[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                      0xC3D2E1F0};
    uint32_t sw[5];
    std::memcpy(sw, hw, sizeof(sw));
    uint8_t blk[64];
    for (int i = 0; i < 64; i++) blk[i] = static_cast<uint8_t>(i * 7 + 3);
    SHA1ProcessBlockHW(hw, blk);
    SHA1ProcessBlockSoft(sw, blk);
    return std::memcmp(hw, sw, sizeof(sw)) == 0;
  }();
  return ok;
}
#endif  // XE_TINYSHA1_ARM64_HW

inline void SHA1ProcessBlockDispatch(uint32_t digest[5],
                                     const uint8_t block[64]) {
#if XE_TINYSHA1_ARM64_HW
  if (SHA1HardwareUsable()) {
    SHA1ProcessBlockHW(digest, block);
    return;
  }
#endif
  SHA1ProcessBlockSoft(digest, block);
}

class SHA1 {
 public:
  typedef uint32_t digest32_t[5];
  typedef uint8_t digest8_t[20];
  inline static uint32_t LeftRotate(uint32_t value, size_t count) {
    return (value << count) ^ (value >> (32 - count));
  }
  SHA1() { reset(); }
  virtual ~SHA1() {}
  SHA1(const SHA1& s) { *this = s; }
  const SHA1& operator=(const SHA1& s) {
    memcpy(m_digest, s.m_digest, 5 * sizeof(uint32_t));
    memcpy(m_block, s.m_block, 64);
    m_blockByteIndex = s.m_blockByteIndex;
    m_byteCount = s.m_byteCount;

    return *this;
  }

  SHA1& init(const uint32_t digest[5], const uint8_t block[64],
             uint32_t count) {
    std::memcpy(m_digest, digest, 20);
    std::memcpy(m_block, block, count % 64);
    m_byteCount = count;
    m_blockByteIndex = count % 64;

    return *this;
  }

  const uint32_t* getDigest() const { return m_digest; }
  const uint8_t* getBlock() const { return m_block; }
  size_t getBlockByteIndex() const { return m_blockByteIndex; }
  size_t getByteCount() const { return m_byteCount; }

  SHA1& reset() {
    m_digest[0] = 0x67452301;
    m_digest[1] = 0xEFCDAB89;
    m_digest[2] = 0x98BADCFE;
    m_digest[3] = 0x10325476;
    m_digest[4] = 0xC3D2E1F0;
    m_blockByteIndex = 0;
    m_byteCount = 0;
    return *this;
  }

  SHA1& processByte(uint8_t octet) {
    this->m_block[this->m_blockByteIndex++] = octet;
    ++this->m_byteCount;
    if (m_blockByteIndex == 64) {
      this->m_blockByteIndex = 0;
      processBlock();
    }

    return *this;
  }

  SHA1& processBlock(const void* const start, const void* const end) {
    const uint8_t* begin = static_cast<const uint8_t*>(start);
    const uint8_t* finish = static_cast<const uint8_t*>(end);
    while (begin != finish) {
      processByte(*begin);
      begin++;
    }
    return *this;
  }

  SHA1& processBytes(const void* const data, size_t len) {
    const uint8_t* block = static_cast<const uint8_t*>(data);
    processBlock(block, block + len);
    return *this;
  }

  const uint32_t* finalize(digest32_t digest) {
    size_t bitCount = this->m_byteCount * 8;
    processByte(0x80);
    if (this->m_blockByteIndex > 56) {
      while (m_blockByteIndex != 0) {
        processByte(0);
      }
      while (m_blockByteIndex < 56) {
        processByte(0);
      }
    } else {
      while (m_blockByteIndex < 56) {
        processByte(0);
      }
    }
    processByte(0);
    processByte(0);
    processByte(0);
    processByte(0);
    processByte(static_cast<unsigned char>((bitCount >> 24) & 0xFF));
    processByte(static_cast<unsigned char>((bitCount >> 16) & 0xFF));
    processByte(static_cast<unsigned char>((bitCount >> 8) & 0xFF));
    processByte(static_cast<unsigned char>((bitCount)&0xFF));

    memcpy(digest, m_digest, 5 * sizeof(uint32_t));
    return digest;
  }

  const uint8_t* finalize(digest8_t digest) {
    digest32_t d32;
    finalize(d32);
    size_t di = 0;
    digest[di++] = ((d32[0] >> 24) & 0xFF);
    digest[di++] = ((d32[0] >> 16) & 0xFF);
    digest[di++] = ((d32[0] >> 8) & 0xFF);
    digest[di++] = ((d32[0]) & 0xFF);

    digest[di++] = ((d32[1] >> 24) & 0xFF);
    digest[di++] = ((d32[1] >> 16) & 0xFF);
    digest[di++] = ((d32[1] >> 8) & 0xFF);
    digest[di++] = ((d32[1]) & 0xFF);

    digest[di++] = ((d32[2] >> 24) & 0xFF);
    digest[di++] = ((d32[2] >> 16) & 0xFF);
    digest[di++] = ((d32[2] >> 8) & 0xFF);
    digest[di++] = ((d32[2]) & 0xFF);

    digest[di++] = ((d32[3] >> 24) & 0xFF);
    digest[di++] = ((d32[3] >> 16) & 0xFF);
    digest[di++] = ((d32[3] >> 8) & 0xFF);
    digest[di++] = ((d32[3]) & 0xFF);

    digest[di++] = ((d32[4] >> 24) & 0xFF);
    digest[di++] = ((d32[4] >> 16) & 0xFF);
    digest[di++] = ((d32[4] >> 8) & 0xFF);
    digest[di++] = ((d32[4]) & 0xFF);
    return digest;
  }

 protected:
  void processBlock() { SHA1ProcessBlockDispatch(m_digest, m_block); }

 private:
  digest32_t m_digest;
  uint8_t m_block[64];
  size_t m_blockByteIndex;
  size_t m_byteCount;
};
}
#endif
