/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_PARTY_VOICE_H_
#define XENIA_APU_VOICE_PARTY_VOICE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/apu/voice/voice_codec.h"
#include "xenia/apu/voice/voice_device.h"
#include "xenia/net/packet_transport.h"

namespace xe {
namespace apu {
namespace voice {

// What this member knows of another member's voice.
struct PartyVoiceMember {
  uint64_t xuid = 0;
  // Heard from in the last few seconds: a direct path reaches them. False for
  // members no path reaches (there's no TURN relay), who aren't heard.
  bool connected = false;
  bool talking = false;
  // They muted themselves.
  bool muted = false;
  bool operator==(const PartyVoiceMember&) const = default;
};

struct PartyVoiceConfig {
  // This member's online XUID.
  uint64_t xuid = 0;
  // The party's voice room (OpenRelayTransport in the app) for this member's
  // MAC there. Called on the session's thread.
  std::function<std::unique_ptr<net::secure::PacketTransport>(
      const net::secure::Mac& mac)>
      open_transport;
  // The microphone and speaker; CreateVoiceDevice() when empty.
  std::function<std::unique_ptr<VoiceDevice>()> open_device;
  // GetVoiceCodecFactory() when empty.
  VoiceCodecFactory codec;
  // Members() or running() changed. On the session's thread.
  std::function<void()> changed;
};

// Party chat for one member: speech from the voice device goes to every other
// member the voice room reaches, and theirs is mixed into the device's
// speaker. It holds the host from the running title's headset (game chat)
// while it exists, and starts its own device once the title's has stopped.
class PartyVoice {
 public:
  explicit PartyVoice(PartyVoiceConfig config);
  // Stops the device and gives the host back to the title.
  ~PartyVoice();

  // The other members. Only these are played; the list in Members() follows.
  void SetMembers(std::vector<uint64_t> xuids);
  // Muted: nothing is sent, and the others see this member muted. Still
  // hears them.
  void SetSelfMuted(bool muted);

  std::vector<PartyVoiceMember> Members() const;
  // The voice device runs.
  bool running() const;
  // The device couldn't start (no microphone access, say).
  bool failed() const;

  static constexpr uint32_t kFrameMs = 20;
  static constexpr uint32_t kKeepaliveMs = 1000;
  // A member not heard for this long isn't connected.
  static constexpr uint32_t kConnectedMs = 3000;
  // A member whose last speech is older than this isn't talking.
  static constexpr uint32_t kTalkingMs = 300;
  // Packets a member's queue holds before playing (60 ms).
  static constexpr size_t kJitterPrime = 3;
  static constexpr size_t kJitterMax = 12;
  // Longest the title gets to stop its device.
  static constexpr uint32_t kTitleWaitMs = 3000;

 private:
  struct Remote;
  void Run(std::stop_token stop);
  void ServiceCapture(uint64_t now);
  void ServicePlayback(uint64_t now);
  void Receive(std::span<const uint8_t> frame, uint64_t now);
  void Send(bool audio, std::vector<uint8_t> bytes, uint64_t now);
  void Report(uint64_t now);

  PartyVoiceConfig config_;
  net::secure::Mac mac_{};

  mutable std::mutex mutex_;
  std::vector<uint64_t> members_;
  bool self_muted_ = false;
  std::vector<PartyVoiceMember> reported_;
  bool running_ = false;
  bool failed_ = false;
  net::secure::PacketTransport* transport_ = nullptr;  // Guarded, for Wake().

  // The session thread's.
  std::unique_ptr<VoiceDevice> device_;
  std::unique_ptr<VoiceEncoder> encoder_;
  std::vector<std::unique_ptr<Remote>> remotes_;
  uint16_t sequence_ = 0;
  uint64_t sent_ms_ = 0;
  uint64_t voice_until_ms_ = 0;

  std::jthread thread_;
};

}  // namespace voice
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_PARTY_VOICE_H_
