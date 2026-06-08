/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/mutex.h"
#if XE_PLATFORM_WIN32 == 1
#include "xenia/base/platform_win.h"
#elif XE_PLATFORM_LINUX == 1
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif XE_PLATFORM_APPLE == 1
#include <os/lock.h>
#include <pthread.h>
#endif

namespace xe {
#if XE_PLATFORM_WIN32 == 1 && XE_ENABLE_FAST_WIN32_MUTEX == 1

// xe_global_mutex: recursive mutex via SRWLOCK
void xe_global_mutex::lock() {
  DWORD self = GetCurrentThreadId();
  if (owner_thread_ == self) {
    ++recursion_count_;
    return;
  }
  AcquireSRWLockExclusive(&srwlock_);
  owner_thread_ = self;
  recursion_count_ = 1;
}

void xe_global_mutex::unlock() {
  if (--recursion_count_ == 0) {
    owner_thread_ = 0;
    ReleaseSRWLockExclusive(&srwlock_);
  }
}

bool xe_global_mutex::try_lock() {
  DWORD self = GetCurrentThreadId();
  if (owner_thread_ == self) {
    ++recursion_count_;
    return true;
  }
  if (TryAcquireSRWLockExclusive(&srwlock_)) {
    owner_thread_ = self;
    recursion_count_ = 1;
    return true;
  }
  return false;
}

// xe_fast_mutex: non-recursive mutex via SRWLOCK
void xe_fast_mutex::lock() {
  DWORD self = GetCurrentThreadId();
  if (owner_thread_ == self) {
    assert_always("xe_fast_mutex: recursive lock detected");
  }
  AcquireSRWLockExclusive(&srwlock_);
  owner_thread_ = self;
}

void xe_fast_mutex::unlock() {
  owner_thread_ = 0;
  ReleaseSRWLockExclusive(&srwlock_);
}

bool xe_fast_mutex::try_lock() {
  if (TryAcquireSRWLockExclusive(&srwlock_)) {
    owner_thread_ = GetCurrentThreadId();
    return true;
  }
  return false;
}
#elif XE_PLATFORM_LINUX == 1 && XE_ENABLE_FAST_LINUX_MUTEX == 1

namespace {

inline int futex_wait(std::atomic<uint32_t>* addr, uint32_t expected) {
  return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr,
                 nullptr, 0);
}

inline int futex_wake(std::atomic<uint32_t>* addr, int count) {
  return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr,
                 0);
}

inline pid_t gettid() { return static_cast<pid_t>(syscall(SYS_gettid)); }

}  // namespace

// xe_global_mutex implementation (recursive)
void xe_global_mutex::lock() {
  pid_t self = gettid();

  // Fast path: check if we already own it (recursive lock)
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return;
  }

  // Try to acquire with a simple CAS first (uncontended case)
  uint32_t expected = 0;
  if (XE_LIKELY(state_.compare_exchange_strong(
          expected, 1, std::memory_order_acquire, std::memory_order_relaxed))) {
    owner_.store(self, std::memory_order_relaxed);
    recursion_count_ = 1;
    return;
  }

  lock_slow();
}

void xe_global_mutex::lock_slow() {
  pid_t self = gettid();

  // Spin phase
  for (int i = 0; i < XE_LINUX_MUTEX_SPINCOUNT; ++i) {
#if XE_ARCH_AMD64 == 1
    _mm_pause();
#endif
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      return;
    }
  }

  // Slow path: use futex
  while (true) {
    // Mark as contended (state = 2) and wait
    uint32_t state = state_.exchange(2, std::memory_order_acquire);
    if (state == 0) {
      // We got the lock while marking contended
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      return;
    }

    // Wait on futex
    futex_wait(&state_, 2);

    // Try to acquire after wakeup
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      return;
    }
  }
}

void xe_global_mutex::unlock() {
  if (--recursion_count_ > 0) {
    return;  // Still have recursive locks
  }

  owner_.store(0, std::memory_order_relaxed);

  // If state was 2 (contended), we need to wake a waiter
  if (state_.exchange(0, std::memory_order_release) == 2) {
    futex_wake(&state_, 1);
  }
}

bool xe_global_mutex::try_lock() {
  pid_t self = gettid();

  // Check for recursive lock
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return true;
  }

  uint32_t expected = 0;
  if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
    owner_.store(self, std::memory_order_relaxed);
    recursion_count_ = 1;
    return true;
  }
  return false;
}

// xe_fast_mutex implementation (non-recursive)
void xe_fast_mutex::lock() {
  // Fast path: uncontended
  uint32_t expected = 0;
  if (XE_LIKELY(state_.compare_exchange_strong(
          expected, 1, std::memory_order_acquire, std::memory_order_relaxed))) {
    return;
  }

  lock_slow();
}

void xe_fast_mutex::lock_slow() {
  // Spin phase
  for (int i = 0; i < XE_LINUX_MUTEX_SPINCOUNT; ++i) {
#if XE_ARCH_AMD64 == 1
    _mm_pause();
#endif
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      return;
    }
  }

  // Slow path: use futex
  while (true) {
    // Mark as contended (state = 2) and wait
    uint32_t state = state_.exchange(2, std::memory_order_acquire);
    if (state == 0) {
      // We got the lock while marking contended
      return;
    }

    // Wait on futex
    futex_wait(&state_, 2);

    // Try to acquire after wakeup
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      return;
    }
  }
}

void xe_fast_mutex::unlock() {
  // If state was 2 (contended), we need to wake a waiter
  if (state_.exchange(0, std::memory_order_release) == 2) {
    futex_wake(&state_, 1);
  }
}

bool xe_fast_mutex::try_lock() {
  uint32_t expected = 0;
  return state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                        std::memory_order_relaxed);
}

#elif XE_PLATFORM_APPLE == 1 && XE_ENABLE_FAST_APPLE_MUTEX == 1

namespace {
// Cheap, stable per-thread identity. pthread_self() reads thread-local storage
// (no syscall) and is unique among live threads; it is only ever compared for
// equality here to detect recursive acquisition by the current owner.
inline uint64_t xe_current_thread_id() {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
}
}  // namespace

// xe_global_mutex implementation (recursive, built on os_unfair_lock).
// os_unfair_lock is non-recursive, so the owner thread and recursion depth are
// tracked here and the underlying lock is taken only on the outermost acquire
// (mirrors the Win32 SRWLOCK implementation above).
void xe_global_mutex::lock() {
  uint64_t self = xe_current_thread_id();
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;  // already own it; underlying lock is held
    return;
  }
  os_unfair_lock_lock(&lock_);
  owner_.store(self, std::memory_order_relaxed);
  recursion_count_ = 1;
}

void xe_global_mutex::unlock() {
  if (--recursion_count_ == 0) {
    owner_.store(0, std::memory_order_relaxed);
    os_unfair_lock_unlock(&lock_);
  }
}

bool xe_global_mutex::try_lock() {
  uint64_t self = xe_current_thread_id();
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return true;
  }
  if (os_unfair_lock_trylock(&lock_)) {
    owner_.store(self, std::memory_order_relaxed);
    recursion_count_ = 1;
    return true;
  }
  return false;
}

// xe_fast_mutex implementation (non-recursive).
void xe_fast_mutex::lock() { os_unfair_lock_lock(&lock_); }
void xe_fast_mutex::unlock() { os_unfair_lock_unlock(&lock_); }
bool xe_fast_mutex::try_lock() { return os_unfair_lock_trylock(&lock_); }

#endif
global_mutex_type& global_critical_region::mutex() {
  static global_mutex_type global_mutex;
  return global_mutex;
}

}  // namespace xe
