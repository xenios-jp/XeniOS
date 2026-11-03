/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_CRASH_CONTEXT_H_
#define XENIA_UI_IOS_SHARED_IOS_CRASH_CONTEXT_H_

// Plain C: also imported by the crash reporter extension (Swift), which reads
// the app's copy, the exported symbol xe_ios_crash_context, out of the crashed
// process through its read-only task port. Fields are only ever appended;
// bump the version when they are.

#include <stdint.h>

#define XE_IOS_CRASH_CONTEXT_MAGIC 0x31434358u  // "XCC1", little endian
#define XE_IOS_CRASH_CONTEXT_VERSION 1u
#define XE_IOS_CRASH_CONTEXT_MAX_FRAMES 32

typedef struct xe_ios_crash_context_t {
  uint32_t magic;
  uint32_t version;
  // Online Features and Share Diagnostics both on, kept current by the app.
  // The extension sends nothing unless it is 1.
  uint32_t upload_allowed;
  uint32_t title_id;
  // Random per launch; joins the extension's report with the app's own crash
  // record from the next launch.
  char launch_id[40];
  // The diagnostics endpoint, empty if the build has none.
  char endpoint[256];
  // {"build":{...},"device":{...}}, the envelope fields the app would send.
  char envelope_json[2048];
  // Code ranges: the app executable (header to end of __TEXT) and the JIT.
  uint64_t image_base;
  uint64_t image_size;
  uint64_t jit_base;
  uint64_t jit_size;
  // An unhandled fault seen by Xenia's exception handler; fault_signal is 0
  // if there was none (the crash came from elsewhere, e.g. abort()).
  uint32_t fault_signal;
  uint32_t fault_frame_count;
  uint64_t fault_thread_id;
  uint64_t fault_address;
  uint64_t fault_pc;
  uint64_t fault_lr;
  uint64_t fault_sp;
  uint64_t fault_fp;
  uint64_t fault_uptime_ms;
  // Return addresses from the frame-pointer walk, pointer authentication
  // stripped.
  uint64_t fault_frames[XE_IOS_CRASH_CONTEXT_MAX_FRAMES];
} xe_ios_crash_context_t;

#endif  // XENIA_UI_IOS_SHARED_IOS_CRASH_CONTEXT_H_
