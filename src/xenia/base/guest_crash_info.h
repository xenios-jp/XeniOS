/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_GUEST_CRASH_INFO_H_
#define XENIA_BASE_GUEST_CRASH_INFO_H_

#include <cstdint>

#include "xenia/base/host_thread_context.h"

namespace xe {

// Where in the game's own code a host crash happened, for crash reports:
// guest addresses only, never guest memory contents.
struct GuestCrashInfo {
  static constexpr uint32_t kMaxFrames = 8;

  // The guest thread running on the crashed host thread, 0 if none.
  uint32_t thread_id;
  // The guest function and instruction the JIT code was translated from, 0 if
  // the crash wasn't in or called from JIT code.
  uint32_t function;
  uint32_t pc;
  // The guest link register, and return addresses from the guest stack's back
  // chain, each one checked to point into guest code.
  uint32_t lr;
  uint32_t frames[kMaxFrames];
  uint32_t frame_count;
  // The host crashed in a helper called from JIT code: function and pc are
  // the call site.
  bool via_helper;
  // The fault address fell in guest memory: its guest address.
  bool fault_in_guest;
  uint32_t fault_address;
};

// Fills |out| (zeroed by the caller) and returns whether anything was found.
// Called from a signal handler on the crashed thread: async-signal-safe work
// only.
using GuestCrashInfoProvider = bool (*)(const HostThreadContext& context,
                                        uint64_t fault_address,
                                        GuestCrashInfo* out);

// The CPU backend sets it once it can map its code back to guest code; nullptr
// removes it.
void SetGuestCrashInfoProvider(GuestCrashInfoProvider provider);
bool GetGuestCrashInfo(const HostThreadContext& context, uint64_t fault_address,
                       GuestCrashInfo* out);

}  // namespace xe

#endif  // XENIA_BASE_GUEST_CRASH_INFO_H_
