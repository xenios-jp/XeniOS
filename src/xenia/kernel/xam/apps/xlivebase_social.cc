/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_social.h"

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

namespace {

// One argument of a Live call: a pointer to its value (layouts from the Xenia
// Netplay fork).
struct X_ARGUMENT_ENTRY {
  xe::be<uint32_t> native_size;
  xe::be<uint64_t> argument_value_ptr;
};
static_assert_size(X_ARGUMENT_ENTRY, 0x10);

// XMessageEnumerate's arguments: the user's XUID, the message count to set
// and the summaries to fill.
struct X_MESSAGE_ENUMERATE {
  X_ARGUMENT_ENTRY xuid;
  X_ARGUMENT_ENTRY count_ptr;
  X_ARGUMENT_ENTRY summaries_ptr;
};
static_assert_size(X_MESSAGE_ENUMERATE, 0x30);

uint32_t ArgPointer(const X_ARGUMENT_ENTRY& entry) {
  return static_cast<uint32_t>(entry.argument_value_ptr);
}

}  // namespace

X_HRESULT XMessageEnumerate(Memory* memory, uint32_t args_ptr) {
  if (!args_ptr) {
    return X_E_INVALIDARG;
  }
  const auto* args =
      memory->TranslateVirtual<const X_MESSAGE_ENUMERATE*>(args_ptr);
  const uint32_t count_ptr = ArgPointer(args->count_ptr);
  if (!count_ptr) {
    return X_E_INVALIDARG;
  }
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(count_ptr), 0);
  XELOGD("XMessageEnumerate: 0 messages");
  return X_E_SUCCESS;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
