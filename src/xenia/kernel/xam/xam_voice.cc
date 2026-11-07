/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_voice.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/apu/voice/g726.h"
#include "xenia/apu/voice/upsampler.h"
#include "xenia/apu/voice/voice_device.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/memory.h"

namespace {
// iPhones and iPads have a microphone, and XeniOS switches voice in game, so
// it is on there; elsewhere it stays something to turn on.
#if XE_PLATFORM_IOS
constexpr bool kVoiceChatDefault = true;
#else
constexpr bool kVoiceChatDefault = false;
#endif
}  // namespace

DEFINE_bool(voice_chat, kVoiceChatDefault,
            "Voice chat in games through the host microphone and speakers: "
            "titles see a headset on the first controller.",
            "APU");

namespace xe {
namespace kernel {
namespace xam {

namespace {

using apu::voice::kSampleRate;

// The title reads the microphone at 16 kHz (the device rate) and plays to the
// earpiece at 8 kHz: a 600 Hz tone sent at 16 kHz arrives at 8 kHz.
constexpr uint32_t kCaptureRate = kSampleRate;
constexpr uint32_t kPlaybackRate = kSampleRate / 2;
static_assert(kCaptureRate == kSampleRate && kPlaybackRate * 2 == kSampleRate,
              "the headset converts only 8 kHz playback to the device rate");

// One host microphone, so one headset: the first controller's.
constexpr uint32_t kHeadsetUser = 0;
// The headset object titles get back. Nothing reads it but us; the size
// leaves room in case a title peeks at it.
constexpr uint32_t kHeadsetObjectSize = 0x100;
// Larger than any packet a title has been seen to use (0xA0 bytes), and
// small enough that one packet's worth of samples fits in the pacing credit.
constexpr uint32_t kMaxPacketBytes = 0x200;
// Packets a title may queue in each direction. Halo 3 keeps 10 capture packets
// queued.
constexpr size_t kMaxQueuedPackets = 32;
// What XamVoiceSubmitPacket returned before there were headsets, and still
// returns when there is none or its queue is full.
constexpr X_RESULT kSubmitBusy = 0x800700AA;
constexpr auto kTick = std::chrono::milliseconds(5);
// Glitches are logged at most this often (in samples): 5 s.
constexpr uint64_t kStatsInterval = kSampleRate * 5;
// Pacing credit is capped, so a stall (a debugger, a slow frame) doesn't
// release a burst of packets afterwards.
constexpr uint64_t kMaxCredit = kSampleRate / 5;
// Host audio older than this is dropped, which keeps latency bounded when the
// host's audio clock runs faster than ours.
constexpr size_t kMaxBuffered = kSampleRate / 10;
static_assert(kMaxPacketBytes * 4 <= kMaxCredit &&
                  kMaxPacketBytes * 2 <= kMaxBuffered,
              "a packet's worth of samples must fit in the pacing credit");
// Decoded playback louder than this (RMS of 16-bit samples) counts as speech
// for the "hears a talker" log line.
constexpr double kTalkRms = 300.0;

// A queued packet as the title submitted it. The headset reads the guest
// packet only at submit, so a title rewriting it while queued can't move
// where the headset writes.
struct QueuedPacket {
  uint32_t packet_ptr;
  uint32_t buffer_ptr;
  uint32_t size;
};

struct Headset {
  uint32_t handle = 0;
  uint32_t user_index = 0;
  apu::voice::G726 encoder;
  apu::voice::G726 decoder;
  apu::voice::Upsampler2x upsampler;
  std::deque<QueuedPacket> capture_queue;
  std::deque<QueuedPacket> playback_queue;
  uint64_t capture_credit = 0;
  uint64_t playback_credit = 0;
  uint64_t captured = 0;
  uint64_t played = 0;
  // Samples the microphone didn't have in time (sent as silence), and samples
  // the speaker had no room for (dropped).
  uint64_t capture_short = 0;
  uint64_t playback_dropped = 0;
  // Microphone samples discarded because they came faster than packets.
  uint64_t capture_trimmed = 0;
  // What the last stats line reported, so only changes are logged.
  uint64_t reported_short = 0;
  uint64_t reported_dropped = 0;
  uint64_t reported_trimmed = 0;
  uint32_t reported_dry = 0;
  bool hearing = false;
  std::chrono::steady_clock::time_point last_heard{};
};

class Headsets {
 public:
  static Headsets& Get() {
    static Headsets headsets;
    return headsets;
  }

  // At exit, for a kernel that never shut down: the thread and the host
  // audio only, since guest memory may already be gone.
  ~Headsets() {
    std::unique_ptr<apu::voice::VoiceDevice> device;
    std::thread thread;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      headset_.reset();
      StopThreadLocked(&thread, &device);
    }
    Finish(std::move(thread), std::move(device));
  }

  // Whether titles get a headset at all.
  bool enabled() const {
    if (apu::voice::TestDeviceRequested()) {
      return true;
    }
    return apu::voice::HostDeviceAvailable() &&
           (apu::voice::HostVoiceSwitchable() || cvars::voice_chat);
  }

  // Whether the headset's host microphone and speaker should run now. The
  // app may switch voice_chat while a game runs; checked every tick.
  static bool host_audio_wanted() {
    return apu::voice::TestDeviceRequested() || cvars::voice_chat;
  }

  X_RESULT Create(uint32_t user_index, uint32_t* out_handle) {
    *out_handle = 0;
    if (user_index != kHeadsetUser || !enabled()) {
      return X_ERROR_ACCESS_DENIED;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (headset_) {
      return X_ERROR_ACCESS_DENIED;
    }
    memory_ = kernel_state()->memory();
    const uint32_t handle = memory_->SystemHeapAlloc(kHeadsetObjectSize);
    if (!handle) {
      return X_ERROR_ACCESS_DENIED;
    }
    std::memset(memory_->TranslateVirtual(handle), 0, kHeadsetObjectSize);
    headset_ = std::make_unique<Headset>();
    headset_->handle = handle;
    headset_->user_index = user_index;
    // The headset thread opens the host audio while it is wanted, and closes
    // it when the app switches voice off.
    device_failed_ = false;
    StartThreadLocked();
    *out_handle = handle;
    XELOGI("Voice: headset on user {}", user_index);
    return X_ERROR_SUCCESS;
  }

  void Close(uint32_t handle) {
    std::unique_ptr<apu::voice::VoiceDevice> device;
    std::thread thread;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!headset_ || headset_->handle != handle) {
        return;
      }
      CloseHeadsetLocked();
      StopThreadLocked(&thread, &device);
    }
    Finish(std::move(thread), std::move(device));
  }

  bool Present(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    return headset_ && headset_->handle == handle;
  }

  X_RESULT Submit(uint32_t handle, uint32_t direction, uint32_t packet_ptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!headset_ || headset_->handle != handle) {
      return kSubmitBusy;
    }
    if (direction != kVoicePacketCapture && direction != kVoicePacketPlayback) {
      return X_E_INVALIDARG;
    }
    if (!Mapped(packet_ptr, sizeof(X_VOICE_PACKET))) {
      WarnRejected("packet", packet_ptr, sizeof(X_VOICE_PACKET));
      return X_E_INVALIDARG;
    }
    auto* packet = memory_->TranslateVirtual<X_VOICE_PACKET*>(packet_ptr);
    const QueuedPacket queued{packet_ptr, packet->buffer_ptr,
                              packet->buffer_size};
    if (!queued.size || queued.size > kMaxPacketBytes ||
        !Mapped(queued.buffer_ptr, queued.size)) {
      WarnRejected("buffer", queued.buffer_ptr, queued.size);
      return X_E_INVALIDARG;
    }
    if (packet->format != kVoicePacketFormatG726 && !warned_format_) {
      warned_format_ = true;
      // TODO(xenios-jp): only format 1 has been seen; treat others as it.
      XELOGW("Voice: packet format {} is unknown, treated as G.726",
             uint32_t(packet->format));
    }
    auto& queue = direction == kVoicePacketCapture ? headset_->capture_queue
                                                   : headset_->playback_queue;
    if (queue.size() >= kMaxQueuedPackets) {
      return kSubmitBusy;
    }
    // A packet still queued (in either direction) can't be queued again.
    for (const auto* other_queue :
         {&headset_->capture_queue, &headset_->playback_queue}) {
      for (const QueuedPacket& other : *other_queue) {
        if (other.packet_ptr == packet_ptr) {
          return X_E_INVALIDARG;
        }
      }
    }
    packet->status = X_STATUS_PENDING;
    queue.push_back(queued);
    return X_ERROR_SUCCESS;
  }

  // The kernel is going away, after its threads: a later kernel (the next
  // game in the same process) starts again with no headset.
  void Shutdown() {
    std::unique_ptr<apu::voice::VoiceDevice> device;
    std::thread thread;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (headset_) {
        CloseHeadsetLocked();
      }
      StopThreadLocked(&thread, &device);
    }
    Finish(std::move(thread), std::move(device));
  }

 private:
  // True when every byte of the range is in committed guest memory.
  bool Mapped(uint32_t address, uint32_t size) const {
    if (!address || !size || address + size - 1 < address) {
      return false;
    }
    for (uint32_t end : {address, address + size - 1}) {
      BaseHeap* heap = memory_->LookupHeap(end);
      if (!heap || !heap->IsPageCommitted(end)) {
        return false;
      }
    }
    return true;
  }

  void WarnRejected(const char* what, uint32_t address, uint32_t size) {
    if (!warned_rejected_) {
      warned_rejected_ = true;
      XELOGW("Voice: rejected a packet: {} {:08X}+{:X} is not mapped", what,
             address, size);
    }
  }

  // Completes what is still queued with nothing done, so a title waiting on a
  // packet sees it finish, then frees the headset.
  void CloseHeadsetLocked() {
    Headset& headset = *headset_;
    for (auto* queue : {&headset.capture_queue, &headset.playback_queue}) {
      for (const QueuedPacket& queued : *queue) {
        Complete(queued, 0);
      }
      queue->clear();
    }
    XELOGI("Voice: headset on user {} closed after {} captured, {} played "
           "packets ({} samples short from the microphone, {} dropped at the "
           "speaker)",
           headset.user_index, headset.captured, headset.played,
           headset.capture_short, headset.playback_dropped);
    memory_->SystemHeapFree(headset.handle);
    headset_.reset();
  }

  void StartThreadLocked() {
    if (thread_.joinable()) {
      return;
    }
    // Each thread has its own stop flag: a Close that is still joining the
    // last thread must not stop the one a following Create starts.
    stop_ = std::make_shared<bool>(false);
    thread_ = std::thread([this, stop = stop_] { Run(stop); });
  }

  void StopThreadLocked(std::thread* thread,
                        std::unique_ptr<apu::voice::VoiceDevice>* device) {
    if (stop_) {
      *stop_ = true;
      stop_.reset();
    }
    wake_.notify_all();
    *thread = std::move(thread_);
    *device = std::move(device_);
  }

  // Outside the lock: the thread takes it on every tick.
  static void Finish(std::thread thread,
                     std::unique_ptr<apu::voice::VoiceDevice> device) {
    if (thread.joinable()) {
      thread.join();
    }
    if (device) {
      device->Stop();
    }
  }

  void Run(std::shared_ptr<bool> stop) {
    threading::set_name("Voice Headset");
    const auto start = std::chrono::steady_clock::now();
    uint64_t clock = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    next_stats_ = kStatsInterval;
    while (!*stop) {
      wake_.wait_for(lock, kTick);
      if (*stop) {
        break;
      }
      // Samples due since the last tick, from the start so rounding never
      // accumulates.
      const auto elapsed = std::chrono::steady_clock::now() - start;
      const uint64_t target = uint64_t(
          std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
              .count() *
          kSampleRate / 1000000);
      const uint64_t due = target - clock;
      clock = target;
      if (!headset_) {
        continue;
      }
      FollowHostSwitch(lock, *stop);
      if (*stop || !headset_) {
        break;
      }
      if (device_ && !device_->clocked()) {
        device_->Advance(size_t(due));
      }
      ServiceCapture(*headset_, due);
      ServicePlayback(*headset_, due);
      if (device_ && clock >= next_stats_) {
        next_stats_ = clock + kStatsInterval;
        LogGlitches(*headset_);
      }
    }
  }

  // On the headset thread, with |lock| held: opens the host audio when voice
  // is wanted and closes it when it is switched off, so the microphone (and
  // the system's microphone indicator) is off while voice is. Starting and
  // stopping can block, so they run outside the lock.
  void FollowHostSwitch(std::unique_lock<std::mutex>& lock, const bool& stop) {
    const bool wanted = host_audio_wanted();
    if (!wanted) {
      device_failed_ = false;
      if (device_) {
        std::unique_ptr<apu::voice::VoiceDevice> device = std::move(device_);
        lock.unlock();
        device->Stop();
        device.reset();
        lock.lock();
        XELOGI("Voice: host audio off (voice switched off)");
      }
      return;
    }
    if (device_ || device_failed_) {
      return;
    }
    lock.unlock();
    std::unique_ptr<apu::voice::VoiceDevice> device =
        apu::voice::CreateVoiceDevice();
    const bool started = device && device->Start();
    lock.lock();
    if (!started || stop || !headset_) {
      if (!started) {
        // Tried again once voice is switched off and on.
        device_failed_ = true;
        XELOGW("Voice: no host audio for the headset");
      }
      lock.unlock();
      // Stopped by its destructor if it started at all.
      device.reset();
      lock.lock();
      return;
    }
    device->capture().Discard();
    device_ = std::move(device);
    XELOGI("Voice: host audio on for user {}", headset_->user_index);
  }

  // Each audible glitch source since the last line, when there were any.
  void LogGlitches(Headset& headset) {
    const uint32_t dry = device_->playback_dry();
    if (headset.capture_short == headset.reported_short &&
        headset.playback_dropped == headset.reported_dropped &&
        headset.capture_trimmed == headset.reported_trimmed &&
        dry == headset.reported_dry) {
      return;
    }
    constexpr uint64_t kSamplesPerMs = kSampleRate / 1000;
    XELOGI(
        "Voice: speaker dropped {} ms and ran dry {} times; microphone padded "
        "{} ms and trimmed {} ms",
        (headset.playback_dropped - headset.reported_dropped) / kSamplesPerMs,
        dry - headset.reported_dry,
        (headset.capture_short - headset.reported_short) / kSamplesPerMs,
        (headset.capture_trimmed - headset.reported_trimmed) / kSamplesPerMs);
    headset.reported_short = headset.capture_short;
    headset.reported_dropped = headset.playback_dropped;
    headset.reported_trimmed = headset.capture_trimmed;
    headset.reported_dry = dry;
  }

  // Fills queued capture packets from the microphone, one packet per packet's
  // worth of elapsed time.
  // Without host audio (voice switched off) the packets carry silence.
  void ServiceCapture(Headset& headset, uint64_t due) {
    headset.capture_credit =
        std::min(headset.capture_credit + due, kMaxCredit);
    if (!device_) {
      while (!headset.capture_queue.empty() &&
             headset.capture_credit >= headset.capture_queue.front().size * 2) {
        const QueuedPacket queued = headset.capture_queue.front();
        headset.capture_queue.pop_front();
        headset.capture_credit -= queued.size * 2;
        pcm_.assign(queued.size * 2, int16_t(0));
        headset.encoder.EncodePacked(
            pcm_.data(), queued.size,
            memory_->TranslateVirtual<uint8_t*>(queued.buffer_ptr));
        Complete(queued, queued.size);
        ++headset.captured;
      }
      return;
    }
    auto& ring = device_->capture();
    while (!headset.capture_queue.empty()) {
      const QueuedPacket queued = headset.capture_queue.front();
      const uint32_t samples = queued.size * 2;
      if (headset.capture_credit < samples) {
        break;
      }
      // Credit built up while the title was late is spent on audio the
      // microphone has, not on silence; only a microphone that has given
      // nothing for kMaxCredit gets silence, so the title still hears back.
      if (ring.Available() < samples && headset.capture_credit < kMaxCredit) {
        break;
      }
      headset.capture_credit -= samples;
      headset.capture_queue.pop_front();
      pcm_.resize(samples);
      const size_t got = ring.Read(pcm_.data(), samples);
      std::fill(pcm_.begin() + got, pcm_.end(), int16_t(0));
      headset.capture_short += samples - got;
      // The title owns a queued packet's memory until it completes, as on a
      // console: checked at submit, not again here.
      headset.encoder.EncodePacked(
          pcm_.data(), queued.size,
          memory_->TranslateVirtual<uint8_t*>(queued.buffer_ptr));
      Complete(queued, queued.size);
      ++headset.captured;
    }
    // A host clock faster than ours would otherwise build up delay.
    const size_t buffered = ring.Available();
    if (buffered > kMaxBuffered) {
      pcm_.resize(buffered - kMaxBuffered);
      ring.Read(pcm_.data(), pcm_.size());
      headset.capture_trimmed += pcm_.size();
    }
  }

  // Plays queued playback packets, one packet per packet's worth of elapsed
  // time, so the title produces audio at the rate it is heard.
  // Without host audio (voice switched off) the packets are dropped unheard.
  void ServicePlayback(Headset& headset, uint64_t due) {
    // Credit kept while the queue is empty makes up for a title that submits
    // late; the cap and kMaxBuffered keep that from building up delay.
    headset.playback_credit =
        std::min(headset.playback_credit + due, kMaxCredit);
    while (!headset.playback_queue.empty()) {
      const QueuedPacket queued = headset.playback_queue.front();
      // Two codes a byte at 8 kHz, so four device-rate samples a byte.
      const uint32_t samples = queued.size * 4;
      if (headset.playback_credit < samples) {
        break;
      }
      headset.playback_credit -= samples;
      headset.playback_queue.pop_front();
      decoded_.resize(queued.size * 2);
      headset.decoder.DecodePacked(
          memory_->TranslateVirtual<const uint8_t*>(queued.buffer_ptr),
          queued.size, decoded_.data());
      pcm_.resize(samples);
      headset.upsampler.Process(decoded_.data(), decoded_.size(), pcm_.data());
      NoteSpeech(headset, pcm_);
      // A host speaker slower than our clock drops audio instead of building
      // up delay.
      if (!device_) {
        // Voice is switched off: not heard, and not a glitch.
      } else if (device_->playback().Available() + samples <= kMaxBuffered) {
        device_->playback().Write(pcm_.data(), samples);
      } else {
        headset.playback_dropped += samples;
      }
      Complete(queued, queued.size);
      ++headset.played;
    }
  }

  void Complete(const QueuedPacket& queued, uint32_t bytes) {
    auto* packet =
        memory_->TranslateVirtual<X_VOICE_PACKET*>(queued.packet_ptr);
    packet->bytes_done = bytes;
    // The title polls the status: everything else must be visible first.
    std::atomic_thread_fence(std::memory_order_release);
    packet->status = X_STATUS_SUCCESS;
  }

  // Logs when someone starts talking to this headset, for tests and support.
  static void NoteSpeech(Headset& headset, const std::vector<int16_t>& pcm) {
    double sum = 0;
    for (int16_t s : pcm) {
      sum += double(s) * s;
    }
    const double rms = pcm.empty() ? 0 : std::sqrt(sum / pcm.size());
    const auto now = std::chrono::steady_clock::now();
    if (rms >= kTalkRms) {
      if (!headset.hearing) {
        headset.hearing = true;
        XELOGI("Voice: user {} hears a talker", headset.user_index);
      }
      headset.last_heard = now;
    } else if (headset.hearing &&
               now - headset.last_heard > std::chrono::seconds(2)) {
      headset.hearing = false;
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  // The headset thread's: when it next logs glitches, on its sample clock.
  uint64_t next_stats_ = 0;
  std::thread thread_;
  // The running thread's stop flag, guarded by mutex_.
  std::shared_ptr<bool> stop_;
  std::unique_ptr<apu::voice::VoiceDevice> device_;
  std::unique_ptr<Headset> headset_;
  Memory* memory_ = nullptr;
  std::vector<int16_t> pcm_;
  std::vector<int16_t> decoded_;
  // The host audio failed to start; not retried until voice is switched off.
  bool device_failed_ = false;
  bool warned_format_ = false;
  bool warned_rejected_ = false;
};

}  // namespace

void VoiceShutdown() { Headsets::Get().Shutdown(); }

dword_result_t XamVoiceIsActiveProcess_entry() {
  // Returning 0 here will short-circuit a bunch of voice stuff.
  return Headsets::Get().enabled() ? 1 : 0;
}
DECLARE_XAM_EXPORT1(XamVoiceIsActiveProcess, kNone, kSketchy);

dword_result_t XamVoiceCreate_entry(dword_t user_index,
                                    dword_t max_attached_packets,  // 0xF
                                    lpdword_t out_voice_ptr) {
  if (!out_voice_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  uint32_t handle = 0;
  const X_RESULT result = Headsets::Get().Create(user_index, &handle);
  *out_voice_ptr = handle;
  return result;
}
DECLARE_XAM_EXPORT1(XamVoiceCreate, kNone, kSketchy);

dword_result_t XamVoiceClose_entry(lpunknown_t voice_ptr) {
  Headsets::Get().Close(voice_ptr.guest_address());
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceClose, kNone, kImplemented);

dword_result_t XamVoiceHeadsetPresent_entry(lpunknown_t voice_ptr) {
  return Headsets::Get().Present(voice_ptr.guest_address()) ? 1 : 0;
}
DECLARE_XAM_EXPORT1(XamVoiceHeadsetPresent, kNone, kImplemented);

dword_result_t XamVoiceSubmitPacket_entry(lpunknown_t voice_ptr,
                                          dword_t direction,
                                          lpunknown_t packet_ptr) {
  return Headsets::Get().Submit(voice_ptr.guest_address(), direction,
                                packet_ptr.guest_address());
}
DECLARE_XAM_EXPORT1(XamVoiceSubmitPacket, kNone, kSketchy);

dword_result_t XamVoiceGetMicArrayStatus_entry() {
  // Returning 0 here tells caller mic is not connected
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayStatus, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Voice);
