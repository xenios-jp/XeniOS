/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_RESOLVE_REFRESH_WATCH_LOG_H_
#define XENIA_GPU_RESOLVE_REFRESH_WATCH_LOG_H_

#include <algorithm>
#include <array>
#include <cstdint>

namespace xe::gpu {

// Bounded invalidation history for partial texture refresh. Callers synchronize
// all access with the shared-memory global critical region. Missing history or
// a write outside the refreshed band requires the ordinary texture reload.
class ResolveRefreshWatchLog {
 public:
  static constexpr uint32_t kCapacity = 1024;

  void Add(uint64_t sequence, uint32_t first, uint32_t last, bool by_gpu) {
    entries_[count_++ % kCapacity] = {sequence, first, last, by_gpu};
  }

  bool FiresWithin(uint64_t first_sequence, uint64_t current_sequence,
                   uint32_t range_first, uint32_t range_last,
                   uint32_t cpu_first, uint32_t cpu_last, uint32_t gpu_first,
                   uint32_t gpu_last) const {
    if (!first_sequence) {
      return false;
    }
    if (first_sequence > current_sequence) {
      return first_sequence - current_sequence == 1;
    }
    uint64_t needed = current_sequence - first_sequence + 1;
    if (needed > std::min(count_, uint64_t(kCapacity))) {
      return false;
    }
    for (uint64_t i = 0; i < needed; ++i) {
      const Entry& entry = entries_[(count_ - 1 - i) % kCapacity];
      if (entry.sequence != current_sequence - i) {
        return false;
      }
      uint32_t first = std::max(entry.first, range_first);
      uint32_t last = std::min(entry.last, range_last);
      if (first > last) {
        continue;
      }
      if (entry.by_gpu ? (first < gpu_first || last > gpu_last)
                       : (first < cpu_first || last > cpu_last)) {
        return false;
      }
    }
    return true;
  }

 private:
  struct Entry {
    uint64_t sequence;
    uint32_t first;
    uint32_t last;
    bool by_gpu;
  };
  std::array<Entry, kCapacity> entries_;
  uint64_t count_ = 0;
};

}  // namespace xe::gpu

#endif  // XENIA_GPU_RESOLVE_REFRESH_WATCH_LOG_H_
