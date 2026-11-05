/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_GAME_IOS_SHADER_ACTIVITY_H_
#define XENIA_UI_IOS_GAME_IOS_SHADER_ACTIVITY_H_

// What the shader UI shows, as pure functions of the Metal backend's compile
// counters, so they can be checked alone (no UIKit here).

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "xenia/gpu/metal/metal_compile_activity.h"

namespace xe {
namespace ui {
namespace ios {

enum class ShaderActivityPhase { kNone, kLoadingCache, kCompiling };

struct ShaderActivity {
  ShaderActivityPhase phase = ShaderActivityPhase::kNone;
  // While loading the cache: how much of the rebuild is done, 0 to 1, or < 0
  // while its size isn't known (the stored files are still being read).
  double progress = -1.0;
  // Requests queued to the compile threads or being compiled, the rebuild's and
  // the game's.
  uint64_t backlog = 0;
};

inline ShaderActivity DescribeShaderActivity(
    const gpu::metal::CompileActivitySample& sample) {
  ShaderActivity activity;
  activity.backlog =
      sample.queued > sample.finished ? sample.queued - sample.finished : 0;
  if (sample.storage_loading && !sample.storage_empty) {
    // The rebuild's own counts: the game's compiles go ahead of it, so the
    // global ones would run ahead and end it early.
    activity.phase = ShaderActivityPhase::kLoadingCache;
    if (sample.storage_requests) {
      activity.progress =
          std::min(1.0, double(sample.storage_requests_done) /
                            double(sample.storage_requests));
    }
  } else if (activity.backlog) {
    activity.phase = ShaderActivityPhase::kCompiling;
  }
  return activity;
}

// The shader rebuild forecast.

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
  // Stored descriptions being rebuilt (0 while unknown), and the rebuild's
  // requests, the shaders they need and the pipelines (0 while unknown).
  uint64_t stored_total = 0;
  uint64_t requests_total = 0;
  // Of those requests, the ones finished, and seconds since the load began.
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

#endif  // XENIA_UI_IOS_GAME_IOS_SHADER_ACTIVITY_H_
