/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The return site of a guest call has two jobs on A64:
//
// - FindLiveReturn (a64_backend.cc) takes a stackpoint only when the host
//   return address the call left equals MapGuestAddressToMachineCode() of the
//   guest return address. A return site whose mapped code starts anywhere but
//   right after the call's blr fails that check for every return, so dynamic
//   code that returns through ResolveFunction (XeFu) falls back to
//   ResolveLongjmp on every return and stalls. Halo 2 under XeFu hung at boot
//   this way when the w7 reload was emitted between the blr and the return
//   site.
// - Where the host maps coarser than 4 KB, w7 holds the physical remap bound
//   for the whole function, and a call can reach host code that clobbers it,
//   so the return site has to reload it before any guest instruction runs.
//
// Both are checked on the machine code: the instruction before the mapped
// return site is the blr, and the mapped return site starts with the reload.

#include "xenia/base/platform.h"

#if XE_ARCH_ARM64

#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/testing/util.h"

DECLARE_bool(a64_enable_host_guest_stack_synchronization);

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;

namespace {

constexpr uint32_t kCallerAddr = 0x80000000;
constexpr uint32_t kReturnSiteAddr = kCallerAddr + 4;
constexpr uint32_t kCalleeAddr = 0x80001000;
constexpr uint64_t kSentinel = 0xCAFEBEEFD00DF00Dull;
constexpr uint64_t kCanary = 0x1111111122222222ull;

uint32_t InstructionAt(uintptr_t address) {
  uint32_t instruction;
  std::memcpy(&instruction, reinterpret_cast<const void*>(address),
              sizeof(instruction));
  return instruction;
}

// BLR Xn.
bool IsBlr(uint32_t instruction) {
  return (instruction & 0xFFFFFC1Fu) == 0xD63F0000u;
}

// MOVZ W7, #0xE000, LSL #16, which is ReloadPhysicalRemapBound().
constexpr uint32_t kReloadPhysicalRemapBound = 0x52BC0007u;

enum class CallKind { kDirect, kIndirect };

// Builds a caller whose call at kCallerAddr returns to kReturnSiteAddr, the
// way the PPC frontend marks a bl, checks the machine code around the return
// site, then runs it.
void CheckReturnSite(CallKind kind) {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  int gen_invocation = 0;
  Function* callee_fn = nullptr;
  auto module_owner = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) {
        return address == kCallerAddr || address == kCalleeAddr;
      },
      [&](HIRBuilder& b) {
        if (gen_invocation++ == 0) {
          StoreGPR(b, 3, b.LoadConstantUint64(kSentinel));
          b.Return();
          return true;
        }
        b.SourceOffset(kCallerAddr);
        // A value live across the call, so the return site has guest work.
        auto live = LoadGPR(b, 4);
        if (kind == CallKind::kDirect) {
          REQUIRE(callee_fn != nullptr);
          b.Call(callee_fn);
        } else {
          // From the context, so no pass folds it into a direct call.
          b.CallIndirect(LoadGPR(b, 9));
        }
        b.SourceOffset(kReturnSiteAddr);
        StoreGPR(b, 20, live);
        b.Return();
        return true;
      },
      // The block after a call has no fallthrough edge in this HIR, so the
      // simplification pass would drop it (see GUEST_TO_GUEST_CALL).
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module_owner));
  processor->backend()->CommitExecutableRange(kCallerAddr,
                                              kCalleeAddr + 0x1000);

  callee_fn = processor->ResolveFunction(kCalleeAddr);
  REQUIRE(callee_fn != nullptr);
  auto* caller_fn =
      static_cast<GuestFunction*>(processor->ResolveFunction(kCallerAddr));
  REQUIRE(caller_fn != nullptr);

  const uintptr_t return_site =
      caller_fn->MapGuestAddressToMachineCode(kReturnSiteAddr);
  REQUIRE(return_site != 0);
  const uintptr_t code_begin =
      reinterpret_cast<uintptr_t>(caller_fn->machine_code());
  REQUIRE(return_site >= code_begin + 4);

  // The call returns to exactly the mapped return site.
  INFO("instruction before the return site: 0x"
       << std::hex << InstructionAt(return_site - 4));
  REQUIRE(IsBlr(InstructionAt(return_site - 4)));

  // The return site, which is also where a longjmp into the frame lands,
  // restores w7 before any guest code.
  if (xe::memory::allocation_granularity() > 0x1000) {
    INFO("first instruction of the return site: 0x"
         << std::hex << InstructionAt(return_site));
    REQUIRE(InstructionAt(return_site) == kReloadPhysicalRemapBound);
  }

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  {
    auto thread_state = std::make_unique<ThreadState>(
        processor.get(), 0x100, stack_address + stack_size);
    auto ctx = thread_state->context();
    ctx->lr = 0xBCBCBCBC;
    ctx->r[3] = 0;
    ctx->r[4] = kCanary;
    ctx->r[9] = kCalleeAddr;
    caller_fn->Call(thread_state.get(), uint32_t(ctx->lr));
    REQUIRE(ctx->r[3] == kSentinel);
    REQUIRE(ctx->r[20] == kCanary);
  }
  memory->SystemHeapFree(stack_address);
}

}  // namespace

TEST_CASE("A64_CALL_RETURN_SITE", "[backend][a64]") {
  const bool saved_sync = cvars::a64_enable_host_guest_stack_synchronization;
  for (bool sync : {true, false}) {
    cvars::a64_enable_host_guest_stack_synchronization = sync;
    SECTION(sync ? "direct call, stack sync" : "direct call, no stack sync") {
      CheckReturnSite(CallKind::kDirect);
    }
    SECTION(sync ? "indirect call, stack sync"
                 : "indirect call, no stack sync") {
      CheckReturnSite(CallKind::kIndirect);
    }
  }
  cvars::a64_enable_host_guest_stack_synchronization = saved_sync;
}

#endif  // XE_ARCH_ARM64
