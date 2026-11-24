/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_STACK_LAYOUT_H_
#define XENIA_CPU_BACKEND_A64_A64_STACK_LAYOUT_H_

#include "xenia/base/vec128.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class StackLayout {
 public:
  /**
   * ARM64 Thunk Stack Layout (HostToGuest)
   * NOTE: stack must always be 16-byte aligned.
   *
   *  +------------------+
   *  | x19, x20         | sp + 0x000
   *  | x21, x22         | sp + 0x010
   *  | x23, x24         | sp + 0x020
   *  | x25, x26         | sp + 0x030
   *  | x27, x28         | sp + 0x040
   *  | x29 (fp), x30    | sp + 0x050
   *  | q8, q9           | sp + 0x060  (full 128-bit, used by JIT)
   *  | q10, q11         | sp + 0x080
   *  | q12, q13         | sp + 0x0A0
   *  | q14, q15         | sp + 0x0C0
   *  | host FPCR        | sp + 0x0E0  (HostToGuestThunk, 16 bytes)
   *  +------------------+
   *  Total: 0xF0 = 240 bytes (16-byte aligned)
   */
  static constexpr size_t THUNK_STACK_SIZE = 240;
  static constexpr size_t THUNK_HOST_FPCR = 0xE0;

  /**
   * ARM64 Guest Stack Layout
   *  +------------------+
   *  | scratch, 48b     | sp + 0x000  (3 x Q for VMX FP scratch)
   *  | guest ret addr   | sp + 0x030  (guest PPC return address)
   *  | call ret addr    | sp + 0x038  (next call's guest PPC return addr)
   *  | host ret addr    | sp + 0x040  (host x30/LR, for ret instruction)
   *  | range test failed| sp + 0x048  (byte, see EmitSingleRangeCheck)
   *  |  ... locals ...  |
   *  +------------------+
   *
   * Minimum size: 80 bytes (aligned to 16).
   *
   * Convention: at guest function entry, x0 holds the guest PPC return
   * address. The prolog stores it to GUEST_RET_ADDR and saves x30 (host
   * LR) to HOST_RET_ADDR.
   */
  static constexpr size_t GUEST_STACK_SIZE = 80;  // 16-byte aligned
  static constexpr size_t GUEST_SCRATCH = 0;      // 48 bytes (3 x Q)
  static constexpr size_t GUEST_RET_ADDR = 48;
  static constexpr size_t GUEST_CALL_RET_ADDR = 56;
  static constexpr size_t HOST_RET_ADDR = 64;
  // Nonzero once a single-range test has failed that the block reuses, see
  // EmitSingleRangeCheck. Only functions with such tests initialize it. Longjmp
  // detection state lives in A64BackendContext instead so it can be checked
  // even when native SP still points at a skipped frame.
  static constexpr size_t GUEST_RANGE_TEST_FAILED = 72;

  // The guest-to-host thunks save x30 in the top 8 bytes of their frames, so
  // the host address a guest frame's call into host code returns to sits this
  // far below that frame.
  static constexpr size_t THUNK_CALLER_RET_ADDR = 8;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_STACK_LAYOUT_H_
