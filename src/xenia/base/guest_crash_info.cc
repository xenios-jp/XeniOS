/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/guest_crash_info.h"

#include <atomic>

namespace xe {

namespace {
std::atomic<GuestCrashInfoProvider> guest_crash_info_provider_{nullptr};
}  // namespace

void SetGuestCrashInfoProvider(GuestCrashInfoProvider provider) {
  guest_crash_info_provider_.store(provider, std::memory_order_release);
}

bool GetGuestCrashInfo(const HostThreadContext& context, uint64_t fault_address,
                       GuestCrashInfo* out) {
  GuestCrashInfoProvider provider =
      guest_crash_info_provider_.load(std::memory_order_acquire);
  return provider && provider(context, fault_address, out);
}

}  // namespace xe
