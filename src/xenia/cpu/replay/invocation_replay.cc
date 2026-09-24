/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/replay/invocation_replay.h"

#include <algorithm>
#include <cstring>

#include "fmt/format.h"
#include "xenia/base/clock.h"
#include "xenia/base/math.h"
#include "xenia/cpu/function.h"
#include "xenia/kernel/invocation_capture.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

namespace {

InvocationReplay* current_ = nullptr;

// The hash of a page in a sorted list, 0 if absent.
uint64_t Find(const JitCorpus::PageList& pages, uint32_t address) {
  auto it = std::lower_bound(
      pages.begin(), pages.end(), address,
      [](const auto& page, uint32_t value) { return page.first < value; });
  return it != pages.end() && it->first == address ? it->second : 0;
}

}  // namespace

InvocationReplay::InvocationReplay(Processor* processor,
                                   const JitCorpus& corpus)
    : processor_(processor), memory_(processor->memory()), corpus_(corpus) {
  current_ = this;
}

InvocationReplay::~InvocationReplay() { current_ = nullptr; }

bool InvocationReplay::HandleException(Exception* ex, void* data) {
  InvocationReplay* self = current_;
  if (!self || !self->watching_ ||
      ex->code() != Exception::Code::kAccessViolation) {
    return false;
  }
  // Anywhere else, such as the MMIO window, is for the other handlers.
  const uint64_t address = ex->fault_address();
  for (const auto& [base, size] : self->views_) {
    if (address - uint64_t(base) < size) {
      const size_t page =
          size_t(address - uint64_t(self->memory_->virtual_membase())) /
          self->host_page_size_;
      self->touched_[page] = 1;
      return xe::memory::Protect(
          self->memory_->virtual_membase() + page * self->host_page_size_,
          self->host_page_size_, xe::memory::PageAccess::kReadWrite);
    }
  }
  return false;
}

bool InvocationReplay::Initialize() {
  // Everything the capture could have had committed, which leaves the MMIO
  // window faulting into its handler.
  host_page_size_ = xe::memory::page_size();
  for (uint32_t base : {0x00000000u, 0x40000000u, 0x7F000000u, 0x80000000u,
                        0x90000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    BaseHeap* heap = memory_->LookupHeap(base);
    const size_t offset = heap->host_address_offset();
    views_.emplace_back(
        heap->TranslateRelative(0) - offset,
        xe::round_up(heap->heap_size() + offset, host_page_size_));
  }
  touched_.resize((uint64_t(1) << 32) / host_page_size_ + 1);
  thread_state_ = std::make_unique<ThreadState>(processor_, 0x100);
  return SetAccess(xe::memory::PageAccess::kReadWrite);
}

bool InvocationReplay::SetAccess(xe::memory::PageAccess access) {
  for (const auto& [base, size] : views_) {
    if (!xe::memory::Protect(base, size, access)) {
      return false;
    }
  }
  return true;
}

uint8_t* InvocationReplay::HostPage(uint32_t address) const {
  return address >= JitCorpus::kPhysicalMemory
             ? memory_->TranslatePhysical(address - JitCorpus::kPhysicalMemory)
             : memory_->TranslateVirtual(address);
}

void InvocationReplay::WritePage(uint32_t address, uint64_t hash) {
  auto it = corpus_.memory_pages.find(hash);
  if (it != corpus_.memory_pages.end()) {
    std::memcpy(HostPage(address), it->second, JitCorpus::kMemoryPageSize);
  }
}

std::vector<uint32_t> InvocationReplay::TouchedPages() const {
  std::vector<uint32_t> pages;
  for (size_t page = 0; page < touched_.size(); ++page) {
    if (!touched_[page]) {
      continue;
    }
    const uint8_t* host = memory_->virtual_membase() + page * host_page_size_;
    for (size_t offset = 0; offset < host_page_size_;
         offset += JitCorpus::kMemoryPageSize) {
      const uint32_t address = memory_->HostToGuestVirtual(host + offset);
      BaseHeap* heap = memory_->LookupHeap(address);
      if (heap && heap->heap_type() == HeapType::kGuestPhysical) {
        pages.push_back(JitCorpus::kPhysicalMemory |
                        memory_->GetPhysicalAddress(address));
      } else {
        pages.push_back(address);
      }
      pages.back() &= ~(JitCorpus::kMemoryPageSize - 1);
    }
  }
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  return pages;
}

Function* InvocationReplay::Run(const JitCorpus::Invocation& invocation,
                                bool watch) {
  invocation_ = &invocation;
  next_export_ = 0;
  diverged_ = false;
  std::memcpy(thread_state_->context(), invocation.entry_registers.data(),
              JitCorpus::kRegistersSize);
  // Compiled first, as the compiler reads guest memory too.
  Function* function = processor_->ResolveFunction(invocation.function);
  if (!function) {
    return nullptr;
  }
  if (watch) {
    std::fill(touched_.begin(), touched_.end(), uint8_t(0));
    watching_ = SetAccess(xe::memory::PageAccess::kNoAccess);
  }
  function->Call(thread_state_.get(), invocation.return_address);
  if (watch) {
    watching_ = false;
    SetAccess(xe::memory::PageAccess::kReadWrite);
  }
  return function;
}

std::string InvocationReplay::Check(const JitCorpus::Invocation& invocation,
                                    JitCorpus::Invocation* checked,
                                    uint32_t* others, uint32_t* unrecorded) {
  // Guest memory takes the entry state of every page it may touch.
  if (invocation.checked) {
    for (const auto& [address, hash] : invocation.memory) {
      WritePage(address, hash);
    }
  } else {
    entry_ = JitCorpus::ApplyPages(entry_, invocation.memory);
    for (const auto& [address, hash] : JitCorpus::DiffPages(loaded_, entry_)) {
      WritePage(address, hash);
    }
    loaded_ = entry_;
  }
  if (!Run(invocation, true)) {
    return "does not compile";
  }
  const JitCorpus::PageList& entry =
      invocation.checked ? invocation.memory : entry_;
  *checked = {invocation.function,
              invocation.return_address,
              invocation.thread,
              true,
              invocation.entry_registers,
              invocation.exit_registers,
              invocation.exports,
              {},
              {}};
  std::string mismatch;
  for (uint32_t address : TouchedPages()) {
    const uint64_t before = Find(entry, address);
    if (!before) {
      // A page the capture did not see it read, such as one its code folded
      // constants from and code built differently loads.
      if (invocation.checked) {
        ++*unrecorded;
      } else {
        mismatch =
            fmt::format("touches page {:08X}, which was not recorded", address);
      }
      continue;
    }
    const uint64_t after = JitCorpus::HashMemoryPage(HostPage(address));
    const uint64_t written = Find(invocation.writes, address);
    const uint64_t expected = written ? written : before;
    checked->memory.emplace_back(address, before);
    if (after != before) {
      checked->writes.emplace_back(address, after);
      if (!invocation.checked) {
        std::lower_bound(loaded_.begin(), loaded_.end(),
                         std::make_pair(address, uint64_t(0)))
            ->second = after;
      }
    }
    if (after == expected) {
      continue;
    }
    if (invocation.checked || after != before) {
      mismatch = fmt::format("memory page {:08X} differs", address);
    } else {
      ++*others;
    }
  }
  // A checked run must also make every change the first one made.
  for (const auto& [address, hash] : invocation.writes) {
    if (invocation.checked && !Find(checked->writes, address)) {
      mismatch = fmt::format("memory page {:08X} differs", address);
    }
  }
  if (!mismatch.empty()) {
    return mismatch;
  }
  if (diverged_ || next_export_ != invocation.exports.size()) {
    return "export calls or MMIO reads differ";
  }
  const auto* registers =
      reinterpret_cast<const uint8_t*>(thread_state_->context());
  for (size_t i = 0; i < JitCorpus::kRegistersSize; ++i) {
    if (registers[i] != invocation.exit_registers[i]) {
      return fmt::format("context byte {:#x} differs", i);
    }
  }
  return {};
}

uint64_t InvocationReplay::Time(const JitCorpus::Invocation& invocation) {
  for (const auto& [address, hash] : invocation.memory) {
    WritePage(address, hash);
  }
  invocation_ = &invocation;
  next_export_ = 0;
  std::memcpy(thread_state_->context(), invocation.entry_registers.data(),
              JitCorpus::kRegistersSize);
  Function* function = processor_->ResolveFunction(invocation.function);
  const uint64_t start = Clock::host_tick_count_platform();
  function->Call(thread_state_.get(), invocation.return_address);
  return Clock::host_tick_count_platform() - start;
}

void InvocationReplay::ExternHandler(ppc::PPCContext* context,
                                     kernel::KernelState* kernel_state) {
  InvocationReplay* self = current_;
  if (!self || !self->invocation_) {
    return;
  }
  const auto& invocation = *self->invocation_;
  const auto& exports = invocation.exports;
  if (self->next_export_ >= exports.size() ||
      exports[self->next_export_].return_address != uint32_t(context->lr)) {
    self->diverged_ = true;
    return;
  }
  const auto& call = exports[self->next_export_++];
  if (auto model = kernel::FindExportModel(call.name)) {
    model(context, invocation.thread, call.result);
  }
  context->r[3] = call.result;
}

uint32_t InvocationReplay::MmioRead(void* context, void* callback_context,
                                    uint32_t address) {
  if (current_) {
    current_->diverged_ = true;
  }
  return 0;
}

void InvocationReplay::MmioWrite(void* context, void* callback_context,
                                 uint32_t address, uint32_t value) {}

}  // namespace cpu
}  // namespace xe
