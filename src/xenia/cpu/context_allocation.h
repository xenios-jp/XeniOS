/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_CONTEXT_ALLOCATION_H_
#define XENIA_CPU_CONTEXT_ALLOCATION_H_

#include <cstddef>
#include <cstdint>

namespace xe::cpu::detail {

struct ContextAllocation {
  void* context = nullptr;
  bool used_fallback = false;
};

// Reserve the preceding granule for backend state. The fixed address pattern
// is required by some backends; callers must explicitly allow arbitrary VAs.
template <typename Allocate>
ContextAllocation AllocateContext(size_t context_size, size_t granularity,
                                  bool allow_fallback, Allocate allocate) {
  const size_t allocation_size = granularity + context_size;
  auto context_at = [granularity](void* allocation) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(allocation) +
                                    granularity);
  };
  for (uint32_t slot = 0x40; slot < 8192; ++slot) {
    const uintptr_t base =
        ((uint64_t(slot) << 32) | 0xE0000000) - granularity;
    if (void* allocation = allocate(reinterpret_cast<void*>(base),
                                    allocation_size)) {
      return {context_at(allocation), false};
    }
  }
  if (allow_fallback) {
    if (void* allocation = allocate(nullptr, allocation_size)) {
      return {context_at(allocation), true};
    }
  }
  return {};
}

}  // namespace xe::cpu::detail

#endif  // XENIA_CPU_CONTEXT_ALLOCATION_H_
