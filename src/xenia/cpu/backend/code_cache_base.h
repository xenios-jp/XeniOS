/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_CODE_CACHE_BASE_H_
#define XENIA_CPU_BACKEND_CODE_CACHE_BASE_H_

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"

#if XE_PLATFORM_APPLE && !XE_PLATFORM_IOS && XE_ARCH_ARM64
#include <pthread.h>
#endif
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
#include <dirent.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace xe {
namespace cpu {
namespace backend {

struct EmitFunctionInfo {
  struct _code_size {
    size_t prolog;
    size_t body;
    size_t epilog;
    size_t tail;
    size_t total;
  } code_size;
  size_t prolog_stack_alloc_offset;
  size_t stack_size;
#if XE_ARCH_ARM64
  // Offset from SP where x30 (LR) is saved.  ARM64 callees save LR
  // explicitly at varying offsets; the unwind info generator needs this
  // to tell the unwinder where to find the return address.
  // Currently only used by the POSIX DWARF .eh_frame generator; the
  // Windows .xdata format encodes LR saves differently.  Adds 8 bytes
  // to the struct on ARM64 Windows builds where it is unused, to avoid
  // #if clutter in the backend/emitter code that sets it.
  size_t lr_save_offset;
#endif
};

// CRTP base class for JIT code caches. Contains all platform-independent
// logic for memory management, the indirection table (fast + encoded paths),
// code placement, and function lookup. Derived classes provide architecture-
// specific hooks:
//
//   void FillCode(void* address, size_t size)
//     Fill unused code regions with trap instructions (0xCC / BRK).
//
//   void FlushCodeRange(void* address, size_t size)
//     Flush I-cache after writing code (no-op on x86, required on ARM64).
//
//   UnwindReservation RequestUnwindReservation(uint8_t* entry_address)
//     Reserve space for platform-specific unwind info.
//
//   void PlaceCode(uint32_t guest_address, void* machine_code,
//                  const EmitFunctionInfo& func_info,
//                  void* code_execute_address,
//                  UnwindReservation unwind_reservation)
//     Register unwind info and perform platform-specific post-placement.
//
//   void OnCodePlaced(uint32_t guest_address, GuestFunction* function_info,
//                     void* code_execute_address, size_t code_size)
//     Optional hook called after code is placed outside the critical section
//     (used for VTune integration on x64). Default is no-op.
//
// Indirection dispatch operates in one of two modes, picked at Initialize:
//  * Fast: fixed-VA table at kIndirectionTableBase; slots hold truncated
//          32-bit absolute host addresses.  Emitter uses a 2-insn lookup.
//  * Encoded: OS-chosen table; slots hold rel32 (bit 31 clear, target in
//          the code cache) or a tagged index into an external 64-bit table
//          (bit 31 set).  Emitter compensates via indirection_table_base_bias_.
template <typename Derived>
class CodeCacheBase : public CodeCache {
 public:
  ~CodeCacheBase() override {
    if (indirection_table_base_) {
      xe::memory::DeallocFixed(indirection_table_base_, kIndirectionTableSize,
                               xe::memory::DeallocationType::kRelease);
    }
    if (mapping_ != xe::memory::kFileMappingHandleInvalid) {
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      if (!generated_code_uses_ios_persistent_mapping_) {
#endif
        if (generated_code_write_base_ &&
            generated_code_write_base_ != generated_code_execute_base_) {
          xe::memory::UnmapFileView(mapping_, generated_code_write_base_,
                                    kGeneratedCodeSize);
        }
        if (generated_code_execute_base_) {
          xe::memory::UnmapFileView(mapping_, generated_code_execute_base_,
                                    kGeneratedCodeSize);
        }
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      }
#endif
      xe::memory::CloseFileMappingHandle(mapping_, file_name_);
      mapping_ = xe::memory::kFileMappingHandleInvalid;
    }
  }

  const std::filesystem::path& file_name() const override { return file_name_; }
  uintptr_t execute_base_address() const override {
    return generated_code_execute_base_
               ? reinterpret_cast<uintptr_t>(generated_code_execute_base_)
               : kGeneratedCodeExecuteBase;
  }
  size_t total_size() const override { return kGeneratedCodeSize; }

  bool has_indirection_table() { return indirection_table_base_ != nullptr; }

  // True when slots hold encoded rel32 + tagged-external values, false when
  // fixed allocation succeeded and slots hold raw 32-bit absolute addresses.
  bool encoded_indirection() const { return encoded_indirection_; }

  // Hint from the backend: if false, skip the fast-path attempt entirely
  // (trampolines couldn't land sub-4GB, so fast-mode slot encoding won't
  // fit them).  Must be set before Initialize.
  void set_allow_fast_indirection(bool v) { allow_fast_indirection_ = v; }

  // Accessors the emitter bakes as immediates for the encoded lookup.
  uintptr_t indirection_table_base_bias() const {
    return indirection_table_base_bias_;
  }
  uintptr_t external_indirection_table_base_address() const {
    return reinterpret_cast<uintptr_t>(external_indirection_targets_.get());
  }
  uintptr_t indirection_table_base_address() const {
    return indirection_table_actual_base_;
  }

  // Slot-encoding constants for the encoded path.
  static constexpr uint32_t kIndirectionExternalTag = 0x80000000u;
  static constexpr uint32_t kIndirectionExternalIndexMask = 0x7FFFFFFFu;
  static constexpr uint32_t kIndirectionExternalCapacity = 0x00010000u;

  // Virtual so that platform-specific derived classes (Win32/POSIX) can
  // override and chain up to add unwind registration.
  virtual bool Initialize() {
    generated_code_uses_mprotect_flip_ = false;
    generated_code_uses_vm_remap_fallback_ = false;
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
    generated_code_uses_ios_persistent_mapping_ = false;
#endif  // XE_PLATFORM_IOS && XE_ARCH_ARM64

    file_name_ =
        fmt::format("xenia_code_cache_{}", Clock::QueryHostTickCount());
    mapping_ = xe::memory::CreateFileMappingHandle(
        file_name_, kGeneratedCodeSize,
        xe::memory::PageAccess::kExecuteReadWrite, false);
    if (mapping_ == xe::memory::kFileMappingHandleInvalid) {
      XELOGE("Unable to create code cache mmap");
      return false;
    }

    const bool wx_preferred = xe::memory::IsWritableExecutableMemoryPreferred();

    // Fast path: fixed-VA table + code cache, slots hold raw 32-bit targets.
    // Disabled on macOS x86_64 (Rosetta): shm+PROT_EXEC mapping appears to
    // succeed but the page isn't actually executable. The encoded path
    // uses an anonymous MAP_JIT mapping which works.
    bool try_fast_indirection = allow_fast_indirection_;
#if XE_PLATFORM_MAC && XE_ARCH_AMD64
    try_fast_indirection = false;
#endif
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
    try_fast_indirection = false;
#endif
    if (try_fast_indirection) {
      indirection_table_base_ =
          reinterpret_cast<uint8_t*>(xe::memory::AllocFixed(
              reinterpret_cast<void*>(kIndirectionTableBase),
              kIndirectionTableSize, xe::memory::AllocationType::kReserve,
              xe::memory::PageAccess::kReadWrite));
      if (indirection_table_base_) {
        uint8_t* exec = reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
            mapping_, reinterpret_cast<void*>(kGeneratedCodeExecuteBase),
            kGeneratedCodeSize,
            wx_preferred ? xe::memory::PageAccess::kExecuteReadWrite
                         : xe::memory::PageAccess::kExecuteReadOnly,
            0));
        uint8_t* write = exec;
        if (exec && !wx_preferred) {
          write = reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
              mapping_, reinterpret_cast<void*>(kGeneratedCodeWriteBase),
              kGeneratedCodeSize, xe::memory::PageAccess::kReadWrite, 0));
          if (!write) {
            xe::memory::UnmapFileView(mapping_, exec, kGeneratedCodeSize);
            exec = nullptr;
          }
        }
        if (exec) {
          generated_code_execute_base_ = exec;
          generated_code_write_base_ = write;
          indirection_table_actual_base_ = kIndirectionTableBase;
          indirection_table_base_bias_ = 0;
          encoded_indirection_ = false;
          generated_code_map_.reserve(kMaximumFunctionCount);
          return true;
        }
        xe::memory::DeallocFixed(indirection_table_base_, kIndirectionTableSize,
                                 xe::memory::DeallocationType::kRelease);
        indirection_table_base_ = nullptr;
      }
    }

    // Encoded path: OS-chosen allocation, slots hold rel32 + tagged external.
    encoded_indirection_ = true;
    indirection_table_base_ = reinterpret_cast<uint8_t*>(xe::memory::AllocFixed(
        nullptr, kIndirectionTableSize, xe::memory::AllocationType::kReserve,
        xe::memory::PageAccess::kReadWrite));
    if (!indirection_table_base_) {
      XELOGE("Unable to reserve indirection table at any address (size=0x{:X})",
             static_cast<uint64_t>(kIndirectionTableSize));
      return false;
    }
    indirection_table_actual_base_ =
        reinterpret_cast<uintptr_t>(indirection_table_base_);
    indirection_table_base_bias_ =
        indirection_table_actual_base_ -
        static_cast<uintptr_t>(kIndirectionTableBase);

    external_indirection_targets_ =
        std::make_unique<uint64_t[]>(kIndirectionExternalCapacity);
    if (!external_indirection_targets_) {
      XELOGE("Unable to allocate external indirection table (entries={})",
             static_cast<uint32_t>(kIndirectionExternalCapacity));
      return false;
    }
    external_indirection_target_count_.store(0, std::memory_order_relaxed);

    // Try the preferred fixed address first; fall back to OS-chosen on fail.
    if (wx_preferred) {
#if XE_PLATFORM_APPLE && !XE_PLATFORM_IOS
      // macOS allows RWX only on anonymous MAP_JIT regions; the W^X gate
      // happens via pthread_jit_write_protect_np in PlaceGuestCode/PlaceData.
      generated_code_execute_base_ = reinterpret_cast<uint8_t*>(
          xe::memory::AllocFixed(nullptr, kGeneratedCodeSize,
                                 xe::memory::AllocationType::kReserveCommit,
                                 xe::memory::PageAccess::kExecuteReadWrite));
      generated_code_write_base_ = generated_code_execute_base_;
#else
      generated_code_execute_base_ =
          reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
              mapping_, reinterpret_cast<void*>(kGeneratedCodeExecuteBase),
              kGeneratedCodeSize, xe::memory::PageAccess::kExecuteReadWrite,
              0));
      if (!generated_code_execute_base_) {
        generated_code_execute_base_ =
            reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
                mapping_, nullptr, kGeneratedCodeSize,
                xe::memory::PageAccess::kExecuteReadWrite, 0));
      }
      generated_code_write_base_ = generated_code_execute_base_;
#endif
    } else {
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      const bool use_txm_broker_path = IOSUseTXMBrokerPath();
      if (use_txm_broker_path) {
        const int ios_major_version = IOSProductMajorVersion();
        if (ios_major_version > 0) {
          XELOGI("iOS JIT: TXM detected on iOS {}; using broker-capable path",
                 ios_major_version);
        } else {
          XELOGI("iOS JIT: TXM detected; using broker-capable path");
        }

        if (AcquireIOSPersistentGeneratedCodeMapping(
                generated_code_execute_base_, generated_code_write_base_)) {
          generated_code_uses_vm_remap_fallback_ = true;
          generated_code_uses_ios_persistent_mapping_ = true;
        } else {
          generated_code_execute_base_ = nullptr;
          generated_code_write_base_ = nullptr;
          XELOGW("iOS JIT persistent dual-map setup failed");
        }
      } else if (IOSHasTXM()) {
        const int ios_major_version = IOSProductMajorVersion();
        if (ios_major_version > 0) {
          XELOGI(
              "iOS JIT: TXM detected on iOS {}; using non-broker W^X defaults",
              ios_major_version);
        } else {
          XELOGI(
              "iOS JIT: TXM detected with unknown OS version; using "
              "non-broker W^X defaults");
        }
      } else {
        const int ios_major_version = IOSProductMajorVersion();
        if (ios_major_version > 0) {
          XELOGI(
              "iOS JIT: non-TXM path on iOS {}; using non-broker W^X "
              "defaults",
              ios_major_version);
        } else {
          XELOGI("iOS JIT: non-TXM path; using non-broker W^X defaults");
        }
      }

      if (!generated_code_execute_base_ || !generated_code_write_base_) {
        if (generated_code_execute_base_) {
          munmap(generated_code_execute_base_, kGeneratedCodeSize);
        }
        generated_code_execute_base_ = nullptr;
        generated_code_write_base_ = nullptr;
        generated_code_uses_vm_remap_fallback_ = false;
        generated_code_uses_ios_persistent_mapping_ = false;

        generated_code_write_base_ = reinterpret_cast<uint8_t*>(
            mmap(nullptr, kGeneratedCodeSize, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (generated_code_write_base_ == MAP_FAILED) {
          generated_code_write_base_ = nullptr;
          XELOGE("Unable to allocate iOS JIT code cache (RX mapping)");
          return false;
        }
        generated_code_execute_base_ = generated_code_write_base_;
        generated_code_uses_mprotect_flip_ = true;
        if (use_txm_broker_path) {
          XELOGI("iOS JIT mprotect-flip fallback active (TXM/broker path)");
        } else if (IOSHasTXM()) {
          XELOGI(
              "iOS JIT mprotect-flip fallback active (TXM, non-broker path)");
        } else {
          XELOGI(
              "iOS JIT mprotect-flip fallback active (non-TXM, no external "
              "BRK)");
        }
        XELOGI("iOS JIT mprotect-flip mapping (RX base): {:p}",
               static_cast<void*>(generated_code_execute_base_));
      }
#else
      generated_code_execute_base_ =
          reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
              mapping_, reinterpret_cast<void*>(kGeneratedCodeExecuteBase),
              kGeneratedCodeSize, xe::memory::PageAccess::kExecuteReadOnly, 0));
      if (!generated_code_execute_base_) {
        generated_code_execute_base_ =
            reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
                mapping_, nullptr, kGeneratedCodeSize,
                xe::memory::PageAccess::kExecuteReadOnly, 0));
      }
      generated_code_write_base_ =
          reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
              mapping_, reinterpret_cast<void*>(kGeneratedCodeWriteBase),
              kGeneratedCodeSize, xe::memory::PageAccess::kReadWrite, 0));
      if (!generated_code_write_base_) {
        generated_code_write_base_ = reinterpret_cast<uint8_t*>(
            xe::memory::MapFileView(mapping_, nullptr, kGeneratedCodeSize,
                                    xe::memory::PageAccess::kReadWrite, 0));
      }
#endif
    }
    if (!generated_code_execute_base_ || !generated_code_write_base_) {
      XELOGE("Unable to allocate code cache generated code storage");
      return false;
    }

    generated_code_map_.reserve(kMaximumFunctionCount);
    return true;
  }

  // Encode a host address as a 32-bit indirection entry:
  //  - bit 31 clear: rel32 offset from code cache base (target in cache).
  //  - bit 31 set:   tagged index into external 64-bit table (target out).
  // In fast mode this is never called; slots hold raw 32-bit addresses.
  uint32_t EncodeIndirectionTarget(uint64_t host_address) {
    const uintptr_t code_base = execute_base_address();
    const uintptr_t code_end = code_base + kGeneratedCodeSize;
    if (host_address >= code_base && host_address < code_end) {
      // Bit 31 of the offset is always clear because kGeneratedCodeSize
      // (0x0FFFFFFF) is less than 0x80000000 — that's what keeps the tag
      // bit free for the external-table case below.
      return static_cast<uint32_t>(host_address - code_base);
    }

    std::lock_guard<std::mutex> lock(external_indirection_mutex_);
    const uint32_t current_count =
        external_indirection_target_count_.load(std::memory_order_relaxed);

    // Table is small (dozens of entries in practice); linear scan is fine.
    for (uint32_t i = 0; i < current_count; i++) {
      if (external_indirection_targets_[i] == host_address) {
        return kIndirectionExternalTag | i;
      }
    }

    if (current_count >= kIndirectionExternalCapacity) {
      XELOGE(
          "Indirection external table overflow (count={} capacity={}); "
          "falling back to default target",
          current_count, static_cast<uint32_t>(kIndirectionExternalCapacity));
      return indirection_default_value_;
    }

    external_indirection_targets_[current_count] = host_address;
    external_indirection_target_count_.store(current_count + 1,
                                             std::memory_order_release);
    return kIndirectionExternalTag | current_count;
  }

  void set_indirection_default_64(uint64_t default_value) {
    if (encoded_indirection_) {
      indirection_default_value_ = EncodeIndirectionTarget(default_value);
    } else {
      assert_zero(default_value & 0xFFFFFFFF00000000ull);
      indirection_default_value_ = static_cast<uint32_t>(default_value);
    }
  }

  void AddIndirection(uint32_t guest_address, uint32_t host_address) {
    AddIndirection64(guest_address, static_cast<uint64_t>(host_address));
  }

  void AddIndirection64(uint32_t guest_address, uint64_t host_address) {
    if (!indirection_table_base_) {
      return;
    }
    if (guest_address < kIndirectionTableBase) {
      return;
    }
    const uint64_t guest_delta = guest_address - kIndirectionTableBase;
    const uint64_t slot_offset = (guest_delta / 4) * 4;
    if (slot_offset + 4 > kIndirectionTableSize) {
      return;
    }
    uint32_t* slot =
        reinterpret_cast<uint32_t*>(indirection_table_base_ + slot_offset);
    if (encoded_indirection_) {
      *slot = EncodeIndirectionTarget(host_address);
    } else {
      assert_zero(host_address & 0xFFFFFFFF00000000ull);
      *slot = static_cast<uint32_t>(host_address);
    }
  }

  // CRTP hook invoked from PlaceGuestCode after a new function is written.
  void UpdateIndirection(uint32_t guest_address, void* code_execute_address) {
    if (guest_address < kIndirectionTableBase) {
      return;
    }
    const uint64_t guest_delta = guest_address - kIndirectionTableBase;
    const uint64_t slot_offset = (guest_delta / 4) * 4;
    if (slot_offset + 4 > kIndirectionTableSize) {
      return;
    }
    uint32_t* slot =
        reinterpret_cast<uint32_t*>(indirection_table_base_ + slot_offset);
    const uint64_t host_address =
        reinterpret_cast<uint64_t>(code_execute_address);
    if (encoded_indirection_) {
      *slot = EncodeIndirectionTarget(host_address);
    } else {
      assert_zero(host_address & 0xFFFFFFFF00000000ull);
      *slot = static_cast<uint32_t>(host_address);
    }
  }

  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high) {
    if (!indirection_table_base_) {
      return;
    }
    if (guest_low < kIndirectionTableBase) {
      return;
    }

    const size_t start_offset =
        static_cast<size_t>(guest_low - kIndirectionTableBase);
    const size_t size = static_cast<size_t>(guest_high - guest_low);

    if (start_offset + size > kIndirectionTableSize) {
      XELOGE("CommitExecutableRange: range [0x{:08X}, 0x{:08X}) exceeds table",
             guest_low, guest_high);
      return;
    }

    xe::memory::AllocFixed(indirection_table_base_ + start_offset, size,
                           xe::memory::AllocationType::kCommit,
                           xe::memory::PageAccess::kReadWrite);

    uint32_t* p =
        reinterpret_cast<uint32_t*>(indirection_table_base_ + start_offset);
    const size_t entry_count = size / 4;
    for (size_t i = 0; i < entry_count; i++) {
      p[i] = indirection_default_value_;
    }
  }

  void PlaceHostCode(uint32_t guest_address, void* machine_code,
                     const EmitFunctionInfo& func_info,
                     void*& code_execute_address_out,
                     void*& code_write_address_out) {
    PlaceGuestCode(guest_address, machine_code, func_info, nullptr,
                   code_execute_address_out, code_write_address_out);
  }

  void PlaceGuestCode(uint32_t guest_address, void* machine_code,
                      const EmitFunctionInfo& func_info,
                      GuestFunction* function_info,
                      void*& code_execute_address_out,
                      void*& code_write_address_out) {
    using namespace xe::literals;
    uint8_t* code_execute_address;
    {
      auto global_lock = global_critical_region_.Acquire();

#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      if (generated_code_uses_mprotect_flip_) {
        generated_code_offset_ =
            xe::align(generated_code_offset_, xe::memory::page_size());
      }
#endif

      code_execute_address =
          generated_code_execute_base_ + generated_code_offset_;
      code_execute_address_out = code_execute_address;
      uint8_t* code_write_address =
          generated_code_write_base_ + generated_code_offset_;
      code_write_address_out = code_write_address;
      generated_code_offset_ += xe::round_up(func_info.code_size.total, 16);

      auto tail_write_address =
          generated_code_write_base_ + generated_code_offset_;

      auto unwind_reservation = self().RequestUnwindReservation(
          generated_code_write_base_ + generated_code_offset_);
      generated_code_offset_ += xe::round_up(unwind_reservation.data_size, 16);

      auto end_write_address =
          generated_code_write_base_ + generated_code_offset_;
      const size_t write_span_length =
          static_cast<size_t>(end_write_address - code_write_address);

      size_t high_mark = generated_code_offset_;

      generated_code_map_.emplace_back(
          (uint64_t(code_execute_address - generated_code_execute_base_)
           << 32) |
              generated_code_offset_,
          function_info);

      // Commit memory if needed.
      EnsureCommitted(high_mark);

#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      if (generated_code_uses_mprotect_flip_ &&
          !RegionLockRead(code_write_address, write_span_length)) {
        XELOGE("iOS JIT mprotect flip: failed to lock code range for writes");
        assert_always();
      }
      if (generated_code_uses_mprotect_flip_ &&
          !RegionUnlockWrite(code_write_address, write_span_length)) {
        XELOGE(
            "iOS JIT mprotect flip: failed to enable writes before code "
            "publish");
        assert_always();
      }
#endif

#if XE_PLATFORM_APPLE && !XE_PLATFORM_IOS && XE_ARCH_ARM64
      // Toggle the per-thread MAP_JIT write gate around writes. Only needed
      // when execute and write bases alias (single MAP_JIT region); a
      // separate RW view needs no gating.
      const bool jit_write_toggle =
          generated_code_execute_base_ == generated_code_write_base_;
      if (jit_write_toggle) {
        pthread_jit_write_protect_np(0);
      }
#endif

      // Copy code.
      std::memcpy(code_write_address, machine_code, func_info.code_size.total);

      // Fill unused tail/unwind gap with arch-specific trap instructions.
      self().FillCode(
          tail_write_address,
          static_cast<size_t>(end_write_address - tail_write_address));

      // Platform-specific unwind registration. Must stay inside the JIT
      // write window: on Mac it writes DWARF entries into the cache view.
      self().PlaceCode(guest_address, machine_code, func_info,
                       code_execute_address, unwind_reservation);

#if XE_PLATFORM_APPLE && !XE_PLATFORM_IOS && XE_ARCH_ARM64
      if (jit_write_toggle) {
        pthread_jit_write_protect_np(1);
      }
#endif

      // Flush I-cache for code and fill regions.
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      auto* tail_execute_address =
          generated_code_execute_base_ +
          (tail_write_address - generated_code_write_base_);
      self().FlushCodeRange(code_execute_address, func_info.code_size.total);
      if (tail_execute_address <
          generated_code_execute_base_ +
              (end_write_address - generated_code_write_base_)) {
        self().FlushCodeRange(
            tail_execute_address,
            static_cast<size_t>(end_write_address - tail_write_address));
      }
#else
      self().FlushCodeRange(code_write_address, func_info.code_size.total);
      if (tail_write_address < end_write_address) {
        self().FlushCodeRange(
            tail_write_address,
            static_cast<size_t>(end_write_address - tail_write_address));
      }
#endif

#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      if (generated_code_uses_mprotect_flip_ &&
          !RegionSetExec(code_execute_address, write_span_length)) {
        XELOGE("iOS JIT mprotect flip: failed to restore RX after code write");
        assert_always();
      }
#endif
    }

    // Post-placement hook (e.g. VTune notification).
    self().OnCodePlaced(guest_address, function_info, code_execute_address,
                        func_info.code_size.total);

    // Fix up indirection table.
    if (guest_address && indirection_table_base_) {
      UpdateIndirection(guest_address, code_execute_address);
    }
  }

  uint32_t PlaceData(const void* data, size_t length) {
    size_t high_mark;
    uint8_t* data_address = nullptr;
    {
      auto global_lock = global_critical_region_.Acquire();
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      if (generated_code_uses_mprotect_flip_) {
        generated_code_offset_ =
            xe::align(generated_code_offset_, xe::memory::page_size());
      }
#endif
      data_address = generated_code_write_base_ + generated_code_offset_;
      generated_code_offset_ += xe::round_up(length, 16);
      high_mark = generated_code_offset_;
    }
    EnsureCommitted(high_mark);
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
    const size_t reserved_length = xe::round_up(length, 16);
    if (generated_code_uses_mprotect_flip_ &&
        !RegionLockRead(data_address, reserved_length)) {
      XELOGE("iOS JIT mprotect flip: failed to lock data range for writes");
      assert_always();
    }
    if (generated_code_uses_mprotect_flip_ &&
        !RegionUnlockWrite(data_address, reserved_length)) {
      XELOGE(
          "iOS JIT mprotect flip: failed to enable writes before data "
          "publish");
      assert_always();
    }
#endif
#if XE_PLATFORM_APPLE && !XE_PLATFORM_IOS && XE_ARCH_ARM64
    const bool jit_write_toggle =
        generated_code_execute_base_ == generated_code_write_base_;
    if (jit_write_toggle) {
      pthread_jit_write_protect_np(0);
    }
#endif
    std::memcpy(data_address, data, length);
#if XE_PLATFORM_APPLE && !XE_PLATFORM_IOS && XE_ARCH_ARM64
    if (jit_write_toggle) {
      pthread_jit_write_protect_np(1);
    }
#endif
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
    if (generated_code_uses_mprotect_flip_ &&
        !RegionLockRead(data_address, reserved_length)) {
      XELOGE("iOS JIT mprotect flip: failed to lock data range before RX");
      assert_always();
    }
    if (generated_code_uses_mprotect_flip_ &&
        !RegionSetExec(data_address, reserved_length)) {
      XELOGE("iOS JIT mprotect flip: failed to restore RX after data write");
      assert_always();
    }
#endif
    return uint32_t(uintptr_t(data_address));
  }

  GuestFunction* LookupFunction(uint64_t host_pc) override {
    if (generated_code_map_.empty()) {
      return nullptr;
    }
    const uint64_t code_base = execute_base_address();
    const uint64_t code_end = code_base + total_size();
    if (host_pc < code_base || host_pc >= code_end) {
      return nullptr;
    }
    uint32_t key = uint32_t(host_pc - code_base);
    void* fn_entry = std::bsearch(
        &key, generated_code_map_.data(), generated_code_map_.size(),
        sizeof(std::pair<uint32_t, Function*>),
        [](const void* key_ptr, const void* element_ptr) {
          auto key = *reinterpret_cast<const uint32_t*>(key_ptr);
          auto element =
              reinterpret_cast<const std::pair<uint64_t, GuestFunction*>*>(
                  element_ptr);
          if (key < (element->first >> 32)) {
            return -1;
          } else if (key > uint32_t(element->first)) {
            return 1;
          } else {
            return 0;
          }
        });
    if (fn_entry) {
      return reinterpret_cast<const std::pair<uint64_t, GuestFunction*>*>(
                 fn_entry)
          ->second;
    } else {
      return nullptr;
    }
  }

 protected:
  static constexpr size_t kIndirectionTableSize = 0x1FFFFFFF;
  static constexpr uintptr_t kIndirectionTableBase = 0x80000000;
  static constexpr size_t kGeneratedCodeSize = 0x0FFFFFFF;
  static constexpr uintptr_t kGeneratedCodeExecuteBase = 0xA0000000;
  static const uintptr_t kGeneratedCodeWriteBase =
      kGeneratedCodeExecuteBase + kGeneratedCodeSize + 1;
  static constexpr size_t kMaximumFunctionCount = 1000000;

  struct UnwindReservation {
    size_t data_size = 0;
    size_t table_slot = 0;
    uint8_t* entry_address = 0;
  };

  CodeCacheBase() = default;

  // Default no-op for the OnCodePlaced hook.
  void OnCodePlaced(uint32_t guest_address, GuestFunction* function_info,
                    void* code_execute_address, size_t code_size) {}

  std::filesystem::path file_name_;
  xe::memory::FileMappingHandle mapping_ =
      xe::memory::kFileMappingHandleInvalid;
  xe::global_critical_region global_critical_region_;
  uint32_t indirection_default_value_ = 0xFEEDF00D;
  uint8_t* indirection_table_base_ = nullptr;
  uint8_t* generated_code_execute_base_ = nullptr;
  uint8_t* generated_code_write_base_ = nullptr;
  bool generated_code_uses_vm_remap_fallback_ = false;
  bool generated_code_uses_mprotect_flip_ = false;
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
  bool generated_code_uses_ios_persistent_mapping_ = false;
#endif  // XE_PLATFORM_IOS && XE_ARCH_ARM64
  size_t generated_code_offset_ = 0;
  std::atomic<size_t> generated_code_commit_mark_ = {0};
  std::vector<std::pair<uint64_t, GuestFunction*>> generated_code_map_;

  bool encoded_indirection_ = true;
  bool allow_fast_indirection_ = true;
  uintptr_t indirection_table_actual_base_ = 0;
  uintptr_t indirection_table_base_bias_ = 0;
  std::unique_ptr<uint64_t[]> external_indirection_targets_;
  std::atomic<uint32_t> external_indirection_target_count_{0};
  std::mutex external_indirection_mutex_;

 private:
  Derived& self() { return static_cast<Derived&>(*this); }

#if XE_PLATFORM_IOS && XE_ARCH_ARM64
  static std::atomic<bool>& IOSExternalPrepareIssued() {
    static std::atomic<bool> issued{false};
    return issued;
  }

  static std::atomic<bool>& IOSExternalDetachIssued() {
    static std::atomic<bool> issued{false};
    return issued;
  }

  struct IOSPersistentGeneratedCodeMapping {
    std::mutex mutex;
    uint8_t* execute_base = nullptr;
    uint8_t* write_base = nullptr;
    bool initialized = false;
  };

  static IOSPersistentGeneratedCodeMapping&
  IOSPersistentGeneratedCodeMappingState() {
    static IOSPersistentGeneratedCodeMapping state;
    return state;
  }

  static bool AcquireIOSPersistentGeneratedCodeMapping(uint8_t*& execute_base,
                                                       uint8_t*& write_base) {
    auto& state = IOSPersistentGeneratedCodeMappingState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.initialized) {
      execute_base = state.execute_base;
      write_base = state.write_base;
      return true;
    }

    auto* execute = reinterpret_cast<uint8_t*>(
        mmap(nullptr, kGeneratedCodeSize, PROT_READ | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (execute == MAP_FAILED) {
      XELOGW("iOS JIT persistent dual-map setup (RX base) failed");
      execute_base = nullptr;
      write_base = nullptr;
      return false;
    }

    if (!MaybeRequestExternalJitPrepare(execute, kGeneratedCodeSize)) {
      XELOGW("iOS JIT TXM: persistent external prepare request failed");
    }

    vm_address_t remap_addr = 0;
    vm_prot_t cur_prot = 0;
    vm_prot_t max_prot = 0;
    const kern_return_t remap_result = vm_remap(
        mach_task_self(), &remap_addr, kGeneratedCodeSize, 0, VM_FLAGS_ANYWHERE,
        mach_task_self(), reinterpret_cast<vm_address_t>(execute), FALSE,
        &cur_prot, &max_prot, VM_INHERIT_NONE);
    if (remap_result != KERN_SUCCESS) {
      XELOGW("iOS JIT persistent dual-mapping: vm_remap failed (kr={})",
             remap_result);
      munmap(execute, kGeneratedCodeSize);
      execute_base = nullptr;
      write_base = nullptr;
      return false;
    }

    auto* write = reinterpret_cast<uint8_t*>(remap_addr);
    if (mprotect(write, kGeneratedCodeSize, PROT_READ | PROT_WRITE) != 0) {
      XELOGE(
          "iOS JIT persistent dual-mapping: mprotect RW alias failed "
          "addr=0x{:X} len=0x{:X} err={} ({})",
          reinterpret_cast<uintptr_t>(write),
          static_cast<uint32_t>(kGeneratedCodeSize), errno,
          std::strerror(errno));
      vm_deallocate(mach_task_self(), remap_addr, kGeneratedCodeSize);
      munmap(execute, kGeneratedCodeSize);
      execute_base = nullptr;
      write_base = nullptr;
      return false;
    }

    state.execute_base = execute;
    state.write_base = write;
    state.initialized = true;
    execute_base = execute;
    write_base = write;
    XELOGI("iOS JIT persistent dual-mapping active: execute={:p} write={:p}",
           static_cast<void*>(execute_base), static_cast<void*>(write_base));
    return true;
  }

  static std::string FindChildWithNameLength(const std::string& directory,
                                             size_t name_length) {
    DIR* dir = opendir(directory.c_str());
    if (!dir) {
      return std::string();
    }

    std::string found;
    while (dirent* entry = readdir(dir)) {
      const char* name = entry->d_name;
      if (!name || name[0] == '.') {
        continue;
      }
      if (std::strlen(name) == name_length) {
        found = directory + "/" + name;
        break;
      }
    }
    closedir(dir);
    return found;
  }

  static bool IOSHasTXM() {
    static const bool has_txm = []() -> bool {
      if (const char* env = std::getenv("HAS_TXM")) {
        if (env[0] == '1' && env[1] == '\0') {
          return true;
        }
        if (env[0] == '0' && env[1] == '\0') {
          return false;
        }
      }

      const std::string preboot_uuid =
          FindChildWithNameLength("/System/Volumes/Preboot", 36);
      if (!preboot_uuid.empty()) {
        const std::string txm_root =
            FindChildWithNameLength(preboot_uuid + "/boot", 96);
        if (!txm_root.empty()) {
          const std::string txm_path =
              txm_root +
              "/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4";
          if (access(txm_path.c_str(), F_OK) == 0) {
            return true;
          }
        }
      }

      const std::string private_preboot_root =
          FindChildWithNameLength("/private/preboot", 96);
      if (!private_preboot_root.empty()) {
        const std::string txm_path = private_preboot_root +
                                     "/usr/standalone/firmware/FUD/"
                                     "Ap,TrustedExecutionMonitor.img4";
        if (access(txm_path.c_str(), F_OK) == 0) {
          return true;
        }
      }

      // Definitive preboot probe came up empty. On newer devices the app
      // sandbox often can't read /private/preboot at all (so access() fails
      // even though the SoC ships TXM), which would misclassify e.g. an
      // iPhone 17 as non-TXM. Fall back to a SoC + OS capability inference.
      return IOSInfersTXMClassHardware();
    }();
    return has_txm;
  }

  // Capability inference used only when the definitive preboot probe above is
  // unavailable. Apple's Platform Security guide ("Operating system
  // integrity") states: "In the A15 or later and M2 or later SOCs, SPTM (in
  // combination with TXM) replaces the PPL." Apple does NOT publish a
  // chip<->hw.cpufamily mapping or an OS-version floor, so:
  //   - the family values below are empirical, taken from xnu
  //     osfmk/mach/machine.h (verified against the current header), and
  //   - the iOS 17 floor is the release that shipped SPTM/TXM, not an Apple
  //     requirement (an A15 on iOS 15/16 predates it).
  // Treat the result as an inference, not proof. Unknown/newer SoCs default to
  // true so future hardware keeps the TXM-aware JIT path by default.
  // True when this iOS binary is running as an "iPhone/iPad app on Mac" on
  // Apple silicon. In that case hw.cpufamily reports the host Mac's SoC, so the
  // TXM inference below would read the Mac chip instead of a real iOS device.
  // NSProcessInfo.isiOSAppOnMac (iOS 14+) is the documented signal; it is false
  // for Mac Catalyst and on real devices. Invoked via the Objective-C runtime
  // so this C++ header needs no Objective-C++ translation unit.
  static bool IOSRunningAsAppOnMac() {
    Class process_info_class = objc_getClass("NSProcessInfo");
    if (!process_info_class) {
      return false;
    }
    using SharedFn = id (*)(Class, SEL);
    const id process_info = reinterpret_cast<SharedFn>(objc_msgSend)(
        process_info_class, sel_registerName("processInfo"));
    if (!process_info) {
      return false;
    }
    const SEL selector = sel_registerName("isiOSAppOnMac");
    using RespondsFn = BOOL (*)(id, SEL, SEL);
    if (!reinterpret_cast<RespondsFn>(objc_msgSend)(
            process_info, sel_registerName("respondsToSelector:"), selector)) {
      return false;
    }
    using BoolFn = BOOL (*)(id, SEL);
    return reinterpret_cast<BoolFn>(objc_msgSend)(process_info, selector) != NO;
  }

  static bool IOSInfersTXMClassHardware() {
    // iOS-app-on-Mac exposes the host Mac's hw.cpufamily; never infer TXM from
    // it (see IOSRunningAsAppOnMac).
    if (IOSRunningAsAppOnMac()) {
      return false;
    }

    const int major = IOSProductMajorVersion();
    if (major > 0 && major < 17) {
      return false;
    }

    uint32_t family = 0;
    size_t size = sizeof(family);
    if (sysctlbyname("hw.cpufamily", &family, &size, nullptr, 0) != 0 ||
        size != sizeof(family) || family == 0) {
      return false;  // can't determine -> stay conservative
    }

    switch (family) {
      case 0x1e2d6381u:  // Swift              (A6)
      case 0x37a09642u:  // Cyclone            (A7)
      case 0x2c91a47eu:  // Typhoon            (A8)
      case 0x92fb37c8u:  // Twister            (A9)
      case 0x67ceee93u:  // Hurricane          (A10)
      case 0xe81e7ef6u:  // Monsoon/Mistral    (A11)
      case 0x07d34b9fu:  // Vortex/Tempest     (A12)
      case 0x462504d2u:  // Lightning/Thunder  (A13)
      case 0x1b588bb3u:  // Firestorm/Icestorm (A14 / M1) - last pre-TXM family
        return false;
      default:
        // 0xda33d83d (Blizzard/Avalanche = A15 / M2) and everything newer.
        return true;
    }
  }

  static int IOSProductMajorVersion() {
    static const int major_version = []() -> int {
      size_t version_size = 0;
      if (sysctlbyname("kern.osproductversion", nullptr, &version_size, nullptr,
                       0) != 0 ||
          version_size == 0) {
        return -1;
      }

      std::string version(version_size, '\0');
      if (sysctlbyname("kern.osproductversion", version.data(), &version_size,
                       nullptr, 0) != 0 ||
          version_size == 0) {
        return -1;
      }
      if (!version.empty() && version.back() == '\0') {
        version.pop_back();
      }
      if (version.empty()) {
        return -1;
      }

      int parsed_major = 0;
      size_t index = 0;
      while (index < version.size() && version[index] >= '0' &&
             version[index] <= '9') {
        parsed_major = parsed_major * 10 + (version[index] - '0');
        ++index;
      }
      return parsed_major > 0 ? parsed_major : -1;
    }();
    return major_version;
  }

  static bool IOSUseTXMBrokerPath() {
    if (!IOSHasTXM()) {
      return false;
    }
    const int ios_major_version = IOSProductMajorVersion();
    return ios_major_version >= 26;
  }

  static bool GetPageAlignedGeneratedCodeRange(void* address, size_t length,
                                               uintptr_t& aligned_start,
                                               size_t& aligned_length) {
    if (!length) {
      aligned_start = 0;
      aligned_length = 0;
      return true;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const size_t host_page_size = xe::memory::page_size();
    aligned_start = start & ~(host_page_size - 1);
    const uintptr_t aligned_end = xe::align(start + length, host_page_size);
    if (aligned_end <= aligned_start) {
      aligned_length = 0;
      return true;
    }
    aligned_length = aligned_end - aligned_start;
    return true;
  }

  static vm_prot_t ToMachProtectFlags(xe::memory::PageAccess access) {
    switch (access) {
      case xe::memory::PageAccess::kNoAccess:
        return VM_PROT_NONE;
      case xe::memory::PageAccess::kReadOnly:
        return VM_PROT_READ;
      case xe::memory::PageAccess::kReadWrite:
        return VM_PROT_READ | VM_PROT_WRITE;
      case xe::memory::PageAccess::kExecuteReadOnly:
        return VM_PROT_READ | VM_PROT_EXECUTE;
      case xe::memory::PageAccess::kExecuteReadWrite:
        return VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
      default:
        assert_unhandled_case(access);
        return VM_PROT_NONE;
    }
  }

  static bool SetPageAlignedAccess(void* address, size_t length,
                                   xe::memory::PageAccess access) {
    uintptr_t aligned_start = 0;
    size_t aligned_length = 0;
    if (!GetPageAlignedGeneratedCodeRange(address, length, aligned_start,
                                          aligned_length) ||
        !aligned_length) {
      return true;
    }
    return xe::memory::Protect(reinterpret_cast<void*>(aligned_start),
                               aligned_length, access);
  }

  static bool SetPageAlignedAccessWithMaxProtRetry(
      void* address, size_t length, xe::memory::PageAccess access) {
    if (SetPageAlignedAccess(address, length, access)) {
      return true;
    }

    uintptr_t aligned_start = 0;
    size_t aligned_length = 0;
    if (!GetPageAlignedGeneratedCodeRange(address, length, aligned_start,
                                          aligned_length) ||
        !aligned_length) {
      return true;
    }

    constexpr vm_prot_t kMaxProtect =
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    const kern_return_t max_result =
        vm_protect(mach_task_self(), static_cast<vm_address_t>(aligned_start),
                   aligned_length, TRUE, kMaxProtect);
    if (max_result != KERN_SUCCESS) {
      XELOGE(
          "iOS JIT code cache: vm_protect max failed addr=0x{:X} len=0x{:X} "
          "kr={}",
          aligned_start, static_cast<uint32_t>(aligned_length), max_result);
      return false;
    }

    const vm_prot_t target_protect = ToMachProtectFlags(access);
    const kern_return_t protect_result =
        vm_protect(mach_task_self(), static_cast<vm_address_t>(aligned_start),
                   aligned_length, FALSE, target_protect);
    if (protect_result != KERN_SUCCESS) {
      XELOGE(
          "iOS JIT code cache: vm_protect current failed addr=0x{:X} "
          "len=0x{:X} prot=0x{:X} kr={}",
          aligned_start, static_cast<uint32_t>(aligned_length),
          static_cast<uint32_t>(target_protect), protect_result);
      return false;
    }
    return true;
  }

  static bool AccessSatisfies(xe::memory::PageAccess actual,
                              xe::memory::PageAccess desired) {
    const uint32_t actual_bits = static_cast<uint32_t>(actual);
    const uint32_t desired_bits = static_cast<uint32_t>(desired);
    return (actual_bits & desired_bits) == desired_bits;
  }

  static void InvokeUniversalPrepareBreakpoint(uintptr_t aligned_start,
                                               size_t aligned_length) {
    register uint64_t x0 __asm("x0") = static_cast<uint64_t>(aligned_start);
    register uint64_t x1 __asm("x1") = static_cast<uint64_t>(aligned_length);
    register uint64_t x16 __asm("x16") = 1;
    asm volatile("brk #0xf00d" : "+r"(x0), "+r"(x1), "+r"(x16) : : "memory");
  }

  static void InvokeUniversalDetachBreakpoint() {
    register uint64_t x16 __asm("x16") = 0;
    asm volatile("brk #0xf00d" : "+r"(x16) : : "memory");
  }

  static bool MaybeRequestExternalJitDetach() {
    bool expected = false;
    if (!IOSExternalDetachIssued().compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return true;
    }
    InvokeUniversalDetachBreakpoint();
    return true;
  }

  static bool RequestExternalJitPrepare(void* address, size_t length) {
    uintptr_t aligned_start = 0;
    size_t aligned_length = 0;
    if (!GetPageAlignedGeneratedCodeRange(address, length, aligned_start,
                                          aligned_length) ||
        !aligned_length) {
      return true;
    }
    InvokeUniversalPrepareBreakpoint(aligned_start, aligned_length);
    MaybeRequestExternalJitDetach();
    return true;
  }

  static bool MaybeRequestExternalJitPrepare(void* address, size_t length) {
    bool expected = false;
    if (!IOSExternalPrepareIssued().compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return true;
    }
    return RequestExternalJitPrepare(address, length);
  }

  static bool SetPageAlignedAccessWithExternalPrepareFallback(
      void* address, size_t length, xe::memory::PageAccess desired_access,
      const char* transition_name) {
    const bool use_txm_broker_path = IOSUseTXMBrokerPath();
    if (use_txm_broker_path) {
      if (SetPageAlignedAccessWithMaxProtRetry(address, length,
                                               desired_access)) {
        return true;
      }
    } else if (SetPageAlignedAccess(address, length, desired_access)) {
      return true;
    }

    if (!use_txm_broker_path) {
      const int ios_major_version = IOSProductMajorVersion();
      if (ios_major_version > 0) {
        XELOGW(
            "iOS JIT mprotect flip: {} denied on iOS {} without RWX retry or "
            "external brk fallback (non-broker path)",
            transition_name, ios_major_version);
      } else {
        XELOGW(
            "iOS JIT mprotect flip: {} denied without RWX retry or external "
            "brk fallback (non-broker path)",
            transition_name);
      }
      return false;
    }

    XELOGW(
        "iOS JIT mprotect flip: {} denied, requesting external prepare via "
        "brk #0xf00d (x16=1)",
        transition_name);
    if (!MaybeRequestExternalJitPrepare(address, length)) {
      return false;
    }

    if (SetPageAlignedAccessWithMaxProtRetry(address, length, desired_access)) {
      return true;
    }

    uintptr_t aligned_start = 0;
    size_t aligned_length = 0;
    if (!GetPageAlignedGeneratedCodeRange(address, length, aligned_start,
                                          aligned_length) ||
        !aligned_length) {
      return true;
    }

    size_t query_length = 0;
    xe::memory::PageAccess query_access = xe::memory::PageAccess::kNoAccess;
    const bool query_ok = xe::memory::QueryProtect(
        reinterpret_cast<void*>(aligned_start), query_length, query_access);
    if (query_ok && AccessSatisfies(query_access, desired_access)) {
      return true;
    }

    XELOGE(
        "iOS JIT mprotect flip: external prepare did not yield {} mapping "
        "addr=0x{:X} len=0x{:X} query_ok={} query_access={} query_len=0x{:X}",
        transition_name, aligned_start, static_cast<uint32_t>(aligned_length),
        query_ok, static_cast<uint32_t>(query_access),
        static_cast<uint32_t>(query_length));
    return false;
  }

  static bool RegionLockRead(void* address, size_t length) {
    if (IOSUseTXMBrokerPath()) {
      return SetPageAlignedAccessWithMaxProtRetry(
          address, length, xe::memory::PageAccess::kReadOnly);
    }
    return SetPageAlignedAccess(address, length,
                                xe::memory::PageAccess::kReadOnly);
  }

  static bool RegionUnlockWrite(void* address, size_t length) {
    return SetPageAlignedAccessWithExternalPrepareFallback(
        address, length, xe::memory::PageAccess::kReadWrite, "RW transition");
  }

  static bool RegionSetExec(void* address, size_t length) {
    return SetPageAlignedAccessWithExternalPrepareFallback(
        address, length, xe::memory::PageAccess::kExecuteReadOnly,
        "RX transition");
  }

#endif

  void EnsureCommitted(size_t high_mark) {
    using namespace xe::literals;
    size_t old_commit_mark, new_commit_mark;
    do {
      old_commit_mark = generated_code_commit_mark_;
      if (high_mark <= old_commit_mark) {
        break;
      }
      new_commit_mark = old_commit_mark + 16_MiB;
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      if (!generated_code_uses_vm_remap_fallback_ &&
          !generated_code_uses_mprotect_flip_) {
#endif
        if (generated_code_execute_base_ == generated_code_write_base_) {
          xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                                 xe::memory::AllocationType::kCommit,
                                 xe::memory::PageAccess::kExecuteReadWrite);
        } else {
          xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                                 xe::memory::AllocationType::kCommit,
                                 xe::memory::PageAccess::kExecuteReadOnly);
          xe::memory::AllocFixed(generated_code_write_base_, new_commit_mark,
                                 xe::memory::AllocationType::kCommit,
                                 xe::memory::PageAccess::kReadWrite);
        }
#if XE_PLATFORM_IOS && XE_ARCH_ARM64
      }
#endif
    } while (generated_code_commit_mark_.compare_exchange_weak(
        old_commit_mark, new_commit_mark));
  }
};

}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_CODE_CACHE_BASE_H_
