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
};

inline CompileActivity compile_activity;

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_COMPILE_ACTIVITY_H_
