/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/thread_state.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/context_allocation.h"
#include "xenia/cpu/processor.h"

#include "xenia/xbox.h"
namespace xe {
namespace cpu {

thread_local ThreadState* thread_state_ = nullptr;

static void* AllocateContext() {
  // iOS may refuse the preferred high virtual addresses even while ordinary
  // mappings remain available. A64 uses literal guest-address bounds, unlike
  // backends that require the context's low bits to contain 0xE0000000.
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
  constexpr bool allow_fallback = true;
#else
  constexpr bool allow_fallback = false;
#endif
  const auto allocation = detail::AllocateContext(
      sizeof(ppc::PPCContext), memory::allocation_granularity(), allow_fallback,
      [](void* base, size_t size) {
        return memory::AllocFixed(base, size,
                                  memory::AllocationType::kReserveCommit,
                                  memory::PageAccess::kReadWrite);
      });
  if (!allocation.context) {
    xe::FatalError("Unable to allocate PPC thread context memory.");
  }
  if (allocation.used_fallback) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
      XELOGW("PPC context: fixed-address reservations failed; using an "
             "OS-chosen mapping at {:p}", allocation.context);
    }
  }
  return allocation.context;
}

static void FreeContext(void* ctx) {
  size_t granularity = xe::memory::allocation_granularity();
  char* true_start_of_ctx =
      &reinterpret_cast<char*>(ctx)[-static_cast<ptrdiff_t>(granularity)];
  memory::DeallocFixed(true_start_of_ctx, granularity + sizeof(ppc::PPCContext),
                       memory::DeallocationType::kRelease);
}

ThreadState::ThreadState(Processor* processor, uint32_t thread_id,
                         uint32_t stack_base, uint32_t pcr_address)
    : processor_(processor),
      memory_(processor->memory()),
      thread_id_(thread_id) {
  if (thread_id_ == UINT_MAX) {
    // System thread. Assign the system thread ID with a high bit
    // set so people know what's up.
    uint32_t system_thread_handle = xe::threading::current_thread_system_id();
    thread_id_ = 0x80000000 | system_thread_handle;
  }
  backend_data_ = processor->backend()->AllocThreadData();

  // Allocate with 64b alignment.

  context_ = reinterpret_cast<ppc::PPCContext*>(AllocateContext());
  processor->backend()->InitializeBackendContext(context_);
  assert_true(((uint64_t)context_ & 0x3F) == 0);
  std::memset(context_, 0, sizeof(ppc::PPCContext));

  // Stash pointers to common structures that callbacks may need.
  context_->global_mutex = &xe::global_critical_region::mutex();
  context_->virtual_membase = memory_->virtual_membase();
  context_->tlb_read = memory_->kernel_tlb();
  context_->tlb_write = memory_->kernel_tlb();
  context_->physical_membase = memory_->physical_membase();
  context_->processor = processor_;
  context_->thread_state = this;
  context_->thread_id = thread_id_;
  context_->trace_counts = processor->AcquireTraceCounts(thread_id_);

  // Set initial registers.
  context_->r[1] = stack_base;

  // constant register, used by hv only i think
  context_->r[2] = 0x20000000;

  context_->r[13] = pcr_address;
  // VSCR - Vector Status and Control Register
  // NJ bit (bit 16) = 1: Non-Java IEEE mode (default for Xbox 360)
  context_->vscr_vec = vec128i(0, 0, 0, 0x00010000);
  context_->msr = 0x9030;  // dumped from a real 360, 0x8000

  // this register can be used for arbitrary data according to the PPC docs
  // but the suggested use is to mark which vector registers are in use, for
  // faster save/restore it seems unlikely anything uses this, especially since
  // we have way more than 32 vrs, but setting it to all ones seems closer to
  // correct than 0
  context_->vrsave = ~0u;
}

ThreadState::~ThreadState() {
  if (backend_data_) {
    processor_->backend()->FreeThreadData(backend_data_);
  }
  if (thread_state_ == this) {
    thread_state_ = nullptr;
  }
  if (context_) {
    processor_->ReleaseTraceCounts(context_->trace_counts);
    context_->trace_counts = nullptr;
    processor_->backend()->DeinitializeBackendContext(context_);
    FreeContext(reinterpret_cast<void*>(context_));
  }
}

void ThreadState::Bind(ThreadState* thread_state) {
  thread_state_ = thread_state;
}

ThreadState* ThreadState::Get() { return thread_state_; }

uint32_t ThreadState::GetThreadID() {
  return thread_state_ ? thread_state_->thread_id_ : 0xFFFFFFFF;
}

}  // namespace cpu
}  // namespace xe
