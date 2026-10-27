/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_SPIN_WAIT_INJECTION_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_SPIN_WAIT_INJECTION_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Tags the header of each polling loop, of at most kMaxBodyGuestInstrs guest
// instructions that load memory and make a direct call but store no memory,
// make no indirect call and count nothing, with a DELAY_EXECUTION_INJECTED for
// the backend to release the core from it. Only added for backends with
// MachineInfo::supports_spin_wait_release.
class SpinWaitInjectionPass : public CompilerPass {
 public:
  SpinWaitInjectionPass();
  ~SpinWaitInjectionPass() override;

  bool Run(hir::HIRBuilder* builder) override;

  static constexpr uint32_t kMaxBodyGuestInstrs = 8;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_SPIN_WAIT_INJECTION_PASS_H_
