/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

#if XE_PLATFORM_APPLE
#include <mach/mach.h>
#if XE_PLATFORM_MAC
#include <mach/mach_vm.h>
#endif
#include <mach/vm_region.h>
#endif  // XE_PLATFORM_APPLE

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_ANDROID
#include <dlfcn.h>
#include <linux/ashmem.h>
#include <string.h>
#include <sys/ioctl.h>

#include "xenia/base/main_android.h"
#endif

namespace xe {
namespace memory {

#if XE_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (xe::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ =
          reinterpret_cast<decltype(android_ASharedMemory_create_)>(
              dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() {
  // getpagesize() is fixed for the process lifetime but showed up as ~0.3% of
  // total CPU (a repeated syscall on hot paths); cache it once.
  static const size_t cached_page_size = getpagesize();
  return cached_page_size;
}
size_t allocation_granularity() { return page_size(); }

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

PageAccess ToXeniaProtectFlags(const char* protection) {
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == 'x') {
    return PageAccess::kExecuteReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == 'x') {
    return PageAccess::kExecuteReadOnly;
  }
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == '-') {
    return PageAccess::kReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == '-') {
    return PageAccess::kReadOnly;
  }
  return PageAccess::kNoAccess;
}

bool IsWritableExecutableMemorySupported() {
#if XE_PLATFORM_APPLE
#if XE_PLATFORM_IOS
  // iOS app builds don't have MAP_JIT entitlement in this project setup.
  // Use the split execute/write mapping path instead.
  return false;
#else
  // macOS allows RWX only on anonymous MAP_JIT regions. Callers that see
  // true must allocate via AllocFixed (which sets MAP_JIT) and toggle
  // pthread_jit_write_protect_np around writes. MAP_JIT requires the
  // com.apple.security.cs.allow-jit entitlement; without it the probe
  // fails and JIT is disabled.
  static const bool supported = []() {
    const size_t test_size = page_size();
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    flags |= MAP_JIT;
#endif
    void* test_mapping = mmap(nullptr, test_size,
                              PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    if (test_mapping == MAP_FAILED) {
      XELOGE("MAP_JIT probe failed: {} ({}); JIT will not work",
             strerror(errno), errno);
      return false;
    }
    munmap(test_mapping, test_size);
    return true;
  }();
  return supported;
#endif  // XE_PLATFORM_IOS
#else
  return true;
#endif  // XE_PLATFORM_APPLE
}

struct MappedFileRange {
  uintptr_t region_begin;
  uintptr_t region_end;
};

std::vector<MappedFileRange> mapped_file_ranges;
std::mutex g_mapped_file_ranges_mutex;

// Track shm file names for cleanup on exit
std::vector<std::string> g_shm_file_names;
std::mutex g_shm_file_names_mutex;
static bool g_cleanup_handlers_installed = false;

#if !XE_PLATFORM_ANDROID
static void CleanupAtExit() {
  for (const auto& name : g_shm_file_names) {
    shm_unlink(name.c_str());
  }
}

static void InstallCleanupHandlers() {
  if (g_cleanup_handlers_installed) {
    return;
  }
  g_cleanup_handlers_installed = true;

  std::atexit(CleanupAtExit);
  std::at_quick_exit(CleanupAtExit);
}
#endif  // !XE_PLATFORM_ANDROID

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  // mmap does not support reserve / commit, so ignore allocation_type.
  uint32_t prot = ToPosixProtectFlags(access);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#if XE_PLATFORM_APPLE
  if (access == PageAccess::kExecuteReadWrite ||
      access == PageAccess::kExecuteReadOnly) {
#if !XE_PLATFORM_IOS
#ifdef MAP_JIT
    flags |= MAP_JIT;
#endif
#endif
  }
#endif  // XE_PLATFORM_APPLE

  if (base_address != nullptr) {
    if (allocation_type == AllocationType::kCommit) {
#if XE_PLATFORM_APPLE
      size_t host_page_size = page_size();
      uintptr_t aligned_start =
          reinterpret_cast<uintptr_t>(base_address) & ~(host_page_size - 1);
      uintptr_t aligned_end = xe::align(
          reinterpret_cast<uintptr_t>(base_address) + length, host_page_size);
      size_t aligned_length =
          aligned_end > aligned_start ? aligned_end - aligned_start : 0;
      if (!aligned_length) {
        return base_address;
      }
      return mprotect(reinterpret_cast<void*>(aligned_start), aligned_length,
                      prot) == 0
                 ? base_address
                 : nullptr;
#else
      if (Protect(base_address, length, access)) {
        return base_address;
      }
      return nullptr;
#endif  // XE_PLATFORM_APPLE
    }
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
  }

  void* result = mmap(base_address, length, prot, flags, -1, 0);

  if (result == MAP_FAILED) {
    return nullptr;
  }

  if (base_address != nullptr && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  const auto region_begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t region_end =
      reinterpret_cast<uintptr_t>(base_address) + length;

  std::lock_guard guard(g_mapped_file_ranges_mutex);
  for (const auto& mapped_range : mapped_file_ranges) {
    if (region_begin >= mapped_range.region_begin &&
        region_end <= mapped_range.region_end) {
      switch (deallocation_type) {
        case DeallocationType::kDecommit:
          return Protect(base_address, length, PageAccess::kNoAccess);
        case DeallocationType::kRelease:
          return false;
        default:
          assert_unhandled_case(deallocation_type);
      }
    }
  }

  switch (deallocation_type) {
    case DeallocationType::kDecommit:
      return Protect(base_address, length, PageAccess::kNoAccess);
    case DeallocationType::kRelease:
      return munmap(base_address, length) == 0;
    default:
      assert_unhandled_case(deallocation_type);
  }
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (out_old_access) {
    size_t length_copy = length;
    QueryProtect(base_address, length_copy, *out_old_access);
  }

  uint32_t prot = ToPosixProtectFlags(access);
  int ret = mprotect(base_address, length, prot);
  if (ret != 0) {
    XELOGE("mprotect({}, 0x{:X}, {}) failed: {} ({})", base_address, length,
           prot, strerror(errno), errno);
  }
  return ret == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if XE_PLATFORM_APPLE
  access_out = PageAccess::kNoAccess;
#if XE_PLATFORM_MAC
  mach_vm_address_t address =
      static_cast<mach_vm_address_t>(reinterpret_cast<uintptr_t>(base_address));
  mach_vm_size_t region_size = 0;
#else
  vm_address_t address =
      static_cast<vm_address_t>(reinterpret_cast<uintptr_t>(base_address));
  vm_size_t region_size = 0;
#endif
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;

#if XE_PLATFORM_MAC
  kern_return_t kr = mach_vm_region(
      mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
#else
  kern_return_t kr = vm_region_64(
      mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
#endif

  if (object_name != MACH_PORT_NULL) {
    mach_port_deallocate(mach_task_self(), object_name);
  }

  if (kr != KERN_SUCCESS) {
    return false;
  }

  auto base = reinterpret_cast<uintptr_t>(base_address);
  if (address > base) {
    return false;
  }

  length = static_cast<size_t>((address + region_size) - base);

  const vm_prot_t prot = info.protection;
  const bool can_read = (prot & VM_PROT_READ) != 0;
  const bool can_write = (prot & VM_PROT_WRITE) != 0;
  const bool can_execute = (prot & VM_PROT_EXECUTE) != 0;
  if (can_read && can_write && can_execute) {
    access_out = PageAccess::kExecuteReadWrite;
  } else if (can_read && can_execute) {
    access_out = PageAccess::kExecuteReadOnly;
  } else if (can_read && can_write) {
    access_out = PageAccess::kReadWrite;
  } else if (can_read) {
    access_out = PageAccess::kReadOnly;
  } else {
    access_out = PageAccess::kNoAccess;
  }

  return true;
#else
  // No generic POSIX solution exists. The Linux solution should work on all
  // Linux kernel based OS, including Android.
  std::ifstream memory_maps;
  memory_maps.open("/proc/self/maps", std::ios_base::in);
  std::string maps_entry_string;

  while (std::getline(memory_maps, maps_entry_string)) {
    std::stringstream entry_stream(maps_entry_string);
    uintptr_t map_region_begin, map_region_end;
    char separator;
    char protection[5];  // 4 chars (e.g., "r-xp") + null terminator

    entry_stream >> std::hex >> map_region_begin >> separator >>
        map_region_end >> protection;

    if (map_region_begin <= reinterpret_cast<uintptr_t>(base_address) &&
        map_region_end > reinterpret_cast<uintptr_t>(base_address)) {
      length = map_region_end - reinterpret_cast<uintptr_t>(base_address);

      access_out = ToXeniaProtectFlags(protection);

      // Look at the next consecutive mappings
      while (std::getline(memory_maps, maps_entry_string)) {
        std::stringstream next_entry_stream(maps_entry_string);
        uintptr_t next_map_region_begin, next_map_region_end;
        char next_protection[5];  // 4 chars (e.g., "r-xp") + null terminator

        next_entry_stream >> std::hex >> next_map_region_begin >> separator >>
            next_map_region_end >> next_protection;
        if (map_region_end == next_map_region_begin &&
            access_out == ToXeniaProtectFlags(next_protection)) {
          length =
              next_map_region_end - reinterpret_cast<uintptr_t>(base_address);
          continue;
        }
        break;
      }

      memory_maps.close();
      return true;
    }
  }

  memory_maps.close();
  return false;
#endif  // XE_PLATFORM_APPLE
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
#if XE_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? sharedmem_fd : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), xe::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return ashmem_fd;
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;

#if XE_PLATFORM_IOS
  // iOS app sandboxing may reject POSIX shared memory namespaces, so use an
  // unlinked temporary file-backed mapping instead.
  const char* tmpdir_env = std::getenv("TMPDIR");
  if (!tmpdir_env || !tmpdir_env[0]) {
    XELOGE("TMPDIR is unavailable for iOS file mapping fallback.");
    return kFileMappingHandleInvalid;
  }
  std::string backing_path(tmpdir_env);
  if (backing_path.back() != '/') {
    backing_path.push_back('/');
  }
  backing_path += path.filename().string();
  int open_flags = oflag;
  if (open_flags & O_RDWR) {
    open_flags |= O_TRUNC;
  }
  int ret = open(backing_path.c_str(), open_flags, 0600);
  if (ret < 0) {
    XELOGE("open({}) failed: {} ({})", backing_path, strerror(errno), errno);
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, length) < 0) {
    XELOGE("ftruncate({}, 0x{:X}) failed: {} ({})", backing_path, length,
           strerror(errno), errno);
    close(ret);
    unlink(backing_path.c_str());
    return kFileMappingHandleInvalid;
  }
  if (unlink(backing_path.c_str()) != 0) {
    XELOGW("unlink({}) failed: {} ({})", backing_path, strerror(errno), errno);
  }
  return ret;
#else
#if XE_PLATFORM_APPLE
  std::string shm_name = "/" + path.filename().string();
  if (shm_name.size() > 30) {
    std::size_t h = std::hash<std::string>{}(shm_name);
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "/%016zx", h);
    shm_name = hash_buf;
  }
  int ret = shm_open(shm_name.c_str(), oflag, 0777);
  if (ret < 0) {
    XELOGE("shm_open({}) failed: {} ({})", shm_name, strerror(errno), errno);
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, length) < 0) {
    XELOGE("ftruncate({}, 0x{:X}) failed: {} ({})", shm_name, length,
           strerror(errno), errno);
    close(ret);
    shm_unlink(shm_name.c_str());
    return kFileMappingHandleInvalid;
  }
  // Track for cleanup on abnormal exit and install cleanup handlers
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    g_shm_file_names.push_back(shm_name);
  }
  InstallCleanupHandlers();
  return ret;
#else
  auto full_path = "/" / path;
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    XELOGE("shm_open({}) failed: {} ({})", full_path.string(), strerror(errno),
           errno);
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, length) < 0) {
    XELOGE("ftruncate({}, 0x{:X}) failed: {} ({})", full_path.string(), length,
           strerror(errno), errno);
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  // Track for cleanup on abnormal exit and install cleanup handlers
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    g_shm_file_names.push_back(full_path.string());
  }
  InstallCleanupHandlers();
  return ret;
#endif  // XE_PLATFORM_APPLE
#endif  // XE_PLATFORM_IOS
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  close(handle);
#if !XE_PLATFORM_ANDROID
#if XE_PLATFORM_IOS
  // iOS uses already-unlinked temporary file-backed mappings.
  return;
#endif
#if XE_PLATFORM_APPLE
  std::string shm_name = "/" + path.filename().string();
  if (shm_name.size() > 30) {
    std::size_t h = std::hash<std::string>{}(shm_name);
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "/%016zx", h);
    shm_name = hash_buf;
  }
  shm_unlink(shm_name.c_str());
  // Remove from tracking
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    auto it =
        std::find(g_shm_file_names.begin(), g_shm_file_names.end(), shm_name);
    if (it != g_shm_file_names.end()) {
      g_shm_file_names.erase(it);
    }
  }
#else
  auto full_path = "/" / path;
  shm_unlink(full_path.c_str());
  // Remove from tracking
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    auto it = std::find(g_shm_file_names.begin(), g_shm_file_names.end(),
                        full_path.string());
    if (it != g_shm_file_names.end()) {
      g_shm_file_names.erase(it);
    }
  }
#endif  // XE_PLATFORM_APPLE
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
  uint32_t prot = ToPosixProtectFlags(access);

  int flags = MAP_SHARED;
  if (base_address != nullptr) {
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
  }

  void* result = mmap(base_address, length, prot, flags, handle, file_offset);

  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Without MAP_FIXED_NOREPLACE (e.g. macOS), a non-null base_address is just
  // a hint. Enforce the caller's contract by failing on address mismatch so
  // callers can retry at a different base, matching AllocFixed's behavior.
  if (base_address != nullptr && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  std::lock_guard guard(g_mapped_file_ranges_mutex);
  mapped_file_ranges.push_back({reinterpret_cast<uintptr_t>(result),
                                reinterpret_cast<uintptr_t>(result) + length});
  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  std::lock_guard guard(g_mapped_file_ranges_mutex);

#if XE_PLATFORM_APPLE
  uintptr_t unmap_begin = reinterpret_cast<uintptr_t>(base_address);
  uintptr_t unmap_end = unmap_begin + length;

  for (auto mapped_range = mapped_file_ranges.begin();
       mapped_range != mapped_file_ranges.end(); ++mapped_range) {
    if (unmap_begin >= mapped_range->region_begin &&
        unmap_end <= mapped_range->region_end) {
      uintptr_t orig_begin = mapped_range->region_begin;
      uintptr_t orig_end = mapped_range->region_end;
      mapped_file_ranges.erase(mapped_range);

      if (orig_begin < unmap_begin) {
        mapped_file_ranges.push_back({orig_begin, unmap_begin});
      }
      if (unmap_end < orig_end) {
        mapped_file_ranges.push_back({unmap_end, orig_end});
      }

      return munmap(base_address, length) == 0;
    }
  }

  return munmap(base_address, length) == 0;
#else
  for (auto mapped_range = mapped_file_ranges.begin();
       mapped_range != mapped_file_ranges.end();) {
    if (mapped_range->region_begin ==
            reinterpret_cast<uintptr_t>(base_address) &&
        mapped_range->region_end ==
            reinterpret_cast<uintptr_t>(base_address) + length) {
      mapped_file_ranges.erase(mapped_range);
      return munmap(base_address, length) == 0;
    }
    ++mapped_range;
  }
  // TODO: Implement partial file unmapping.
  assert_always("Error: Partial unmapping of files not yet supported.");
  return munmap(base_address, length) == 0;
#endif  // XE_PLATFORM_APPLE
}

}  // namespace memory
}  // namespace xe
