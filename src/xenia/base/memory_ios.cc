/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory_ios.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>
#include <string>
#include <unistd.h>

#include "xenia/base/logging.h"

namespace xe::memory {

bool QueryProtectIOS(void* base_address, size_t& length, PageAccess& access_out) {
  length = 0;
  access_out = PageAccess::kNoAccess;
  vm_address_t address = reinterpret_cast<uintptr_t>(base_address);
  vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info{};
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;
  const kern_return_t result = vm_region_64(
      mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info), &count, &object_name);
  if (object_name != MACH_PORT_NULL) {
    mach_port_deallocate(mach_task_self(), object_name);
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(base_address);
  if (result != KERN_SUCCESS || address > base) {
    return false;
  }
  length = address + region_size - base;
  if (info.protection & VM_PROT_EXECUTE) {
    access_out = info.protection & VM_PROT_WRITE
                     ? PageAccess::kExecuteReadWrite
                     : PageAccess::kExecuteReadOnly;
  } else if (info.protection & VM_PROT_WRITE) {
    access_out = PageAccess::kReadWrite;
  } else if (info.protection & VM_PROT_READ) {
    access_out = PageAccess::kReadOnly;
  }
  return true;
}

FileMappingHandle CreateFileMappingHandleIOS(size_t length) {
  // The app sandbox does not provide a POSIX shared-memory namespace. Give
  // each mapping a private, immediately unlinked backing file instead.
  const char* temporary_directory = std::getenv("TMPDIR");
  if (!temporary_directory || !temporary_directory[0]) {
    XELOGE("TMPDIR is unavailable for the iOS memory mapping");
    return kFileMappingHandleInvalid;
  }
  std::string path = temporary_directory;
  if (path.back() != '/') {
    path += '/';
  }
  path += "xenia-mapping-XXXXXX";
  int handle = mkstemp(path.data());
  if (handle < 0) {
    XELOGE("iOS memory mapping creation failed: {}", std::strerror(errno));
    return kFileMappingHandleInvalid;
  }
  if (unlink(path.c_str()) != 0 || ftruncate(handle, length) != 0) {
    XELOGE("iOS memory mapping setup failed: {}", std::strerror(errno));
    close(handle);
    unlink(path.c_str());
    return kFileMappingHandleInvalid;
  }
  return handle;
}

}  // namespace xe::memory
