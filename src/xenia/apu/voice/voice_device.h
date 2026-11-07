/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_VOICE_DEVICE_H_
#define XENIA_APU_VOICE_VOICE_DEVICE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace xe {
namespace apu {
namespace voice {

// The headset's sample rate: mono 16-bit PCM at 16 kHz in both directions.
constexpr uint32_t kSampleRate = 16000;

// A ring of samples with one writer thread and one reader thread, and no
// locks, so either side can be a real-time audio callback.
class SampleRing {
 public:
  // |capacity| is rounded up to a power of two.
  explicit SampleRing(size_t capacity);

  // Writer side. Returns how many samples fit.
  size_t Write(const int16_t* samples, size_t count);
  size_t Free() const;

  // Reader side. Returns how many samples were read.
  size_t Read(int16_t* out, size_t count);
  // Drops everything written so far.
  void Discard();
  size_t Available() const;

  size_t capacity() const { return buffer_.size(); }

 private:
  std::vector<int16_t> buffer_;
  size_t mask_;
  // Total samples ever written and read; the difference is the fill level.
  std::atomic<size_t> written_{0};
  std::atomic<size_t> read_{0};
};

// The host side of the emulated headset: a microphone that writes capture()
// and a speaker that reads playback(), both at kSampleRate.
class VoiceDevice {
 public:
  VoiceDevice();
  virtual ~VoiceDevice();

  // Opens the microphone and speaker. False when neither could be opened.
  virtual bool Start() = 0;
  virtual void Stop() = 0;

  // True when the host's audio hardware moves the rings. Otherwise the caller
  // paces the device with Advance().
  virtual bool clocked() const { return true; }
  // For devices that are not clocked: produce and consume |samples| now.
  virtual void Advance(size_t samples) {}

  // Written by the microphone, read by the headset.
  SampleRing& capture() { return capture_; }
  // Written by the headset, read by the speaker.
  SampleRing& playback() { return playback_; }

  // For the speaker: reads |count| samples of playback, silence-padded. It
  // holds back until kPlaybackPrime samples are buffered, and again after
  // running dry, so packet-sized bursts play without gaps. Only the speaker's
  // thread may call it.
  void ReadPlayback(int16_t* out, size_t count);
  static constexpr size_t kPlaybackPrime = kSampleRate / 25;  // 40 ms
  // Times playback ran out mid-stream and went silent until primed again.
  uint32_t playback_dry() const {
    return playback_dry_.load(std::memory_order_relaxed);
  }

 private:
  SampleRing capture_;
  SampleRing playback_;
  // The speaker thread's.
  bool playback_primed_ = false;
  std::atomic<uint32_t> playback_dry_{0};
};

using VoiceDeviceFactory = std::function<std::unique_ptr<VoiceDevice>()>;

// Installed by the app for its platform's audio stack. Without one, and
// without the voice_test_* flags, titles see no headset.
void SetVoiceDeviceFactory(VoiceDeviceFactory factory);

// True when the app installed a factory.
bool HostDeviceAvailable();

// True when voice_test_input or voice_test_output is set.
bool TestDeviceRequested();

// The device for a headset: the test device when TestDeviceRequested(), else
// the app's, else null.
std::unique_ptr<VoiceDevice> CreateVoiceDevice();

// For an app that switches voice chat on and off while a game runs (XeniOS's
// Microphone choice). Once set, a title gets a headset whenever the app's host
// audio exists, and the host microphone and speaker run only while the
// voice_chat cvar is on: off stops them, and the title's headset hears and
// plays silence. Otherwise voice_chat decides, once, when a title asks for a
// headset.
void SetHostVoiceSwitchable(bool switchable);
bool HostVoiceSwitchable();

}  // namespace voice
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_VOICE_DEVICE_H_
