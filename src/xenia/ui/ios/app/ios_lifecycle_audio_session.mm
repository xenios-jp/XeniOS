/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_lifecycle_audio_session.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

#include "xenia/base/logging.h"

namespace xe {
namespace ui {

namespace {

// Posted once the session is active again. The PHASE driver
// (xenia/apu/phase/phase_audio_driver.mm) restarts its engine on it.
NSString* const kAudioSessionRestoredNotification = @"XeniaAudioSessionRestored";

// Posted by the headset (xenia/apu/voice/apple/apple_voice_device.mm) when a
// title opens or closes it, with userInfo @{@"active": @(BOOL)}.
NSString* const kVoiceChatChangedNotification = @"XeniaVoiceChatChanged";

// Posted on the main queue whenever what the Microphone control shows may have
// changed: voice started or stopped, the route changed, the session came back.
NSString* const kMicrophoneStatusChangedNotification = @"XeniaMicrophoneStatusChanged";

// True while a headset needs the microphone.
std::atomic<bool> g_voice_chat{false};
// The Microphone choice is Headset: a connected headset's own microphone.
std::atomic<bool> g_headset_microphone{false};

void PostMicrophoneStatusChanged() {
  dispatch_async(dispatch_get_main_queue(), ^{
    [[NSNotificationCenter defaultCenter] postNotificationName:kMicrophoneStatusChangedNotification
                                                        object:nil];
  });
}

bool IsHeadsetMicrophonePort(NSString* port_type) {
  return [port_type isEqualToString:AVAudioSessionPortBluetoothHFP] ||
         [port_type isEqualToString:AVAudioSessionPortHeadsetMic] ||
         [port_type isEqualToString:AVAudioSessionPortUSBAudio] ||
         [port_type isEqualToString:AVAudioSessionPortCarAudio];
}

// Runs on SessionQueue while voice is on. This iPhone/iPad: the built-in
// microphone, even with wired headphones that have one. Headset: the
// headset's microphone when one is connected, else the built-in one.
void ApplyPreferredInput(AVAudioSession* session) {
  const bool headset = g_headset_microphone.load(std::memory_order_acquire);
  AVAudioSessionPortDescription* preferred = nil;
  for (AVAudioSessionPortDescription* port in session.availableInputs) {
    const bool wanted = headset ? IsHeadsetMicrophonePort(port.portType)
                                : [port.portType isEqualToString:AVAudioSessionPortBuiltInMic];
    if (wanted) {
      preferred = port;
      break;
    }
  }
  AVAudioSessionPortDescription* current = session.preferredInput;
  if (preferred == current || [preferred.UID isEqualToString:current.UID]) {
    return;
  }
  NSError* error = nil;
  if (![session setPreferredInput:preferred error:&error]) {
    XELOGW("iOS audio session: setPreferredInput failed: {}",
           error.localizedDescription ? std::string([error.localizedDescription UTF8String])
                                      : std::string("unknown error"));
    return;
  }
  XELOGI("iOS audio session: microphone {}",
         preferred ? std::string([preferred.portName UTF8String]) : std::string("default"));
}

// The one place the category is chosen. Playback ignores the silent switch, as
// decided for XeniOS, and stops other apps' audio; AVAudioSessionCategorySoloAmbient
// would follow the silent switch. Voice chat needs the microphone, so it plays and
// records, which also ignores the silent switch.
NSString* AudioSessionCategory() {
  return g_voice_chat.load(std::memory_order_acquire) ? AVAudioSessionCategoryPlayAndRecord
                                                      : AVAudioSessionCategoryPlayback;
}

// Voice chat: the loudspeaker rather than the earpiece when nothing is connected,
// and game audio to connected AirPods or headphones. Bluetooth stays A2DP (full
// quality, the device's own microphone) unless the Microphone choice is Headset,
// which takes the hands-free profile and so the headset's microphone, at call quality.
AVAudioSessionCategoryOptions AudioSessionOptions() {
  if (!g_voice_chat.load(std::memory_order_acquire)) {
    return 0;
  }
  AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionDefaultToSpeaker |
                                          AVAudioSessionCategoryOptionAllowBluetoothA2DP;
  if (g_headset_microphone.load(std::memory_order_acquire)) {
    options |= AVAudioSessionCategoryOptionAllowBluetoothHFP;
  }
  return options;
}

constexpr int kRestoreMaxAttempts = 10;
constexpr int64_t kRestoreRetryDelayNs = 1 * NSEC_PER_SEC;

std::atomic<bool> g_needs_restore{false};
// Bumped by every restore request, so a newer one supersedes pending retries.
std::atomic<uint64_t> g_restore_generation{0};

dispatch_queue_t SessionQueue() {
  static dispatch_queue_t queue =
      dispatch_queue_create("xenia.audio-session", DISPATCH_QUEUE_SERIAL);
  return queue;
}

std::string ErrorText(NSError* error) {
  NSString* description = [error localizedDescription];
  return description ? std::string([description UTF8String]) : "unknown error";
}

// Runs on SessionQueue. Returns whether the session ended up active.
bool ConfigureAndActivate(bool log_state) {
  @autoreleasepool {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;

    if (![session setCategory:AudioSessionCategory()
                         mode:AVAudioSessionModeDefault
                      options:AudioSessionOptions()
                        error:&error]) {
      XELOGW("iOS audio session: setCategory failed: {}", ErrorText(error));
      error = nil;
    }
    if (![session setPreferredSampleRate:48000.0 error:&error]) {
      XELOGW("iOS audio session: setPreferredSampleRate failed: {}", ErrorText(error));
      error = nil;
    }
    constexpr NSTimeInterval kPreferredIOBufferDuration = 0.02;
    if (![session setPreferredIOBufferDuration:kPreferredIOBufferDuration error:&error]) {
      XELOGW("iOS audio session: setPreferredIOBufferDuration failed: {}", ErrorText(error));
      error = nil;
    }
    const bool active = [session setActive:YES error:&error];
    if (!active) {
      XELOGW("iOS audio session: activation failed: {}", ErrorText(error));
    } else if (g_voice_chat.load(std::memory_order_acquire)) {
      ApplyPreferredInput(session);
    }
    if (log_state) {
      XELOGI(
          "iOS audio session: category={}, sample_rate={}, "
          "preferred_sample_rate={}, io_buffer={}, preferred_io_buffer={}",
          [[session category] UTF8String], [session sampleRate], [session preferredSampleRate],
          [session IOBufferDuration], [session preferredIOBufferDuration]);
    }
    return active;
  }
}

void RestoreAttempt(std::string reason, int attempt, uint64_t generation) {
  if (generation != g_restore_generation.load(std::memory_order_acquire)) {
    return;
  }
  if (ConfigureAndActivate(false)) {
    g_needs_restore.store(false, std::memory_order_release);
    XELOGI("iOS audio session: restored ({}, attempt {})", reason, attempt);
    [[NSNotificationCenter defaultCenter] postNotificationName:kAudioSessionRestoredNotification
                                                        object:nil];
    PostMicrophoneStatusChanged();
    return;
  }
  if (attempt >= kRestoreMaxAttempts) {
    // Left flagged: the next return to the foreground tries again.
    XELOGW("iOS audio session: giving up restoring after {} attempts ({})", attempt, reason);
    return;
  }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRestoreRetryDelayNs), SessionQueue(), ^{
    RestoreAttempt(reason, attempt + 1, generation);
  });
}

void HandleInterruption(NSNotification* notification) {
  NSDictionary* info = notification.userInfo;
  const AVAudioSessionInterruptionType type = (AVAudioSessionInterruptionType)
      [info[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
  if (type == AVAudioSessionInterruptionTypeBegan) {
    XELOGI("iOS audio session: interruption began");
    g_needs_restore.store(true, std::memory_order_release);
    return;
  }
  const AVAudioSessionInterruptionOptions options =
      (AVAudioSessionInterruptionOptions)[info[AVAudioSessionInterruptionOptionKey]
          unsignedIntegerValue];
  const bool should_resume = (options & AVAudioSessionInterruptionOptionShouldResume) != 0;
  XELOGI("iOS audio session: interruption ended (should_resume={})", should_resume);
  if (should_resume) {
    IOSAudioSessionRestore("interruption ended");
    return;
  }
  // Without shouldResume the user went elsewhere, but an alarm or another app's audio can end
  // while this app stays active and no scene change follows. Restore then; otherwise it is
  // restored when the app next becomes active.
  dispatch_async(dispatch_get_main_queue(), ^{
    if ([UIApplication sharedApplication].applicationState == UIApplicationStateActive) {
      IOSAudioSessionRestore("interruption ended while active");
    }
  });
}

void HandleRouteChange(NSNotification* notification) {
  const AVAudioSessionRouteChangeReason reason =
      (AVAudioSessionRouteChangeReason)[notification.userInfo[AVAudioSessionRouteChangeReasonKey]
          unsignedIntegerValue];
  XELOGI("iOS audio session: route changed (reason={})", static_cast<unsigned long>(reason));
  // PHASE follows route changes itself. If another app switched our category,
  // put it back.
  if (reason == AVAudioSessionRouteChangeReasonCategoryChange) {
    NSString* category = [[AVAudioSession sharedInstance] category];
    if (![category isEqualToString:AudioSessionCategory()]) {
      IOSAudioSessionRestore("category changed");
    }
  }
  // A headset came or went: the Microphone choice picks the input again.
  if ((reason == AVAudioSessionRouteChangeReasonNewDeviceAvailable ||
       reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable) &&
      g_voice_chat.load(std::memory_order_acquire)) {
    dispatch_async(SessionQueue(), ^{
      @autoreleasepool {
        ApplyPreferredInput([AVAudioSession sharedInstance]);
      }
    });
  }
  PostMicrophoneStatusChanged();
}

void HandleMediaServicesReset(NSNotification*) {
  XELOGW("iOS audio session: media services were reset");
  IOSAudioSessionRestore("media services reset");
}

}  // namespace

void IOSAudioSessionStart() {
  static std::once_flag once;
  std::call_once(once, []() {
    // AVAudioSession's setters, especially setActive:, are synchronous and can
    // block for a noticeable time, so configure on a background queue. Audio
    // playback only begins once a title is running.
    dispatch_async(SessionQueue(), ^{
      ConfigureAndActivate(true);
    });
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    AVAudioSession* session = [AVAudioSession sharedInstance];
    // A dedicated serial queue keeps the handlers off the main thread. It lives
    // as long as the observers, which is the process.
    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1;
    queue.name = @"xenia.audio-session.notifications";
    [center addObserverForName:AVAudioSessionInterruptionNotification
                        object:session
                         queue:queue
                    usingBlock:^(NSNotification* n) {
                      HandleInterruption(n);
                    }];
    [center addObserverForName:AVAudioSessionRouteChangeNotification
                        object:session
                         queue:queue
                    usingBlock:^(NSNotification* n) {
                      HandleRouteChange(n);
                    }];
    [center addObserverForName:kVoiceChatChangedNotification
                        object:nil
                         queue:queue
                    usingBlock:^(NSNotification* n) {
                      const bool active = [n.userInfo[@"active"] boolValue];
                      // Counted: party chat's device starts while the game's is
                      // still stopping, and the game's "ended" mustn't end it.
                      // Always reconfigure on a start: the device starts its engine
                      // once the session is restored. A device without microphone
                      // access posts false while none started; nothing to change.
                      static int voice_devices = 0;
                      const int was = voice_devices;
                      voice_devices = std::max(0, voice_devices + (active ? 1 : -1));
                      g_voice_chat.store(voice_devices > 0, std::memory_order_release);
                      if (active || (was > 0 && voice_devices == 0)) {
                        IOSAudioSessionRestore(active ? "voice chat started"
                                                      : "voice chat ended");
                      }
                      PostMicrophoneStatusChanged();
                    }];
    [center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                        object:session
                         queue:queue
                    usingBlock:^(NSNotification* n) {
                      HandleMediaServicesReset(n);
                    }];
  });
}

void IOSAudioSessionRestore(const char* reason) {
  std::string reason_copy = reason ? reason : "";
  const uint64_t generation = g_restore_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  g_needs_restore.store(true, std::memory_order_release);
  dispatch_async(SessionQueue(), ^{
    RestoreAttempt(reason_copy, 1, generation);
  });
}

void IOSAudioSessionRestoreIfNeeded(const char* reason) {
  if (g_needs_restore.load(std::memory_order_acquire)) {
    IOSAudioSessionRestore(reason);
  }
}

void IOSAudioSessionSetHeadsetMicrophone(bool headset) {
  const bool was_headset = g_headset_microphone.exchange(headset, std::memory_order_acq_rel);
  if (was_headset != headset && g_voice_chat.load(std::memory_order_acquire)) {
    // The options and input change now; the headset rebuilds its unit for them.
    IOSAudioSessionRestore("microphone changed");
  }
  PostMicrophoneStatusChanged();
}

bool IOSAudioSessionVoiceActive() { return g_voice_chat.load(std::memory_order_acquire); }

void IOSAudioSessionNoteBackgrounded() {
  g_needs_restore.store(true, std::memory_order_release);
}

}  // namespace ui
}  // namespace xe
