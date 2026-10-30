/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_MEMORY_IOS_H_
#define XENIA_BASE_MEMORY_IOS_H_

#include "xenia/base/memory.h"

namespace xe::memory {

bool QueryProtectIOS(void* base_address, size_t& length, PageAccess& access_out);
FileMappingHandle CreateFileMappingHandleIOS(size_t length);

}  // namespace xe::memory

#endif  // XENIA_BASE_MEMORY_IOS_H_
