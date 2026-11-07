/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_APPS_XLIVEBASE_SOCIAL_H_
#define XENIA_KERNEL_XAM_APPS_XLIVEBASE_SOCIAL_H_

#include <cstdint>

#include "xenia/xbox.h"

namespace xe {
class Memory;
namespace kernel {
namespace xam {
namespace apps {

// XMessageEnumerate, the messages in the user's inbox from the arguments at
// |args_ptr|. There are none yet, so the inbox shows empty rather than an
// error.
X_HRESULT XMessageEnumerate(Memory* memory, uint32_t args_ptr);

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_APPS_XLIVEBASE_SOCIAL_H_
