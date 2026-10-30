/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"

#if XE_PLATFORM_WIN32
#include <Windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

// AddressSanitizer needs to be told about stack switches or it poisons and
// unwinds against the wrong stack, making sanitized builds unusable with
// fibers.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define XE_FIBER_ASAN 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define XE_FIBER_ASAN 1
#endif
#if XE_FIBER_ASAN
#include <sanitizer/common_interface_defs.h>
#endif

// Boost.Context fcontext_t primitives. The implementation is the hand-written
// assembly in the boostorg/context submodule (third_party/boost_context),
// linked via the boost_context static lib. These three routines carry no Boost
// header dependencies, so we declare the two we use directly here.
extern "C" {
typedef void* fcontext_t;
struct fcontext_transfer_t {
  fcontext_t fctx;
  void* data;
};
fcontext_t make_fcontext(void* sp, size_t size,
                         void (*fn)(fcontext_transfer_t));
fcontext_transfer_t jump_fcontext(fcontext_t to, void* vp);
}  // extern "C"

namespace xe {
namespace threading {

namespace {

// The fiber currently executing on this host thread.
thread_local Fiber* current_fiber_ = nullptr;

size_t HostPageSize() {
#if XE_PLATFORM_WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return static_cast<size_t>(si.dwPageSize);
#else
  return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

// A guard-paged fiber stack: the lowest (floor) page is never usable so an
// overflow faults instead of corrupting adjacent memory. Windows commits an
// initial window and grows on guard faults, POSIX mmap commits lazily.
struct FiberStack {
  void* base = nullptr;  // low address of the whole allocation
  size_t total = 0;      // whole allocation, including the floor page
  void* high = nullptr;  // one past the top (what make_fcontext wants)
  size_t usable = 0;     // total minus the floor page
};

// Live stacks, for growth and for classifying a fault address as an overflow.
// Faults are rare, a mutex and linear scan are fine.
struct StackRecord {
  uintptr_t base = 0;  // reserve low, the floor page
  size_t total = 0;
  uintptr_t guard = 0;  // current PAGE_GUARD page, base when fully grown
};
std::mutex stack_registry_lock_;
std::vector<StackRecord> stack_registry_;

#if XE_PLATFORM_WIN32
// Committed up front at the top, then grown per guard fault.
constexpr size_t kInitialCommitBytes = 64 * 1024;
constexpr size_t kGrowCommitBytes = 256 * 1024;

// Grows a registered fiber stack on a guard fault and reports the overflow
// when one reaches the floor page. An alloca can jump past the guard into
// reserved pages, which arrives as an access violation instead.
LONG NTAPI FiberStackExceptionHandler(PEXCEPTION_POINTERS info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code != STATUS_GUARD_PAGE_VIOLATION && code != STATUS_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uintptr_t addr =
      static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
  const size_t page = HostPageSize();
  std::lock_guard<std::mutex> lock(stack_registry_lock_);
  for (auto& r : stack_registry_) {
    if (addr < r.base || addr >= r.base + r.total) {
      continue;
    }
    const uintptr_t floor_end = r.base + page;
    if (addr < floor_end || r.guard <= r.base) {
      // Exhausted. Commit the floor so the report itself has stack to run on,
      // then let the fault propagate as a crash.
      VirtualAlloc(reinterpret_cast<void*>(r.base), page, MEM_COMMIT,
                   PAGE_READWRITE);
      XELOGE(
          "Fiber stack overflow: {} KiB stack exhausted, fault at {:X} (host "
          "tid {:08X})",
          r.total / 1024, addr, GetCurrentThreadId());
      return EXCEPTION_CONTINUE_SEARCH;
    }
    // Commit down to the faulting page plus a chunk of headroom and re-arm
    // the guard below that.
    uintptr_t fault_page = addr & ~static_cast<uintptr_t>(page - 1);
    uintptr_t new_low =
        fault_page > kGrowCommitBytes ? fault_page - kGrowCommitBytes : 0;
    if (new_low < floor_end) {
      new_low = floor_end;
    }
    uintptr_t commit_end = (r.guard > fault_page ? r.guard : fault_page) + page;
    VirtualAlloc(reinterpret_cast<void*>(new_low), commit_end - new_low,
                 MEM_COMMIT, PAGE_READWRITE);
    // Strip a bypassed guard page's flag so it cannot fire mid-stack later.
    DWORD old_protect;
    VirtualProtect(reinterpret_cast<void*>(r.guard), page, PAGE_READWRITE,
                   &old_protect);
    if (new_low > floor_end) {
      uintptr_t new_guard = new_low - page;
      VirtualAlloc(reinterpret_cast<void*>(new_guard), page, MEM_COMMIT,
                   PAGE_READWRITE | PAGE_GUARD);
      r.guard = new_guard;
    } else {
      // Fully grown, the next stop is the floor.
      r.guard = r.base;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

std::once_flag veh_install_once_;
#endif  // XE_PLATFORM_WIN32

FiberStack AllocFiberStack(size_t size) {
  const size_t page = HostPageSize();
  size_t pages = (size + page - 1) / page;
  if (pages < 1) {
    pages = 1;
  }
  FiberStack s;
  s.total = (pages + 1) * page;  // +1 floor page at the low end
  StackRecord record;

#if XE_PLATFORM_WIN32
  // First-position handler, so guard faults grow the stack before any other
  // handler sees them.
  std::call_once(veh_install_once_, []() {
    AddVectoredExceptionHandler(1, FiberStackExceptionHandler);
  });
  s.base = VirtualAlloc(nullptr, s.total, MEM_RESERVE, PAGE_READWRITE);
  if (!s.base) {
    xe::FatalError("Failed to reserve fiber stack");
  }
  char* top = static_cast<char*>(s.base) + s.total;
  size_t initial = kInitialCommitBytes;
  // Small stack: commit everything above the floor, no growth.
  if (initial + 2 * page >= s.total) {
    initial = s.total - page;
    VirtualAlloc(top - initial, initial, MEM_COMMIT, PAGE_READWRITE);
    record.guard = reinterpret_cast<uintptr_t>(s.base);
  } else {
    VirtualAlloc(top - initial, initial, MEM_COMMIT, PAGE_READWRITE);
    char* guard = top - initial - page;
    VirtualAlloc(guard, page, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
    record.guard = reinterpret_cast<uintptr_t>(guard);
  }
#else
  s.base = mmap(nullptr, s.total, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (s.base == MAP_FAILED) {
    xe::FatalError("Failed to map fiber stack");
  }
  mprotect(s.base, page, PROT_NONE);
  record.guard = reinterpret_cast<uintptr_t>(s.base);
#endif

  s.high = static_cast<char*>(s.base) + s.total;
  s.usable = s.total - page;
  record.base = reinterpret_cast<uintptr_t>(s.base);
  record.total = s.total;
  {
    std::lock_guard<std::mutex> lock(stack_registry_lock_);
    stack_registry_.push_back(record);
  }
  return s;
}

void FreeFiberStack(FiberStack& s) {
  if (!s.base) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(stack_registry_lock_);
    for (auto it = stack_registry_.begin(); it != stack_registry_.end(); ++it) {
      if (it->base == reinterpret_cast<uintptr_t>(s.base)) {
        stack_registry_.erase(it);
        break;
      }
    }
  }
#if XE_PLATFORM_WIN32
  VirtualFree(s.base, 0, MEM_RELEASE);
#else
  munmap(s.base, s.total);
#endif
  s = FiberStack{};
}

class FcontextFiber : public Fiber {
 public:
  // Created fiber: owns a guard-paged stack and a context entered the first
  // time it is switched to.
  FcontextFiber(size_t stack_size, std::function<void()> start_routine)
      : entry_(std::move(start_routine)) {
    stack_ = AllocFiberStack(stack_size);
    ResetContext();
    owns_stack_ = true;
  }

  // Adopted fiber: wraps the calling thread's existing stack. Its context is
  // captured the first time something switches away from it.
  FcontextFiber() {
#if XE_FIBER_ASAN
    // The sanitizer needs real bounds when fibers switch back to this stack.
#if XE_PLATFORM_WIN32
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    stack_.base = reinterpret_cast<void*>(low);
    stack_.usable = high - low;
#elif XE_PLATFORM_LINUX || XE_PLATFORM_ANDROID
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
      pthread_attr_getstack(&attr, &stack_.base, &stack_.usable);
      pthread_attr_destroy(&attr);
    }
#elif XE_PLATFORM_APPLE
    stack_.usable = pthread_get_stacksize_np(pthread_self());
    stack_.base = static_cast<char*>(pthread_get_stackaddr_np(pthread_self())) -
                  stack_.usable;
#endif
#endif  // XE_FIBER_ASAN
  }

  ~FcontextFiber() override {
    if (owns_stack_) {
      FreeFiberStack(stack_);
    }
  }

  void SwitchTo() override {
    auto* prev = static_cast<FcontextFiber*>(current_fiber_);
    // SwitchTo requires the host thread to have a current fiber (established by
    // CreateFromThread or a prior switch).
    assert_not_null(prev);
    assert_true(prev != this);
    current_fiber_ = this;
#if XE_FIBER_ASAN
    __sanitizer_start_switch_fiber(&prev->asan_fake_stack_, stack_.base,
                                   stack_.usable);
#endif
    // Enter this fiber, handing it |prev| so it can record where prev resumes.
    fcontext_transfer_t t = jump_fcontext(fctx_, prev);
#if XE_FIBER_ASAN
    // Back on |prev|'s stack.
    __sanitizer_finish_switch_fiber(prev->asan_fake_stack_, nullptr, nullptr);
#endif
    // Resumed: |t.data| is the fiber that switched back into us, |t.fctx| its
    // continuation point. Save it so we can return to that fiber later.
    auto* resumer = static_cast<FcontextFiber*>(t.data);
    resumer->fctx_ = t.fctx;
  }

  void SetTerminated() override { terminated_ = true; }

  void Restart() override {
    assert_true(owns_stack_);
    assert_true(current_fiber_ != this);
    ResetContext();
    terminated_ = false;
  }

 private:
  // The stack's floor page is not part of what the context may use.
  void ResetContext() {
    fctx_ = make_fcontext(stack_.high, stack_.usable,
                          &FcontextFiber::EntryTrampoline);
  }

  static void EntryTrampoline(fcontext_transfer_t t) {
    // First entry into a created fiber. |t.data| is the switcher (prev),
    // |t.fctx| its continuation - record it so prev can be resumed.
    auto* prev = static_cast<FcontextFiber*>(t.data);
    prev->fctx_ = t.fctx;

    auto* self = static_cast<FcontextFiber*>(current_fiber_);
#if XE_FIBER_ASAN
    __sanitizer_finish_switch_fiber(self->asan_fake_stack_, nullptr, nullptr);
#endif
    self->entry_();
    self->terminated_ = true;

    // The start routine returned. In normal use the guest scheduler switches
    // away before this point, so this is only a safety net: hand control back
    // to whoever last resumed us. Resuming a terminated fiber is undefined.
#if XE_FIBER_ASAN
    // Passing null frees this dying fiber's fake stack.
    __sanitizer_start_switch_fiber(nullptr, prev->stack_.base,
                                   prev->stack_.usable);
#endif
    jump_fcontext(prev->fctx_, prev);
  }

  std::function<void()> entry_;
  fcontext_t fctx_ = nullptr;
  FiberStack stack_;
  bool owns_stack_ = false;
  bool terminated_ = false;
#if XE_FIBER_ASAN
  // Sanitizer fake-stack handle, saved when this fiber suspends and consumed
  // when it resumes.
  void* asan_fake_stack_ = nullptr;
#endif
};

}  // namespace

std::unique_ptr<Fiber> Fiber::Create(CreationParameters params,
                                     std::function<void()> start_routine) {
  return std::make_unique<FcontextFiber>(params.stack_size,
                                         std::move(start_routine));
}

std::unique_ptr<Fiber> Fiber::CreateFromThread() {
  auto fiber = std::make_unique<FcontextFiber>();
  current_fiber_ = fiber.get();
  return fiber;
}

Fiber* Fiber::GetCurrentFiber() { return current_fiber_; }

bool Fiber::IsStackOverflowFault(const void* address) {
  auto addr = reinterpret_cast<uintptr_t>(address);
  const size_t page = HostPageSize();
  std::lock_guard<std::mutex> lock(stack_registry_lock_);
  for (auto& r : stack_registry_) {
    if (addr >= r.base && addr < r.base + page) {
      return true;
    }
  }
  return false;
}

}  // namespace threading
}  // namespace xe
