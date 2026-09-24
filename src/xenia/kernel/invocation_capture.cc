/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/invocation_capture.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/jit_corpus.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_rtl.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DEFINE_uint32(jit_corpus_capture_after, 0,
              "With jit_corpus_out, this many seconds after starting find the "
              "guest functions using the most host CPU and record real "
              "invocations of them into the corpus, for xenia-cpu-replay to "
              "run. The title stalls briefly for each.",
              "CPU");

namespace xe {
namespace kernel {

std::atomic<cpu::ppc::PPCContext*> capturing_context{nullptr};

namespace {

using cpu::JitCorpus;
using cpu::ppc::PPCContext;

constexpr uint64_t kSampleMs = 10000;
constexpr size_t kFunctions = 32;
constexpr uint32_t kInvocationsPerFunction = 8;
// Between two invocations of a function, so they see different inputs.
constexpr uint64_t kSpacingMs = 50;
constexpr uint64_t kCaptureMs = 60000;
constexpr auto kFreezeTimeout = std::chrono::milliseconds(20);
// Longer than any invocation not waiting on a thread that cannot run.
constexpr uint64_t kBudgetMs = 100;
// For recording memory, the first time most of it.
constexpr uint64_t kSnapshotBudgetMs = 10000;

// The memory effects of the uncontended paths in xboxkrnl_threading.cc and
// xboxkrnl_rtl.cc. A contended one would have blocked, which drops the
// invocation, as nothing else runs meanwhile.
void SetIrql(PPCContext* context, uint64_t irql) {
  context->TranslateVirtualGPR<X_KPCR*>(context->r[13])->current_irql =
      uint8_t(irql);
}
void Lock(PPCContext* context, uint32_t, uint64_t) {
  context->TranslateVirtualGPR<X_KSPINLOCK*>(context->r[3])->prcb_of_owner =
      uint32_t(context->r[13]);
}
void Unlock(PPCContext* context, uint32_t, uint64_t) {
  context->TranslateVirtualGPR<X_KSPINLOCK*>(context->r[3])->prcb_of_owner = 0;
}
void EnterCriticalSection(PPCContext* context, uint32_t thread,
                          uint64_t result) {
  if (!context->r[3] || !result) {
    return;
  }
  auto cs = context->TranslateVirtualGPR<xboxkrnl::X_RTL_CRITICAL_SECTION*>(
      context->r[3]);
  if (cs->owning_thread == thread) {
    ++cs->lock_count;
    cs->recursion_count = cs->recursion_count + 1;
  } else {
    cs->lock_count = 0;
    cs->owning_thread = thread;
    cs->recursion_count = 1;
  }
}
void LeaveCriticalSection(PPCContext* context, uint32_t, uint64_t) {
  if (!context->r[3]) {
    return;
  }
  auto cs = context->TranslateVirtualGPR<xboxkrnl::X_RTL_CRITICAL_SECTION*>(
      context->r[3]);
  cs->recursion_count = cs->recursion_count - 1;
  if (!cs->recursion_count) {
    cs->owning_thread = 0;
  }
  --cs->lock_count;
}
const std::pair<std::string_view, ExportModel> kExportModels[] = {
    {"KeRaiseIrqlToDpcLevel",
     [](PPCContext* c, uint32_t, uint64_t) { SetIrql(c, 2); }},
    {"KfRaiseIrql",
     [](PPCContext* c, uint32_t, uint64_t) { SetIrql(c, c->r[3]); }},
    {"KfLowerIrql",
     [](PPCContext* c, uint32_t, uint64_t) { SetIrql(c, c->r[3]); }},
    {"KfAcquireSpinLock",
     [](PPCContext* c, uint32_t thread, uint64_t result) {
       SetIrql(c, 2);
       Lock(c, thread, result);
     }},
    {"KfReleaseSpinLock",
     [](PPCContext* c, uint32_t thread, uint64_t result) {
       Unlock(c, thread, result);
       if (uint32_t(c->r[4]) < 2) {
         SetIrql(c, c->r[4]);
       }
     }},
    {"KeAcquireSpinLockAtRaisedIrql", Lock},
    {"KeTryToAcquireSpinLockAtRaisedIrql",
     [](PPCContext* c, uint32_t thread, uint64_t result) {
       if (result) {
         Lock(c, thread, result);
       }
     }},
    {"KeReleaseSpinLockFromRaisedIrql", Unlock},
    // Always returns, so any result means it entered.
    {"RtlEnterCriticalSection",
     [](PPCContext* c, uint32_t thread, uint64_t) {
       EnterCriticalSection(c, thread, 1);
     }},
    {"RtlTryEnterCriticalSection", EnterCriticalSection},
    {"RtlLeaveCriticalSection", LeaveCriticalSection},
};

class Session {
 public:
  explicit Session(KernelState* kernel_state)
      : kernel_state_(kernel_state),
        writer_(kernel_state->processor()->jit_corpus_writer()) {
    thread_ = threading::Thread::Create({}, [this] { Run(); });
    thread_->set_name("Invocation Capture");
  }
  // Unhooks the functions once its thread ends.
  void Stop() {
    stop_ = true;
    threading::Wait(thread_.get(), false);
  }

  uint64_t OnEntry(PPCContext* context, cpu::GuestFunction* function,
                   uint32_t return_address);
  void OnExport(PPCContext* context, const cpu::Export* export_entry,
                bool returned);

 private:
  struct Target {
    uint32_t captured = 0;
    uint32_t attempts = 0;
    uint64_t next_ms = 0;
    // Done, or dropped often enough to give up on.
    bool done() const {
      return captured >= kInvocationsPerFunction ||
             attempts >= 2 * kInvocationsPerFunction;
    }
  };

  void Run();
  // Sleeps, meanwhile dropping an invocation past its budget. False once
  // stopping.
  bool Wait(uint64_t duration_ms);
  // Samples which guest functions the guest CPUs are in, hottest first.
  std::vector<std::pair<cpu::GuestFunction*, uint32_t>> Profile();
  // Every committed guest page, keeping a copy of the contents not stored
  // yet, which are written once the invocation is kept.
  void Snapshot(JitCorpus::PageList* pages);
  void Discard() {
    discarded_ = true;
    kernel_state_->guest_scheduler()->Thaw();
  }

  KernelState* kernel_state_;
  cpu::JitCorpusWriter* writer_;
  std::unique_ptr<threading::Thread> thread_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::unordered_map<cpu::GuestFunction*, Target> targets_;
  // Hashes of the pages stored, the contents to store with the invocation
  // being captured, and the entry memory of the last invocation.
  std::unordered_set<uint64_t> stored_;
  std::unordered_map<uint64_t, std::vector<uint8_t>> pending_;
  JitCorpus::PageList previous_;
  // The invocation being captured, and when to give up on it.
  JitCorpus::Invocation invocation_;
  std::atomic<bool> discarded_{false};
  std::atomic<uint64_t> deadline_ms_{0};
  // Why others were dropped, unmodeled_ under mutex_.
  std::atomic<uint32_t> not_frozen_{0};
  std::atomic<uint32_t> blocked_{0};
  std::map<std::string, uint32_t> unmodeled_;
};

std::atomic<Session*> session_{nullptr};
// Entry hooks running, which may use the session until they return.
std::atomic<uint32_t> hooks_running_{0};

uint64_t EntryHook(void* raw_context, uint64_t function,
                   uint64_t return_address) {
  hooks_running_.fetch_add(1);
  Session* session = session_.load();
  const uint64_t result =
      session
          ? session->OnEntry(static_cast<PPCContext*>(raw_context),
                             reinterpret_cast<cpu::GuestFunction*>(function),
                             uint32_t(return_address))
          : 0;
  hooks_running_.fetch_sub(1);
  return result;
}

bool Session::Wait(uint64_t duration_ms) {
  const uint64_t end = Clock::QueryHostUptimeMillis() + duration_ms;
  while (!stop_ && Clock::QueryHostUptimeMillis() < end) {
    threading::Sleep(std::chrono::milliseconds(5));
    // Past it, the capture waits on a thread that cannot run. Unless the
    // invocation moved on meanwhile, which would discard the next one.
    uint64_t deadline = deadline_ms_;
    if (deadline && Clock::QueryHostUptimeMillis() > deadline &&
        deadline_ms_.compare_exchange_strong(deadline, 0)) {
      ++blocked_;
      Discard();
    }
  }
  return !stop_;
}

std::vector<std::pair<cpu::GuestFunction*, uint32_t>> Session::Profile() {
  std::vector<uint64_t> pcs;
  const uint64_t end = Clock::QueryHostUptimeMillis() + kSampleMs;
  while (!stop_ && Clock::QueryHostUptimeMillis() < end) {
    kernel_state_->guest_scheduler()->SampleProgramCounters(&pcs);
    threading::Sleep(std::chrono::milliseconds(1));
  }
  std::unordered_map<cpu::GuestFunction*, uint32_t> weights;
  {
    // Placing code grows the map looked up.
    auto global_lock = global_critical_region::AcquireDirect();
    auto code_cache = kernel_state_->processor()->backend()->code_cache();
    for (uint64_t pc : pcs) {
      if (auto function = pc ? code_cache->LookupFunction(pc) : nullptr) {
        ++weights[function];
      }
    }
  }
  std::vector<std::pair<cpu::GuestFunction*, uint32_t>> ranked(weights.begin(),
                                                               weights.end());
  std::sort(ranked.begin(), ranked.end(),
            [](auto& a, auto& b) { return a.second > b.second; });
  std::vector<std::pair<uint32_t, uint32_t>> profile;
  for (auto [function, samples] : ranked) {
    profile.emplace_back(function->address(), samples);
  }
  writer_->WriteProfile(pcs.size(), profile);
  return ranked;
}

void Session::Run() {
  if (!Wait(uint64_t(cvars::jit_corpus_capture_after) * 1000)) {
    return;
  }
  auto ranked = Profile();
  auto backend = kernel_state_->processor()->backend();
  uint64_t samples = 0, hooked_samples = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < ranked.size(); ++i) {
      samples += ranked[i].second;
      if (i < kFunctions &&
          backend->HookFunctionEntry(ranked[i].first, EntryHook)) {
        targets_.emplace(ranked[i].first, Target());
        hooked_samples += ranked[i].second;
      }
    }
  }
  if (targets_.empty()) {
    XELOGW("Invocation capture: nothing captured, {}",
           samples ? "the " + backend->name() + " backend cannot hook functions"
                   : std::string("no guest code was sampled, sampling threads "
                                 "is implemented on Windows and Apple hosts"));
    return;
  }
  XELOGI(
      "Invocation capture: capturing {} functions, {} of the {} samples in "
      "guest code",
      targets_.size(), hooked_samples, samples);
  const uint64_t end = Clock::QueryHostUptimeMillis() + kCaptureMs;
  while (Clock::QueryHostUptimeMillis() < end && Wait(20)) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::all_of(targets_.begin(), targets_.end(),
                    [](auto& target) { return target.second.done(); })) {
      break;
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t captured = 0;
  for (auto& [function, target] : targets_) {
    backend->UnhookFunctionEntry(function);
    captured += target.captured;
    target.captured = kInvocationsPerFunction;
  }
  std::string unmodeled;
  for (const auto& [name, count] : unmodeled_) {
    unmodeled += fmt::format(" {} {}", name, count);
  }
  XELOGI(
      "Invocation capture: recorded {} invocations, dropped {} for not "
      "stopping the other threads, {} for blocking or taking too long, and "
      "these for calling exports the replay cannot reproduce:{}",
      captured, not_frozen_.load(), blocked_.load(), unmodeled);
}

void Session::Snapshot(JitCorpus::PageList* pages) {
  Memory* memory = kernel_state_->memory();
  auto add = [&](uint32_t address, const uint8_t* data) {
    uint64_t hash = JitCorpus::HashMemoryPage(data);
    if (!stored_.count(hash) && !pending_.count(hash)) {
      // The GPU may be writing it, so keep exactly what gets hashed.
      std::vector<uint8_t> copy(data, data + JitCorpus::kMemoryPageSize);
      hash = JitCorpus::HashMemoryPage(copy.data());
      if (!stored_.count(hash)) {
        pending_.emplace(hash, std::move(copy));
      }
    }
    pages->emplace_back(address, hash);
  };
  auto walk = [&](BaseHeap* heap, uint32_t tag, auto translate) {
    const uint64_t end = uint64_t(heap->heap_base()) + heap->heap_size();
    for (uint64_t base = heap->heap_base(); base < end;) {
      HeapAllocationInfo info;
      if (!heap->QueryRegionInfo(uint32_t(base), &info) || !info.region_size) {
        break;
      }
      if ((info.state & kMemoryAllocationCommit) &&
          (info.protect & kMemoryProtectRead)) {
        for (uint32_t offset = 0; offset < info.region_size;
             offset += JitCorpus::kMemoryPageSize) {
          const uint32_t address = uint32_t(base) + offset;
          add(tag | address, translate(address));
        }
      }
      base += info.region_size;
    }
  };
  pages->clear();
  for (uint32_t base : {0x00000000u, 0x40000000u, 0x80000000u, 0x90000000u}) {
    if (BaseHeap* heap = memory->LookupHeap(base)) {
      walk(heap, 0, [memory](uint32_t address) {
        return memory->TranslateVirtual(address);
      });
    }
  }
  // Loading a module resets its heap's page table (XexModule::ReadImage), so
  // module images are taken from the modules.
  for (cpu::Module* module : kernel_state_->processor()->GetModules()) {
    auto xex = dynamic_cast<cpu::XexModule*>(module);
    if (xex && xex->loaded() && !xex->is_patch()) {
      for (uint32_t offset = 0; offset < xex->image_size();
           offset += JitCorpus::kMemoryPageSize) {
        const uint32_t address = xex->base_address() + offset;
        add(address, memory->TranslateVirtual(address));
      }
    }
  }
  // Physical memory once, rather than through each view of it.
  walk(memory->GetPhysicalHeap(), JitCorpus::kPhysicalMemory,
       [memory](uint32_t address) {
         return memory->TranslatePhysical(address);
       });
  std::stable_sort(pages->begin(), pages->end(),
                   [](auto& a, auto& b) { return a.first < b.first; });
  pages->erase(std::unique(pages->begin(), pages->end(),
                           [](auto& a, auto& b) { return a.first == b.first; }),
               pages->end());
}

uint64_t Session::OnEntry(PPCContext* context, cpu::GuestFunction* function,
                          uint32_t return_address) {
  if (capturing_context.load()) {
    return 0;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = targets_.find(function);
    if (it == targets_.end() || it->second.done() ||
        Clock::QueryHostUptimeMillis() < it->second.next_ms) {
      return 0;
    }
  }
  PPCContext* expected = nullptr;
  if (!capturing_context.compare_exchange_strong(expected, context)) {
    return 0;
  }
  auto scheduler = kernel_state_->guest_scheduler();
  bool ran = false, captured = false;
  if (scheduler->FreezeOthers(kFreezeTimeout)) {
    discarded_ = false;
    deadline_ms_ = Clock::QueryHostUptimeMillis() + kSnapshotBudgetMs;
    JitCorpus::PageList entry, exit;
    Snapshot(&entry);
    invocation_ = {function->address(), return_address,
                   XThread::GetCurrentThread()->guest_object()};
    auto registers = reinterpret_cast<const uint8_t*>(context);
    invocation_.entry_registers.assign(registers,
                                       registers + JitCorpus::kRegistersSize);
    deadline_ms_ = Clock::QueryHostUptimeMillis() + kBudgetMs;
    function->Call(context->thread_state, return_address);
    deadline_ms_ = Clock::QueryHostUptimeMillis() + kSnapshotBudgetMs;
    ran = true;
    if (!discarded_) {
      Snapshot(&exit);
      invocation_.exit_registers.assign(registers,
                                        registers + JitCorpus::kRegistersSize);
    }
    // False if it blocked or yielded, letting other threads run.
    deadline_ms_ = 0;
    const bool frozen = scheduler->Thaw();
    if (!discarded_ && frozen) {
      for (const auto& [hash, data] : pending_) {
        writer_->WriteMemoryPage(hash, data.data());
        stored_.insert(hash);
      }
      invocation_.memory = JitCorpus::DiffPages(previous_, entry);
      invocation_.writes = JitCorpus::DiffPages(entry, exit);
      writer_->WriteInvocation(invocation_);
      previous_ = std::move(entry);
      captured = true;
    }
    pending_.clear();
    blocked_ += !discarded_ && !frozen;
  } else {
    ++not_frozen_;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Target& target = targets_[function];
    target.captured += captured;
    target.attempts += ran;
    target.next_ms = Clock::QueryHostUptimeMillis() + kSpacingMs;
  }
  capturing_context = nullptr;
  return ran;
}

void Session::OnExport(PPCContext* context, const cpu::Export* export_entry,
                       bool returned) {
  if (discarded_) {
    return;
  }
  if (returned) {
    if (!invocation_.exports.empty()) {
      invocation_.exports.back().result = context->r[3];
    }
  } else if (FindExportModel(export_entry->name)) {
    invocation_.exports.push_back(
        {uint32_t(context->lr), export_entry->name, 0});
  } else {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++unmodeled_[export_entry->name];
    }
    Discard();
  }
}

}  // namespace

void CaptureExportCall(PPCContext* context, const cpu::Export* export_entry,
                       bool returned) {
  // Only on capturing_context, so within the entry hook.
  if (Session* session = session_.load()) {
    session->OnExport(context, export_entry, returned);
  }
}

ExportModel FindExportModel(std::string_view name) {
  for (const auto& [model_name, model] : kExportModels) {
    if (model_name == name) {
      return model;
    }
  }
  return nullptr;
}

void StartInvocationCapture(KernelState* kernel_state) {
  if (cvars::jit_corpus_capture_after &&
      kernel_state->processor()->jit_corpus_writer() && !session_.load()) {
    session_ = new Session(kernel_state);
  }
}

void StopInvocationCapture() {
  Session* session = session_.exchange(nullptr);
  if (!session) {
    return;
  }
  session->Stop();
  // A hook still running, such as one whose fiber blocked and never resumes,
  // keeps the session.
  const uint64_t end = Clock::QueryHostUptimeMillis() + 1000;
  while (hooks_running_.load()) {
    if (Clock::QueryHostUptimeMillis() >= end) {
      XELOGW("Invocation capture: an entry hook is still running, leaking it");
      return;
    }
    threading::Sleep(std::chrono::milliseconds(1));
  }
  delete session;
}

}  // namespace kernel
}  // namespace xe
