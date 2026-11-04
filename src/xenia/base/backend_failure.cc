/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/backend_failure.h"

#include <atomic>

namespace xe {

namespace {
std::atomic<BackendFailureHandler> backend_failure_handler_{nullptr};
}  // namespace

void SetBackendFailureHandler(BackendFailureHandler handler) {
  backend_failure_handler_.store(handler, std::memory_order_release);
}

void NoteBackendFailure(BackendFailure failure, uint32_t guest_address,
                        bool continues) {
  if (BackendFailureHandler handler =
          backend_failure_handler_.load(std::memory_order_acquire)) {
    handler(failure, guest_address, continues);
  }
}

}  // namespace xe
