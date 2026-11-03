/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_IOS_SHADER_COMPILE_INDICATOR_H_
#define XENIA_UI_IOS_GAME_IOS_SHADER_COMPILE_INDICATOR_H_

#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xe {
namespace ui {
namespace ios {

// The shader rebuild forecast, as pure functions so they can be tested alone.

// A rebuild forecast at or above this many seconds offers the Wait / Play now sheet.
constexpr double kShaderWaitOfferSeconds = 45.0;
// Below this the rebuild is not worth an estimate.
constexpr double kShaderEtaMinSeconds = 15.0;
// Above this only "over 10 minutes" is said.
constexpr double kShaderEtaMaxSeconds = 600.0;
// The live rate is trusted only after this many completions and seconds.
constexpr uint64_t kShaderLiveMinDone = 20;
constexpr double kShaderLiveMinSeconds = 3.0;

struct ShaderForecastInput {
  // Stored descriptions being rebuilt (0 while unknown), and the requests they
  // became once queued (0 until known).
  uint64_t stored_total = 0;
  uint64_t requests_total = 0;
  // Requests finished since the load began, and seconds since it began.
  uint64_t done = 0;
  double observed_seconds = 0.0;
  // Measured milliseconds per request (<= 0: not measured).
  double live_ms_per_request = 0.0;
  // The last complete rebuild's microseconds per stored description (0: none),
  // and a factor for conditions that make it slower (Low Power Mode).
  double seed_us_per_stored = 0.0;
  double seed_slowdown = 1.0;
};

// Seconds left, or < 0 when nothing can be said yet. The record from the last
// launch carries the forecast until 20 completions and 3 seconds have been
// measured, then hands over to the live rate over the next 40 completions.
inline double ForecastShaderSeconds(const ShaderForecastInput& in) {
  double fraction_done = 0.0;
  if (in.requests_total) {
    fraction_done = std::min(1.0, double(in.done) / double(in.requests_total));
  }
  double seed_seconds = -1.0;
  if (in.seed_us_per_stored > 0.0 && in.stored_total) {
    seed_seconds = in.seed_us_per_stored * double(in.stored_total) *
                   (1.0 - fraction_done) * in.seed_slowdown / 1e6;
  }
  double live_seconds = -1.0;
  if (in.requests_total && in.live_ms_per_request > 0.0 &&
      in.done >= kShaderLiveMinDone &&
      in.observed_seconds >= kShaderLiveMinSeconds) {
    double remaining = in.requests_total > in.done
                           ? double(in.requests_total - in.done)
                           : 0.0;
    live_seconds = remaining * in.live_ms_per_request / 1000.0;
  }
  if (live_seconds >= 0.0 && seed_seconds >= 0.0) {
    double weight = std::min(1.0, double(in.done) / 60.0);
    return weight * live_seconds + (1.0 - weight) * seed_seconds;
  }
  return live_seconds >= 0.0 ? live_seconds : seed_seconds;
}

struct ShaderEta {
  enum class Kind { kNone, kSeconds, kMinutes, kOver };
  Kind kind = Kind::kNone;
  // Seconds (a multiple of 5) or minutes.
  int64_t value = 0;
};

// What to say for a forecast: nothing under 15 s, seconds to the nearest 5 up to
// a minute, whole minutes to 10, then only "over 10 minutes".
inline ShaderEta ClassifyShaderEta(double seconds) {
  ShaderEta eta;
  if (!(seconds >= kShaderEtaMinSeconds)) {
    return eta;
  }
  if (seconds > kShaderEtaMaxSeconds) {
    eta.kind = ShaderEta::Kind::kOver;
    return eta;
  }
  int64_t rounded = int64_t(std::llround(seconds / 5.0)) * 5;
  if (rounded < 58) {
    eta.kind = ShaderEta::Kind::kSeconds;
    eta.value = rounded;
    return eta;
  }
  eta.kind = ShaderEta::Kind::kMinutes;
  eta.value = std::max<int64_t>(1, int64_t(std::llround(seconds / 60.0)));
  return eta;
}

}  // namespace ios
}  // namespace ui
}  // namespace xe
#endif  // __cplusplus

// A small capsule at the top of the game view while the Metal backend has a real
// shader backlog: loading the title's shader cache (with progress), or compiling
// shaders the game asked for. On a title's first run it opens once into a short
// explanation. When the stored shaders are forecast to take a long while to rebuild it also
// offers a sheet to wait for them or play now. The view covers the host's bounds (frame and autoresizing set by the
// host) and never takes touches; it places the capsule itself.
@interface XeniaShaderCompileIndicator : UIView

// Polls the compile counters only while active (a game is running). Turning it off
// hides the capsule at once.
@property(nonatomic, assign, getter=isActive) BOOL active;

@end

// The game-side hold for the Wait choice: called on the main thread with YES to hold the guest
// (and GPU and audio) while the stored shaders rebuild, NO to let it play. The host installs it
// for the app's lifetime; pass nil to remove it.
void xe_ios_set_shader_wait_handler(void (^handler)(BOOL waiting));

// Settings > Display > Shader Compile Status. On by default.
BOOL xe_ios_shader_compile_status_enabled(void);
void xe_ios_set_shader_compile_status_enabled(BOOL enabled);

#endif  // XENIA_UI_IOS_GAME_IOS_SHADER_COMPILE_INDICATOR_H_
