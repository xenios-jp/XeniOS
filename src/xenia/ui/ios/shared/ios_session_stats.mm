/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_session_stats.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/proc.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <iterator>

#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_compile_activity.h"
#include "xenia/ui/metal/metal_presenter.h"
#import "xenia/ui/ios/shared/ios_diagnostics.h"
#import "xenia/ui/ios/shared/ios_diagnostics_outbox.h"

namespace {

// Upper edges of the guest frame interval buckets, in ms; the last bucket is
// everything longer. The guest presents on its 60 Hz vertical blank, so frame
// times cluster at multiples of 16.67 ms: 15-18.33, 30-36.67, 45-55 and
// 60-73.33 are bands around 60, 30, 20 and 15 fps, wide enough for jitter, and
// the buckets between them hold the frames off that cadence.
constexpr double kIntervalEdgesMs[] = {10, 15, 18.33, 25, 30, 36.67, 45, 55, 60, 73.33, 100, 250};
constexpr size_t kBucketCount = std::size(kIntervalEdgesMs) + 1;
// Longer than this is a long frame, for the shader compile overlap count.
constexpr double kLongFrameMs = 50;
// Longer than this is a pause (menu, background, loading without swaps), not
// a frame: counted as a gap and left out of the histogram.
constexpr double kGapMs = 2000;
constexpr int kSampleSeconds = 5;
// The first checkpoint comes soon after the game starts, so a session cut
// short early still has numbers; then one a minute.
constexpr double kFirstCheckpointSeconds = 10;
constexpr double kCheckpointSeconds = 60;

// Five-second windows of play (the sampler's period), each classified once the
// session's record is built: whether its median frame time missed the game's
// own target and, if it did, what the frame waited on. The signals, as shares
// of the window's frame time (the sum of its frame intervals):
// - GPU busy: the union of the command processor's command buffer GPU
//   intervals. Time at the current clock, not work, and it includes GPU idle
//   inside a command buffer.
// - CPU: the frame time the command processor thread was not blocked on the
//   GPU (1 - its GPU waits), so time it spent encoding or waiting for the guest.
// - Drawable wait: time the painting thread was blocked in nextDrawable.
// - Shader compiles finished in the window, and whether the stored pipelines
//   were being rebuilt.
// The thresholds, from the measured cases (results/2026-11/ios/metrics/v2-audit/
// perf-evidence.md; MST unless noted, iPhone 17):
// - BF4 hot: GPU busy 76.9 of 97 ms and 81.7 of 101 ms per frame (0.79-0.81),
//   GPU-limited at the induced Minimum clock. So "near the frame time" is
//   >= 0.75, low enough to take it with margin for the union's in-buffer idle.
// - Halo 3 capped at 30: 22.5-29 ms of 33.3 (0.66-0.87) while holding its cap,
//   at an unknown clock: busy share alone can't tell a GPU limit there, which
//   is why only missed windows are classified.
// - GTA IV: 19-22 ms GPU (33 at most) at 6-12 fps (83-166 ms), 0.15-0.25,
//   CPU-limited on one guest thread. BF4 cool: ~26.8 ms at ~22 fps, ~0.59,
//   serialized. "Well under" for the CPU label is < 0.5: GTA IV sits far below,
//   and BF4 cool can't be called CPU-limited from it.
// - Serialized (both well under) is GPU and CPU each < 0.75: they take turns
//   with ~1 frame in flight (BF4 live), so neither reaches the frame time.
// - Drawable wait was never measured live (0 in replays). "Small" is < 0.25
//   of the frame time: past that the frame waits on the display path, and the
//   window is left unlabeled rather than called GPU-limited. Unvalidated.
// - Stutter: >= 20 compiles in the window, or the stored pipeline rebuild
//   running. The Mac stream replay compiled 197 pipelines in 11.5-14.4 s of
//   summed compile time (~65 ms each), so 20 is >= 1.3 s of compile work in
//   5 s; the phone compiles slower. Cold frames also inflate GPU time 2-5x
//   (54-163 vs 24-33 ms), so these windows stay out of the baseline and the
//   throttled count.
// - Throttled: GPU ms per frame >= 2x the session's cool baseline. Same BF4
//   scene: 26.8 ms cool, 77-82 ms hot (2.8-3x); pinned A/Bs of one binary swing
//   up to ~2x without heat, so 2x is the smallest rise that isn't noise.
// - Baseline: the median GPU ms per frame of the session's first 6 windows
//   (30 s) at thermal Nominal, or Fair if it never reaches Nominal, that aren't
//   stutter and keep the GPU >= 0.5 busy: with more slack the governor lowers
//   the clock on its own, so the baseline would measure slack, not the cool
//   clock. Thermal state is context only: the GPU sat at its induced Minimum
//   while iOS still said Fair.
// - Target: the guest presents on its vertical blank, so a game's own target is
//   one or two blanks (60 or 30 fps at NTSC). A session whose window medians
//   held one blank in at least a third of its windows targets that; otherwise
//   two blanks, so 30 fps games with 60 fps menus aren't read as missing 60.
//   Missed is a median over 1.1x the target (18.33 / 36.67 ms), the edges of
//   the session histogram's bands. A 60 fps game that never reaches 60 is read
//   as a 30 fps one: an undercount, seen in the histogram instead.
constexpr double kWindowNear = 0.75;
constexpr double kWindowWellUnder = 0.5;
constexpr double kWindowDrawableSmall = 0.25;
constexpr uint32_t kWindowStutterCompiles = 20;
constexpr double kThrottledRatio = 2.0;
constexpr double kBaselineMinBusy = 0.5;
constexpr size_t kBaselineWindows = 6;
constexpr double kHeldTargetShare = 1.0 / 3.0;
constexpr double kMissedTargetRatio = 1.1;
// A window counts when frames covered at least half of it: the game was
// running and in the foreground (a background app presents nothing).
constexpr double kWindowMinActiveShare = 0.5;
// Frame intervals per window in 1 ms buckets; the last one holds the rest.
constexpr size_t kWindowBucketCount = 251;
// Six hours of windows; later windows count in the total only.
constexpr size_t kMaxWindows = 6 * 3600 / kSampleSeconds;

struct WindowRecord {
  // The median frame interval's 1 ms bucket.
  uint16_t median_ms;
  // Shares of the window's frame time, in thousandths.
  uint16_t gpu_busy;
  uint16_t cpu;
  uint16_t drawable_wait;
  // GPU busy per frame, in tenths of a millisecond.
  uint16_t gpu_ms10;
  uint16_t compiles;
  uint8_t thermal;
  bool storage_loading;
};

dispatch_queue_t Queue() {
  static dispatch_queue_t queue =
      dispatch_queue_create("jp.xenios.session-stats", DISPATCH_QUEUE_SERIAL);
  return queue;
}

NSString* CheckpointPath() {
  return [NSHomeDirectory()
      stringByAppendingPathComponent:@"Library/Application Support/XeniOS/Metrics/session.json"];
}

uint64_t MsToTicks(double ms) {
  static mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info;
    mach_timebase_info(&info);
    return info;
  }();
  return static_cast<uint64_t>(ms * 1e6 * timebase.denom / timebase.numer);
}

double TicksToSeconds(uint64_t ticks) {
  return static_cast<double>(ticks) / static_cast<double>(MsToTicks(1000));
}

struct Footprint {
  uint64_t current = 0;
  // The process's peak since launch, kept by the kernel.
  uint64_t lifetime_peak = 0;
};

Footprint ReadFootprint() {
  task_vm_info_data_t info = {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS) {
    return {};
  }
  Footprint footprint;
  footprint.current = info.phys_footprint;
  if (count >= TASK_VM_INFO_REV3_COUNT && info.ledger_phys_footprint_peak > 0) {
    footprint.lifetime_peak = static_cast<uint64_t>(info.ledger_phys_footprint_peak);
  }
  return footprint;
}

// Written by the presenting thread (frames) and the stats queue (everything
// else); read by the stats queue.
std::atomic<bool> g_running{false};
std::atomic<uint32_t> g_title_id{0};
std::atomic<uint64_t> g_last_frame_ticks{0};
std::atomic<uint64_t> g_last_compiles_finished{0};
std::atomic<uint32_t> g_counts[kBucketCount];
std::atomic<uint32_t> g_frames{0};
std::atomic<uint32_t> g_gaps{0};
std::atomic<uint32_t> g_long_frames{0};
std::atomic<uint32_t> g_long_frames_compiling{0};
// Guest faults the emulator halted the thread for (a frozen game, no crash).
std::atomic<uint32_t> g_halted_faults{0};
std::atomic<uint32_t> g_backend_failures{0};
std::atomic<uint64_t> g_active_ticks{0};
// From the start of the session to the game's first frame; 0 until then.
std::atomic<uint64_t> g_first_frame_ticks{0};
uint64_t g_edge_ticks[std::size(kIntervalEdgesMs)];
uint64_t g_long_ticks = 0;
uint64_t g_gap_ticks = 0;
uint64_t g_ticks_per_ms = 1;
std::atomic<uint32_t> g_window_counts[kWindowBucketCount];

// Written by the stats queue before the session starts running; read by the
// presenting thread too.
uint64_t g_begin_ticks = 0;
// Stats queue only: when the session started, for its day.
time_t g_begin_time = 0;

// Stats queue only.
uint64_t g_last_sample_ticks = 0;
double g_next_checkpoint_seconds = 0;
uint64_t g_begin_lifetime_peak = 0;
uint64_t g_begin_compiles_finished = 0;
uint32_t g_begin_storage_generation = 0;
uint64_t g_begin_command_buffer_failures = 0;
double g_thermal_seconds[4] = {};
double g_low_power_seconds = 0;
// Time with the guest refresh capped and uncapped: uncapped play runs the
// guest as fast as it goes, so its frame pacing is read apart.
double g_capped_seconds = 0;
double g_uncapped_seconds = 0;
uint64_t g_peak_footprint = 0;
uint64_t g_min_available = UINT64_MAX;
uint32_t g_memory_warnings = 0;
dispatch_source_t g_timer = nullptr;
dispatch_source_t g_memory_source = nullptr;
// The counters at the start of the current window.
uint32_t g_window_begin_frames = 0;
uint64_t g_window_begin_active_ticks = 0;
uint64_t g_window_begin_gpu_busy_ns = 0;
uint64_t g_window_begin_gpu_wait_ticks = 0;
uint64_t g_window_begin_drawable_wait_ticks = 0;
uint64_t g_window_begin_compiles_finished = 0;
WindowRecord g_windows[kMaxWindows];
size_t g_window_count = 0;
// Windows past kMaxWindows.
uint32_t g_windows_unrecorded = 0;

uint16_t Thousandths(double share) {
  return static_cast<uint16_t>(std::clamp(share * 1000, 0.0, 65535.0));
}

// Closes the window that ends now; it's recorded if the game ran through most
// of it.
void CloseWindowLocked(uint64_t window_ticks, NSProcessInfoThermalState thermal) {
  const uint32_t frames = g_frames.load(std::memory_order_relaxed);
  const uint64_t active_ticks = g_active_ticks.load(std::memory_order_relaxed);
  const xe::gpu::metal::GpuTiming& timing = xe::gpu::metal::gpu_timing;
  const uint64_t gpu_busy_ns = timing.busy_ns.load(std::memory_order_relaxed);
  const uint64_t gpu_wait_ticks = timing.wait_ticks.load(std::memory_order_relaxed);
  const uint64_t drawable_wait_ticks =
      xe::ui::metal::drawable_wait_ticks.load(std::memory_order_relaxed);
  const xe::gpu::metal::CompileActivity& activity = xe::gpu::metal::compile_activity;
  const uint64_t compiles_finished = activity.finished.load(std::memory_order_relaxed);
  const uint32_t window_frames = frames - g_window_begin_frames;
  const uint64_t window_active = active_ticks - g_window_begin_active_ticks;
  const uint64_t window_gpu_busy_ns = gpu_busy_ns - g_window_begin_gpu_busy_ns;
  const uint64_t window_gpu_wait = gpu_wait_ticks - g_window_begin_gpu_wait_ticks;
  const uint64_t window_drawable_wait =
      drawable_wait_ticks - g_window_begin_drawable_wait_ticks;
  const uint64_t window_compiles = compiles_finished - g_window_begin_compiles_finished;
  g_window_begin_frames = frames;
  g_window_begin_active_ticks = active_ticks;
  g_window_begin_gpu_busy_ns = gpu_busy_ns;
  g_window_begin_gpu_wait_ticks = gpu_wait_ticks;
  g_window_begin_drawable_wait_ticks = drawable_wait_ticks;
  g_window_begin_compiles_finished = compiles_finished;
  uint32_t counts[kWindowBucketCount];
  uint64_t counted = 0;
  for (size_t i = 0; i < kWindowBucketCount; ++i) {
    counts[i] = g_window_counts[i].exchange(0, std::memory_order_relaxed);
    counted += counts[i];
  }
  if (!window_frames || !counted ||
      window_active < static_cast<uint64_t>(window_ticks * kWindowMinActiveShare)) {
    return;
  }
  if (g_window_count >= kMaxWindows) {
    ++g_windows_unrecorded;
    return;
  }
  size_t median = 0;
  for (uint64_t below = 0; median < kWindowBucketCount - 1; ++median) {
    below += counts[median];
    if (below * 2 >= counted) {
      break;
    }
  }
  const double active_ns = TicksToSeconds(window_active) * 1e9;
  const double gpu_busy = window_gpu_busy_ns / active_ns;
  WindowRecord& window = g_windows[g_window_count++];
  window.median_ms = static_cast<uint16_t>(median);
  window.gpu_busy = Thousandths(gpu_busy);
  window.cpu = Thousandths(1 - std::min(1.0, double(window_gpu_wait) / window_active));
  window.drawable_wait = Thousandths(double(window_drawable_wait) / window_active);
  window.gpu_ms10 = static_cast<uint16_t>(
      std::min(window_gpu_busy_ns / 1e5 / window_frames, 65535.0));
  window.compiles = static_cast<uint16_t>(std::min<uint64_t>(window_compiles, UINT16_MAX));
  window.thermal = static_cast<uint8_t>(thermal);
  window.storage_loading = activity.storage_loading.load(std::memory_order_relaxed);
}

bool IsStutter(const WindowRecord& window) {
  return window.compiles >= kWindowStutterCompiles || window.storage_loading;
}

// The cool baseline's GPU ms per frame, in tenths, or 0 without one.
uint32_t BaselineGpuMs10Locked() {
  for (NSProcessInfoThermalState thermal :
       {NSProcessInfoThermalStateNominal, NSProcessInfoThermalStateFair}) {
    uint16_t values[kBaselineWindows];
    size_t count = 0;
    for (size_t i = 0; i < g_window_count && count < kBaselineWindows; ++i) {
      const WindowRecord& window = g_windows[i];
      if (window.thermal == thermal && !IsStutter(window) && window.gpu_ms10 &&
          window.gpu_busy >= kBaselineMinBusy * 1000) {
        values[count++] = window.gpu_ms10;
      }
    }
    if (count) {
      std::sort(values, values + count);
      return count & 1 ? values[count / 2]
                       : (uint32_t(values[count / 2 - 1]) + values[count / 2] + 1) / 2;
    }
  }
  return 0;
}

void AddWindowsLocked(NSMutableDictionary* record) {
  // One or two guest vertical blanks; see the comment on the thresholds.
  const double blank_ms = 1000.0 / GetGuestVblankRateHz();
  size_t held_one_blank = 0;
  for (size_t i = 0; i < g_window_count; ++i) {
    held_one_blank += g_windows[i].median_ms < blank_ms * kMissedTargetRatio;
  }
  const double target_ms =
      g_window_count && held_one_blank >= g_window_count * kHeldTargetShare ? blank_ms
                                                                              : blank_ms * 2;
  const double missed_ms = target_ms * kMissedTargetRatio;
  const uint32_t baseline = BaselineGpuMs10Locked();
  uint32_t missed = 0, gpu = 0, cpu = 0, serialized = 0, stutter = 0, throttled = 0;
  for (size_t i = 0; i < g_window_count; ++i) {
    const WindowRecord& window = g_windows[i];
    const bool is_stutter = IsStutter(window);
    if (baseline && !is_stutter && window.gpu_ms10 >= baseline * kThrottledRatio) {
      ++throttled;
    }
    if (window.median_ms < missed_ms) {
      continue;
    }
    ++missed;
    const double gpu_busy = window.gpu_busy / 1000.0;
    const double cpu_share = window.cpu / 1000.0;
    if (is_stutter) {
      ++stutter;
    } else if (gpu_busy >= kWindowNear) {
      gpu += window.drawable_wait < kWindowDrawableSmall * 1000;
    } else if (gpu_busy < kWindowWellUnder && cpu_share >= kWindowNear) {
      ++cpu;
    } else if (cpu_share < kWindowNear) {
      ++serialized;
    }
  }
  record[@"windows"] = @{
    @"total" : @(g_window_count + g_windows_unrecorded),
    @"missed" : @(missed),
    @"gpu" : @(gpu),
    @"cpu" : @(cpu),
    @"serialized" : @(serialized),
    @"stutter" : @(stutter),
    @"throttled" : @(throttled),
  };
  if (baseline) {
    record[@"gpuBaselineMs10"] = @(baseline);
  }
}

// "2026-11-04": the UTC day of |time|.
NSString* UTCDay(time_t time) {
  struct tm utc = {};
  gmtime_r(&time, &utc);
  char day[16];
  strftime(day, sizeof(day), "%Y-%m-%d", &utc);
  return @(day);
}

NSMutableDictionary* RecordLocked(NSString* end) {
  NSMutableArray* edges = [NSMutableArray array];
  for (double edge : kIntervalEdgesMs) {
    [edges addObject:@(edge)];
  }
  NSMutableArray* counts = [NSMutableArray array];
  for (const auto& count : g_counts) {
    [counts addObject:@(count.load(std::memory_order_relaxed))];
  }
  NSMutableDictionary* record = [NSMutableDictionary dictionary];
  if (const uint32_t title_id = g_title_id.load(std::memory_order_relaxed)) {
    record[@"titleId"] = [NSString stringWithFormat:@"%08X", title_id];
  }
  record[@"durationS"] = @(lround(TicksToSeconds(mach_absolute_time() - g_begin_ticks)));
  record[@"activeS"] =
      @(lround(TicksToSeconds(g_active_ticks.load(std::memory_order_relaxed))));
  record[@"frames"] = @(g_frames.load(std::memory_order_relaxed));
  if (const uint64_t first_frame = g_first_frame_ticks.load(std::memory_order_relaxed)) {
    record[@"firstFrameMs"] = @(llround(TicksToSeconds(first_frame) * 1000));
  }
  record[@"intervalMs"] = edges;
  record[@"counts"] = counts;
  record[@"gaps"] = @(g_gaps.load(std::memory_order_relaxed));
  record[@"longFrames"] = @(g_long_frames.load(std::memory_order_relaxed));
  record[@"longFramesDuringShaderCompile"] =
      @(g_long_frames_compiling.load(std::memory_order_relaxed));
  const xe::gpu::metal::CompileActivity& activity = xe::gpu::metal::compile_activity;
  record[@"pipelinesCompiled"] =
      @(activity.finished.load(std::memory_order_relaxed) - g_begin_compiles_finished);
  // Known once the game's shader storage has started loading; 0 is a first
  // run, with every pipeline compiled anew.
  if (activity.storage_generation.load(std::memory_order_relaxed) != g_begin_storage_generation) {
    const uint64_t stored = activity.storage_total.load(std::memory_order_relaxed);
    if (activity.storage_empty.load(std::memory_order_relaxed)) {
      record[@"storedPipelines"] = @0;
    } else if (stored) {
      record[@"storedPipelines"] = @(stored);
    }
  }
  if (const uint32_t halted = g_halted_faults.load(std::memory_order_relaxed)) {
    record[@"haltedFaults"] = @(halted);
  }
  if (const uint32_t failures = g_backend_failures.load(std::memory_order_relaxed)) {
    record[@"backendFailures"] = @(failures);
  }
  record[@"gpuErrors"] =
      @(xe::gpu::metal::command_buffer_failures.load(std::memory_order_relaxed) -
        g_begin_command_buffer_failures);
  record[@"thermalS"] = @{
    @"nominal" : @(lround(g_thermal_seconds[NSProcessInfoThermalStateNominal])),
    @"fair" : @(lround(g_thermal_seconds[NSProcessInfoThermalStateFair])),
    @"serious" : @(lround(g_thermal_seconds[NSProcessInfoThermalStateSerious])),
    @"critical" : @(lround(g_thermal_seconds[NSProcessInfoThermalStateCritical])),
  };
  record[@"lowPowerS"] = @(lround(g_low_power_seconds));
  const bool any_seconds = g_capped_seconds > 0 || g_uncapped_seconds > 0;
  const bool capped = any_seconds ? g_uncapped_seconds <= 0 : cvars::guest_display_refresh_cap;
  record[@"guestRefreshCap"] = g_capped_seconds > 0 && g_uncapped_seconds > 0
                                   ? @"mixed"
                                   : (capped ? @"capped" : @"uncapped");
  record[@"framerateLimit"] = @(cvars::framerate_limit);
  record[@"peakFootprintMB"] = @(g_peak_footprint >> 20);
  if (g_min_available != UINT64_MAX) {
    record[@"minAvailableMB"] = @(g_min_available >> 20);
  }
  record[@"memoryWarnings"] = @(g_memory_warnings);
  AddWindowsLocked(record);
  record[@"end"] = end;
  // The day it was played: the day it ended, or for a checkpoint (a session
  // the next launch may report as cut short), the day it started.
  record[@"day"] = UTCDay([end isEqualToString:@"unfinished"] ? g_begin_time : time(nullptr));
  return record;
}

void WriteCheckpointLocked() {
  @autoreleasepool {
    NSMutableDictionary* record = RecordLocked(@"unfinished");
    // Kept on the device: the next launch gives it to a crash record of this
    // launch, the session record goes without it.
    if (NSString* launch_id = xe_ios_diagnostics_launch_id()) {
      record[@"launchId"] = launch_id;
    }
    NSData* json = [NSJSONSerialization dataWithJSONObject:record options:0 error:nil];
    [json writeToFile:CheckpointPath() atomically:YES];
  }
}

// Adds the time since the last sample to the state it ends in.
void SampleLocked(bool checkpoint) {
  const uint64_t now = mach_absolute_time();
  const uint64_t previous_sample_ticks = g_last_sample_ticks;
  const double seconds = TicksToSeconds(now - previous_sample_ticks);
  g_last_sample_ticks = now;
  NSProcessInfo* info = [NSProcessInfo processInfo];
  const NSProcessInfoThermalState thermal = info.thermalState;
  if (thermal >= NSProcessInfoThermalStateNominal && thermal <= NSProcessInfoThermalStateCritical) {
    g_thermal_seconds[thermal] += seconds;
  }
  CloseWindowLocked(now - previous_sample_ticks, thermal);
  if (info.lowPowerModeEnabled) {
    g_low_power_seconds += seconds;
  }
  (cvars::guest_display_refresh_cap ? g_capped_seconds : g_uncapped_seconds) += seconds;
  const Footprint footprint = ReadFootprint();
  g_peak_footprint = std::max(g_peak_footprint, footprint.current);
  // Once this session has set the process's peak, the kernel's figure also
  // covers spikes between samples.
  if (footprint.lifetime_peak > g_begin_lifetime_peak) {
    g_peak_footprint = std::max(g_peak_footprint, footprint.lifetime_peak);
  }
  if (const uint64_t available = os_proc_available_memory()) {
    g_min_available = std::min<uint64_t>(g_min_available, available);
  }
  const double elapsed = TicksToSeconds(now - g_begin_ticks);
  if (checkpoint && elapsed >= g_next_checkpoint_seconds) {
    g_next_checkpoint_seconds = elapsed + kCheckpointSeconds;
    WriteCheckpointLocked();
  }
}

void StopSourcesLocked() {
  if (g_timer) {
    dispatch_source_cancel(g_timer);
    dispatch_release(g_timer);
    g_timer = nullptr;
  }
  if (g_memory_source) {
    dispatch_source_cancel(g_memory_source);
    dispatch_release(g_memory_source);
    g_memory_source = nullptr;
  }
}

}  // namespace

void xe_ios_session_stats_begin(void) {
  dispatch_sync(Queue(), ^{
    if (g_running.load(std::memory_order_relaxed)) {
      return;
    }
    for (size_t i = 0; i < std::size(kIntervalEdgesMs); ++i) {
      g_edge_ticks[i] = MsToTicks(kIntervalEdgesMs[i]);
    }
    g_long_ticks = MsToTicks(kLongFrameMs);
    g_gap_ticks = MsToTicks(kGapMs);
    g_ticks_per_ms = std::max<uint64_t>(MsToTicks(1), 1);
    for (auto& count : g_counts) {
      count.store(0, std::memory_order_relaxed);
    }
    for (auto& count : g_window_counts) {
      count.store(0, std::memory_order_relaxed);
    }
    g_title_id.store(0, std::memory_order_relaxed);
    g_last_frame_ticks.store(0, std::memory_order_relaxed);
    g_frames.store(0, std::memory_order_relaxed);
    g_gaps.store(0, std::memory_order_relaxed);
    g_long_frames.store(0, std::memory_order_relaxed);
    g_long_frames_compiling.store(0, std::memory_order_relaxed);
    g_halted_faults.store(0, std::memory_order_relaxed);
    g_backend_failures.store(0, std::memory_order_relaxed);
    g_active_ticks.store(0, std::memory_order_relaxed);
    g_first_frame_ticks.store(0, std::memory_order_relaxed);
    g_begin_ticks = mach_absolute_time();
    g_begin_time = time(nullptr);
    g_last_sample_ticks = g_begin_ticks;
    g_next_checkpoint_seconds = kFirstCheckpointSeconds;
    g_begin_lifetime_peak = ReadFootprint().lifetime_peak;
    const xe::gpu::metal::CompileActivity& activity = xe::gpu::metal::compile_activity;
    g_begin_compiles_finished = activity.finished.load(std::memory_order_relaxed);
    g_begin_storage_generation = activity.storage_generation.load(std::memory_order_relaxed);
    g_begin_command_buffer_failures =
        xe::gpu::metal::command_buffer_failures.load(std::memory_order_relaxed);
    std::fill(std::begin(g_thermal_seconds), std::end(g_thermal_seconds), 0);
    g_low_power_seconds = 0;
    g_capped_seconds = 0;
    g_uncapped_seconds = 0;
    g_peak_footprint = 0;
    g_min_available = UINT64_MAX;
    g_memory_warnings = 0;
    g_window_begin_frames = 0;
    g_window_begin_active_ticks = 0;
    g_window_begin_gpu_busy_ns = xe::gpu::metal::gpu_timing.busy_ns.load(std::memory_order_relaxed);
    g_window_begin_gpu_wait_ticks =
        xe::gpu::metal::gpu_timing.wait_ticks.load(std::memory_order_relaxed);
    g_window_begin_drawable_wait_ticks =
        xe::ui::metal::drawable_wait_ticks.load(std::memory_order_relaxed);
    g_window_begin_compiles_finished = g_begin_compiles_finished;
    g_window_count = 0;
    g_windows_unrecorded = 0;

    g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, Queue());
    dispatch_source_set_timer(g_timer,
                              dispatch_time(DISPATCH_TIME_NOW, kSampleSeconds * NSEC_PER_SEC),
                              kSampleSeconds * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(g_timer, ^{
      SampleLocked(true);
    });
    dispatch_resume(g_timer);
    // The pressure events behind UIKit's memory warnings.
    g_memory_source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                               DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
                               Queue());
    dispatch_source_set_event_handler(g_memory_source, ^{
      ++g_memory_warnings;
    });
    dispatch_resume(g_memory_source);

    xe_ios_diagnostics_create_directory([CheckpointPath() stringByDeletingLastPathComponent]);
    g_running.store(true, std::memory_order_release);
    WriteCheckpointLocked();
  });
}

void xe_ios_session_stats_note_title(uint32_t title_id) {
  g_title_id.store(title_id, std::memory_order_relaxed);
}

NSDictionary* xe_ios_session_stats_end(void) {
  __block NSDictionary* record = nil;
  dispatch_sync(Queue(), ^{
    if (!g_running.load(std::memory_order_relaxed)) {
      return;
    }
    g_running.store(false, std::memory_order_relaxed);
    StopSourcesLocked();
    SampleLocked(false);
    record = [RecordLocked(@"normal") retain];
    [[NSFileManager defaultManager] removeItemAtPath:CheckpointPath() error:nil];
  });
  return [record autorelease];
}

void xe_ios_session_stats_discard(void) {
  dispatch_sync(Queue(), ^{
    if (g_running.load(std::memory_order_relaxed)) {
      g_running.store(false, std::memory_order_relaxed);
      StopSourcesLocked();
    }
    [[NSFileManager defaultManager] removeItemAtPath:CheckpointPath() error:nil];
  });
}

BOOL xe_ios_session_stats_running(void) {
  return g_running.load(std::memory_order_relaxed);
}

void xe_ios_session_stats_note_halted_faults(uint32_t count) {
  if (g_running.load(std::memory_order_relaxed)) {
    g_halted_faults.store(count, std::memory_order_relaxed);
  }
}

void xe_ios_session_stats_note_backend_failure(void) {
  if (g_running.load(std::memory_order_relaxed)) {
    g_backend_failures.fetch_add(1, std::memory_order_relaxed);
  }
}

void xe_ios_session_stats_note_guest_frame(void) {
  if (!g_running.load(std::memory_order_acquire)) {
    return;
  }
  const uint64_t now = mach_absolute_time();
  const uint64_t previous = g_last_frame_ticks.exchange(now, std::memory_order_relaxed);
  const xe::gpu::metal::CompileActivity& activity = xe::gpu::metal::compile_activity;
  const uint64_t compiles_finished = activity.finished.load(std::memory_order_relaxed);
  const uint64_t previous_compiles_finished =
      g_last_compiles_finished.exchange(compiles_finished, std::memory_order_relaxed);
  if (!previous) {
    // The game's first frame: how long it took to start.
    g_first_frame_ticks.store(now - g_begin_ticks, std::memory_order_relaxed);
    return;
  }
  const uint64_t interval = now - previous;
  if (interval > g_gap_ticks) {
    g_gaps.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  size_t bucket = 0;
  while (bucket < std::size(g_edge_ticks) && interval > g_edge_ticks[bucket]) {
    ++bucket;
  }
  g_counts[bucket].fetch_add(1, std::memory_order_relaxed);
  g_window_counts[std::min<uint64_t>(interval / g_ticks_per_ms, kWindowBucketCount - 1)]
      .fetch_add(1, std::memory_order_relaxed);
  g_frames.fetch_add(1, std::memory_order_relaxed);
  g_active_ticks.fetch_add(interval, std::memory_order_relaxed);
  if (interval > g_long_ticks) {
    g_long_frames.fetch_add(1, std::memory_order_relaxed);
    // A compile was in flight at the end of the frame, or finished during it.
    if (activity.queued.load(std::memory_order_relaxed) != compiles_finished ||
        compiles_finished != previous_compiles_finished) {
      g_long_frames_compiling.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

NSMutableDictionary* xe_ios_session_stats_take_unfinished(void) {
  NSString* path = CheckpointPath();
  NSData* json = [NSData dataWithContentsOfFile:path];
  [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
  if (!json) {
    return nil;
  }
  id record = [NSJSONSerialization JSONObjectWithData:json
                                              options:NSJSONReadingMutableContainers
                                                error:nil];
  return [record isKindOfClass:[NSMutableDictionary class]] ? record : nil;
}
