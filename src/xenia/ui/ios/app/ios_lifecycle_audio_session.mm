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

// The one place the category is chosen. Playback ignores the silent switch, as
// decided for XeniOS, and stops other apps' audio; AVAudioSessionCategorySoloAmbient
// would follow the silent switch.
NSString* AudioSessionCategory() { return AVAudioSessionCategoryPlayback; }

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
                      options:0
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

void IOSAudioSessionNoteBackgrounded() {
  g_needs_restore.store(true, std::memory_order_release);
}

}  // namespace ui
}  // namespace xe
