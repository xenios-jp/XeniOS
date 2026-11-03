/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_lifecycle_observer.h"

#import <UIKit/UIKit.h>

#include "xenia/base/logging.h"
#include "xenia/ui/ios/app/ios_lifecycle_audio_session.h"
#include "xenia/ui/ios/app/windowed_app_context_ios.h"

namespace {

using xe::ui::IOSWindowedAppContext;
using Reason = IOSWindowedAppContext::EmulationPauseReason;

// How long the background task waits for the pause to settle. iOS gives a
// background task roughly 30 s, but the GPU should drain in milliseconds; a
// wedged emulator must not keep the app alive any longer than this.
constexpr int64_t kSettleTimeoutNs = 4 * NSEC_PER_SEC;

}  // namespace

@interface XeniaLifecycleObserver : NSObject {
  IOSWindowedAppContext* _context;
  UIBackgroundTaskIdentifier _task;
}
- (instancetype)initWithContext:(IOSWindowedAppContext*)context;
- (void)stop;
@end

@implementation XeniaLifecycleObserver

- (instancetype)initWithContext:(IOSWindowedAppContext*)context {
  if ((self = [super init])) {
    _context = context;
    _task = UIBackgroundTaskInvalid;
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserver:self
               selector:@selector(sceneWillDeactivate:)
                   name:UISceneWillDeactivateNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(sceneDidEnterBackground:)
                   name:UISceneDidEnterBackgroundNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(sceneWillEnterForeground:)
                   name:UISceneWillEnterForegroundNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(sceneDidActivate:)
                   name:UISceneDidActivateNotification
                 object:nil];
  }
  return self;
}

- (void)stop {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  _context = nullptr;
  [self endTask];
}

- (void)dealloc {
  [self stop];
  [super dealloc];
}

- (void)endTask {
  if (_task != UIBackgroundTaskInvalid) {
    UIBackgroundTaskIdentifier task = _task;
    _task = UIBackgroundTaskInvalid;
    [[UIApplication sharedApplication] endBackgroundTask:task];
  }
}

// Keeps the app running until the pause requested just before this call has
// taken effect (or the timeout passes).
- (void)holdBackgroundTaskUntilSettled {
  if (!_context) {
    return;
  }
  if (_task == UIBackgroundTaskInvalid) {
    _task = [[UIApplication sharedApplication]
        beginBackgroundTaskWithName:@"xenia.pause-emulation"
                  expirationHandler:^{
                    [self endTask];
                  }];
  }
  if (_task == UIBackgroundTaskInvalid) {
    return;
  }
  const UIBackgroundTaskIdentifier task = _task;
  [self retain];
  _context->RunWhenEmulationPauseSettled([self, task]() {
    if (_task == task) {
      [self endTask];
    }
    [self release];
  });
  [self retain];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kSettleTimeoutNs), dispatch_get_main_queue(), ^{
    if (_task == task) {
      XELOGW("iOS lifecycle: emulation pause did not settle in time; ending background task");
      [self endTask];
    }
    [self release];
  });
}

- (void)sceneWillDeactivate:(NSNotification*)notification {
  if (!_context) {
    return;
  }
  XELOGI("iOS lifecycle: scene will deactivate");
  _context->SetEmulationPaused(Reason::kInactive, true);
  [self holdBackgroundTaskUntilSettled];
}

- (void)sceneDidEnterBackground:(NSNotification*)notification {
  if (!_context) {
    return;
  }
  XELOGI("iOS lifecycle: scene entered background");
  xe::ui::IOSAudioSessionNoteBackgrounded();
  _context->SetEmulationPaused(Reason::kBackground, true);
  [self holdBackgroundTaskUntilSettled];
}

- (void)sceneWillEnterForeground:(NSNotification*)notification {
  if (!_context) {
    return;
  }
  XELOGI("iOS lifecycle: scene will enter foreground");
  _context->SetEmulationPaused(Reason::kBackground, false);
}

- (void)sceneDidActivate:(NSNotification*)notification {
  if (!_context) {
    return;
  }
  XELOGI("iOS lifecycle: scene did activate");
  xe::ui::IOSAudioSessionRestoreIfNeeded("scene became active");
  _context->SetEmulationPaused(Reason::kInactive, false);
}

@end

namespace xe {
namespace ui {

namespace {
XeniaLifecycleObserver* g_observer = nil;
}  // namespace

void IOSLifecycleObserverInstall(IOSWindowedAppContext* context) {
  if (g_observer || !context) {
    return;
  }
  g_observer = [[XeniaLifecycleObserver alloc] initWithContext:context];
}

void IOSLifecycleObserverRemove() {
  if (!g_observer) {
    return;
  }
  [g_observer stop];
  [g_observer release];
  g_observer = nil;
}

}  // namespace ui
}  // namespace xe
