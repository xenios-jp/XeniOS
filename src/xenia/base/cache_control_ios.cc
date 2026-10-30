/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstddef>
#include <cstdint>

#include <libkern/OSCacheControl.h>

extern "C" void __clear_cache(void* start, void* end) {
  auto* start_bytes = static_cast<uint8_t*>(start);
  auto* end_bytes = static_cast<uint8_t*>(end);
  if (end_bytes <= start_bytes) {
    return;
  }

  sys_icache_invalidate(start, static_cast<size_t>(end_bytes - start_bytes));
}
