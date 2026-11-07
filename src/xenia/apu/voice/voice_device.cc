/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice/voice_device.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"

DEFINE_transient_string(
    voice_test_input, "",
    "Diagnostic: feed the headset microphone from a 16 kHz mono 16-bit WAV "
    "file (played in a loop with a second of silence between) or from "
    "\"tone:<hz>\", instead of the host microphone.",
    "APU");
DEFINE_transient_string(
    voice_test_output, "",
    "Diagnostic: write what the headset plays to this 16 kHz mono WAV file "
    "instead of the host speaker.",
    "APU");

namespace xe {
namespace apu {
namespace voice {

namespace {

size_t RoundUpPow2(size_t value) {
  size_t result = 1;
  while (result < value) {
    result <<= 1;
  }
  return result;
}

// A second at the headset rate is far more than either side ever holds.
constexpr size_t kRingCapacity = kSampleRate;

}  // namespace

SampleRing::SampleRing(size_t capacity)
    : buffer_(RoundUpPow2(capacity)), mask_(buffer_.size() - 1) {}

size_t SampleRing::Write(const int16_t* samples, size_t count) {
  const size_t written = written_.load(std::memory_order_relaxed);
  const size_t read = read_.load(std::memory_order_acquire);
  const size_t n = std::min(count, buffer_.size() - (written - read));
  for (size_t i = 0; i < n; ++i) {
    buffer_[(written + i) & mask_] = samples[i];
  }
  written_.store(written + n, std::memory_order_release);
  return n;
}

size_t SampleRing::Free() const {
  return buffer_.size() - (written_.load(std::memory_order_acquire) -
                           read_.load(std::memory_order_acquire));
}

size_t SampleRing::Read(int16_t* out, size_t count) {
  const size_t read = read_.load(std::memory_order_relaxed);
  const size_t written = written_.load(std::memory_order_acquire);
  const size_t n = std::min(count, written - read);
  for (size_t i = 0; i < n; ++i) {
    out[i] = buffer_[(read + i) & mask_];
  }
  read_.store(read + n, std::memory_order_release);
  return n;
}

void SampleRing::Discard() {
  read_.store(written_.load(std::memory_order_acquire),
              std::memory_order_release);
}

size_t SampleRing::Available() const {
  return written_.load(std::memory_order_acquire) -
         read_.load(std::memory_order_acquire);
}

VoiceDevice::VoiceDevice()
    : capture_(kRingCapacity), playback_(kRingCapacity) {}

VoiceDevice::~VoiceDevice() = default;

void VoiceDevice::ReadPlayback(int16_t* out, size_t count) {
  if (!playback_primed_ && playback_.Available() >= kPlaybackPrime) {
    playback_primed_ = true;
  }
  size_t got = 0;
  if (playback_primed_) {
    got = playback_.Read(out, count);
    if (got < count) {
      playback_primed_ = false;
      playback_dry_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  std::fill(out + got, out + count, int16_t(0));
}

namespace {

// A test clip longer than this (ten minutes) is cut short.
constexpr uint32_t kMaxWavBytes = kSampleRate * 2 * 600;

// Reads a 16 kHz mono 16-bit PCM WAV file. Empty on any other format.
std::vector<int16_t> ReadWav(const std::string& path) {
  std::vector<int16_t> samples;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return samples;
  }
  uint8_t riff[12];
  bool format_ok = false;
  if (std::fread(riff, 1, sizeof(riff), file) == sizeof(riff) &&
      !std::memcmp(riff, "RIFF", 4) && !std::memcmp(riff + 8, "WAVE", 4)) {
    uint8_t header[8];
    while (std::fread(header, 1, sizeof(header), file) == sizeof(header)) {
      const uint32_t size = header[4] | (header[5] << 8) | (header[6] << 16) |
                            (uint32_t(header[7]) << 24);
      if (!std::memcmp(header, "fmt ", 4) && size >= 16) {
        uint8_t fmt[16];
        if (std::fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) {
          break;
        }
        const uint16_t tag = fmt[0] | (fmt[1] << 8);
        const uint16_t channels = fmt[2] | (fmt[3] << 8);
        const uint32_t rate =
            fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (uint32_t(fmt[7]) << 24);
        const uint16_t bits = fmt[14] | (fmt[15] << 8);
        format_ok =
            tag == 1 && channels == 1 && rate == kSampleRate && bits == 16;
        std::fseek(file, long(size - 16 + (size & 1)), SEEK_CUR);
      } else if (!std::memcmp(header, "data", 4) && format_ok) {
        samples.resize(std::min(size, kMaxWavBytes) / 2);
        const size_t n = std::fread(samples.data(), 2, samples.size(), file);
        samples.resize(n);
        break;
      } else {
        std::fseek(file, long(size + (size & 1)), SEEK_CUR);
      }
    }
  }
  std::fclose(file);
  // WAV is little-endian, as is every host this runs on.
  return samples;
}

// A WAV file that is valid on disk after every Append, so a test that kills
// the process still leaves a readable file.
class WavWriter {
 public:
  explicit WavWriter(const std::string& path) {
    file_ = std::fopen(path.c_str(), "wb");
    if (file_) {
      WriteHeader();
    }
  }
  ~WavWriter() {
    if (file_) {
      std::fclose(file_);
    }
  }
  bool ok() const { return file_ != nullptr; }

  void Append(const int16_t* samples, size_t count) {
    if (!file_ || !count) {
      return;
    }
    std::fwrite(samples, 2, count, file_);
    data_bytes_ += uint32_t(count * 2);
    WriteHeader();
    std::fseek(file_, 0, SEEK_END);
  }

 private:
  void WriteHeader() {
    auto le32 = [](uint8_t* p, uint32_t v) {
      p[0] = uint8_t(v);
      p[1] = uint8_t(v >> 8);
      p[2] = uint8_t(v >> 16);
      p[3] = uint8_t(v >> 24);
    };
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V',
                     'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0,
                     1,   0,   0,   0,   0,   0,   0,  0,  0,  0,  2,
                     0,   16,  0,   'd', 'a', 't', 'a', 0, 0, 0, 0};
    le32(h + 4, 36 + data_bytes_);
    le32(h + 24, kSampleRate);
    le32(h + 28, kSampleRate * 2);
    le32(h + 40, data_bytes_);
    std::fseek(file_, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), file_);
    std::fflush(file_);
  }

  FILE* file_ = nullptr;
  uint32_t data_bytes_ = 0;
};

// The diagnostic device behind voice_test_input and voice_test_output. It has
// no clock of its own: the headset paces it in real time with Advance().
class TestVoiceDevice final : public VoiceDevice {
 public:
  TestVoiceDevice(std::string input, std::string output)
      : input_(std::move(input)), output_(std::move(output)) {}

  bool Start() override {
    if (utf8::starts_with(input_, "tone:")) {
      tone_hz_ = std::atof(input_.c_str() + 5);
    } else if (!input_.empty()) {
      clip_ = ReadWav(input_);
      if (clip_.empty()) {
        XELOGE("Voice test: {} is not a 16 kHz mono 16-bit WAV file", input_);
      } else {
        // A second of silence between repeats, so voice activity detection
        // sees separate bursts.
        clip_.resize(clip_.size() + kSampleRate, 0);
      }
    }
    if (!output_.empty()) {
      writer_ = std::make_unique<WavWriter>(output_);
      if (!writer_->ok()) {
        XELOGE("Voice test: can't write {}", output_);
        writer_.reset();
      }
    }
    XELOGI("Voice test: microphone {}, speaker {}",
           input_.empty() ? "silent" : input_,
           output_.empty() ? "discarded" : output_);
    return true;
  }

  void Stop() override { writer_.reset(); }

  bool clocked() const override { return false; }

  void Advance(size_t samples) override {
    scratch_.resize(samples);
    // Microphone.
    for (size_t i = 0; i < samples; ++i) {
      int16_t value = 0;
      if (tone_hz_ > 0) {
        value = int16_t(std::lround(
            9830.0 * std::sin(2.0 * 3.14159265358979323846 * tone_hz_ *
                              double(position_) / kSampleRate)));
      } else if (!clip_.empty()) {
        value = clip_[position_ % clip_.size()];
      }
      scratch_[i] = value;
      ++position_;
    }
    capture().Write(scratch_.data(), samples);
    // Speaker.
    ReadPlayback(scratch_.data(), samples);
    if (writer_) {
      writer_->Append(scratch_.data(), samples);
    }
  }

 private:
  std::string input_;
  std::string output_;
  double tone_hz_ = 0;
  std::vector<int16_t> clip_;
  uint64_t position_ = 0;
  std::unique_ptr<WavWriter> writer_;
  std::vector<int16_t> scratch_;
};

std::mutex factory_mutex;
std::atomic<bool> host_voice_switchable{false};
VoiceDeviceFactory& Factory() {
  static VoiceDeviceFactory factory;
  return factory;
}

}  // namespace

void SetVoiceDeviceFactory(VoiceDeviceFactory factory) {
  std::lock_guard<std::mutex> lock(factory_mutex);
  Factory() = std::move(factory);
}

bool HostDeviceAvailable() {
  std::lock_guard<std::mutex> lock(factory_mutex);
  return bool(Factory());
}

bool TestDeviceRequested() {
  return !cvars::voice_test_input.empty() || !cvars::voice_test_output.empty();
}

std::unique_ptr<VoiceDevice> CreateVoiceDevice() {
  if (TestDeviceRequested()) {
    return std::make_unique<TestVoiceDevice>(cvars::voice_test_input,
                                             cvars::voice_test_output);
  }
  std::lock_guard<std::mutex> lock(factory_mutex);
  return Factory() ? Factory()() : nullptr;
}

void SetHostVoiceSwitchable(bool switchable) {
  host_voice_switchable.store(switchable, std::memory_order_release);
}

bool HostVoiceSwitchable() {
  return host_voice_switchable.load(std::memory_order_acquire);
}

}  // namespace voice
}  // namespace apu
}  // namespace xe
