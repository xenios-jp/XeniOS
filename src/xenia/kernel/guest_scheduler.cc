/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_scheduler.h"

#include <string>

#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

DECLARE_bool(ignore_thread_affinities);

namespace xe {
namespace kernel {

// Logical CPU index of the host thread currently executing, or -1 on any
// non-dispatch thread. Set by each CPU's RunLoop.
static thread_local int t_current_cpu = -1;

GuestScheduler::GuestScheduler(KernelState* kernel_state)
    : kernel_state_(kernel_state) {
  int n = static_cast<int>(cvars::guest_scheduler_cpus);
  host_cpu_count_ = n < 1 ? 1 : (n > kMaxCpus ? kMaxCpus : n);
}

GuestScheduler::~GuestScheduler() { Shutdown(); }

bool GuestScheduler::enabled() { return cvars::guest_scheduler; }

int GuestScheduler::CpuOf(XThread* thread) const {
  uint8_t guest_cpu = thread->guest_object<X_KTHREAD>()->current_cpu;
  if (guest_cpu >= kMaxCpus) {
    guest_cpu = 0;
  }
  // Map the guest CPU onto the active dispatch threads. With 6 it is 1:1, with
  // 3 each physical core's SMT pair shares a thread, with 1 all share thread 0.
  return guest_cpu * host_cpu_count_ / kMaxCpus;
}

void GuestScheduler::EnsureStarted() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  for (int i = 0; i < host_cpu_count_; ++i) {
    cpus_[i].ready_event = xe::threading::Event::CreateAutoResetEvent(false);
  }
  // Pin each dispatch thread to its own host logical processor so the host
  // scheduler keeps them genuinely parallel instead of migrating them or
  // stacking several on one core. Gated on the affinity cvar, and only when
  // there are enough host cores to spare.
  bool pin = !cvars::ignore_thread_affinities &&
             xe::threading::logical_processor_count() >=
                 static_cast<uint32_t>(host_cpu_count_);
  for (int i = 0; i < host_cpu_count_; ++i) {
    xe::threading::Thread::CreationParameters params;
    cpus_[i].host_thread =
        xe::threading::Thread::Create(params, [this, i]() { RunLoop(i); });
    cpus_[i].host_thread->set_name(std::string("Guest CPU ") +
                                   std::to_string(i));
    if (pin) {
      cpus_[i].host_thread->set_affinity_mask(uint64_t(1) << i);
    }
  }
}

void GuestScheduler::Shutdown() {
  if (!started_.load()) {
    return;
  }
  shutting_down_.store(true);
  for (Cpu& cpu : cpus_) {
    if (cpu.ready_event) {
      cpu.ready_event->Set();
    }
  }
  for (Cpu& cpu : cpus_) {
    if (cpu.host_thread) {
      // Join before reset(): reset() only closes the handle.
      xe::threading::Wait(cpu.host_thread.get(), false);
      cpu.host_thread.reset();
    }
  }
}

void GuestScheduler::EnqueueReady(XThread* thread, int cpu_index) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = thread->scheduler_links();
    // Blocked threads move via RereadyBlocked, not here, since ready and
    // blocked share ready_next and a thread must be in only one list.
    assert_false(links.blocked);
    if (links.queued) {
      return;
    }
    links.queued = true;
    links.ready_next = nullptr;
    Cpu& cpu = cpus_[cpu_index];
    if (cpu.ready_tail) {
      cpu.ready_tail->scheduler_links().ready_next = thread;
    } else {
      cpu.ready_head = thread;
    }
    cpu.ready_tail = thread;
  }
  if (cpus_[cpu_index].ready_event) {
    cpus_[cpu_index].ready_event->Set();
  }
}

void GuestScheduler::MarkReady(XThread* thread) {
  assert_not_null(thread);
  // Don't re-enqueue a terminated thread, or a stray Resume could revive a
  // zombie.
  if (thread->guest_object<X_KTHREAD>()->thread_state ==
      KTHREAD_STATE_TERMINATED) {
    return;
  }
  // Affinity placement: only safe because the caller guarantees the thread is
  // not currently running (a new thread, or one woken from a wait/suspend).
  EnqueueReady(thread, CpuOf(thread));
}

XThread* GuestScheduler::DequeueReady(int cpu_index) {
  std::lock_guard<std::mutex> lock(lock_);
  Cpu& cpu = cpus_[cpu_index];
  XThread* thread = cpu.ready_head;
  if (!thread) {
    return nullptr;
  }
  auto& links = thread->scheduler_links();
  cpu.ready_head = links.ready_next;
  if (!cpu.ready_head) {
    cpu.ready_tail = nullptr;
  }
  links.ready_next = nullptr;
  links.queued = false;
  return thread;
}

void GuestScheduler::UnlinkLocked(XThread*& head, XThread*& tail,
                                  XThread* thread) {
  XThread** link = &head;
  XThread* prev = nullptr;
  while (*link) {
    if (*link == thread) {
      *link = thread->scheduler_links().ready_next;
      if (tail == thread) {
        tail = prev;
      }
      return;
    }
    prev = *link;
    link = &(*link)->scheduler_links().ready_next;
  }
}

void GuestScheduler::ForgetThread(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  auto& links = thread->scheduler_links();
  // The thread is in at most one CPU's list, but we don't track which, so scan
  // all of them. Rare path (external Terminate of a queued/blocked thread).
  if (links.queued) {
    for (Cpu& cpu : cpus_) {
      UnlinkLocked(cpu.ready_head, cpu.ready_tail, thread);
    }
    links.queued = false;
  } else if (links.blocked) {
    for (Cpu& cpu : cpus_) {
      UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
    }
    links.blocked = false;
  }
  links.ready_next = nullptr;
}

void GuestScheduler::SwitchTo(XThread* next) {
  assert_not_null(next);
  assert_not_null(next->fiber());
  auto& links = next->scheduler_links();
  if (!links.has_run) {
    links.has_run = true;
    XELOGI("GuestScheduler: first run tid={:08X} '{}'", next->thread_id(),
           next->thread_name());
  }
  XThread::SetCurrentThread(next);
  next->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_RUNNING;
  next->fiber()->SwitchTo();
  // Back on the idle fiber.
  XThread::SetCurrentThread(nullptr);
}

void GuestScheduler::YieldToScheduler() {
  assert_true(t_current_cpu >= 0);
  cpus_[t_current_cpu].idle_fiber->SwitchTo();
}

void GuestScheduler::YieldCurrentThread() {
  assert_true(t_current_cpu >= 0);
  XThread* self = XThread::GetCurrentThread();
  // Re-queue on the current CPU, not the affinity CPU: the running fiber's
  // context isn't saved until it yields below, so another CPU must not be able
  // to grab it yet.
  EnqueueReady(self, t_current_cpu);
  YieldToScheduler();
}

void GuestScheduler::NotifyThreadExited(XThread* thread) {
  assert_true(t_current_cpu >= 0);
  XELOGI("GuestScheduler: exited tid={:08X} '{}'", thread->thread_id(),
         thread->thread_name());
  // This CPU's dispatch loop reclaims it, since we can't drop the last handle
  // while running on its fiber.
  cpus_[t_current_cpu].exited_thread = thread;
}

void GuestScheduler::BlockCurrentThread() {
  assert_true(t_current_cpu >= 0);
  XThread* self = XThread::GetCurrentThread();
  int cpu_index = t_current_cpu;
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = self->scheduler_links();
    // Park self (running, in no list) on this CPU's blocked list.
    links.blocked = true;
    links.ready_next = nullptr;
    Cpu& cpu = cpus_[cpu_index];
    if (cpu.blocked_tail) {
      cpu.blocked_tail->scheduler_links().ready_next = self;
    } else {
      cpu.blocked_head = self;
    }
    cpu.blocked_tail = self;
  }
  self->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_WAITING;
  YieldToScheduler();
}

void GuestScheduler::RereadyBlocked(int cpu_index) {
  std::lock_guard<std::mutex> lock(lock_);
  Cpu& cpu = cpus_[cpu_index];
  XThread* t = cpu.blocked_head;
  while (t) {
    auto& links = t->scheduler_links();
    XThread* next = links.ready_next;
    links.blocked = false;
    links.queued = true;
    links.ready_next = nullptr;
    if (cpu.ready_tail) {
      cpu.ready_tail->scheduler_links().ready_next = t;
    } else {
      cpu.ready_head = t;
    }
    cpu.ready_tail = t;
    t = next;
  }
  cpu.blocked_head = nullptr;
  cpu.blocked_tail = nullptr;
}

void GuestScheduler::RunLoop(int cpu_index) {
  t_current_cpu = cpu_index;
  Cpu& cpu = cpus_[cpu_index];
  // Adopt this host thread's stack as this CPU's idle fiber.
  cpu.idle_fiber = xe::threading::Fiber::CreateFromThread();
  XELOGI("GuestScheduler: CPU {} dispatch loop started", cpu_index);

  uint64_t next_repoll_ms = 0;
  while (!shutting_down_.load()) {
    // Re-poll blocked waiters on a timer even while other fibers run, or a busy
    // fiber that rarely waits would starve them.
    uint64_t now = Clock::QueryHostUptimeMillis();
    if (now >= next_repoll_ms) {
      RereadyBlocked(cpu_index);
      next_repoll_ms = now + kPollBackoffMs;
    }

    XThread* next = DequeueReady(cpu_index);
    if (next) {
      cpu.exited_thread = nullptr;
      SwitchTo(next);
      if (cpu.exited_thread) {
        // Safe to drop the last handle now, which may free the XThread and its
        // fiber. We're on the idle fiber and the exited fiber is parked on its
        // final yield, so we never free the running fiber.
        XThread* dead = cpu.exited_thread;
        cpu.exited_thread = nullptr;
        dead->ReleaseHandle();
      }
      continue;
    }

    // Nothing ready, so sleep until the next re-poll if waiters are blocked (a
    // MarkReady wakes us sooner), otherwise idle until something is runnable.
    bool have_blocked;
    {
      std::lock_guard<std::mutex> lock(lock_);
      have_blocked = cpu.blocked_head != nullptr;
    }
    if (!have_blocked) {
      xe::threading::Wait(cpu.ready_event.get(), false);
      continue;
    }
    now = Clock::QueryHostUptimeMillis();
    uint64_t sleep_ms = next_repoll_ms > now ? next_repoll_ms - now : 0;
    xe::threading::Wait(cpu.ready_event.get(), false,
                        std::chrono::milliseconds(sleep_ms));
  }
  XELOGI("GuestScheduler: CPU {} dispatch loop exited (shutting_down={})",
         cpu_index, shutting_down_.load());
}

}  // namespace kernel
}  // namespace xe
