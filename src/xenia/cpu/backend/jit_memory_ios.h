/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_JIT_MEMORY_IOS_H_
#define XENIA_CPU_BACKEND_JIT_MEMORY_IOS_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace xe::cpu::backend {

// Owns the iOS JIT mapping and its write/execute permission transitions.
// The code cache retains Edge's allocation, dispatch and unwind algorithms.
class IOSJitMemory {
 public:
  IOSJitMemory();
  ~IOSJitMemory();
  IOSJitMemory(const IOSJitMemory&) = delete;
  IOSJitMemory& operator=(const IOSJitMemory&) = delete;

  bool Initialize(size_t size, uint8_t*& execute_base, uint8_t*& write_base);

  // Called with true just before the app asks the JIT tool to prepare the code
  // region, which stops every thread until the tool answers, and with false
  // after. Set once, before the first Initialize.
  using ExternalPrepareHook = void (*)(bool preparing);
  static void SetExternalPrepareHook(ExternalPrepareHook hook);
  size_t AlignOffset(size_t offset) const;
  bool BeginWrite(void* address, size_t size);
  bool EndWrite(void* address, size_t size);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The scope must include both machine code and unwind-data writes.
class IOSJitWriteScope {
 public:
  IOSJitWriteScope(IOSJitMemory& memory, void* address, size_t size);
  ~IOSJitWriteScope();
  IOSJitWriteScope(const IOSJitWriteScope&) = delete;
  IOSJitWriteScope& operator=(const IOSJitWriteScope&) = delete;

 private:
  IOSJitMemory& memory_;
  void* address_;
  size_t size_;
};

}  // namespace xe::cpu::backend
#endif  // XENIA_CPU_BACKEND_JIT_MEMORY_IOS_H_
