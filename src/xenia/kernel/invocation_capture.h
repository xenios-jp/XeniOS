/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_INVOCATION_CAPTURE_H_
#define XENIA_KERNEL_INVOCATION_CAPTURE_H_

#include <atomic>
#include <string_view>

#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace kernel {

class KernelState;

// Records real invocations of the guest functions using the most host CPU
// into the JIT corpus, for xenia-cpu-replay to run and time offline.
//
// jit_corpus_capture_after seconds in, host thread program counters are
// sampled for a while and mapped to the guest functions they are in. Calls to
// the hottest then go through an entry hook, which for a few of them each
// stops every other guest thread, records all of guest memory and the
// registers, runs the function to its return while recording what the kernel
// exports it calls return, and records the registers and memory it ended
// with. One that calls an export the replay cannot reproduce, blocks or runs
// too long is dropped. Anything else that wrote memory meanwhile, such as the
// GPU, is sorted out by the replay.
void StartInvocationCapture(KernelState* kernel_state);
void StopInvocationCapture();

// The context of the invocation being captured, if any.
extern std::atomic<cpu::ppc::PPCContext*> capturing_context;

// Called by an export trampoline on capturing_context, before the export runs
// and once it returned.
void CaptureExportCall(cpu::ppc::PPCContext* context,
                       const cpu::Export* export_entry, bool returned);

// Reproduces what an export did to guest memory, from the registers it was
// called with, the guest KTHREAD calling it and what it returned in r3. There
// is one for every export a captured invocation may call.
using ExportModel = void (*)(cpu::ppc::PPCContext* context, uint32_t thread,
                             uint64_t result);
ExportModel FindExportModel(std::string_view name);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_INVOCATION_CAPTURE_H_
