/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_resource_pressure.h"

#import <UIKit/UIKit.h>

#include "xenia/base/logging.h"

namespace {

BOOL (^g_is_game_running)(void) = nil;
void (^g_present_toast)(NSString*) = nil;
// True once the toast was shown for the current rise to Serious or above.
BOOL g_thermal_toast_shown = NO;

void HandleThermalStateChange() {
  NSProcessInfoThermalState state = [NSProcessInfo processInfo].thermalState;
  if (state < NSProcessInfoThermalStateSerious) {
    g_thermal_toast_shown = NO;
    return;
  }
  if (g_thermal_toast_shown || !g_is_game_running || !g_is_game_running()) {
    return;
  }
  g_thermal_toast_shown = YES;
  XELOGI("iOS: Thermal state {} while a game is running; showing a toast", (int)state);
  if (g_present_toast) {
    g_present_toast(
        NSLocalizedString(@"Your device is warm. The game may run slower until it cools down.",
                          @"Toast shown when the device is thermally throttled"));
  }
}

void HandleMemoryWarning() {
  // The launcher keeps no decoded art or parsed compatibility data in memory
  // (both are read from Caches on demand), so the only app-side cache that is
  // safe to drop is the shared HTTP response cache's memory part. Zeroing the
  // capacity evicts it; the disk part stays, so nothing is downloaded again.
  NSURLCache* url_cache = [NSURLCache sharedURLCache];
  NSUInteger url_bytes = url_cache.currentMemoryUsage;
  NSUInteger capacity = url_cache.memoryCapacity;
  url_cache.memoryCapacity = 0;
  url_cache.memoryCapacity = capacity;
  XELOGW("iOS: Memory warning; released URL response cache ({} KB in memory)",
         (unsigned long)(url_bytes / 1024));
}

}  // namespace

void xe_ios_resource_pressure_game_started(void) {
  HandleThermalStateChange();
}

void xe_ios_resource_pressure_install(BOOL (^is_game_running)(void),
                                      void (^present_toast)(NSString* message)) {
  static dispatch_once_t once_token;
  dispatch_once(&once_token, ^{
    g_is_game_running = [is_game_running copy];
    g_present_toast = [present_toast copy];
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    NSOperationQueue* main_queue = [NSOperationQueue mainQueue];
    // Delivered on the main queue; the thermal notification itself is posted
    // from an arbitrary thread.
    [center addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                        object:nil
                         queue:main_queue
                    usingBlock:^(NSNotification*) {
                      HandleThermalStateChange();
                    }];
    [center addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                        object:nil
                         queue:main_queue
                    usingBlock:^(NSNotification*) {
                      HandleMemoryWarning();
                    }];
  });
}
