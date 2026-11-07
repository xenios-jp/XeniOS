/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/apu/voice/party_voice.h"
#include "xenia/base/platform.h"
#include "xenia/net/party_voice_wire.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using namespace xe::apu::voice;
using xe::net::secure::Mac;
using xe::net::secure::PacketTransport;

// Frames sent by one end reach every other end, as the voice room's broadcast.
struct Wire {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::deque<std::vector<uint8_t>>*> inboxes;
};

class WireTransport final : public PacketTransport {
 public:
  WireTransport(std::shared_ptr<Wire> wire, Mac mac)
      : wire_(std::move(wire)), mac_(mac) {
    std::lock_guard lock(wire_->mutex);
    wire_->inboxes.push_back(&inbox_);
  }
  ~WireTransport() override {
    std::lock_guard lock(wire_->mutex);
    std::erase(wire_->inboxes, &inbox_);
  }
  Mac identity() const override { return mac_; }
  uint32_t mtu() const override { return 1500; }
  int Send(std::span<const uint8_t> frame) override {
    {
      std::lock_guard lock(wire_->mutex);
      for (auto* inbox : wire_->inboxes) {
        if (inbox != &inbox_) {
          inbox->emplace_back(frame.begin(), frame.end());
        }
      }
    }
    wire_->changed.notify_all();
    return int(frame.size());
  }
  bool Receive(std::vector<uint8_t>& frame, uint32_t timeout_ms) override {
    std::unique_lock lock(wire_->mutex);
    wire_->changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return !inbox_.empty() || woken_; });
    woken_ = false;
    if (inbox_.empty()) {
      return false;
    }
    frame = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
  }
  void Wake() override {
    {
      std::lock_guard lock(wire_->mutex);
      woken_ = true;
    }
    wire_->changed.notify_all();
  }

 private:
  std::shared_ptr<Wire> wire_;
  Mac mac_;
  std::deque<std::vector<uint8_t>> inbox_;
  bool woken_ = false;
};

// A microphone that hears a constant level, and a speaker that keeps what it
// plays. Paced by the session.
class FakeDevice final : public VoiceDevice {
 public:
  explicit FakeDevice(int16_t level)
      : source_([level](size_t) { return level; }) {}
  explicit FakeDevice(std::function<int16_t(size_t)> source)
      : source_(std::move(source)) {}
  bool Start() override {
    started = true;
    return true;
  }
  void Stop() override { started = false; }
  bool clocked() const override { return false; }
  void Advance(size_t samples) override {
    std::vector<int16_t> heard(samples);
    for (auto& sample : heard) {
      sample = source_(heard_++);
    }
    capture().Write(heard.data(), heard.size());
    std::vector<int16_t> played(samples);
    ReadPlayback(played.data(), played.size());
    std::lock_guard lock(mutex);
    this->played.insert(this->played.end(), played.begin(), played.end());
  }
  size_t PlayedNear(int16_t level) {
    std::lock_guard lock(mutex);
    return size_t(std::count_if(played.begin(), played.end(), [&](int16_t s) {
      return std::abs(s - level) < 50;
    }));
  }

  std::atomic<bool> started{false};
  std::mutex mutex;
  std::vector<int16_t> played;

 private:
  std::function<int16_t(size_t)> source_;
  size_t heard_ = 0;
};

// Each packet is its frame's level, and plays back as that level.
VoiceCodecFactory LevelCodec() {
  struct Encoder final : VoiceEncoder {
    std::vector<uint8_t> Encode(
        std::span<const int16_t, kPartyFrameSamples> frame) override {
      const auto level = uint16_t(frame[0]);
      return {uint8_t(level >> 8), uint8_t(level)};
    }
  };
  struct Decoder final : VoiceDecoder {
    bool Decode(std::span<const uint8_t> packet,
                std::span<int16_t, kPartyFrameSamples> frame) override {
      const auto level = int16_t((packet[0] << 8) | packet[1]);
      std::fill(frame.begin(), frame.end(), level);
      return true;
    }
  };
  VoiceCodecFactory factory;
  factory.encoder = [] { return std::make_unique<Encoder>(); };
  factory.decoder = [] { return std::make_unique<Decoder>(); };
  return factory;
}

struct Member {
  FakeDevice* device = nullptr;
  std::unique_ptr<PartyVoice> voice;
};

Member Join(const std::shared_ptr<Wire>& wire, uint64_t xuid,
            std::unique_ptr<FakeDevice> device,
            VoiceCodecFactory codec = LevelCodec()) {
  Member member;
  member.device = device.get();
  auto held = std::make_shared<std::unique_ptr<FakeDevice>>(std::move(device));
  PartyVoiceConfig config;
  config.xuid = xuid;
  config.codec = std::move(codec);
  config.open_device = [held]() -> std::unique_ptr<VoiceDevice> {
    return std::move(*held);
  };
  config.open_transport = [wire](const Mac& mac) {
    return std::make_unique<WireTransport>(wire, mac);
  };
  member.voice = std::make_unique<PartyVoice>(std::move(config));
  return member;
}

Member Join(const std::shared_ptr<Wire>& wire, uint64_t xuid, int16_t level) {
  return Join(wire, xuid, std::make_unique<FakeDevice>(level));
}

template <typename F>
bool WaitFor(F done, int ms = 3000) {
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return done();
}

constexpr uint64_t kAnn = 0x0009000000000001;
constexpr uint64_t kBo = 0x0009000000000002;
constexpr uint64_t kCy = 0x0009000000000003;

}  // namespace

TEST_CASE("Party voice carries speech between members", "[voice][party]") {
  auto wire = std::make_shared<Wire>();
  Member ann = Join(wire, kAnn, 4000);
  Member bo = Join(wire, kBo, 0);  // Quiet: sends keepalives only.
  ann.voice->SetMembers({kAnn, kBo});
  bo.voice->SetMembers({kAnn, kBo});
  CHECK(PartyHoldsHost());

  REQUIRE(WaitFor([&] { return bo.device->PlayedNear(4000) >= 3200; }));
  const auto members = bo.voice->Members();
  REQUIRE(members.size() == 1);
  CHECK(members[0].xuid == kAnn);
  CHECK(members[0].connected);
  CHECK(members[0].talking);
  CHECK_FALSE(members[0].muted);
  // Bo is quiet: Ann hears nothing, but knows Bo is there.
  REQUIRE(WaitFor([&] {
    const auto m = ann.voice->Members();
    return m.size() == 1 && m[0].connected;
  }));
  CHECK_FALSE(ann.voice->Members()[0].talking);
  CHECK(ann.device->PlayedNear(4000) == 0);

  ann.voice.reset();
  bo.voice.reset();
  CHECK_FALSE(PartyHoldsHost());
}

TEST_CASE("Party voice mutes and leaves out strangers", "[voice][party]") {
  auto wire = std::make_shared<Wire>();
  Member ann = Join(wire, kAnn, 4000);
  Member bo = Join(wire, kBo, 0);
  // Cy isn't in Bo's party; Bo plays nothing of Cy.
  Member cy = Join(wire, kCy, 2500);
  ann.voice->SetMembers({kBo});
  bo.voice->SetMembers({kAnn});
  cy.voice->SetMembers({kBo});
  ann.voice->SetSelfMuted(true);

  REQUIRE(WaitFor([&] {
    const auto m = bo.voice->Members();
    return m.size() == 1 && m[0].connected && m[0].muted;
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(bo.device->PlayedNear(4000) == 0);
  CHECK(bo.device->PlayedNear(2500) == 0);

  ann.voice->SetSelfMuted(false);
  CHECK(WaitFor([&] { return bo.device->PlayedNear(4000) >= 1600; }));
  CHECK(bo.device->PlayedNear(2500) == 0);
}

TEST_CASE("Party voice waits for the game's headset to let go",
          "[voice][party]") {
  auto wire = std::make_shared<Wire>();
  SetTitleHoldsHost(true);
  Member ann = Join(wire, kAnn, 0);
  // The title's headset sees the claim and stops its device.
  CHECK(PartyHoldsHost());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK_FALSE(ann.device->started);
  CHECK_FALSE(ann.voice->running());
  SetTitleHoldsHost(false);
  CHECK(WaitFor([&] { return ann.voice->running(); }));
  CHECK(ann.device->started);
  ann.voice.reset();
  CHECK_FALSE(PartyHoldsHost());
}

#if XE_PLATFORM_APPLE
#include "xenia/apu/voice/apple/apple_voice_codec.h"

namespace {
// Speech-like: a 140 Hz voice with its harmonics, in syllables (4 a second)
// and phrases with pauses long enough for the talk gate to close.
int16_t Speech(size_t index) {
  const double t = double(index) / kSampleRate;
  const double phrase = std::fmod(t, 1.5);
  if (phrase > 1.0) {
    return 0;
  }
  const double syllable = 0.5 - 0.5 * std::cos(2.0 * M_PI * 4.0 * t);
  double voice = 0.0;
  for (int harmonic = 1; harmonic <= 12; ++harmonic) {
    voice += std::sin(2.0 * M_PI * 140.0 * harmonic * t) / harmonic;
  }
  return int16_t(5000.0 * syllable * voice);
}

// RMS of each 20 ms.
std::vector<double> Envelope(const std::vector<int16_t>& samples) {
  std::vector<double> envelope;
  for (size_t i = 0; i + kPartyFrameSamples <= samples.size();
       i += kPartyFrameSamples) {
    double sum = 0.0;
    for (size_t j = 0; j < kPartyFrameSamples; ++j) {
      sum += double(samples[i + j]) * samples[i + j];
    }
    envelope.push_back(std::sqrt(sum / kPartyFrameSamples));
  }
  return envelope;
}

double Correlation(const std::vector<double>& a, const std::vector<double>& b,
                   size_t lag) {
  const size_t n = std::min(a.size(), b.size() - lag);
  double ma = 0, mb = 0;
  for (size_t i = 0; i < n; ++i) {
    ma += a[i];
    mb += b[i + lag];
  }
  ma /= n;
  mb /= n;
  double ab = 0, aa = 0, bb = 0;
  for (size_t i = 0; i < n; ++i) {
    ab += (a[i] - ma) * (b[i + lag] - mb);
    aa += (a[i] - ma) * (a[i] - ma);
    bb += (b[i + lag] - mb) * (b[i + lag] - mb);
  }
  return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0.0;
}
}  // namespace

TEST_CASE("Apple's Opus carries speech through party voice",
          "[voice][party][apple]") {
  const VoiceCodecFactory opus = AppleOpusCodecFactory();
  REQUIRE(opus.encoder());
  REQUIRE(opus.decoder());
  auto wire = std::make_shared<Wire>();
  Member ann = Join(wire, kAnn, std::make_unique<FakeDevice>(Speech), opus);
  Member bo = Join(wire, kBo, std::make_unique<FakeDevice>(int16_t(0)), opus);
  ann.voice->SetMembers({kBo});
  bo.voice->SetMembers({kAnn});
  // Six phrases.
  std::this_thread::sleep_for(std::chrono::milliseconds(9000));
  std::vector<int16_t> played;
  {
    std::lock_guard lock(bo.device->mutex);
    played = bo.device->played;
  }
  ann.voice.reset();
  bo.voice.reset();

  std::vector<int16_t> sent(kSampleRate * 9);
  for (size_t i = 0; i < sent.size(); ++i) {
    sent[i] = Speech(i);
  }
  const auto heard = Envelope(played);
  const auto spoken = Envelope(sent);
  // The session's pacing and jitter buffer delay it by some frames.
  double best = 0.0;
  size_t best_lag = 0;
  for (size_t lag = 0; lag < 50 && lag < heard.size(); ++lag) {
    const double c = Correlation(spoken, heard, lag);
    if (c > best) {
      best = c;
      best_lag = lag;
    }
  }
  double spoken_level = 0, heard_level = 0;
  for (size_t i = 0; i + best_lag < heard.size() && i < spoken.size(); ++i) {
    spoken_level += spoken[i];
    heard_level += heard[i + best_lag];
  }
  INFO("envelope correlation " << best << " at " << best_lag * 20
                               << " ms, level ratio "
                               << heard_level / spoken_level);
  CHECK(best > 0.9);
  CHECK(heard_level / spoken_level > 0.7);
  CHECK(heard_level / spoken_level < 1.3);
}
#endif  // XE_PLATFORM_APPLE
