/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "xenia/apu/voice/g726.h"
#include "xenia/apu/voice/upsampler.h"
#include "xenia/apu/voice/voice_device.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::apu::voice::G726;
using xe::apu::voice::kSampleRate;
using xe::apu::voice::SampleRing;

std::vector<int16_t> Sine(double hz, double amplitude, size_t count) {
  std::vector<int16_t> pcm(count);
  for (size_t i = 0; i < count; ++i) {
    pcm[i] = int16_t(std::lround(
        amplitude * std::sin(2.0 * 3.14159265358979323846 * hz * double(i) /
                             kSampleRate)));
  }
  return pcm;
}

// Signal-to-noise ratio of |decoded| against |original| in dB, skipping the
// codec's first |skip| samples while it adapts.
double Snr(const std::vector<int16_t>& original,
           const std::vector<int16_t>& decoded, size_t skip) {
  double signal = 0, noise = 0;
  for (size_t i = skip; i < original.size(); ++i) {
    const double d = double(original[i]) - decoded[i];
    signal += double(original[i]) * original[i];
    noise += d * d;
  }
  return 10.0 * std::log10(signal / std::max(noise, 1.0));
}

std::vector<int16_t> RoundTrip(const std::vector<int16_t>& pcm) {
  G726 encoder, decoder;
  std::vector<uint8_t> codes(pcm.size() / 2);
  encoder.EncodePacked(pcm.data(), codes.size(), codes.data());
  std::vector<int16_t> out(pcm.size());
  decoder.DecodePacked(codes.data(), codes.size(), out.data());
  return out;
}

TEST_CASE("G.726: speech-band tones survive a round trip", "[voice]") {
  for (double hz : {200.0, 440.0, 1000.0, 3000.0}) {
    const auto pcm = Sine(hz, 10000, kSampleRate);
    const auto out = RoundTrip(pcm);
    INFO(hz << " Hz");
    CHECK(Snr(pcm, out, kSampleRate / 20) > 20.0);
  }
}

TEST_CASE("G.726: silence decodes to near silence", "[voice]") {
  const std::vector<int16_t> pcm(kSampleRate / 10, 0);
  const auto out = RoundTrip(pcm);
  for (int16_t s : out) {
    CHECK(std::abs(s) < 64);
  }
}

TEST_CASE("G.726: full-scale input does not wrap", "[voice]") {
  auto pcm = Sine(500, 32767, kSampleRate / 4);
  const auto out = RoundTrip(pcm);
  CHECK(Snr(pcm, out, kSampleRate / 20) > 15.0);
}

TEST_CASE("G.726: the encoder tracks its decoder exactly", "[voice]") {
  // Both sides run the same adaptation, so decoding the same codes twice from
  // reset gives the same samples, and Reset() returns to the start.
  const auto pcm = Sine(700, 8000, 4000);
  G726 encoder;
  std::vector<uint8_t> codes(pcm.size() / 2);
  encoder.EncodePacked(pcm.data(), codes.size(), codes.data());
  G726 decoder;
  std::vector<int16_t> first(pcm.size()), second(pcm.size());
  decoder.DecodePacked(codes.data(), codes.size(), first.data());
  decoder.Reset();
  decoder.DecodePacked(codes.data(), codes.size(), second.data());
  CHECK(first == second);
}

TEST_CASE("G.726: the first sample of a byte is in the low nibble",
          "[voice]") {
  G726 a, b;
  const int16_t pcm[2] = {12000, -12000};
  uint8_t packed = 0;
  a.EncodePacked(pcm, 1, &packed);
  CHECK((packed & 0x0F) == b.Encode(12000));
  CHECK((packed >> 4) == b.Encode(-12000));
}

TEST_CASE("G.726: codes are four bits", "[voice]") {
  G726 encoder;
  const auto pcm = Sine(1500, 30000, 2000);
  for (int16_t s : pcm) {
    CHECK(encoder.Encode(s) < 16);
  }
}

TEST_CASE("Upsampler: an 8 kHz tone stays the same tone at 16 kHz",
          "[voice]") {
  for (double hz : {300.0, 1000.0, 3000.0}) {
    // The tone at 8 kHz, and the same tone at 16 kHz to compare against.
    std::vector<int16_t> in(4000), ideal(8000);
    for (size_t i = 0; i < in.size(); ++i) {
      in[i] = int16_t(std::lround(10000 * std::sin(2 * 3.14159265358979 * hz *
                                                   double(i) / 8000)));
    }
    for (size_t i = 0; i < ideal.size(); ++i) {
      ideal[i] = int16_t(std::lround(
          10000 * std::sin(2 * 3.14159265358979 * hz * double(i) / 16000)));
    }
    xe::apu::voice::Upsampler2x up;
    std::vector<int16_t> out(8000);
    up.Process(in.data(), in.size(), out.data());
    // The filter delays by kTaps input samples, 2 * kTaps output samples.
    const size_t delay = 2 * xe::apu::voice::Upsampler2x::kTaps;
    std::vector<int16_t> aligned(out.begin() + delay, out.end());
    std::vector<int16_t> reference(ideal.begin(), ideal.end() - delay);
    INFO(hz << " Hz");
    CHECK(Snr(reference, aligned, 200) > 35.0);
  }
}

TEST_CASE("Upsampler: originals pass through, delayed", "[voice]") {
  xe::apu::voice::Upsampler2x up;
  std::vector<int16_t> in(64);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = int16_t(i * 100);
  }
  std::vector<int16_t> out(128);
  up.Process(in.data(), in.size(), out.data());
  const size_t d = xe::apu::voice::Upsampler2x::kTaps;
  for (size_t i = d; i < in.size(); ++i) {
    CHECK(out[i * 2] == in[i - d]);
  }
}

class NullDevice final : public xe::apu::voice::VoiceDevice {
 public:
  bool Start() override { return true; }
  void Stop() override {}
};

TEST_CASE("Playback waits for a jitter buffer, then again after a gap",
          "[voice]") {
  NullDevice device;
  constexpr size_t kPrime = xe::apu::voice::VoiceDevice::kPlaybackPrime;
  const std::vector<int16_t> ones(kPrime, 1);
  std::vector<int16_t> out(kPrime / 2, -1);
  // Not enough buffered yet: silence, and nothing consumed.
  device.playback().Write(ones.data(), kPrime - 1);
  device.ReadPlayback(out.data(), out.size());
  CHECK(out[0] == 0);
  CHECK(device.playback().Available() == kPrime - 1);
  // Primed: plays.
  device.playback().Write(ones.data(), 1);
  device.ReadPlayback(out.data(), out.size());
  CHECK(out.front() == 1);
  CHECK(out.back() == 1);
  // Runs dry part way: the rest is silence, and it waits to refill.
  std::vector<int16_t> big(kPrime, -1);
  device.ReadPlayback(big.data(), big.size());
  CHECK(big[kPrime / 2 - 1] == 1);
  CHECK(big[kPrime / 2] == 0);
  device.playback().Write(ones.data(), 10);
  device.ReadPlayback(out.data(), out.size());
  CHECK(out[0] == 0);
}

TEST_CASE("Sample ring: wraps and reports its fill", "[voice]") {
  SampleRing ring(5);  // Rounded up to 8.
  REQUIRE(ring.capacity() == 8);
  const int16_t in[6] = {1, 2, 3, 4, 5, 6};
  CHECK(ring.Write(in, 6) == 6);
  CHECK(ring.Available() == 6);
  CHECK(ring.Free() == 2);
  int16_t out[8] = {};
  CHECK(ring.Read(out, 4) == 4);
  CHECK(out[0] == 1);
  CHECK(out[3] == 4);
  // Wraps past the end.
  CHECK(ring.Write(in, 6) == 6);
  CHECK(ring.Write(in, 1) == 0);
  CHECK(ring.Read(out, 8) == 8);
  const int16_t expected[8] = {5, 6, 1, 2, 3, 4, 5, 6};
  for (int i = 0; i < 8; ++i) {
    CHECK(out[i] == expected[i]);
  }
  CHECK(ring.Available() == 0);
}

TEST_CASE("Sample ring: Discard empties it", "[voice]") {
  SampleRing ring(16);
  const int16_t in[10] = {};
  ring.Write(in, 10);
  ring.Discard();
  CHECK(ring.Available() == 0);
  CHECK(ring.Free() == 16);
}

TEST_CASE("Sample ring: one writer and one reader thread", "[voice]") {
  SampleRing ring(64);
  constexpr int kCount = 200000;
  std::thread writer([&] {
    int16_t next = 0;
    int sent = 0;
    while (sent < kCount) {
      int16_t chunk[7];
      const int n = std::min(7, kCount - sent);
      for (int i = 0; i < n; ++i) {
        chunk[i] = int16_t(next + i);
      }
      const size_t wrote = ring.Write(chunk, size_t(n));
      next = int16_t(next + int(wrote));
      sent += int(wrote);
    }
  });
  int16_t expect = 0;
  int received = 0;
  bool in_order = true;
  while (received < kCount) {
    int16_t chunk[5];
    const size_t n = ring.Read(chunk, 5);
    for (size_t i = 0; i < n; ++i) {
      in_order = in_order && chunk[i] == expect;
      expect = int16_t(expect + 1);
    }
    received += int(n);
  }
  writer.join();
  CHECK(in_order);
}

}  // namespace
