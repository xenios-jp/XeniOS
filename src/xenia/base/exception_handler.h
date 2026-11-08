/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_EXCEPTION_HANDLER_H_
#define XENIA_BASE_EXCEPTION_HANDLER_H_

#include <cstdint>
#include <functional>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/host_thread_context.h"

namespace xe {

// AArch64 load and store decoding based on VIXL.
// https://github.com/Linaro/vixl/blob/ae5957cd66517b3f31dbf37e9bf39db6594abfe3/src/aarch64/constants-aarch64.h
//
// Copyright 2015, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// `Instruction address + literal offset` loads.
// This includes PRFM_lit.
constexpr uint32_t kArm64LoadLiteralFMask = UINT32_C(0x3B000000);
constexpr uint32_t kArm64LoadLiteralFixed = UINT32_C(0x18000000);

constexpr uint32_t kArm64LoadStoreAnyFMask = UINT32_C(0x0A000000);
constexpr uint32_t kArm64LoadStoreAnyFixed = UINT32_C(0x08000000);

constexpr uint32_t kArm64LoadStorePairAnyFMask = UINT32_C(0x3A000000);
constexpr uint32_t kArm64LoadStorePairAnyFixed = UINT32_C(0x28000000);
constexpr uint32_t kArm64LoadStorePairLoadBit = UINT32_C(1) << 22;

constexpr uint32_t kArm64LoadStoreMask = UINT32_C(0xC4C00000);
enum class Arm64LoadStoreOp : uint32_t {
  kSTRB_w = UINT32_C(0x00000000),
  kSTRH_w = UINT32_C(0x40000000),
  kSTR_w = UINT32_C(0x80000000),
  kSTR_x = UINT32_C(0xC0000000),
  kLDRB_w = UINT32_C(0x00400000),
  kLDRH_w = UINT32_C(0x40400000),
  kLDR_w = UINT32_C(0x80400000),
  kLDR_x = UINT32_C(0xC0400000),
  kLDRSB_x = UINT32_C(0x00800000),
  kLDRSH_x = UINT32_C(0x40800000),
  kLDRSW_x = UINT32_C(0x80800000),
  kLDRSB_w = UINT32_C(0x00C00000),
  kLDRSH_w = UINT32_C(0x40C00000),
  kSTR_b = UINT32_C(0x04000000),
  kSTR_h = UINT32_C(0x44000000),
  kSTR_s = UINT32_C(0x84000000),
  kSTR_d = UINT32_C(0xC4000000),
  kSTR_q = UINT32_C(0x04800000),
  kLDR_b = UINT32_C(0x04400000),
  kLDR_h = UINT32_C(0x44400000),
  kLDR_s = UINT32_C(0x84400000),
  kLDR_d = UINT32_C(0xC4400000),
  kLDR_q = UINT32_C(0x04C00000),
  kPRFM = UINT32_C(0xC0800000),
};

constexpr uint32_t kArm64LoadStoreOffsetFMask = UINT32_C(0x3B200C00);
enum class Arm64LoadStoreOffsetFixed : uint32_t {
  kUnscaledOffset = UINT32_C(0x38000000),
  kPostIndex = UINT32_C(0x38000400),
  kPreIndex = UINT32_C(0x38000C00),
  kRegisterOffset = UINT32_C(0x38200800),
};

constexpr uint32_t kArm64LoadStoreUnsignedOffsetFMask = UINT32_C(0x3B000000);
constexpr uint32_t kArm64LoadStoreUnsignedOffsetFixed = UINT32_C(0x39000000);

// Load/store exclusive class: LDXR/STXR, LDAXR/STLXR, LDAR/STLR, LDXP/STXP,
// and the LSE CAS/CASP family. Bit 22 selects load (1) / store (0) for the
// LDXR/STXR/LDAR/STLR/LDXP/STXP forms; CAS variants read-modify-write and are
// treated as stores by the caller.
constexpr uint32_t kArm64LoadStoreExclusiveFMask = UINT32_C(0x3F000000);
constexpr uint32_t kArm64LoadStoreExclusiveFixed = UINT32_C(0x08000000);
constexpr uint32_t kArm64LoadStoreExclusiveLoadBit = UINT32_C(1) << 22;
// CAS/CASP subset within the exclusive class: bit 23 = 1 and bit 21 = 1.
constexpr uint32_t kArm64CompareAndSwapFMask = UINT32_C(0x3FA00000);
constexpr uint32_t kArm64CompareAndSwapFixed = UINT32_C(0x08A00000);

// LSE atomic memory ops: LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN
// and SWP, all sized and ordered variants. These atomically read and write
// the target word.
constexpr uint32_t kArm64AtomicMemoryFMask = UINT32_C(0x3F200C00);
constexpr uint32_t kArm64AtomicMemoryFixed = UINT32_C(0x38200000);

// Advanced SIMD load/store of multiple or single structures, in both
// no-offset and post-indexed forms (LD1..LD4 / ST1..ST4). Bit 22 selects
// load (1) / store (0). Apple Silicon libc memcpy/memset emit these.
constexpr uint32_t kArm64NeonLoadStoreStructFMask = UINT32_C(0xBE000000);
constexpr uint32_t kArm64NeonLoadStoreStructFixed = UINT32_C(0x0C000000);
constexpr uint32_t kArm64NeonLoadStoreStructLoadBit = UINT32_C(1) << 22;

// SYS instruction class (DC, IC, AT, TLBI). A fault on one is a store unless
// it's a cache maintenance op by VA. Apple Silicon libc memset emits DC ZVA.
constexpr uint32_t kArm64SystemSysFMask = UINT32_C(0xFFF80000);
constexpr uint32_t kArm64SystemSysFixed = UINT32_C(0xD5080000);
// SYS #3, C7, Cm, #1: DC ZVA (Cm 4), IC IVAU (5) and DC CVAC, CVAU, CVAP,
// CVADP, CIVAC (10 to 14). All but DC ZVA need only read access.
constexpr uint32_t kArm64CacheOpByVaFMask = UINT32_C(0xFFFFF0E0);
constexpr uint32_t kArm64CacheOpByVaFixed = UINT32_C(0xD50B7020);
constexpr uint32_t kArm64DcZvaFMask = UINT32_C(0xFFFFFFE0);
constexpr uint32_t kArm64DcZvaFixed = UINT32_C(0xD50B7420);

bool IsArm64LoadPrefetchStore(uint32_t instruction, bool& is_store_out);

class Exception {
 public:
  enum class Code {
    kInvalidException = 0,
    kAccessViolation,
    kIllegalInstruction,
  };

  enum class AccessViolationOperation {
    kUnknown,
    kRead,
    kWrite,
  };

  void InitializeAccessViolation(HostThreadContext* thread_context,
                                 uint64_t fault_address,
                                 AccessViolationOperation operation) {
    code_ = Code::kAccessViolation;
    thread_context_ = thread_context;
    fault_address_ = fault_address;
    access_violation_operation_ = operation;
  }
  void InitializeIllegalInstruction(HostThreadContext* thread_context) {
    code_ = Code::kIllegalInstruction;
    thread_context_ = thread_context;
  }

  Code code() const { return code_; }

  // Returns the platform-specific thread context info.
  // Note that certain registers must be modified through Modify* proxy
  // functions rather than directly:
  // x86-64:
  // - General-purpose registers (r##, r8-r15).
  // - XMM registers.
  // AArch64:
  // - General-purpose registers (Xn), including FP and LR.
  // - SIMD and floating-point registers (Vn).
  HostThreadContext* thread_context() const { return thread_context_; }

  // Returns the program counter where the exception occurred.
  uint64_t pc() const {
#if XE_ARCH_AMD64
    return thread_context_->rip;
#elif XE_ARCH_ARM64
    return thread_context_->pc;
#else
    assert_always();
    return 0;
#endif  // XE_ARCH
  }

  // Sets the program counter where execution will resume.
  void set_resume_pc(uint64_t pc) {
#if XE_ARCH_AMD64
    thread_context_->rip = pc;
#elif XE_ARCH_ARM64
    thread_context_->pc = pc;
#else
    assert_always();
#endif  // XE_ARCH
  }

#if XE_ARCH_AMD64
  // The index is relative to X64Register::kIntRegisterFirst.
  uint64_t& ModifyIntRegister(uint32_t index) {
    assert_true(index <= 15);
    modified_int_registers_ |= UINT16_C(1) << index;
    return thread_context_->int_registers[index];
  }
  uint16_t modified_int_registers() const { return modified_int_registers_; }
  vec128_t& ModifyXmmRegister(uint32_t index) {
    assert_true(index <= 15);
    modified_xmm_registers_ |= UINT16_C(1) << index;
    return thread_context_->xmm_registers[index];
  }
  uint16_t modified_xmm_registers() const { return modified_xmm_registers_; }
#elif XE_ARCH_ARM64
  uint64_t& ModifyXRegister(uint32_t index) {
    assert_true(index <= 30);
    modified_x_registers_ |= UINT32_C(1) << index;
    return thread_context_->x[index];
  }
  uint32_t modified_x_registers() const { return modified_x_registers_; }
  vec128_t& ModifyVRegister(uint32_t index) {
    assert_true(index <= 31);
    modified_v_registers_ |= UINT32_C(1) << index;
    return thread_context_->v[index];
  }
  uint32_t modified_v_registers() const { return modified_v_registers_; }
#endif  // XE_ARCH

  // In case of AV, address that was read from/written to.
  uint64_t fault_address() const { return fault_address_; }

  // In case of AV, what kind of operation caused it.
  AccessViolationOperation access_violation_operation() const {
    return access_violation_operation_;
  }

 private:
  Code code_ = Code::kInvalidException;
  HostThreadContext* thread_context_ = nullptr;
#if XE_ARCH_AMD64
  uint16_t modified_int_registers_ = 0;
  uint16_t modified_xmm_registers_ = 0;
#elif XE_ARCH_ARM64
  uint32_t modified_x_registers_ = 0;
  uint32_t modified_v_registers_ = 0;
#endif  // XE_ARCH
  uint64_t fault_address_ = 0;
  AccessViolationOperation access_violation_operation_ =
      AccessViolationOperation::kUnknown;
};

// Puts the calling thread's FPCR or MXCSR in the host ABI default: round to
// nearest, no flush-to-zero, exceptions masked.
void SetHostDefaultFpControl();

// Resumes the faulting thread in |thunk|, which must not return, rather than at
// the faulting instruction. Diverting the pc keeps the fault's rsp, so realign
// to the rsp%16 == 8 a call leaves, or the thunk's aligned SSE spills fault.
// The null slot ends a stack walk. AArch64 sp is always aligned.
inline void DivertToThunk(Exception* ex, void (*thunk)()) {
#if XE_ARCH_AMD64
  uint64_t& rsp = ex->ModifyIntRegister(
      uint32_t(X64Register::kRsp) - uint32_t(X64Register::kIntRegisterFirst));
  rsp = (rsp & ~uint64_t(15)) - 8;
  *reinterpret_cast<uint64_t*>(rsp) = 0;
#endif  // XE_ARCH_AMD64
  ex->set_resume_pc(reinterpret_cast<uint64_t>(thunk));
}

#if !XE_PLATFORM_WIN32
// Resumes the calling thread in |context|, which it must have faulted with. x64
// clobbers the 8 bytes below sp and keeps MXCSR, a64 restores FPCR and FPSR.
[[noreturn]] XE_NOINLINE void ResumeHostContext(
    const HostThreadContext* context);
#endif  // !XE_PLATFORM_WIN32

class ExceptionHandler {
 public:
  typedef bool (*Handler)(Exception* ex, void* data);

  // Installs an exception handler.
  // Handlers are called in the order they are installed.
  static void Install(Handler fn, void* data);

  // Uninstalls a previously-installed exception handler.
  static void Uninstall(Handler fn, void* data);

#if !XE_PLATFORM_WIN32
  // Called on the faulting thread for a fault no handler claimed, just before
  // the original disposition takes it (usually the end of the process). From a
  // signal handler: async-signal-safe work only. nullptr removes it.
  typedef void (*UnhandledObserver)(int signal_number, uint64_t fault_address,
                                    const HostThreadContext* context);
  static void SetUnhandledObserver(UnhandledObserver observer);

#if XE_PLATFORM_IOS
  // How many times a "brk #0xf00d" JIT request reached the signal handler,
  // which means no debugger answered it. The handler skips the instruction;
  // the caller compares the count around its request.
  static uint32_t UnansweredJitBreakpoints();
#endif
#endif  // !XE_PLATFORM_WIN32
};

}  // namespace xe

#endif  // XENIA_BASE_EXCEPTION_HANDLER_H_
