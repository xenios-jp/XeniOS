/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_COMPILE_ACTIVITY_H_
#define XENIA_GPU_METAL_METAL_COMPILE_ACTIVITY_H_

#include <atomic>
#include <cstdint>

namespace xe {
namespace gpu {
namespace metal {

// How much shader and pipeline compilation the Metal backend has in hand, for UI
// that warns about stutter and missing effects while it lasts. Written only where
// the compile threads take or finish a request and around the shader storage
// load, never per draw; readers poll it. Fields are read independently, so a
// reader may see them a moment apart.
struct CompileActivity {
  // Requests ever queued to the compile threads, and ever finished or dropped by
  // them. The difference is the backlog.
  std::atomic<uint64_t> queued{0};
  std::atomic<uint64_t> finished{0};

  // Bumped when a title's shader storage starts loading.
  std::atomic<uint32_t> storage_generation{0};
  // From the start of the load until the last stored shader and pipeline has
  // been rebuilt (or the load failed, or the compile threads were shut down).
  std::atomic<bool> storage_loading{false};
  // The rebuild's own requests, apart from what the game asks for meanwhile
  // (which goes first and would make the rebuild look done early): the shaders
  // the stored pipelines need, then the stored pipelines. storage_requests is
  // the total: until every stored pipeline is queued it counts each stored
  // description as one (some are later found unbuildable or already built), and
  // 0 while nothing is known. storage_requests_done never passes it.
  std::atomic<uint64_t> storage_requests{0};
  std::atomic<uint64_t> storage_requests_done{0};
  // The title had no stored pipelines: everything it draws compiles for the first
  // time.
  std::atomic<bool> storage_empty{false};
  // The number of stored pipeline descriptions, published before the shaders
  // they need are compiled (which can take most of the load), so a forecast can
  // start early. 0 until known.
  std::atomic<uint64_t> storage_total{0};
  // How long the title's last complete rebuild took per stored description, in
  // microseconds, from a sidecar next to the stored pipelines. 0: no usable
  // record (first run, or the pipeline version changed).
  std::atomic<uint64_t> storage_seed_us_per_stored{0};
};

inline CompileActivity compile_activity;

// One reading of compile_activity, for UI that polls it.
struct CompileActivitySample {
  uint64_t queued = 0;
  uint64_t finished = 0;
  uint32_t storage_generation = 0;
  bool storage_loading = false;
  bool storage_empty = false;
  uint64_t storage_requests = 0;
  uint64_t storage_requests_done = 0;
  uint64_t storage_total = 0;
  uint64_t storage_seed_us_per_stored = 0;
};

// Each finished count is read before what it is counted against, so a request
// finishing between the loads reads as still pending, never as more done than
// asked for.
inline CompileActivitySample SampleCompileActivity(
    const CompileActivity& activity = compile_activity) {
  CompileActivitySample sample;
  sample.finished = activity.finished.load(std::memory_order_relaxed);
  sample.queued = activity.queued.load(std::memory_order_relaxed);
  sample.storage_generation =
      activity.storage_generation.load(std::memory_order_relaxed);
  sample.storage_loading =
      activity.storage_loading.load(std::memory_order_relaxed);
  sample.storage_empty = activity.storage_empty.load(std::memory_order_relaxed);
  sample.storage_requests_done =
      activity.storage_requests_done.load(std::memory_order_relaxed);
  sample.storage_requests =
      activity.storage_requests.load(std::memory_order_relaxed);
  sample.storage_total = activity.storage_total.load(std::memory_order_relaxed);
  sample.storage_seed_us_per_stored =
      activity.storage_seed_us_per_stored.load(std::memory_order_relaxed);
  return sample;
}

// Guest frames handed to the presenter, for UI that waits for a title's first
// picture. Bumped once per swap with an image.
inline std::atomic<uint64_t> guest_frames_presented{0};

// Command buffers that failed on the GPU (a fault, a timeout, out of memory)
// since launch, for diagnostics.
inline std::atomic<uint64_t> command_buffer_failures{0};

// GPU timing of the command processor's command buffers (not the presenter's),
// for diagnostics that read it every few seconds. Written per command buffer
// and per blocking wait, never per draw.
struct GpuTiming {
  // The union of the command buffers' GPUStartTime..GPUEndTime intervals, in
  // nanoseconds of host time, added as they complete: command buffers that
  // overlap on the GPU count once. It still includes GPU idle inside a command
  // buffer, and runs at whatever clock the GPU had.
  std::atomic<uint64_t> busy_ns{0};
  // The end of the latest interval added to busy_ns.
  std::atomic<uint64_t> busy_end_ns{0};
  // Host ticks the command processor thread spent blocked on the GPU: waiting
  // for a submission to complete, or for the queue to hand out a command buffer
  // while all of them are in flight.
  std::atomic<uint64_t> wait_ticks{0};
};

inline GpuTiming gpu_timing;

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_COMPILE_ACTIVITY_H_
