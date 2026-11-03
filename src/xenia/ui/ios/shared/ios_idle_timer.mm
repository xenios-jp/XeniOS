/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_idle_timer.h"

#import <UIKit/UIKit.h>

namespace {

// Main thread only.
NSMutableSet<NSString*>* IdleTimerHolders() {
  static NSMutableSet<NSString*>* holders = [[NSMutableSet alloc] init];
  return holders;
}

void UpdateIdleTimerHolder(NSString* holder, BOOL hold) {
  if (!holder) {
    return;
  }
  NSString* holder_copy = [holder copy];
  void (^update)(void) = ^{
    NSMutableSet<NSString*>* holders = IdleTimerHolders();
    if (hold) {
      [holders addObject:holder_copy];
    } else {
      [holders removeObject:holder_copy];
    }
    [UIApplication sharedApplication].idleTimerDisabled = holders.count != 0;
    [holder_copy release];
  };
  if ([NSThread isMainThread]) {
    update();
  } else {
    dispatch_async(dispatch_get_main_queue(), update);
  }
}

}  // namespace

void xe_ios_idle_timer_hold(NSString* holder) { UpdateIdleTimerHolder(holder, YES); }

void xe_ios_idle_timer_release(NSString* holder) { UpdateIdleTimerHolder(holder, NO); }
