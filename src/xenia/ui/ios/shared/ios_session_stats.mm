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
#include <iterator>

#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_compile_activity.h"
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
std::atomic<uint64_t> g_active_ticks{0};
// From the start of the session to the game's first frame; 0 until then.
std::atomic<uint64_t> g_first_frame_ticks{0};
uint64_t g_edge_ticks[std::size(kIntervalEdgesMs)];
uint64_t g_long_ticks = 0;
uint64_t g_gap_ticks = 0;

// Written by the stats queue before the session starts running; read by the
// presenting thread too.
uint64_t g_begin_ticks = 0;

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
  record[@"end"] = end;
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
  const double seconds = TicksToSeconds(now - g_last_sample_ticks);
  g_last_sample_ticks = now;
  NSProcessInfo* info = [NSProcessInfo processInfo];
  const NSProcessInfoThermalState thermal = info.thermalState;
  if (thermal >= NSProcessInfoThermalStateNominal && thermal <= NSProcessInfoThermalStateCritical) {
    g_thermal_seconds[thermal] += seconds;
  }
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
    for (auto& count : g_counts) {
      count.store(0, std::memory_order_relaxed);
    }
    g_title_id.store(0, std::memory_order_relaxed);
    g_last_frame_ticks.store(0, std::memory_order_relaxed);
    g_frames.store(0, std::memory_order_relaxed);
    g_gaps.store(0, std::memory_order_relaxed);
    g_long_frames.store(0, std::memory_order_relaxed);
    g_long_frames_compiling.store(0, std::memory_order_relaxed);
    g_halted_faults.store(0, std::memory_order_relaxed);
    g_active_ticks.store(0, std::memory_order_relaxed);
    g_first_frame_ticks.store(0, std::memory_order_relaxed);
    g_begin_ticks = mach_absolute_time();
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
