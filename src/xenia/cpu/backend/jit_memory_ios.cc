/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/jit_memory_ios.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <dirent.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"

namespace xe::cpu::backend {

namespace {

// StikDebug's prepare (3.1.x, iOS 27) drops the high bits of the region's
// address and prepares the low-memory alias instead, zeroing the heap pages
// there. Below 8 GB the address survives.
constexpr uintptr_t kExecuteRegionLimit = uintptr_t(1) << 33;
// The code region's size on iOS: CodeCacheBase::kGeneratedCodeSize, page
// rounded.
constexpr size_t kExecuteRegionReserveSize = size_t(1) << 29;

// Finds room below kExecuteRegionLimit, with hints only so nothing already
// mapped is replaced.
uint8_t* MapBelowExecuteRegionLimit(size_t size, int prot) {
  const size_t step = size_t(1) << 28;
  for (uintptr_t hint = uintptr_t(1) << 32; hint + size <= kExecuteRegionLimit;
       hint += step) {
    void* mapping = mmap(reinterpret_cast<void*>(hint), size, prot,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
      continue;
    }
    if (reinterpret_cast<uintptr_t>(mapping) + size <= kExecuteRegionLimit) {
      return static_cast<uint8_t*>(mapping);
    }
    munmap(mapping, size);
  }
  return nullptr;
}

// Below 8 GB only the space under the shared cache is usable, and by the time
// a game starts UIKit, Metal and the launcher may have left no 512 MB of it in
// one piece. Take it while the process loads; the code region replaces it.
uint8_t* execute_region_reservation =
    MapBelowExecuteRegionLimit(kExecuteRegionReserveSize, PROT_NONE);

std::atomic<IOSJitMemory::ExternalPrepareHook> external_prepare_hook{nullptr};

}  // namespace

// Device detection and the external JIT preparation protocol originate in
// XeniOS preview 2f888db75. Keep the platform policy in this translation unit.
struct IOSJitMemory::Impl {
  uint8_t* execute = nullptr;
  uint8_t* write = nullptr;
  size_t size = 0;
  bool persistent = false;

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
    bool in_use = false;
    size_t size = 0;
  };

  static IOSPersistentGeneratedCodeMapping&
  IOSPersistentGeneratedCodeMappingState() {
    static IOSPersistentGeneratedCodeMapping state;
    return state;
  }

  static uint8_t* MapExecuteRegionLow(size_t size) {
    if (uint8_t* reservation = execute_region_reservation) {
      execute_region_reservation = nullptr;
      if (size <= kExecuteRegionReserveSize &&
          mmap(reservation, size, PROT_READ | PROT_EXEC,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
               0) == reservation) {
        if (size < kExecuteRegionReserveSize) {
          munmap(reservation + size, kExecuteRegionReserveSize - size);
        }
        return reservation;
      }
      munmap(reservation, kExecuteRegionReserveSize);
    }
    if (uint8_t* mapping =
            MapBelowExecuteRegionLimit(size, PROT_READ | PROT_EXEC)) {
      return mapping;
    }
    XELOGW(
        "iOS JIT: no room for the code region below 8 GB, an external "
        "debugger may prepare the wrong memory");
    return static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_EXEC,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  }

  static bool AcquireIOSPersistentGeneratedCodeMapping(uint8_t*& execute_base,
                                                       uint8_t*& write_base,
                                                       size_t size) {
    auto& state = IOSPersistentGeneratedCodeMappingState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.in_use || (state.initialized && state.size != size)) {
      XELOGE("iOS JIT mapping is already in use or has a different size");
      return false;
    }
    if (state.initialized) {
      state.in_use = true;
      execute_base = state.execute_base;
      write_base = state.write_base;
      return true;
    }

    auto* execute = MapExecuteRegionLow(size);
    if (execute == MAP_FAILED) {
      XELOGW("iOS JIT persistent dual-map setup (RX base) failed");
      execute_base = nullptr;
      write_base = nullptr;
      return false;
    }

    if (!MaybeRequestExternalJitPrepare(execute, size)) {
      XELOGW("iOS JIT TXM: persistent external prepare request failed");
    }

    vm_address_t remap_addr = 0;
    vm_prot_t cur_prot = 0;
    vm_prot_t max_prot = 0;
    const kern_return_t remap_result =
        vm_remap(mach_task_self(), &remap_addr, size, 0, VM_FLAGS_ANYWHERE,
                 mach_task_self(), reinterpret_cast<vm_address_t>(execute),
                 FALSE, &cur_prot, &max_prot, VM_INHERIT_NONE);
    if (remap_result != KERN_SUCCESS) {
      XELOGW("iOS JIT persistent dual-mapping: vm_remap failed (kr={})",
             remap_result);
      munmap(execute, size);
      execute_base = nullptr;
      write_base = nullptr;
      return false;
    }

    auto* write = reinterpret_cast<uint8_t*>(remap_addr);
    if (mprotect(write, size, PROT_READ | PROT_WRITE) != 0) {
      XELOGE(
          "iOS JIT persistent dual-mapping: mprotect RW alias failed "
          "addr=0x{:X} len=0x{:X} err={} ({})",
          reinterpret_cast<uintptr_t>(write), static_cast<uint32_t>(size),
          errno, std::strerror(errno));
      vm_deallocate(mach_task_self(), remap_addr, size);
      munmap(execute, size);
      execute_base = nullptr;
      write_base = nullptr;
      return false;
    }

    state.execute_base = execute;
    state.write_base = write;
    state.initialized = true;
    state.in_use = true;
    state.size = size;
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

  // Definitive check: the on-disk TXM firmware image under the preboot volume.
  // Present => the device is running with TXM. Absent is NOT conclusive: the
  // app sandbox frequently cannot read these paths on newer OSes, so a false
  // result just means "unconfirmed" and the caller falls back to inference.
  static bool IOSProbePrebootTXMFirmware() {
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

    return false;
  }

  static bool IOSHasTXM() {
    static const bool has_txm = []() -> bool {
      if (const char* env = std::getenv("HAS_TXM")) {
        if (env[0] == '1' && env[1] == '\0') {
          XELOGI("iOS TXM: forced ON via HAS_TXM=1 (detection skipped)");
          return true;
        }
        if (env[0] == '0' && env[1] == '\0') {
          XELOGI("iOS TXM: forced OFF via HAS_TXM=0 (detection skipped)");
          return false;
        }
      }

      // Two independent signals: the definitive on-disk firmware probe and the
      // SoC/OS capability inference. The probe wins when it finds the image;
      // otherwise we trust the inference (it covers the common case where the
      // sandbox can't read the preboot paths on a TXM-class device).
      const bool preboot_probe = IOSProbePrebootTXMFirmware();
      const bool hw_inference = IOSInfersTXMClassHardware();
      const bool result = preboot_probe || hw_inference;

      // Diagnostic context. This lambda runs exactly once, so it logs a single
      // line capturing every input that fed the decision.
      const uint32_t family = IOSReadCpuFamily();
      const int os_major = IOSProductMajorVersion();
      const bool app_on_mac = IOSRunningAsAppOnMac();
      XELOGI(
          "iOS TXM detection: result={} (preboot_probe={} hw_inference={}) "
          "cpufamily=0x{:08X} os_major={} ios_app_on_mac={}",
          result, preboot_probe, hw_inference, family, os_major, app_on_mac);

      // Warn when the two signals disagree in either direction.
      if (preboot_probe && !hw_inference) {
        XELOGW(
            "iOS TXM: preboot firmware IS present but the hardware/OS "
            "inference reported non-TXM (cpufamily=0x{:08X} os_major={} "
            "ios_app_on_mac={}). The pre-TXM family list is likely stale for "
            "this SoC; trusting the firmware and treating as TXM-capable.",
            family, os_major, app_on_mac);
      } else if (!preboot_probe && hw_inference) {
        XELOGW(
            "iOS TXM: hardware/OS indicates TXM-class (cpufamily=0x{:08X} "
            "os_major={}) but the preboot firmware probe found NOTHING "
            "(sandbox restriction or changed layout). Relying on inference; "
            "JIT will take the TXM path.",
            family, os_major);
      }

      return result;
    }();
    return has_txm;
  }

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

  // Reads hw.cpufamily. Returns 0 when it can't be determined. On an
  // iOS-app-on-Mac this reports the host Mac's SoC (see IOSRunningAsAppOnMac).
  static uint32_t IOSReadCpuFamily() {
    uint32_t family = 0;
    size_t size = sizeof(family);
    if (sysctlbyname("hw.cpufamily", &family, &size, nullptr, 0) != 0 ||
        size != sizeof(family)) {
      return 0;
    }
    return family;
  }

  // Capability inference used only when the definitive preboot probe is
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

    const uint32_t family = IOSReadCpuFamily();
    if (family == 0) {
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

  // The prepare and detach requests are brk instructions that the attached JIT
  // tool handles. A tool that sets CS_DEBUGGED and then detaches leaves no one to
  // handle them, and the brk becomes a SIGTRAP crash with no message.
  static bool IsDebuggerAttached() {
    struct kinfo_proc info = {};
    size_t size = sizeof(info);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size < sizeof(info)) {
      // Unknown: keep requesting, as before this check existed.
      return true;
    }
    return (info.kp_proc.p_flag & P_TRACED) != 0;
  }

  // Returns the region the debugger prepared.
  static uint64_t InvokeUniversalPrepareBreakpoint(uintptr_t aligned_start,
                                                   size_t aligned_length) {
    register uint64_t x0 __asm("x0") = static_cast<uint64_t>(aligned_start);
    register uint64_t x1 __asm("x1") = static_cast<uint64_t>(aligned_length);
    register uint64_t x16 __asm("x16") = 1;
    asm volatile("brk #0xf00d" : "+r"(x0), "+r"(x1), "+r"(x16) : : "memory");
    return x0;
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
    if (!IsDebuggerAttached()) {
      XELOGW("iOS JIT: the JIT tool already detached; no detach request needed");
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
    if (!IsDebuggerAttached()) {
      xe::FatalError(
          "JIT is not available: the tool that enabled it (StikDebug or a JIT "
          "broker) is no longer attached, so games can't run. Enable JIT again "
          "and keep the tool attached until the game has started.");
    }
    const IOSJitMemory::ExternalPrepareHook hook =
        external_prepare_hook.load(std::memory_order_acquire);
    if (hook) {
      hook(true);
    }
    const uint64_t prepared =
        InvokeUniversalPrepareBreakpoint(aligned_start, aligned_length);
    if (hook) {
      hook(false);
    }
    if (prepared != aligned_start) {
      XELOGE("iOS JIT: asked the debugger to prepare 0x{:X}, it prepared 0x{:X}",
             aligned_start, prepared);
    }
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
};

IOSJitMemory::IOSJitMemory() : impl_(std::make_unique<Impl>()) {}

void IOSJitMemory::SetExternalPrepareHook(ExternalPrepareHook hook) {
  external_prepare_hook.store(hook, std::memory_order_release);
}

IOSJitMemory::~IOSJitMemory() {
  if (impl_->persistent) {
    auto& state = Impl::IOSPersistentGeneratedCodeMappingState();
    std::lock_guard<std::mutex> lock(state.mutex);
    // Preparation detaches the external debugger. Keep the prepared mapping
    // across sequential emulator instances, but never share it concurrently.
    state.in_use = false;
  } else if (impl_->write) {
    munmap(impl_->write, impl_->size);
  }
}

bool IOSJitMemory::Initialize(size_t size, uint8_t*& execute_base,
                              uint8_t*& write_base) {
  if (impl_->write || !size) {
    return false;
  }
  impl_->size = xe::round_up(size, xe::memory::page_size());
  if (Impl::IOSUseTXMBrokerPath()) {
    // Preparation is process-wide and one-shot. Do not silently allocate an
    // unprepared fallback after the debugger has detached.
    if (!Impl::AcquireIOSPersistentGeneratedCodeMapping(
            impl_->execute, impl_->write, impl_->size)) {
      return false;
    }
    impl_->persistent = true;
  } else {
    auto* mapping =
        static_cast<uint8_t*>(mmap(nullptr, impl_->size, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (mapping == MAP_FAILED) {
      XELOGE("Unable to allocate iOS JIT memory: {}", std::strerror(errno));
      return false;
    }
    impl_->execute = impl_->write = mapping;
  }
  execute_base = impl_->execute;
  write_base = impl_->write;
  return true;
}

size_t IOSJitMemory::AlignOffset(size_t offset) const {
  // A W^X permission change must not affect another live allocation.
  return impl_->persistent ? offset
                           : xe::round_up(offset, xe::memory::page_size());
}

bool IOSJitMemory::BeginWrite(void* address, size_t size) {
  const uintptr_t offset = reinterpret_cast<uintptr_t>(address) -
                           reinterpret_cast<uintptr_t>(impl_->write);
  if (offset > impl_->size || size > impl_->size - offset) {
    return false;
  }
  return impl_->persistent || !size ||
         (Impl::RegionLockRead(address, size) &&
          Impl::RegionUnlockWrite(address, size));
}

bool IOSJitMemory::EndWrite(void* address, size_t size) {
  if (!size) {
    return true;
  }
  sys_dcache_flush(address, size);
  auto* execute =
      impl_->execute + (static_cast<uint8_t*>(address) - impl_->write);
  if (!impl_->persistent && !Impl::RegionSetExec(execute, size)) {
    return false;
  }
  sys_icache_invalidate(execute, size);
  return true;
}

IOSJitWriteScope::IOSJitWriteScope(IOSJitMemory& memory, void* address,
                                   size_t size)
    : memory_(memory), address_(address), size_(size) {
  if (!memory_.BeginWrite(address_, size_)) {
    xe::FatalError("Unable to make iOS JIT memory writable.");
  }
}

IOSJitWriteScope::~IOSJitWriteScope() {
  if (!memory_.EndWrite(address_, size_)) {
    xe::FatalError("Unable to make iOS JIT memory executable.");
  }
}

}  // namespace xe::cpu::backend
