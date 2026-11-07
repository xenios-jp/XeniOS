/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice/party_voice.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/net/party_voice_wire.h"

namespace xe {
namespace apu {
namespace voice {

namespace {

using net::party_voice::Packet;

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Speech after the voice processing's gain control is well above this; room
// noise it leaves is below. Mean absolute level, of 32767.
constexpr int kTalkLevel = 300;
// Keeps sending this long after the level drops, so word endings get out.
constexpr uint64_t kTalkHangMs = 400;
// The speaker ring holds at most this much (120 ms); more is dropped, so a
// stall doesn't leave the party behind for good.
constexpr size_t kPlaybackMax = kSampleRate * 120 / 1000;
// Captured frames beyond this many are old: dropped.
constexpr size_t kCaptureMaxFrames = 4;

}  // namespace

struct PartyVoice::Remote {
  uint64_t xuid = 0;
  uint64_t heard_ms = 0;
  uint64_t audio_ms = 0;
  bool muted = false;
  // Audio packets by sequence, unwrapped to 32 bits.
  std::map<uint32_t, std::vector<uint8_t>> queue;
  bool have_sequence = false;
  uint16_t last_sequence = 0;
  uint32_t last_unwrapped = 0;
  bool primed = false;
  uint32_t next = 0;
  std::unique_ptr<VoiceDecoder> decoder;

  uint32_t Unwrap(uint16_t sequence) {
    if (!have_sequence) {
      have_sequence = true;
      last_sequence = sequence;
      last_unwrapped = 1u << 16;  // Room to go back.
      return last_unwrapped;
    }
    const uint32_t unwrapped =
        last_unwrapped + int16_t(uint16_t(sequence - last_sequence));
    if (int32_t(unwrapped - last_unwrapped) > 0) {
      last_sequence = sequence;
      last_unwrapped = unwrapped;
    }
    return unwrapped;
  }
};

PartyVoice::PartyVoice(PartyVoiceConfig config) : config_(std::move(config)) {
  if (!config_.open_device) {
    config_.open_device = [] { return CreateVoiceDevice(); };
  }
  if (!config_.codec.encoder || !config_.codec.decoder) {
    config_.codec = GetVoiceCodecFactory();
  }
  mac_ = net::party_voice::RandomVoiceMac();
  SetPartyHoldsHost(true);
  thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

PartyVoice::~PartyVoice() {
  thread_.request_stop();
  {
    std::lock_guard lock(mutex_);
    if (transport_) {
      transport_->Wake();
    }
  }
  thread_.join();
  SetPartyHoldsHost(false);
}

void PartyVoice::SetMembers(std::vector<uint64_t> xuids) {
  std::erase(xuids, config_.xuid);
  std::lock_guard lock(mutex_);
  members_ = std::move(xuids);
}

void PartyVoice::SetSelfMuted(bool muted) {
  std::lock_guard lock(mutex_);
  self_muted_ = muted;
}

std::vector<PartyVoiceMember> PartyVoice::Members() const {
  std::lock_guard lock(mutex_);
  return reported_;
}

bool PartyVoice::running() const {
  std::lock_guard lock(mutex_);
  return running_;
}

bool PartyVoice::failed() const {
  std::lock_guard lock(mutex_);
  return failed_;
}

void PartyVoice::Run(std::stop_token stop) {
  threading::set_name("Party Voice");
  if (!config_.codec.encoder || !config_.codec.decoder ||
      !config_.open_transport) {
    XELOGE("Party voice: no codec or no way to the voice room");
    std::lock_guard lock(mutex_);
    failed_ = true;
    return;
  }
  std::unique_ptr<net::secure::PacketTransport> transport =
      config_.open_transport(mac_);
  if (!transport) {
    XELOGE("Party voice: the voice room can't be opened");
    std::lock_guard lock(mutex_);
    failed_ = true;
    return;
  }
  transport->Activate();
  {
    std::lock_guard lock(mutex_);
    transport_ = transport.get();
  }
  encoder_ = config_.codec.encoder();

  const uint64_t asked_ms = NowMs();
  bool waited_for_title = false;
  uint64_t next_tick = NowMs();
  std::vector<uint8_t> frame;
  while (!stop.stop_requested() && transport->healthy()) {
    uint64_t now = NowMs();
    // The title's headset lets go of the host on its next tick.
    if (!device_ && !waited_for_title) {
      if (!TitleHoldsHost() || now - asked_ms >= kTitleWaitMs) {
        waited_for_title = true;
        if (TitleHoldsHost()) {
          XELOGW("Party voice: the game's headset still has the microphone");
        }
        std::unique_ptr<VoiceDevice> device = config_.open_device();
        if (device) {
          device->set_advanced_ducking(true);
        }
        const bool started = device && device->Start();
        std::lock_guard lock(mutex_);
        if (started) {
          device->capture().Discard();
          device_ = std::move(device);
          running_ = true;
          XELOGI("Party voice: microphone and speaker on");
        } else {
          failed_ = true;
          XELOGW("Party voice: no microphone or speaker; listening only");
        }
      }
    }
    const uint32_t wait =
        next_tick > now ? uint32_t(std::min<uint64_t>(next_tick - now, 50))
                        : 0;
    if (transport->Receive(frame, wait)) {
      Receive(frame, NowMs());
    }
    now = NowMs();
    if (now < next_tick) {
      continue;
    }
    // A long stall (a breakpoint, the app in the background) restarts the
    // clock instead of catching up.
    next_tick = now - next_tick > 200 ? now + kFrameMs : next_tick + kFrameMs;
    if (device_ && !device_->clocked()) {
      device_->Advance(kPartyFrameSamples);
    }
    ServiceCapture(now);
    ServicePlayback(now);
    Report(now);
  }
  if (device_) {
    device_->Stop();
    device_.reset();
  }
  {
    std::lock_guard lock(mutex_);
    transport_ = nullptr;
    running_ = false;
  }
  transport.reset();
  XELOGI("Party voice: off");
}

void PartyVoice::ServiceCapture(uint64_t now) {
  bool muted;
  {
    std::lock_guard lock(mutex_);
    muted = self_muted_;
  }
  bool sent_audio = false;
  if (device_) {
    SampleRing& capture = device_->capture();
    while (capture.Available() > kCaptureMaxFrames * kPartyFrameSamples) {
      int16_t old[kPartyFrameSamples];
      capture.Read(old, kPartyFrameSamples);
    }
    while (capture.Available() >= kPartyFrameSamples) {
      std::array<int16_t, kPartyFrameSamples> samples;
      capture.Read(samples.data(), samples.size());
      if (muted) {
        continue;
      }
      int64_t level = 0;
      for (int16_t sample : samples) {
        level += std::abs(int(sample));
      }
      if (level / int64_t(samples.size()) >= kTalkLevel) {
        voice_until_ms_ = now + kTalkHangMs;
      }
      if (now >= voice_until_ms_) {
        continue;
      }
      std::vector<uint8_t> packet = encoder_->Encode(samples);
      if (!packet.empty()) {
        Send(true, std::move(packet), now);
        sent_audio = true;
      }
    }
  }
  if (!sent_audio && now - sent_ms_ >= kKeepaliveMs) {
    Send(false, {}, now);
  }
}

void PartyVoice::Send(bool audio, std::vector<uint8_t> bytes, uint64_t now) {
  Packet packet;
  packet.kind = audio ? Packet::Kind::kAudio : Packet::Kind::kKeepalive;
  packet.xuid = config_.xuid;
  packet.sequence = sequence_++;
  {
    std::lock_guard lock(mutex_);
    packet.muted = self_muted_;
  }
  packet.audio = std::move(bytes);
  if (auto frame = net::party_voice::EncodeFrame(packet, mac_)) {
    transport_->Send(*frame);
  }
  sent_ms_ = now;
}

void PartyVoice::Receive(std::span<const uint8_t> frame, uint64_t now) {
  auto packet = net::party_voice::DecodeFrame(frame);
  if (!packet || packet->xuid == config_.xuid) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (std::find(members_.begin(), members_.end(), packet->xuid) ==
        members_.end()) {
      return;
    }
  }
  auto found = std::find_if(remotes_.begin(), remotes_.end(),
                            [&](const auto& r) { return r->xuid == packet->xuid; });
  if (found == remotes_.end()) {
    remotes_.push_back(std::make_unique<Remote>());
    remotes_.back()->xuid = packet->xuid;
    found = std::prev(remotes_.end());
  }
  Remote& remote = **found;
  remote.heard_ms = now;
  remote.muted = packet->muted;
  const uint32_t sequence = remote.Unwrap(packet->sequence);
  if (packet->kind != Packet::Kind::kAudio) {
    return;
  }
  remote.audio_ms = now;
  if (remote.primed && int32_t(sequence - remote.next) < 0) {
    return;  // Too late.
  }
  remote.queue.emplace(sequence, std::move(packet->audio));
  while (remote.queue.size() > kJitterMax) {
    remote.queue.erase(remote.queue.begin());
    if (remote.primed) {
      remote.next = remote.queue.begin()->first;
    }
  }
}

void PartyVoice::ServicePlayback(uint64_t now) {
  std::vector<uint64_t> members;
  {
    std::lock_guard lock(mutex_);
    members = members_;
  }
  std::erase_if(remotes_, [&](const auto& r) {
    return std::find(members.begin(), members.end(), r->xuid) ==
           members.end();
  });
  std::array<int32_t, kPartyFrameSamples> mix{};
  bool any = false;
  for (auto& remote : remotes_) {
    if (!remote->primed) {
      if (remote->queue.size() < kJitterPrime) {
        // A short burst (the end of a word) plays once nothing more comes.
        if (remote->queue.empty() || now - remote->audio_ms < 3 * kFrameMs) {
          continue;
        }
      }
      remote->primed = true;
      remote->next = remote->queue.begin()->first;
    }
    if (remote->queue.empty()) {
      remote->primed = false;
      continue;
    }
    std::array<int16_t, kPartyFrameSamples> samples{};
    auto it = remote->queue.find(remote->next);
    if (it != remote->queue.end()) {
      if (!remote->decoder) {
        remote->decoder = config_.codec.decoder();
      }
      if (remote->decoder && remote->decoder->Decode(it->second, samples)) {
        for (size_t i = 0; i < samples.size(); ++i) {
          mix[i] += samples[i];
        }
        any = true;
      }
      remote->queue.erase(it);
    }
    // A lost packet plays as silence.
    ++remote->next;
    while (!remote->queue.empty() &&
           int32_t(remote->queue.begin()->first - remote->next) < 0) {
      remote->queue.erase(remote->queue.begin());
    }
  }
  if (!any || !device_) {
    return;
  }
  SampleRing& playback = device_->playback();
  if (playback.Available() > kPlaybackMax) {
    return;
  }
  std::array<int16_t, kPartyFrameSamples> out;
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = int16_t(std::clamp(mix[i], -32768, 32767));
  }
  playback.Write(out.data(), out.size());
}

void PartyVoice::Report(uint64_t now) {
  std::vector<PartyVoiceMember> members;
  {
    std::lock_guard lock(mutex_);
    for (uint64_t xuid : members_) {
      PartyVoiceMember member;
      member.xuid = xuid;
      for (const auto& remote : remotes_) {
        if (remote->xuid == xuid) {
          member.connected = now - remote->heard_ms < kConnectedMs;
          member.talking = member.connected && remote->audio_ms &&
                           now - remote->audio_ms < kTalkingMs;
          member.muted = member.connected && remote->muted;
        }
      }
      members.push_back(member);
    }
    if (members == reported_) {
      return;
    }
    reported_ = members;
  }
  if (config_.changed) {
    config_.changed();
  }
}

}  // namespace voice
}  // namespace apu
}  // namespace xe
