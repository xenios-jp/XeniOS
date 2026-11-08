/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/phase/phase_audio_driver.h"

#if !__has_feature(objc_arc)
#error "PHASE audio driver requires Objective-C ARC."
#endif

#import <AVFoundation/AVFoundation.h>
#import <PHASE/PHASE.h>
#import <simd/simd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
// Pool is sized at/above the audio system's max queued frames plus the
// permitless buffers below, so the guest (throttled by the pre-filled
// semaphore) never starves the free list.
constexpr size_t kBufferPoolSize = 96;
constexpr int kMaxPermitlessBuffers = 32;
// Bounded wait so Shutdown never blocks forever on a wedged render thread.
constexpr int kShutdownDrainPollMs = 2;
constexpr int kShutdownDrainMaxPolls = 250;  // ~0.5s
// Heartbeat cadence for the steady-state stats log (~5s at 48kHz/256).
constexpr uint64_t kHeartbeatFrames = 1000;

// PHASE reports a buffer rendered only after the output I/O cycle that played
// it, so the buffers of the cycle in progress still hold the audio worker's
// permits. With iOS's 20 ms cycle that is 4 or 5 of the worker's 8, and on its
// 5.33 ms deadlines it then finds none free and skips the frame: 2 to 5% of
// the audio went missing as gaps between buffers, heard as crackle. As many
// buffers as one cycle can touch give their permit back when scheduled.
int OutputCycleBuffers(double sample_rate, uint32_t channel_samples) {
#if TARGET_OS_IOS
  const double cycle_frames = [AVAudioSession sharedInstance].IOBufferDuration * sample_rate;
#else
  const double cycle_frames = 512.0;
#endif
  // A cycle can straddle one buffer more than it covers.
  const int buffers = static_cast<int>(std::ceil(cycle_frames / channel_samples)) + 1;
  return std::clamp(buffers, 1, kMaxPermitlessBuffers);
}

// Posted by the app (xenia_main_ios) once the audio session is active again
// after an interruption, a media services reset or a return from the
// background. The engine does not recover from those on its own.
constexpr const char* kAudioSessionRestoredNotification = "XeniaAudioSessionRestored";

const char* NSErrorCStr(NSError* error) {
  return error ? [[error localizedDescription] UTF8String] : "unknown error";
}

std::atomic<uint32_t> next_driver_id{0};
}  // namespace

// State shared with the scheduleBuffer completion blocks. Held by a shared_ptr
// so a late callback is still safe if the driver is torn down first.
struct PHASEAudioDriverShared {
  std::mutex mutex;
  std::vector<AVAudioPCMBuffer*> free_pool;
  std::atomic<int> outstanding{0};
  // Scheduled buffers that gave their permit back early, and how many may
  // (OutputCycleBuffers). Under |mutex|.
  int permitless = 0;
  int permitless_max = 0;
  xe::threading::Semaphore* semaphore = nullptr;
  // Bumped (under |mutex|) when buffers scheduled on a stopped graph are given
  // up on, so their completion blocks, if they ever run, only recycle the
  // buffer.
  uint64_t epoch = 0;
  // steady_clock ns of the last buffer PHASE finished rendering; 0 if none yet. A recent value
  // means the engine is still consuming audio.
  std::atomic<int64_t> last_completion_ns{0};

  // Diagnostics.
  std::atomic<uint64_t> frames_submitted{0};
  std::atomic<uint64_t> frames_completed{0};
  std::atomic<uint64_t> underruns{0};
  // Frames scheduled when PHASE had nothing left to play: a gap before each.
  // A lower bound, as a cycle that runs short with buffers still queued is
  // not seen.
  std::atomic<uint64_t> stream_dry{0};
  std::atomic<uint64_t> dropped_inactive{0};
  std::atomic<bool> logged_first_submit{false};
  std::atomic<bool> logged_first_complete{false};
};

// A driver's sound event on the shared engine.
class PHASEStreamClient {
 public:
  virtual ~PHASEStreamClient() = default;
  // Stops the sound event and gives up on its scheduled buffers.
  virtual void DetachEvent() = 0;
  // Starts a sound event, with its own push stream, on a running engine.
  virtual bool AttachEvent(PHASEEngine* engine, PHASEListener* listener, NSString* mixer_identifier,
                           AVAudioFormat* format) = 0;
  virtual int64_t LastCompletionNs() const = 0;
};

// One PHASE engine for every guest audio client. Titles such as Left 4 Dead 2
// register more than one XAudio render client and play through any of them,
// so each client gets its own sound event from one registered asset, each with
// its own push stream node and buffer completions, and the engine mixes them
// (PHASEMixerDefinition). Apple: "You can create multiple engine instances,
// but normally, apps create only one" (PHASEEngine).
class PHASESharedEngine {
 public:
  // The running engine, built on first use. Null if it can't be built or runs
  // at another rate.
  static std::shared_ptr<PHASESharedEngine> Acquire(uint32_t frequency);

  explicit PHASESharedEngine(uint32_t frequency) : frequency_(frequency) {}
  ~PHASESharedEngine();

  // Attaches the client's sound event, rebuilding the engine if a restart
  // failed. False leaves the client detached.
  bool AddClient(PHASEStreamClient* client);
  // No restart touches the client once this returns.
  void RemoveClient(PHASEStreamClient* client);

 private:
  bool Build();
  void Teardown();
  void Restart();
  void InstallSessionObserver();
  void RemoveSessionObserver();

  static std::mutex instance_mutex_;
  static std::weak_ptr<PHASESharedEngine> instance_;

  const uint32_t frequency_;
  // Guards the graph and |clients_|. Taken before a client's graph_mutex.
  std::mutex mutex_;
  std::vector<PHASEStreamClient*> clients_;
  PHASEEngine* engine_ = nil;
  PHASEListener* listener_ = nil;
  AVAudioFormat* format_ = nil;
  NSString* mixer_identifier_ = nil;
  // Audio session recovery (see xenia/ui/ios/app/ios_lifecycle_audio_session).
  id session_observer_ = nil;
  NSOperationQueue* session_queue_ = nil;
};

std::mutex PHASESharedEngine::instance_mutex_;
std::weak_ptr<PHASESharedEngine> PHASESharedEngine::instance_;

std::shared_ptr<PHASESharedEngine> PHASESharedEngine::Acquire(uint32_t frequency) {
  std::lock_guard<std::mutex> lock(instance_mutex_);
  if (std::shared_ptr<PHASESharedEngine> engine = instance_.lock()) {
    if (engine->frequency_ != frequency) {
      XELOGE("PHASEAudioDriver: engine runs at {} Hz, a client asked for {} Hz", engine->frequency_,
             frequency);
      return nullptr;
    }
    return engine;
  }
  auto engine = std::make_shared<PHASESharedEngine>(frequency);
  {
    std::lock_guard<std::mutex> graph_lock(engine->mutex_);
    bool built = false;
    @try {
      built = engine->Build();
    } @catch (NSException* ex) {
      XELOGE("PHASEAudioDriver: ObjC exception building the engine: {} — {}",
             ex.name ? [ex.name UTF8String] : "(no name)",
             ex.reason ? [ex.reason UTF8String] : "(no reason)");
    }
    if (!built) {
      engine->Teardown();
      return nullptr;
    }
  }
  engine->InstallSessionObserver();
  instance_ = engine;
  return engine;
}

PHASESharedEngine::~PHASESharedEngine() {
  @autoreleasepool {
    RemoveSessionObserver();
    std::lock_guard<std::mutex> lock(mutex_);
    Teardown();
    XELOGI("PHASEAudioDriver: engine stopped");
  }
}

bool PHASESharedEngine::Build() {
  NSError* error = nil;
  XELOGI("PHASEAudioDriver: building engine (freq={}, channels={}, samples={})", frequency_,
         AudioDriver::kFrameChannelsDefault, kChannelSamples);

  engine_ = [[PHASEEngine alloc] initWithUpdateMode:PHASEUpdateModeAutomatic];
  if (!engine_) {
    XELOGE("PHASEAudioDriver: PHASEEngine creation returned nil");
    return false;
  }
  // Binaural over headphones, channel-based panning on speakers.
  engine_.outputSpatializationMode = PHASESpatializationModeAutomatic;
  XELOGI("PHASEAudioDriver: engine created, spatialization=automatic");

  // 5.1 source layout (L R C LFE Ls Rs) matching the guest channel order.
  AVAudioChannelLayout* layout =
      [[AVAudioChannelLayout alloc] initWithLayoutTag:kAudioChannelLayoutTag_MPEG_5_1_A];
  if (!layout) {
    XELOGE("PHASEAudioDriver: failed to build 5.1 channel layout");
    return false;
  }

  // Deinterleaved float32 stream format; same planar shape the guest delivers.
  format_ = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                             sampleRate:static_cast<double>(frequency_)
                                            interleaved:NO
                                          channelLayout:layout];
  if (!format_) {
    XELOGE("PHASEAudioDriver: failed to build stream AVAudioFormat");
    return false;
  }
  XELOGI("PHASEAudioDriver: stream format ch={}, rate={}, interleaved=no",
         static_cast<uint32_t>(format_.channelCount), format_.sampleRate);

  // Identity orientation: speakers in their canonical directions.
  simd_quatf identity = simd_quaternion(0.0f, 0.0f, 0.0f, 1.0f);
  PHASEAmbientMixerDefinition* mixer =
      [[PHASEAmbientMixerDefinition alloc] initWithChannelLayout:layout orientation:identity];
  if (!mixer) {
    XELOGE("PHASEAudioDriver: ambient mixer creation returned nil");
    return false;
  }
  mixer_identifier_ = mixer.identifier;

  PHASEPushStreamNodeDefinition* stream_def =
      [[PHASEPushStreamNodeDefinition alloc] initWithMixerDefinition:mixer
                                                              format:format_
                                                          identifier:@"xe"];

  // A template: every client creates its own sound event from it.
  PHASESoundEventNodeAsset* asset =
      [engine_.assetRegistry registerSoundEventAssetWithRootNode:stream_def
                                                      identifier:@"xeAsset"
                                                           error:&error];
  if (!asset) {
    XELOGE("PHASEAudioDriver: registerSoundEventAsset failed: {}", NSErrorCStr(error));
    return false;
  }
  XELOGI("PHASEAudioDriver: sound event asset registered");

  listener_ = [[PHASEListener alloc] initWithEngine:engine_];
  listener_.transform = matrix_identity_float4x4;
  // Apple-recommended head tracking updates the listener orientation from
  // compatible AirPods, but it requires the restricted
  // com.apple.developer.coremotion.head-pose entitlement. Without a
  // provisioning profile that grants it, enabling this makes engine start
  // raise an exception. Off by default; opt in via apu_phase_head_tracking
  // only once the entitlement is provisioned.
  if (cvars::apu_phase_head_tracking) {
    listener_.automaticHeadTrackingFlags = PHASEAutomaticHeadTrackingFlagOrientation;
    XELOGI("PHASEAudioDriver: automatic head tracking ENABLED");
  } else {
    XELOGI("PHASEAudioDriver: head tracking disabled (fixed listener)");
  }
  if (![engine_.rootObject addChild:listener_ error:&error]) {
    XELOGE("PHASEAudioDriver: addChild(listener) failed: {}", NSErrorCStr(error));
    return false;
  }
  XELOGI("PHASEAudioDriver: listener added");

  if (![engine_ startAndReturnError:&error]) {
    XELOGE("PHASEAudioDriver: engine start failed: {}", NSErrorCStr(error));
    return false;
  }
  XELOGI("PHASEAudioDriver: engine started");
  return true;
}

void PHASESharedEngine::Teardown() {
  @try {
    if (engine_) {
      [engine_ stop];
    }
  } @catch (NSException* ex) {
    XELOGE("PHASEAudioDriver: exception during PHASE teardown: {} — {}",
           ex.name ? [ex.name UTF8String] : "(no name)",
           ex.reason ? [ex.reason UTF8String] : "(no reason)");
  }
  mixer_identifier_ = nil;
  listener_ = nil;
  format_ = nil;
  engine_ = nil;
}

bool PHASESharedEngine::AddClient(PHASEStreamClient* client) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!engine_) {
    XELOGW("PHASEAudioDriver: engine is down; rebuilding it for a new client");
    if (!Build()) {
      Teardown();
      return false;
    }
  }
  if (!client->AttachEvent(engine_, listener_, mixer_identifier_, format_)) {
    return false;
  }
  clients_.push_back(client);
  XELOGI("PHASEAudioDriver: {} client(s) on the engine", clients_.size());
  return true;
}

void PHASESharedEngine::RemoveClient(PHASEStreamClient* client) {
  std::lock_guard<std::mutex> lock(mutex_);
  clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
}

void PHASESharedEngine::Restart() {
  @autoreleasepool {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clients_.empty()) {
      return;
    }
    // A foreground return or a category change does not always stop the engine. Rebuilding it
    // then only drops queued audio, so leave a graph alone that is still rendering.
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (engine_) {
      for (PHASEStreamClient* client : clients_) {
        const int64_t last_ns = client->LastCompletionNs();
        if (last_ns != 0 && now_ns - last_ns < 300 * 1000 * 1000) {
          XELOGI("PHASEAudioDriver: engine is still rendering; restart skipped");
          return;
        }
      }
    }
    XELOGI("PHASEAudioDriver: restarting the engine after an audio session loss "
           "({} client(s))",
           clients_.size());
    @try {
      for (PHASEStreamClient* client : clients_) {
        client->DetachEvent();
      }
      Teardown();
      if (!Build()) {
        // Frames are dropped (and their permits released) until the next restart.
        Teardown();
        XELOGE("PHASEAudioDriver: restart failed, audio stays silent");
        return;
      }
      for (PHASEStreamClient* client : clients_) {
        if (!client->AttachEvent(engine_, listener_, mixer_identifier_, format_)) {
          XELOGE("PHASEAudioDriver: a client's sound event did not restart; "
                 "it stays silent");
        }
      }
    } @catch (NSException* ex) {
      XELOGE("PHASEAudioDriver: ObjC exception during restart: {} — {}",
             ex.name ? [ex.name UTF8String] : "(no name)",
             ex.reason ? [ex.reason UTF8String] : "(no reason)");
      return;
    }
    XELOGI("PHASEAudioDriver: engine restarted");
  }
}

void PHASESharedEngine::InstallSessionObserver() {
  session_queue_ = [[NSOperationQueue alloc] init];
  session_queue_.maxConcurrentOperationCount = 1;
  session_queue_.name = @"xenia.phase.session";
  PHASESharedEngine* engine = this;
  session_observer_ = [[NSNotificationCenter defaultCenter]
      addObserverForName:[NSString stringWithUTF8String:kAudioSessionRestoredNotification]
                  object:nil
                   queue:session_queue_
              usingBlock:^(NSNotification*) {
                engine->Restart();
              }];
}

void PHASESharedEngine::RemoveSessionObserver() {
  if (session_observer_) {
    [[NSNotificationCenter defaultCenter] removeObserver:session_observer_];
    session_observer_ = nil;
  }
  if (session_queue_) {
    // A restart that is already running finishes before teardown goes on.
    [session_queue_ waitUntilAllOperationsAreFinished];
    session_queue_ = nil;
  }
}

struct PHASEAudioDriver::Impl final : public PHASEStreamClient {
  std::shared_ptr<PHASEAudioDriverShared> shared =
      std::make_shared<PHASEAudioDriverShared>();

  // Tells this driver's log lines apart from other clients'.
  const uint32_t id = next_driver_id.fetch_add(1, std::memory_order_relaxed);
  uint32_t frequency = 0;
  uint32_t channels = 0;
  uint32_t channel_samples = kChannelSamples;
  std::atomic<float> volume{1.0f};
  std::atomic<bool> shutting_down{false};
  std::atomic<bool> paused{false};

  std::shared_ptr<PHASESharedEngine> engine;

  // This client's sound event (strong refs under ARC). Under |graph_mutex|.
  PHASESoundEvent* sound_event = nil;
  PHASEPushStreamNode* stream_node = nil;
  AVAudioFormat* format = nil;
  // Buffers to add to the pool at the next attach: all of it at first, then
  // those given up on at a restart.
  size_t pool_refill = kBufferPoolSize;

  // Serializes SubmitFrame against attaching and detaching the sound event.
  std::mutex graph_mutex;

  void DetachEvent() override;
  bool AttachEvent(PHASEEngine* engine, PHASEListener* listener, NSString* mixer_identifier,
                   AVAudioFormat* format) override;
  int64_t LastCompletionNs() const override {
    return shared->last_completion_ns.load(std::memory_order_acquire);
  }

  void StopEvent();
  // Gives up on buffers that may never report back so the audio worker's
  // permits aren't lost. Returns how many there were.
  int GiveUpOutstanding();

  void ReleaseSemaphore() {
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (xe::threading::Semaphore* semaphore = shared->semaphore) {
      semaphore->Release(1, nullptr);
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

bool PHASEAudioDriver::Impl::AttachEvent(PHASEEngine* engine, PHASEListener* listener,
                                         NSString* mixer_identifier, AVAudioFormat* stream_format) {
  std::lock_guard<std::mutex> graph_lock(graph_mutex);
  if (shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  NSError* error = nil;
  const int permitless_max = OutputCycleBuffers(frequency, channel_samples);
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->permitless_max = permitless_max;
  }

  PHASEMixerParameters* mixer_params = [[PHASEMixerParameters alloc] init];
  [mixer_params addAmbientMixerParametersWithIdentifier:mixer_identifier listener:listener];
  PHASESoundEvent* event = [[PHASESoundEvent alloc] initWithEngine:engine
                                                   assetIdentifier:@"xeAsset"
                                                   mixerParameters:mixer_params
                                                             error:&error];
  if (!event) {
    XELOGE("PHASEAudioDriver[{}]: PHASESoundEvent init failed: {}", id, NSErrorCStr(error));
    return false;
  }
  // PHASESoundEvent uses start(completion:) — NOT startAndReturnError: (that
  // selector only exists on PHASEEngine). The completion block fires when the
  // event later stops or terminates.
  [event startWithCompletion:nil];
  // Each sound event fills its own pushStreamNodes, so every client has a
  // stream of its own.
  PHASEPushStreamNode* node = event.pushStreamNodes[@"xe"];
  if (!node) {
    XELOGE("PHASEAudioDriver[{}]: push stream node missing after start "
           "(pushStreamNodes count={})",
           id, static_cast<uint32_t>(event.pushStreamNodes.count));
    [event stopAndInvalidate];
    return false;
  }

  // Pre-allocate the recycled buffer pool (no allocation on the guest thread
  // in steady state). Late completions of buffers given up on also return to
  // the pool, so fill only up to its size.
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->free_pool.reserve(kBufferPoolSize);
    for (; pool_refill > 0 && shared->free_pool.size() < kBufferPoolSize; --pool_refill) {
      AVAudioPCMBuffer* buf = [[AVAudioPCMBuffer alloc] initWithPCMFormat:stream_format
                                                            frameCapacity:channel_samples];
      if (!buf) {
        XELOGE("PHASEAudioDriver[{}]: failed to allocate a PCM buffer ({} in "
               "the pool)",
               id, shared->free_pool.size());
        break;
      }
      shared->free_pool.push_back(buf);
    }
    pool_refill = 0;
    if (shared->free_pool.empty()) {
      [event stopAndInvalidate];
      return false;
    }
  }

  sound_event = event;
  stream_node = node;
  format = stream_format;
  XELOGI("PHASEAudioDriver[{}]: sound event started, {} buffers per output "
         "cycle schedule without a permit",
         id, permitless_max);
  return true;
}

void PHASEAudioDriver::Impl::StopEvent() {
  @try {
    if (sound_event) {
      [sound_event stopAndInvalidate];
    }
  } @catch (NSException* ex) {
    XELOGE("PHASEAudioDriver[{}]: exception stopping the sound event: {} — {}", id,
           ex.name ? [ex.name UTF8String] : "(no name)",
           ex.reason ? [ex.reason UTF8String] : "(no reason)");
  }
  stream_node = nil;
  sound_event = nil;
}

int PHASEAudioDriver::Impl::GiveUpOutstanding() {
  int leaked = 0;
  int permits = 0;
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    // A buffer that completes after this only returns to the pool.
    ++shared->epoch;
    leaked = shared->outstanding.exchange(0, std::memory_order_acq_rel);
    permits = leaked - shared->permitless;
    shared->permitless = 0;
  }
  for (int i = 0; i < permits; ++i) {
    ReleaseSemaphore();
  }
  return leaked;
}

void PHASEAudioDriver::Impl::DetachEvent() {
  std::lock_guard<std::mutex> graph_lock(graph_mutex);
  StopEvent();
  // Buffers scheduled on the old graph may never report back; replace them in
  // the pool at the next attach.
  pool_refill += GiveUpOutstanding();
}

bool PHASEAudioDriver::Initialize() {
  @autoreleasepool {
   @try {
    Impl* d = impl_.get();
    XELOGI("PHASEAudioDriver[{}]::Initialize begin (freq={}, channels={}, "
           "samples={})",
           d->id, d->frequency, d->channels, d->channel_samples);
    if (d->channels != AudioDriver::kFrameChannelsDefault) {
      XELOGE("PHASEAudioDriver[{}]: {} channels unsupported, the stream is 5.1", d->id,
             d->channels);
      return false;
    }
    d->engine = PHASESharedEngine::Acquire(d->frequency);
    if (!d->engine) {
      return false;
    }
    if (!d->engine->AddClient(d)) {
      d->engine.reset();
      return false;
    }
    XELOGI("PHASEAudioDriver[{}]: INITIALIZED OK — spatial 5.1 over PHASE, "
           "waiting for frames",
           d->id);
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
    XELOGI("PHASEAudioDriver[{}]::Shutdown begin (submitted={}, completed={}, "
           "underruns={}, stream_dry={}, dropped_inactive={}, outstanding={})",
           d->id, s->frames_submitted.load(), s->frames_completed.load(), s->underruns.load(),
           s->stream_dry.load(), s->dropped_inactive.load(), s->outstanding.load());

    d->shutting_down.store(true, std::memory_order_release);
    if (d->engine) {
      d->engine->RemoveClient(d);
    }
    {
      std::lock_guard<std::mutex> graph_lock(d->graph_mutex);
      d->StopEvent();
    }
    // The last client stops the engine.
    d->engine.reset();

    // Wait (bounded) for outstanding completion callbacks so no late block runs
    // after teardown; then flush any permits they would have released so the
    // audio worker never blocks waiting on a frame that will never complete.
    int polls = 0;
    while (s->outstanding.load(std::memory_order_acquire) > 0 &&
           polls++ < kShutdownDrainMaxPolls) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kShutdownDrainPollMs));
    }
    const int leaked = d->GiveUpOutstanding();
    if (leaked > 0) {
      XELOGW("PHASEAudioDriver[{}]: {} buffer(s) never completed; flushing "
             "permits after {}ms wait",
             d->id, leaked, polls * kShutdownDrainPollMs);
    }

    {
      std::lock_guard<std::mutex> lock(s->mutex);
      s->semaphore = nullptr;
      s->free_pool.clear();
    }

    d->format = nil;
    XELOGI("PHASEAudioDriver[{}]::Shutdown complete", d->id);
  }
}

void PHASEAudioDriver::SubmitFrame(float* samples) {
  Impl* d = impl_.get();
  auto& s = *d->shared;

  // Backpressure invariant: exactly one semaphore release per SubmitFrame.
  // A restart in progress owns the graph, so drop the frame instead of waiting.
  std::unique_lock<std::mutex> graph_lock(d->graph_mutex, std::try_to_lock);
  if (!graph_lock.owns_lock() || !d->stream_node || d->shutting_down.load(std::memory_order_acquire) ||
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
      XELOGW("PHASEAudioDriver[{}]: buffer pool underrun (count={}, outstanding={})", d->id, n,
             s.outstanding.load());
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
  if (submitted > 1 && s.outstanding.load(std::memory_order_relaxed) == 0) {
    s.stream_dry.fetch_add(1, std::memory_order_relaxed);
  }
  if (!s.logged_first_submit.exchange(true, std::memory_order_relaxed)) {
    XELOGI("PHASEAudioDriver[{}]: first frame submitted to PHASE (vol={})", d->id, vol);
  }
  if ((submitted % kHeartbeatFrames) == 0) {
    XELOGD("PHASEAudioDriver[{}]: heartbeat submitted={}, completed={}, "
           "underruns={}, stream_dry={}, pool_free={}, outstanding={}",
           d->id, submitted, s.frames_completed.load(), s.underruns.load(), s.stream_dry.load(),
           pool_free, s.outstanding.load());
  }

  s.outstanding.fetch_add(1, std::memory_order_relaxed);
  std::shared_ptr<PHASEAudioDriverShared> shared = d->shared;
  const uint32_t driver_id = d->id;
  uint64_t epoch;
  bool holds_permit = true;
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    epoch = shared->epoch;
    if (shared->permitless < shared->permitless_max) {
      ++shared->permitless;
      holds_permit = false;
      if (shared->semaphore) {
        shared->semaphore->Release(1, nullptr);
      }
    }
  }
  [d->stream_node
        scheduleBuffer:buf
        completionCallbackType:PHASEPushStreamCompletionDataRendered
             completionHandler:^(PHASEPushStreamCompletionCallbackCondition condition) {
               (void)condition;
               {
                 std::lock_guard<std::mutex> lock(shared->mutex);
                 // A buffer from before a restart was already given up on. Keep the pool at its
                 // size; the restart refilled it.
                 if (epoch != shared->epoch) {
                   if (shared->free_pool.size() < kBufferPoolSize) {
                     shared->free_pool.push_back(buf);
                   }
                   return;
                 }
                 shared->free_pool.push_back(buf);
                 // Under the lock, so Restart and Shutdown count permits from a settled state.
                 shared->outstanding.fetch_sub(1, std::memory_order_relaxed);
                 if (!holds_permit) {
                   --shared->permitless;
                 } else if (shared->semaphore) {
                   shared->semaphore->Release(1, nullptr);
                 }
                 shared->last_completion_ns.store(
                     std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count(),
                     std::memory_order_release);
               }
               shared->frames_completed.fetch_add(1, std::memory_order_relaxed);
               if (!shared->logged_first_complete.exchange(
                       true, std::memory_order_relaxed)) {
                 XELOGI("PHASEAudioDriver[{}]: first buffer rendered by PHASE "
                        "(audio path confirmed live)",
                        driver_id);
               }
             }];
}

void PHASEAudioDriver::Pause() {
  XELOGD("PHASEAudioDriver[{}]::Pause", impl_->id);
  impl_->paused.store(true, std::memory_order_release);
}

void PHASEAudioDriver::Resume() {
  XELOGD("PHASEAudioDriver[{}]::Resume", impl_->id);
  impl_->paused.store(false, std::memory_order_release);
}

void PHASEAudioDriver::SetVolume(float volume) {
  XELOGD("PHASEAudioDriver[{}]::SetVolume {}", impl_->id, volume);
  impl_->volume.store(volume, std::memory_order_relaxed);
}

}  // namespace phase
}  // namespace apu
}  // namespace xe
