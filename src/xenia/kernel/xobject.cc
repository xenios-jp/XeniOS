/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xobject.h"

#include <algorithm>
#include <chrono>
#include <optional>

#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

#include "xenia/base/cvar.h"
#include "xenia/base/threading.h"

DEFINE_bool(
    wait_timeout_backoff, false,
    "On a guest wait that keeps timing out immediately (a zero-timeout poll "
    "loop), park the thread briefly after a short fast-yield window instead of "
    "sched_yield-spinning every iteration, so the core can idle. Off = the "
    "previous unconditional MaybeYield.",
    "CPU");
DEFINE_int32(
    wait_backoff_spin_polls, 64,
    "wait_timeout_backoff: consecutive immediate timeouts to keep cheap-yielding "
    "before parking.",
    "CPU");
DEFINE_int32(wait_backoff_step_us, 25,
             "wait_timeout_backoff: park-interval growth per extra miss (us).",
             "CPU");
DEFINE_int32(
    wait_backoff_cap_us, 250,
    "wait_timeout_backoff: max park interval per poll (us); bounds added "
    "latency.",
    "CPU");
DEFINE_int32(
    wait_backoff_reset_us, 1000,
    "wait_timeout_backoff: gap since the last immediate timeout that resets the "
    "spin counter (so only tight loops are parked), microseconds.",
    "CPU");

namespace xe {
namespace kernel {

namespace {

// Adaptive backoff for the wait-timeout path (XObject::Wait / SignalAndWait /
// WaitMultiple). A guest zero-timeout poll loop otherwise costs one sched_yield
// (MaybeYield) per iteration -- the top non-JIT CPU cost on device for poll-
// heavy titles (~11-15%) and an energy/thermal drain on mobile. Keep cheap
// yields for short waits; after a sustained tight spin, park briefly so the core
// can idle. The inter-miss gap self-detects a tight loop, so a real blocking
// wait that merely times out keeps the cheap yield. Gated off by default.
thread_local uint32_t t_wait_spin_count = 0;
thread_local std::chrono::steady_clock::time_point t_wait_spin_last{};

void WaitTimeoutBackoff() {
  if (!cvars::wait_timeout_backoff) {
    xe::threading::MaybeYield();
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (t_wait_spin_last == std::chrono::steady_clock::time_point{} ||
      now - t_wait_spin_last >
          std::chrono::microseconds(cvars::wait_backoff_reset_us)) {
    t_wait_spin_count = 0;  // gap since last miss => not a tight poll loop
  }
  t_wait_spin_last = now;
  const uint32_t spin_polls =
      static_cast<uint32_t>(std::max(cvars::wait_backoff_spin_polls, 0));
  if (++t_wait_spin_count <= spin_polls) {
    xe::threading::MaybeYield();  // short wait: stay fast
    return;
  }
  const uint32_t over = t_wait_spin_count - spin_polls;
  const uint32_t step =
      static_cast<uint32_t>(std::max(cvars::wait_backoff_step_us, 0));
  const uint32_t cap =
      static_cast<uint32_t>(std::max(cvars::wait_backoff_cap_us, 0));
  uint32_t us = cap;
  if (step > 0 && over <= cap / step) {
    us = over * step;  // bounded by cap, no overflow
  }
  xe::threading::Sleep(std::chrono::microseconds(us));
}

}  // namespace

#if XE_PLATFORM_IOS
namespace {

// How often a blocked guest thread re-checks the iOS title-stop flag. A
// signaled wait object still wakes the host primitive immediately mid-slice,
// so this only bounds title-stop detection latency (not normal wait latency).
// Kept generous to minimize per-thread wakeup/clock churn while idle.
constexpr std::chrono::milliseconds kTitleStopWaitPollIntervalIOS(50);

XThread* CurrentXThreadIOS() {
  if (!XThread::IsInThread()) {
    return nullptr;
  }

  return XThread::GetCurrentThread();
}

bool IsTitleStopPollThreadIOS(XThread* thread) {
  return thread &&
         (thread->is_guest_thread() || thread->can_debugger_suspend());
}

XThread* CurrentTitleStopPollThreadIOS() {
  auto* current_thread = CurrentXThreadIOS();
  return IsTitleStopPollThreadIOS(current_thread) ? current_thread : nullptr;
}

bool ShouldPollTitleStopIOS(KernelState* kernel_state) {
  return kernel_state && CurrentTitleStopPollThreadIOS();
}

void ExitCurrentTitleStopThreadIfRequestedIOS(KernelState* kernel_state) {
  auto* current_thread = CurrentTitleStopPollThreadIOS();
  if (!current_thread || !kernel_state ||
      !kernel_state->IsTitleStopRequestedIOS()) {
    return;
  }

  current_thread->Exit(0);
}

std::chrono::milliseconds TitleStopWaitSliceIOS(
    bool infinite_timeout, std::chrono::steady_clock::time_point deadline) {
  if (infinite_timeout) {
    return kTitleStopWaitPollIntervalIOS;
  }

  auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds::zero();
  }

  auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return std::max(std::chrono::milliseconds(1),
                  std::min(kTitleStopWaitPollIntervalIOS, remaining));
}

bool TitleStopWaitTimedOutIOS(xe::threading::WaitResult result) {
  return result == xe::threading::WaitResult::kTimeout;
}

bool TitleStopWaitTimedOutIOS(
    const std::pair<xe::threading::WaitResult, size_t>& result) {
  return result.first == xe::threading::WaitResult::kTimeout;
}

template <typename WaitCall>
auto WaitWithTitleStopPollIOS(KernelState* kernel_state,
                              std::chrono::milliseconds timeout,
                              WaitCall wait_call) {
  if (!ShouldPollTitleStopIOS(kernel_state) ||
      timeout == std::chrono::milliseconds::zero()) {
    return wait_call(timeout);
  }

  const bool infinite_timeout = timeout == std::chrono::milliseconds::max();
  const auto deadline = infinite_timeout
                            ? std::chrono::steady_clock::time_point::max()
                            : std::chrono::steady_clock::now() + timeout;

  while (true) {
    ExitCurrentTitleStopThreadIfRequestedIOS(kernel_state);

    auto result = wait_call(TitleStopWaitSliceIOS(infinite_timeout, deadline));
    if (!TitleStopWaitTimedOutIOS(result)) {
      return result;
    }

    ExitCurrentTitleStopThreadIfRequestedIOS(kernel_state);
    if (!infinite_timeout && std::chrono::steady_clock::now() >= deadline) {
      return result;
    }
  }
}

}  // namespace
#endif  // XE_PLATFORM_IOS

XObject::XObject(Type type)
    : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type, bool host_object)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false),
      host_object_(host_object) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  assert_true(handles_.empty());
  assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t header_addr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(header_addr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(header_addr - kGuestObjectPrePad);
  }
}

Emulator* XObject::emulator() const { return kernel_state_->emulator_; }
KernelState* XObject::kernel_state() const { return kernel_state_; }
Memory* XObject::memory() const { return kernel_state_->memory(); }

XObject::Type XObject::type() const { return type_; }

void XObject::RetainHandle() {
#if XE_PLATFORM_IOS
  if (handles_.empty()) {
    return;
  }
#endif  // XE_PLATFORM_IOS
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
#if XE_PLATFORM_IOS
  if (handles_.empty()) {
    return false;
  }
#endif  // XE_PLATFORM_IOS
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) ==
         X_STATUS_SUCCESS;
}

void XObject::Retain() { ++pointer_ref_count_; }

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(), xe::load_and_swap<uint32_t>(
                    memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // NetDll_WSAWaitForMultipleEvents provides timeout in form of MS.
    return (uint32_t)timeout_ticks;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

namespace {
// Mirror NT-observable KTHREAD wait fields so guest code that inline-reads
// them gets live values. Returns null for non-guest host callers.
X_KTHREAD* WaitEnter(uint32_t wait_reason, uint32_t processor_mode,
                     uint32_t alertable) {
  // Waits can come from non-guest host threads (e.g. waiting on a thread object
  // during teardown), where IsInThread() avoids GetCurrentThread() asserting.
  if (!XThread::IsInThread()) {
    return nullptr;
  }
  XThread* self = XThread::GetCurrentThread();
  auto* kthread = self->guest_object<X_KTHREAD>();
  auto* context = self->thread_state()->context();
  auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  kthread->thread_state = KTHREAD_STATE_WAITING;
  kthread->wait_irql = kpcr->current_irql;
  kthread->wait_reason = static_cast<uint8_t>(wait_reason);
  kthread->processor_mode = static_cast<uint8_t>(processor_mode);
  kthread->alertable = alertable ? 1 : 0;
  return kthread;
}

void WaitExit(X_KTHREAD* kthread, X_STATUS result) {
  if (!kthread) {
    return;
  }
  kthread->thread_state = KTHREAD_STATE_RUNNING;
  kthread->wait_result = result;
}

// Drives the cooperative poll-yield loop for a fiber-backed waiter. Repeatedly
// runs |poll| (a zero-timeout acquire returning the terminal X_STATUS on
// success / abandon / failure, or std::nullopt while not yet signaled),
// yielding to the scheduler between attempts via BlockCurrentThread, until it
// resolves, an alertable user APC is pending, or |deadline_ms| (absolute host
// uptime, 0 = infinite) elapses. Polling the host primitive preserves its exact
// acquire semantics, only the blocking is made cooperative.
template <typename PollFn>
X_STATUS CooperativeWait(GuestScheduler* scheduler, X_KTHREAD* kthread,
                         bool alertable, uint64_t deadline_ms, PollFn&& poll) {
  while (true) {
    // Alertable waits return on a queued user APC (the cooperative equivalent
    // of a host alertable-wait wake), then the caller runs xeProcessUserApcs.
    if (alertable && XThread::GetCurrentThread()->HasPendingUserApc()) {
      WaitExit(kthread, X_STATUS_USER_APC);
      return X_STATUS_USER_APC;
    }
    std::optional<X_STATUS> resolved = poll();
    if (resolved) {
      WaitExit(kthread, *resolved);
      return *resolved;
    }
    if (deadline_ms != 0 && Clock::QueryHostUptimeMillis() >= deadline_ms) {
      WaitExit(kthread, X_STATUS_TIMEOUT);
      return X_STATUS_TIMEOUT;
    }
    scheduler->BlockCurrentThread();
  }
}
}  // namespace

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode,
                       uint32_t alertable, uint64_t* opt_timeout) {
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  if (GuestScheduler::enabled() && XThread::GetCurrentFiberThread()) {
    // Cooperative path: poll the host primitive (preserving its exact
    // semantics) and yield the fiber between polls instead of blocking the
    // dispatch host thread.
    auto* scheduler = kernel_state()->guest_scheduler();
    X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
    uint64_t deadline_ms = opt_timeout ? Clock::QueryHostUptimeMillis() +
                                             Clock::ScaleGuestDurationMillis(
                                                 TimeoutTicksToMs(*opt_timeout))
                                       : 0;
    return CooperativeWait(
        scheduler, kthread, alertable != 0, deadline_ms,
        [&]() -> std::optional<X_STATUS> {
          auto poll = xe::threading::Wait(wait_handle, alertable ? true : false,
                                          std::chrono::milliseconds(0));
          switch (poll) {
            case xe::threading::WaitResult::kSuccess: {
              auto current_thread = XThread::GetCurrentThread();
              if (current_thread) {
                current_thread->BoostOnWake(priority_increment());
              }
              WaitCallback();
              return X_STATUS_SUCCESS;
            }
            case xe::threading::WaitResult::kUserCallback:
              return X_STATUS_USER_APC;
            case xe::threading::WaitResult::kTimeout:
              return std::nullopt;  // not signaled yet
            default:
            case xe::threading::WaitResult::kAbandoned:
            case xe::threading::WaitResult::kFailed:
              return X_STATUS_ABANDONED_WAIT_0;
          }
        });
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
  auto result =
#if XE_PLATFORM_IOS
      WaitWithTitleStopPollIOS(
          kernel_state_, timeout_ms,
          [wait_handle, alertable](std::chrono::milliseconds wait_timeout) {
            return xe::threading::Wait(wait_handle, alertable ? true : false,
                                       wait_timeout);
          });
#else
      xe::threading::Wait(wait_handle, alertable ? true : false, timeout_ms);
#endif  // XE_PLATFORM_IOS

  switch (result) {
    case xe::threading::WaitResult::kSuccess:
    case xe::threading::WaitResult::kUserCallback: {
      if (XThread::IsInThread()) {
        XThread::GetCurrentThread()->BoostOnWake(priority_increment());
      }
      if (result == xe::threading::WaitResult::kSuccess) {
        WaitCallback();
        WaitExit(kthread, X_STATUS_SUCCESS);
        return X_STATUS_SUCCESS;
      }
      WaitExit(kthread, X_STATUS_USER_APC);
      return X_STATUS_USER_APC;
    }
    case xe::threading::WaitResult::kTimeout:
      WaitTimeoutBackoff();
      WaitExit(kthread, X_STATUS_TIMEOUT);
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      WaitExit(kthread, X_STATUS_ABANDONED_WAIT_0);
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object,
                                uint32_t wait_reason, uint32_t processor_mode,
                                uint32_t alertable, uint64_t* opt_timeout) {
  if (GuestScheduler::enabled() && XThread::GetCurrentFiberThread()) {
    // Cooperative: atomically signal + try-acquire with a zero timeout, then
    // fall into the cooperative poll loop on wait_object if it didn't acquire
    // immediately. The signal happens once, atomically, in this first call.
    auto* scheduler = wait_object->kernel_state()->guest_scheduler();
    X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
    auto first = xe::threading::SignalAndWait(
        signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
        alertable ? true : false, std::chrono::milliseconds(0));
    switch (first) {
      case xe::threading::WaitResult::kSuccess: {
        auto* current = XThread::GetCurrentThread();
        if (current) {
          current->BoostOnWake(wait_object->priority_increment());
        }
        wait_object->WaitCallback();
        WaitExit(kthread, X_STATUS_SUCCESS);
        return X_STATUS_SUCCESS;
      }
      case xe::threading::WaitResult::kUserCallback:
        WaitExit(kthread, X_STATUS_USER_APC);
        return X_STATUS_USER_APC;
      case xe::threading::WaitResult::kTimeout:
        break;  // signal done; wait_object would block -- poll cooperatively
      default:
      case xe::threading::WaitResult::kAbandoned:
      case xe::threading::WaitResult::kFailed:
        WaitExit(kthread, X_STATUS_ABANDONED_WAIT_0);
        return X_STATUS_ABANDONED_WAIT_0;
    }
    auto* wait_handle = wait_object->GetWaitHandle();
    uint64_t deadline_ms = opt_timeout ? Clock::QueryHostUptimeMillis() +
                                             Clock::ScaleGuestDurationMillis(
                                                 TimeoutTicksToMs(*opt_timeout))
                                       : 0;
    return CooperativeWait(
        scheduler, kthread, alertable != 0, deadline_ms,
        [&]() -> std::optional<X_STATUS> {
          auto poll = xe::threading::Wait(wait_handle, alertable ? true : false,
                                          std::chrono::milliseconds(0));
          switch (poll) {
            case xe::threading::WaitResult::kSuccess: {
              auto* current = XThread::GetCurrentThread();
              if (current) {
                current->BoostOnWake(wait_object->priority_increment());
              }
              wait_object->WaitCallback();
              return X_STATUS_SUCCESS;
            }
            case xe::threading::WaitResult::kUserCallback:
              return X_STATUS_USER_APC;
            case xe::threading::WaitResult::kTimeout:
              return std::nullopt;
            default:
            case xe::threading::WaitResult::kAbandoned:
            case xe::threading::WaitResult::kFailed:
              return X_STATUS_ABANDONED_WAIT_0;
          }
        });
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
  auto result = xe::threading::SignalAndWait(
      signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
      alertable ? true : false, timeout_ms);

  switch (result) {
    case xe::threading::WaitResult::kSuccess:
    case xe::threading::WaitResult::kUserCallback: {
      if (XThread::IsInThread()) {
        XThread::GetCurrentThread()->BoostOnWake(
            wait_object->priority_increment());
      }
      if (result == xe::threading::WaitResult::kSuccess) {
        wait_object->WaitCallback();
        WaitExit(kthread, X_STATUS_SUCCESS);
        return X_STATUS_SUCCESS;
      }
      WaitExit(kthread, X_STATUS_USER_APC);
      return X_STATUS_USER_APC;
    }
    case xe::threading::WaitResult::kTimeout:
      WaitTimeoutBackoff();
      WaitExit(kthread, X_STATUS_TIMEOUT);
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      WaitExit(kthread, X_STATUS_ABANDONED_WAIT_0);
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects,
                               uint32_t wait_type, uint32_t wait_reason,
                               uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  xe::threading::WaitHandle* wait_handles[64];

  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
  }
#if XE_PLATFORM_IOS
  KernelState* wait_kernel_state = count ? objects[0]->kernel_state_ : nullptr;
#endif  // XE_PLATFORM_IOS

  if (GuestScheduler::enabled() && count > 0 &&
      XThread::GetCurrentFiberThread()) {
    // Cooperative path: poll (WaitAny/WaitAll at zero timeout, preserving the
    // host primitives' atomic acquire) and yield between polls. WaitMultiple is
    // static, so reach the scheduler through an object.
    auto* scheduler = objects[0]->kernel_state()->guest_scheduler();
    X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
    uint64_t deadline_ms = opt_timeout ? Clock::QueryHostUptimeMillis() +
                                             Clock::ScaleGuestDurationMillis(
                                                 TimeoutTicksToMs(*opt_timeout))
                                       : 0;
    return CooperativeWait(
        scheduler, kthread, alertable != 0, deadline_ms,
        [&]() -> std::optional<X_STATUS> {
          if (wait_type) {
            auto r = xe::threading::WaitAny(wait_handles, count,
                                            alertable ? true : false,
                                            std::chrono::milliseconds(0));
            switch (r.first) {
              case xe::threading::WaitResult::kSuccess: {
                objects[r.second]->WaitCallback();
                auto* current = XThread::GetCurrentThread();
                if (current) {
                  current->BoostOnWake(objects[r.second]->priority_increment());
                }
                return X_STATUS(r.second);
              }
              case xe::threading::WaitResult::kUserCallback:
                return X_STATUS_USER_APC;
              case xe::threading::WaitResult::kTimeout:
                return std::nullopt;
              case xe::threading::WaitResult::kAbandoned:
                return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + r.second);
              default:
              case xe::threading::WaitResult::kFailed:
                return X_STATUS_UNSUCCESSFUL;
            }
          }
          auto r = xe::threading::WaitAll(wait_handles, count,
                                          alertable ? true : false,
                                          std::chrono::milliseconds(0));
          switch (r) {
            case xe::threading::WaitResult::kSuccess: {
              uint32_t boost_increment = 0;
              for (uint32_t i = 0; i < count; i++) {
                objects[i]->WaitCallback();
                if (objects[i]->priority_increment() > boost_increment) {
                  boost_increment = objects[i]->priority_increment();
                }
              }
              auto* current = XThread::GetCurrentThread();
              if (current) {
                current->BoostOnWake(boost_increment);
              }
              return X_STATUS_SUCCESS;
            }
            case xe::threading::WaitResult::kUserCallback:
              return X_STATUS_USER_APC;
            case xe::threading::WaitResult::kTimeout:
              return std::nullopt;
            default:
            case xe::threading::WaitResult::kAbandoned:
            case xe::threading::WaitResult::kFailed:
              return X_STATUS_ABANDONED_WAIT_0;
          }
        });
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
  X_STATUS status;
  uint32_t boost_increment = 0;
  if (wait_type) {
    auto result =
#if XE_PLATFORM_IOS
        WaitWithTitleStopPollIOS(wait_kernel_state, timeout_ms,
                                 [&](std::chrono::milliseconds wait_timeout) {
                                   return xe::threading::WaitAny(
                                       wait_handles, count,
                                       alertable ? true : false, wait_timeout);
                                 });
#else
        xe::threading::WaitAny(wait_handles, count, alertable ? true : false,
                               timeout_ms);
#endif  // XE_PLATFORM_IOS
    switch (result.first) {
      case xe::threading::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();
        boost_increment = objects[result.second]->priority_increment();
        status = X_STATUS(result.second);
        break;
      case xe::threading::WaitResult::kUserCallback:
        status = X_STATUS_USER_APC;
        break;
      case xe::threading::WaitResult::kTimeout:
        WaitTimeoutBackoff();
        status = X_STATUS_TIMEOUT;
        break;
      case xe::threading::WaitResult::kAbandoned:
        status = X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
        break;
      default:
      case xe::threading::WaitResult::kFailed:
        status = X_STATUS_UNSUCCESSFUL;
        break;
    }
  } else {
    auto result =
#if XE_PLATFORM_IOS
        WaitWithTitleStopPollIOS(wait_kernel_state, timeout_ms,
                                 [&](std::chrono::milliseconds wait_timeout) {
                                   return xe::threading::WaitAll(
                                       wait_handles, count,
                                       alertable ? true : false, wait_timeout);
                                 });
#else
        xe::threading::WaitAll(wait_handles, count, alertable ? true : false,
                               timeout_ms);
#endif  // XE_PLATFORM_IOS
    switch (result) {
      case xe::threading::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
          // Use the largest increment among the signaled objects.
          if (objects[i]->priority_increment() > boost_increment) {
            boost_increment = objects[i]->priority_increment();
          }
        }
        status = X_STATUS_SUCCESS;
        break;
      case xe::threading::WaitResult::kUserCallback:
        status = X_STATUS_USER_APC;
        break;
      case xe::threading::WaitResult::kTimeout:
        WaitTimeoutBackoff();
        status = X_STATUS_TIMEOUT;
        break;
      default:
      case xe::threading::WaitResult::kAbandoned:
      case xe::threading::WaitResult::kFailed:
        status = X_STATUS_ABANDONED_WAIT_0;
        break;
    }
  }

  // Apply priority boost if the thread actually blocked (not on
  // timeout/failure).
  if (status != X_STATUS_TIMEOUT && status != X_STATUS_UNSUCCESSFUL &&
      status != X_STATUS_ABANDONED_WAIT_0) {
    if (XThread::IsInThread()) {
      XThread::GetCurrentThread()->BoostOnWake(boost_increment);
    }
  }
  WaitExit(kthread, status);
  return status;
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  static_assert((kGuestObjectPrePad + sizeof(X_OBJECT_HEADER)) % 32 == 0);

  uint32_t total_size = kGuestObjectPrePad + sizeof(X_OBJECT_HEADER) + size;

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  uint32_t header_addr = mem + kGuestObjectPrePad;
  SetNativePointer(header_addr + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(header_addr);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header =
      kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list.blink_ptr & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state,
                                             void* native_ptr, int32_t as_type,
                                             bool already_locked) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list.flink_ptr to a magic value. When set,
  // wait_list.blink_ptr will hold a handle to our object.
  if (!already_locked) {
    global_critical_region::mutex().lock();
  }

  XObject* result;

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list.flink_ptr == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list.blink_ptr;
    result = kernel_state->object_table()
                 ->LookupObject<XObject>(handle, true)
                 .release();
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        // Unimplemented object type - just log and return nullptr
        XELOGW("GetNativeObject: Unimplemented object type {}", as_type);
        result = nullptr;
    }
    // InitializeNative paths call SetNativePointer, which stashes the handle.
    // New object types (when implemented) must do the same.
    result = object;
  }

  if (!already_locked) {
    global_critical_region::mutex().unlock();
  }
  return object_ref<XObject>(result);
}

}  // namespace kernel
}  // namespace xe
