/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_BACKEND_FAILURE_H_
#define XENIA_BASE_BACKEND_FAILURE_H_

#include <cstdint>

namespace xe {

// A CPU backend failure that leaves emulation broken: the game may hang or
// misbehave from here, or never start. The backend logs the details itself;
// this tells the app, which may show and record it (the iOS app does).
enum class BackendFailure {
  // A guest function couldn't be compiled.
  kCompileFailure,
  // Compiled code couldn't be placed in the code cache.
  kCodeCache,
  // The backend couldn't start: no executable memory or thunks.
  kJitUnavailable,
  // Guest code reached a state the backend can't handle, e.g. a call into
  // memory with no guest code.
  kBackendException,
};

// |guest_address|: the guest function or target, 0 if none. |continues|:
// emulation goes on after it. Called from any thread; must not block.
using BackendFailureHandler = void (*)(BackendFailure failure,
                                       uint32_t guest_address, bool continues);

void SetBackendFailureHandler(BackendFailureHandler handler);
void NoteBackendFailure(BackendFailure failure, uint32_t guest_address,
                        bool continues);

}  // namespace xe

#endif  // XENIA_BASE_BACKEND_FAILURE_H_
