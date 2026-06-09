/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/phase/phase_audio_driver.h"

#import <AVFoundation/AVFoundation.h>
#import <PHASE/PHASE.h>
#import <simd/simd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

DEFINE_bool(apu_phase_head_tracking, false,
            "Enable PHASE AirPods head tracking for the spatial audio backend. "
            "Requires the com.apple.developer.coremotion.head-pose entitlement; "
            "leaving it on without that entitlement provisioned can make title "
            "audio start raise and crash. Off by default.",
            "APU");

namespace xe {
namespace apu {
namespace phase {

namespace {
// The guest delivers the main system as 6 planar big-endian float channels of
// 256 samples at 48 kHz (FL, FR, FC, LFE, BL, BR). The PHASE ambient mixer
// renders each channel from its speaker direction and ignores LFE.
constexpr uint32_t kChannelSamples = AudioDriver::kChannelSamplesDefault;  // 256
// Pool is sized at/above the audio system's max queued frames so the guest
// (throttled by the pre-filled semaphore) never starves the free list.
constexpr size_t kBufferPoolSize = 64;
// Bounded wait so Shutdown never blocks forever on a wedged render thread.
constexpr int kShutdownDrainPollMs = 2;
constexpr int kShutdownDrainMaxPolls = 250;  // ~0.5s
// Heartbeat cadence for the steady-state stats log (~5s at 48kHz/256).
constexpr uint64_t kHeartbeatFrames = 1000;

const char* NSErrorCStr(NSError* error) {
  return error ? [[error localizedDescription] UTF8String] : "unknown error";
}
}  // namespace

// State shared with the scheduleBuffer completion blocks. Held by a shared_ptr
// so a late callback is still safe if the driver is torn down first.
struct PHASEAudioDriverShared {
  std::mutex mutex;
  std::vector<AVAudioPCMBuffer*> free_pool;
  std::atomic<int> outstanding{0};
  xe::threading::Semaphore* semaphore = nullptr;

  // Diagnostics.
  std::atomic<uint64_t> frames_submitted{0};
  std::atomic<uint64_t> frames_completed{0};
  std::atomic<uint64_t> underruns{0};
  std::atomic<uint64_t> dropped_inactive{0};
  std::atomic<bool> logged_first_submit{false};
  std::atomic<bool> logged_first_complete{false};
};

struct PHASEAudioDriver::Impl {
  std::shared_ptr<PHASEAudioDriverShared> shared =
      std::make_shared<PHASEAudioDriverShared>();

  uint32_t frequency = 0;
  uint32_t channels = 0;
  uint32_t channel_samples = kChannelSamples;
  std::atomic<float> volume{1.0f};
  std::atomic<bool> shutting_down{false};
  std::atomic<bool> paused{false};

  // PHASE graph (strong refs under ARC).
  PHASEEngine* engine = nil;
  PHASESoundEvent* sound_event = nil;
  PHASEPushStreamNode* stream_node = nil;
  PHASEListener* listener = nil;
  AVAudioFormat* format = nil;

  void ReleaseSemaphore() {
    if (shared->semaphore) {
      shared->semaphore->Release(1, nullptr);
    }
  }
};

PHASEAudioDriver::PHASEAudioDriver(xe::threading::Semaphore* semaphore,
                                   uint32_t frequency, uint32_t channels)
    : impl_(std::make_unique<Impl>()) {
  impl_->shared->semaphore = semaphore;
  impl_->frequency = frequency;
  impl_->channels = channels;
}

PHASEAudioDriver::~PHASEAudioDriver() = default;

bool PHASEAudioDriver::Initialize() {
  @autoreleasepool {
   @try {
    Impl* d = impl_.get();
    NSError* error = nil;

    XELOGI("PHASEAudioDriver::Initialize begin (freq={}, channels={}, samples={})",
           d->frequency, d->channels, d->channel_samples);

    d->engine = [[PHASEEngine alloc] initWithUpdateMode:PHASEUpdateModeAutomatic];
    if (!d->engine) {
      XELOGE("PHASEAudioDriver: PHASEEngine creation returned nil");
      return false;
    }
    // Binaural over headphones, channel-based panning on speakers.
    d->engine.outputSpatializationMode = PHASESpatializationModeAutomatic;
    XELOGI("PHASEAudioDriver: engine created, spatialization=automatic");

    // 5.1 source layout (L R C LFE Ls Rs) matching the guest channel order.
    AVAudioChannelLayout* layout = [[AVAudioChannelLayout alloc]
        initWithLayoutTag:kAudioChannelLayoutTag_MPEG_5_1_A];
    if (!layout) {
      XELOGE("PHASEAudioDriver: failed to build 5.1 channel layout");
      return false;
    }

    // Deinterleaved float32 stream format; same planar shape the guest delivers.
    d->format = [[AVAudioFormat alloc]
        initWithCommonFormat:AVAudioPCMFormatFloat32
                  sampleRate:static_cast<double>(d->frequency)
                 interleaved:NO
               channelLayout:layout];
    if (!d->format) {
      XELOGE("PHASEAudioDriver: failed to build stream AVAudioFormat");
      return false;
    }
    XELOGI("PHASEAudioDriver: stream format ch={}, rate={}, interleaved=no",
           static_cast<uint32_t>(d->format.channelCount), d->format.sampleRate);

    // Identity orientation: speakers in their canonical directions.
    simd_quatf identity = simd_quaternion(0.0f, 0.0f, 0.0f, 1.0f);
    PHASEAmbientMixerDefinition* mixer = [[PHASEAmbientMixerDefinition alloc]
        initWithChannelLayout:layout
                  orientation:identity];
    if (!mixer) {
      XELOGE("PHASEAudioDriver: ambient mixer creation returned nil");
      return false;
    }

    PHASEPushStreamNodeDefinition* stream_def =
        [[PHASEPushStreamNodeDefinition alloc] initWithMixerDefinition:mixer
                                                                format:d->format
                                                            identifier:@"xe"];

    PHASESoundEventNodeAsset* asset = [d->engine.assetRegistry
        registerSoundEventAssetWithRootNode:stream_def
                                 identifier:@"xeAsset"
                                      error:&error];
    if (!asset) {
      XELOGE("PHASEAudioDriver: registerSoundEventAsset failed: {}",
             NSErrorCStr(error));
      return false;
    }
    XELOGI("PHASEAudioDriver: sound event asset registered");

    d->listener = [[PHASEListener alloc] initWithEngine:d->engine];
    d->listener.transform = matrix_identity_float4x4;
    // Apple-recommended head tracking updates the listener orientation from
    // compatible AirPods, but it requires the restricted
    // com.apple.developer.coremotion.head-pose entitlement. Without a
    // provisioning profile that grants it, enabling this makes engine start
    // raise an exception. Off by default; opt in via apu_phase_head_tracking
    // only once the entitlement is provisioned.
    if (cvars::apu_phase_head_tracking) {
      d->listener.automaticHeadTrackingFlags =
          PHASEAutomaticHeadTrackingFlagOrientation;
      XELOGI("PHASEAudioDriver: automatic head tracking ENABLED");
    } else {
      XELOGI("PHASEAudioDriver: head tracking disabled (fixed listener)");
    }
    if (![d->engine.rootObject addChild:d->listener error:&error]) {
      XELOGE("PHASEAudioDriver: addChild(listener) failed: {}",
             NSErrorCStr(error));
      return false;
    }
    XELOGI("PHASEAudioDriver: listener added");

    PHASEMixerParameters* mixer_params = [[PHASEMixerParameters alloc] init];
    [mixer_params addAmbientMixerParametersWithIdentifier:mixer.identifier
                                                 listener:d->listener];

    d->sound_event = [[PHASESoundEvent alloc] initWithEngine:d->engine
                                             assetIdentifier:@"xeAsset"
                                             mixerParameters:mixer_params
                                                       error:&error];
    if (!d->sound_event) {
      XELOGE("PHASEAudioDriver: PHASESoundEvent init failed: {}",
             NSErrorCStr(error));
      return false;
    }

    // Pre-allocate the recycled buffer pool (no allocation on the guest thread
    // in steady state).
    {
      std::lock_guard<std::mutex> lock(d->shared->mutex);
      d->shared->free_pool.reserve(kBufferPoolSize);
      for (size_t i = 0; i < kBufferPoolSize; ++i) {
        AVAudioPCMBuffer* buf =
            [[AVAudioPCMBuffer alloc] initWithPCMFormat:d->format
                                          frameCapacity:d->channel_samples];
        if (!buf) {
          XELOGE("PHASEAudioDriver: failed to allocate PCM buffer {} of {}", i,
                 kBufferPoolSize);
          return false;
        }
        d->shared->free_pool.push_back(buf);
      }
    }
    XELOGI("PHASEAudioDriver: allocated {} pooled PCM buffers", kBufferPoolSize);

    if (![d->engine startAndReturnError:&error]) {
      XELOGE("PHASEAudioDriver: engine start failed: {}", NSErrorCStr(error));
      return false;
    }
    XELOGI("PHASEAudioDriver: engine started");

    // PHASESoundEvent uses start(completion:) — NOT startAndReturnError: (that
    // selector only exists on PHASEEngine). The completion block fires when the
    // event later stops or terminates.
    [d->sound_event startWithCompletion:nil];
    XELOGI("PHASEAudioDriver: sound event started");

    d->stream_node = d->sound_event.pushStreamNodes[@"xe"];
    if (!d->stream_node) {
      XELOGE("PHASEAudioDriver: push stream node missing after start "
             "(pushStreamNodes count={})",
             static_cast<uint32_t>(d->sound_event.pushStreamNodes.count));
      return false;
    }

    XELOGI("PHASEAudioDriver: INITIALIZED OK — spatial 5.1 over PHASE, "
           "waiting for frames");
    return true;
   } @catch (NSException* ex) {
     // Never let a PHASE/ObjC exception crash the title — fail the driver so
     // the audio system substitutes a silent fallback, and log the cause.
     XELOGE("PHASEAudioDriver: ObjC exception during init: {} — {}",
            ex.name ? [ex.name UTF8String] : "(no name)",
            ex.reason ? [ex.reason UTF8String] : "(no reason)");
     return false;
   }
  }
}

void PHASEAudioDriver::Shutdown() {
  @autoreleasepool {
    Impl* d = impl_.get();
    auto s = d->shared;
    XELOGI("PHASEAudioDriver::Shutdown begin (submitted={}, completed={}, "
           "underruns={}, dropped_inactive={}, outstanding={})",
           s->frames_submitted.load(), s->frames_completed.load(),
           s->underruns.load(), s->dropped_inactive.load(),
           s->outstanding.load());

    d->shutting_down.store(true, std::memory_order_release);

    // PHASESoundEvent stops via stopAndInvalidate(); the engine via stop().
    // Guard against any selector/state surprises so teardown can't crash.
    @try {
      if (d->sound_event) {
        [d->sound_event stopAndInvalidate];
      }
      if (d->engine) {
        [d->engine stop];
      }
    } @catch (NSException* ex) {
      XELOGE("PHASEAudioDriver: exception during PHASE teardown: {} — {}",
             ex.name ? [ex.name UTF8String] : "(no name)",
             ex.reason ? [ex.reason UTF8String] : "(no reason)");
    }

    // Wait (bounded) for outstanding completion callbacks so no late block runs
    // after teardown; then flush any permits they would have released so the
    // audio worker never blocks waiting on a frame that will never complete.
    int polls = 0;
    while (s->outstanding.load(std::memory_order_acquire) > 0 &&
           polls++ < kShutdownDrainMaxPolls) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kShutdownDrainPollMs));
    }
    int leaked = s->outstanding.exchange(0, std::memory_order_acq_rel);
    if (leaked > 0) {
      XELOGW("PHASEAudioDriver: {} buffer(s) never completed; flushing permits "
             "after {}ms wait",
             leaked, polls * kShutdownDrainPollMs);
    }
    for (int i = 0; i < leaked; ++i) {
      d->ReleaseSemaphore();
    }

    {
      std::lock_guard<std::mutex> lock(s->mutex);
      s->free_pool.clear();
    }

    d->stream_node = nil;
    d->sound_event = nil;
    d->listener = nil;
    d->format = nil;
    d->engine = nil;
    XELOGI("PHASEAudioDriver::Shutdown complete");
  }
}

void PHASEAudioDriver::SubmitFrame(float* samples) {
  Impl* d = impl_.get();
  auto& s = *d->shared;

  // Backpressure invariant: exactly one semaphore release per SubmitFrame.
  if (!d->stream_node || d->shutting_down.load(std::memory_order_acquire) ||
      d->paused.load(std::memory_order_acquire)) {
    s.dropped_inactive.fetch_add(1, std::memory_order_relaxed);
    d->ReleaseSemaphore();
    return;
  }

  AVAudioPCMBuffer* buf = nil;
  size_t pool_free = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.free_pool.empty()) {
      buf = s.free_pool.back();
      s.free_pool.pop_back();
    }
    pool_free = s.free_pool.size();
  }
  if (!buf) {
    // Pool exhausted (underrun): drop this frame but keep the worker advancing.
    uint64_t n = s.underruns.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n % 100) == 0) {
      XELOGW("PHASEAudioDriver: buffer pool underrun (count={}, outstanding={})",
             n, s.outstanding.load());
    }
    d->ReleaseSemaphore();
    return;
  }

  const uint32_t ch_count = d->channels;
  const uint32_t n = d->channel_samples;
  const float vol = d->volume.load(std::memory_order_relaxed);
  buf.frameLength = n;
  float* const* dst = buf.floatChannelData;
  for (uint32_t ch = 0; ch < ch_count; ++ch) {
    const float* src = samples + ch * n;
    float* out = dst[ch];
    for (uint32_t s_i = 0; s_i < n; ++s_i) {
      out[s_i] = xe::byte_swap(src[s_i]) * vol;
    }
  }

  uint64_t submitted = s.frames_submitted.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!s.logged_first_submit.exchange(true, std::memory_order_relaxed)) {
    XELOGI("PHASEAudioDriver: first frame submitted to PHASE (vol={})", vol);
  }
  if ((submitted % kHeartbeatFrames) == 0) {
    XELOGI("PHASEAudioDriver: heartbeat submitted={}, completed={}, "
           "underruns={}, pool_free={}, outstanding={}",
           submitted, s.frames_completed.load(), s.underruns.load(), pool_free,
           s.outstanding.load());
  }

  s.outstanding.fetch_add(1, std::memory_order_relaxed);
  std::shared_ptr<PHASEAudioDriverShared> shared = d->shared;
  [d->stream_node
        scheduleBuffer:buf
        completionCallbackType:PHASEPushStreamCompletionDataRendered
             completionHandler:^(PHASEPushStreamCompletionCallbackCondition condition) {
               (void)condition;
               {
                 std::lock_guard<std::mutex> lock(shared->mutex);
                 shared->free_pool.push_back(buf);
               }
               shared->outstanding.fetch_sub(1, std::memory_order_relaxed);
               shared->frames_completed.fetch_add(1, std::memory_order_relaxed);
               if (!shared->logged_first_complete.exchange(
                       true, std::memory_order_relaxed)) {
                 XELOGI("PHASEAudioDriver: first buffer rendered by PHASE "
                        "(audio path confirmed live)");
               }
               if (shared->semaphore) {
                 shared->semaphore->Release(1, nullptr);
               }
             }];
}

void PHASEAudioDriver::Pause() {
  XELOGI("PHASEAudioDriver::Pause");
  impl_->paused.store(true, std::memory_order_release);
}

void PHASEAudioDriver::Resume() {
  XELOGI("PHASEAudioDriver::Resume");
  impl_->paused.store(false, std::memory_order_release);
}

void PHASEAudioDriver::SetVolume(float volume) {
  XELOGI("PHASEAudioDriver::SetVolume {}", volume);
  impl_->volume.store(volume, std::memory_order_relaxed);
}

}  // namespace phase
}  // namespace apu
}  // namespace xe
