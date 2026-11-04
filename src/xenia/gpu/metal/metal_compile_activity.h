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
  // While the stored shaders and pipelines are rebuilt: the counts when the load
  // began, and the queued count once everything stored was queued (0 until then).
  std::atomic<bool> storage_loading{false};
  std::atomic<uint64_t> storage_queued_begin{0};
  std::atomic<uint64_t> storage_finished_begin{0};
  std::atomic<uint64_t> storage_queued_end{0};
  // The title had no stored pipelines: everything it draws compiles for the first
  // time.
  std::atomic<bool> storage_empty{false};
  // The number of stored pipeline descriptions, published before they are queued
  // (which can take a while), so a forecast can start early. 0 until known.
  std::atomic<uint64_t> storage_total{0};
  // How long the title's last complete rebuild took per stored description, in
  // microseconds, from a sidecar next to the stored pipelines. 0: no usable
  // record (first run, or the pipeline version changed).
  std::atomic<uint64_t> storage_seed_us_per_stored{0};
};

inline CompileActivity compile_activity;

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
