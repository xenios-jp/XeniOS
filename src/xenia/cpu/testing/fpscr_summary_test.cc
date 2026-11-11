/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/cpu/compiler/passes/control_flow_analysis_pass.h"
#include "xenia/cpu/compiler/passes/control_flow_simplification_pass.h"
#include "xenia/cpu/compiler/passes/simplification_pass.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

using namespace xe::cpu::hir;
using namespace xe::cpu;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

namespace {

constexpr uint32_t kCodeBase = 0x80000000;
constexpr uint32_t kSnippetStride = 0x100;

// Instruction encodings.
constexpr uint32_t A(uint32_t xo, uint32_t d, uint32_t a, uint32_t b,
                     uint32_t c, bool rc) {
  return (63u << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) |
         (xo << 1) | (rc ? 1 : 0);
}
constexpr uint32_t Fadd(uint32_t d, uint32_t a, uint32_t b, bool rc = false) {
  return A(21, d, a, b, 0, rc);
}
constexpr uint32_t Fsub(uint32_t d, uint32_t a, uint32_t b, bool rc = false) {
  return A(20, d, a, b, 0, rc);
}
constexpr uint32_t Fdiv(uint32_t d, uint32_t a, uint32_t b, bool rc = false) {
  return A(18, d, a, b, 0, rc);
}
constexpr uint32_t Fmul(uint32_t d, uint32_t a, uint32_t c, bool rc = false) {
  return A(25, d, a, 0, c, rc);
}
constexpr uint32_t Fmadd(uint32_t d, uint32_t a, uint32_t c, uint32_t b,
                         bool rc = false) {
  return A(29, d, a, b, c, rc);
}
constexpr uint32_t Fmr(uint32_t d, uint32_t b, bool rc = false) {
  return (63u << 26) | (d << 21) | (b << 11) | (72u << 1) | (rc ? 1 : 0);
}
constexpr uint32_t Fctiwz(uint32_t d, uint32_t b, bool rc = false) {
  return (63u << 26) | (d << 21) | (b << 11) | (15u << 1) | (rc ? 1 : 0);
}
constexpr uint32_t Mffs(uint32_t d, bool rc = false) {
  return (63u << 26) | (d << 21) | (583u << 1) | (rc ? 1 : 0);
}
constexpr uint32_t Mtfsb1(uint32_t bit, bool rc = false) {
  return (63u << 26) | (bit << 21) | (38u << 1) | (rc ? 1 : 0);
}
constexpr uint32_t Mtfsb0(uint32_t bit, bool rc = false) {
  return (63u << 26) | (bit << 21) | (70u << 1) | (rc ? 1 : 0);
}
constexpr uint32_t Mtfsfi(uint32_t field, uint32_t imm, bool rc = false) {
  return (63u << 26) | (field << 23) | (imm << 12) | (134u << 1) | (rc ? 1 : 0);
}
constexpr uint32_t Mtfsf(uint32_t mask, uint32_t b, bool rc = false) {
  return (63u << 26) | (mask << 17) | (b << 11) | (711u << 1) | (rc ? 1 : 0);
}
constexpr uint32_t Mcrfs(uint32_t crf_d, uint32_t crf_s) {
  return (63u << 26) | (crf_d << 23) | (crf_s << 18) | (64u << 1);
}
constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kCmpwiR3Zero = 0x2C030000;
constexpr uint32_t kMflrR0 = 0x7C0802A6;
constexpr uint32_t kMtlrR0 = 0x7C0803A6;
constexpr uint32_t kMrR30R0 = 0x7C1E0378;  // mr r30, r0
constexpr uint32_t kMrR0R30 = 0x7FC0F378;  // mr r0, r30
constexpr uint32_t Beq(int32_t words) {
  return 0x41820000 | (uint32_t(words * 4) & 0xFFFC);
}
constexpr uint32_t B(int32_t words) {
  return 0x48000000 | (uint32_t(words * 4) & 0x03FFFFFC);
}
constexpr uint32_t Bl(int32_t words) { return B(words) | 1; }

// Each ends by reading the FPSCR into f31. f1 and f2 are the operands, f4 an
// FPSCR image for mtfsf, r3 picks the path.
const std::vector<std::vector<uint32_t>>& Snippets() {
  static const std::vector<std::vector<uint32_t>> snippets = {
      // Without Rc: the summary is cleared.
      {Fadd(3, 1, 2), Mffs(31), kBlr},
      {Fdiv(3, 1, 2), Mffs(31), kBlr},
      {Fmr(3, 1), Mffs(31), kBlr},
      // With Rc: the summary is this instruction's, CR1 its copy.
      {Fadd(3, 1, 2, true), Mffs(31), kBlr},
      {Fdiv(3, 1, 2, true), Mffs(31), kBlr},
      {Fmul(3, 1, 2, true), Mffs(31), kBlr},
      {Fmadd(3, 1, 2, 1, true), Mffs(31), kBlr},
      {Fctiwz(3, 1, true), Mffs(31), kBlr},
      {Fmr(3, 1, true), Mffs(31), kBlr},
      // Set, then cleared by an instruction without Rc, and the reverse.
      {Fdiv(3, 1, 2, true), Fadd(5, 1, 2), Mffs(31), kBlr},
      {Fadd(5, 1, 2), Fdiv(3, 1, 2, true), Mffs(31), kBlr},
      // Nothing touches the FPSCR: it reads back whole.
      {Mffs(31), kBlr},
      {Mffs(31, true), kBlr},
      // The FPSCR writers, on top of whatever the caller left.
      {Mtfsb1(0), Mffs(31), kBlr},
      {Mtfsb1(3, true), Mffs(31), kBlr},
      {Mtfsb1(7), Mffs(31), kBlr},
      {Mtfsb0(0), Mffs(31), kBlr},
      {Mtfsb0(3, true), Mffs(31), kBlr},
      {Mtfsb0(6), Mffs(31), kBlr},
      {Mtfsfi(0, 0xF), Mffs(31), kBlr},
      {Mtfsfi(0, 0x0, true), Mffs(31), kBlr},
      {Mtfsfi(1, 0x9), Mffs(31), kBlr},
      {Mtfsf(0xFF, 4), Mffs(31), kBlr},
      {Mtfsf(0x80, 4, true), Mffs(31), kBlr},
      {Mtfsf(0x7F, 4), Mffs(31), kBlr},
      {Mcrfs(0, 0), Mffs(31), kBlr},
      {Mcrfs(2, 1), Mffs(31), kBlr},
      {Mcrfs(7, 3), Mffs(31), kBlr},
      // A writer, then an instruction without Rc, and the reverse.
      {Mtfsb1(3), Fadd(3, 1, 2), Mffs(31), kBlr},
      {Fadd(3, 1, 2), Mtfsb1(3), Mffs(31), kBlr},
      {Mtfsf(0xFF, 4), Fsub(3, 1, 2), Mffs(31), kBlr},
      {Fsub(3, 1, 2), Mtfsf(0xFF, 4), Fmul(5, 1, 2), Mtfsfi(0, 0xA), Mffs(31),
       kBlr},
      // A clear before a branch and one after the join: the second goes.
      {Fadd(3, 1, 2), kCmpwiR3Zero, Beq(2), Fsub(5, 1, 2), Fmul(6, 1, 2),
       Mffs(31), kBlr},
      // One path sets the summary, the join clears it.
      {Fadd(3, 1, 2), kCmpwiR3Zero, Beq(2), Fdiv(5, 1, 2, true), Fmul(6, 1, 2),
       Mffs(31), kBlr},
      // One path sets the summary and nothing clears it after the join.
      {Fadd(3, 1, 2), kCmpwiR3Zero, Beq(2), Fdiv(5, 1, 2, true), Mffs(31),
       kBlr},
      // The entry is unknown on one path only.
      {kCmpwiR3Zero, Beq(2), Fadd(3, 1, 2), Fmr(6, 1), Mffs(31), kBlr},
      {kCmpwiR3Zero, Beq(2), Fadd(3, 1, 2), Mffs(31), kBlr},
      // A call to a function that sets the summary, then a clear.
      {kMflrR0, kMrR30R0, Fadd(3, 1, 2), Bl(kSnippetStride / 4 - 3),
       Fsub(5, 1, 2), kMrR0R30, kMtlrR0, Mffs(31), kBlr},
      // The callee of the two around it.
      {Fdiv(7, 1, 2, true), Mffs(31), kBlr},
      // The same call with no clear after it.
      {kMflrR0, kMrR30R0, Fadd(3, 1, 2), Bl(-(kSnippetStride / 4) - 3),
       kMrR0R30, kMtlrR0, Mffs(31), kBlr},
  };
  return snippets;
}

constexpr uint64_t kOperands[] = {
    0x3FF0000000000000ull,  // 1
    0x0000000000000000ull,  // 0
    0x8000000000000000ull,  // -0
    0x7FF0000000000000ull,  // inf
    0xFFF0000000000000ull,  // -inf
    0x7FF8000000000001ull,  // quiet NaN
    0x7FF0000000000001ull,  // signalling NaN
    0x0000000000000001ull,  // denormal
    0x7FEFFFFFFFFFFFFFull,  // largest
    0x41E0000000000000ull,  // 2^31
};

constexpr uint32_t kFpscrImages[] = {
    0x00000000, 0xF0000000, 0x90000000, 0x60000000, 0x0F000000,
    0xFFF80700, 0x000000F8, 0x80000080, 0x20F80000, 0xFFFFFFF8,
};

struct Outcome {
  uint64_t f[32];
  uint64_t cr;
  uint32_t fpscr;
  bool operator==(const Outcome& other) const {
    return !std::memcmp(this, &other, sizeof(*this));
  }
};

// Runs every snippet over the operand and FPSCR matrix with the cvar as given.
std::vector<Outcome> RunMatrix(bool summary_byte, bool* byte_used,
                               bool* field_clean) {
  const bool saved = cvars::fpscr_summary_byte;
  cvars::fpscr_summary_byte = summary_byte;
  std::vector<Outcome> outcomes;
  {
    auto memory = std::make_unique<xe::Memory>();
    memory->Initialize();
    auto processor = std::make_unique<Processor>(memory.get(), nullptr);
    processor->Setup(CreateBackend());

    const auto& snippets = Snippets();
    const uint32_t code_size = uint32_t(snippets.size()) * kSnippetStride;
    memory->LookupHeap(kCodeBase)->AllocFixed(
        kCodeBase, code_size, 0,
        xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
        xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
    for (size_t s = 0; s < snippets.size(); ++s) {
      uint8_t* p =
          memory->TranslateVirtual(kCodeBase + uint32_t(s) * kSnippetStride);
      for (uint32_t word : snippets[s]) {
        xe::store_and_swap<uint32_t>(p, word);
        p += 4;
      }
    }
    auto module = std::make_unique<RawModule>(processor.get());
    module->set_name("fpscr");
    module->SetAddressRange(kCodeBase, code_size);
    module->set_executable(true);
    processor->AddModule(std::move(module));
    processor->backend()->CommitExecutableRange(kCodeBase, kCodeBase + 0x10000);

    const uint32_t stack_size = 64 * 1024;
    const uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
    auto thread_state = std::make_unique<ThreadState>(
        processor.get(), 0x100, stack_address + stack_size);
    PPCContext* ctx = thread_state->context();

    for (size_t s = 0; s < snippets.size(); ++s) {
      auto fn =
          processor->ResolveFunction(kCodeBase + uint32_t(s) * kSnippetStride);
      REQUIRE(fn != nullptr);
      for (uint64_t a : kOperands) {
        for (uint64_t b : kOperands) {
          for (uint32_t image : kFpscrImages) {
            for (uint64_t r3 = 0; r3 < 2; ++r3) {
              std::memset(ctx->f, 0, sizeof(ctx->f));
              std::memcpy(&ctx->f[1], &a, sizeof(a));
              std::memcpy(&ctx->f[2], &b, sizeof(b));
              // The image mtfsf writes: the other image, rotated.
              const uint64_t f4 = (image << 13) | (image >> 19) | 0x00000100;
              std::memcpy(&ctx->f[4], &f4, sizeof(f4));
              ctx->set_cr(0);
              ctx->r[3] = r3;
              ctx->lr = 0xBCBCBCBC;
              ctx->set_fpscr_value(image);
              fn->Call(thread_state.get(), uint32_t(ctx->lr));
              Outcome outcome;
              std::memcpy(outcome.f, ctx->f, sizeof(outcome.f));
              outcome.cr = ctx->cr();
              outcome.fpscr = ctx->fpscr_value();
              outcomes.push_back(outcome);
              *byte_used |= ctx->fpscr_summary != 0;
              *field_clean &= !(ctx->fpscr.value >> 28);
              CAPTURE(s, a, b, image, r3, outcome.f[31], outcome.f[3]);
              // What mffs read is the FPSCR the context reports.
              REQUIRE(uint32_t(outcome.f[31]) == outcome.fpscr);
            }
          }
        }
      }
    }
    // Leave the default rounding mode behind for the tests that follow.
    ctx->set_fpscr_value(0);
    thread_state.reset();
    memory->SystemHeapFree(stack_address);
  }
  cvars::fpscr_summary_byte = saved;
  return outcomes;
}

size_t CountSummaryStores(HIRBuilder& b) {
  size_t count = 0;
  for (Block* block = b.first_block(); block; block = block->next) {
    for (Instr* i = block->instr_head; i; i = i->next) {
      if (i->GetOpcodeNum() == OPCODE_STORE_CONTEXT &&
          size_t(i->src1.offset) == offsetof(PPCContext, fpscr_summary)) {
        ++count;
      }
    }
  }
  return count;
}

void ClearSummary(HIRBuilder& b) {
  b.StoreContext(offsetof(PPCContext, fpscr_summary), b.LoadZeroInt8());
}

// The summary stores left after the pass.
size_t StoresLeft(void (*emit)(HIRBuilder& b), bool merge = false,
                  bool* mid_block_branch = nullptr) {
  HIRBuilder b;
  b.MakeCurrent();
  emit(b);
  b.Finalize();
  compiler::passes::ControlFlowAnalysisPass cfa;
  cfa.Run(&b);
  if (merge) {
    compiler::passes::ControlFlowSimplificationPass cfs;
    cfs.Run(&b);
    cfa.Run(&b);
    for (Block* block = b.first_block(); block; block = block->next) {
      for (Instr* i = block->instr_head; i; i = i->next) {
        if (i->BranchLabel() && i != block->instr_tail) {
          *mid_block_branch = true;
        }
      }
    }
  }
  compiler::passes::SimplificationPass pass;
  bool changed = false;
  pass.Run(&b, changed);
  const size_t count = CountSummaryStores(b);
  b.RemoveCurrent();
  return count;
}

}  // namespace

// The FPSCR, CR and every FPR after each snippet are the same wherever the
// summary bits are kept.
TEST_CASE("FPSCR_SUMMARY_BYTE_MATRIX", "[instr]") {
  bool byte_used_off = false, field_clean_off = true;
  bool byte_used_on = false, field_clean_on = true;
  const auto off = RunMatrix(false, &byte_used_off, &field_clean_off);
  const auto on = RunMatrix(true, &byte_used_on, &field_clean_on);
  REQUIRE(off.size() == on.size());
  REQUIRE(off.size() == Snippets().size() * 10 * 10 * 10 * 2);
  size_t differing = 0, summary_set = 0;
  for (size_t i = 0; i < off.size(); ++i) {
    differing += !(off[i] == on[i]);
    summary_set += (off[i].fpscr >> 28) != 0;
  }
  REQUIRE(differing == 0);
  // The matrix does leave summary bits set and clear, and each mode keeps
  // them where it says.
  REQUIRE(summary_set > 0);
  REQUIRE(summary_set < off.size());
  REQUIRE_FALSE(byte_used_off);
  REQUIRE_FALSE(field_clean_off);
  REQUIRE(byte_used_on);
  REQUIRE(field_clean_on);
}

TEST_CASE("FPSCR_SUMMARY_FIXED_VALUES", "[instr]") {
  // Snippet 4 is fdiv. f3, f1, f2: 1 / 0 raises only the zero divide, which
  // sets FX, and 0 / 0 is invalid as well, whatever the caller left.
  for (bool mode : {false, true}) {
    bool used = false, clean = true;
    const auto out = RunMatrix(mode, &used, &clean);
    const size_t per_snippet = 10 * 10 * 10 * 2;
    const auto at = [&](size_t snippet, size_t a, size_t b, size_t image) {
      return out[snippet * per_snippet + ((a * 10 + b) * 10 + image) * 2];
    };
    REQUIRE((at(4, 0, 1, 0).fpscr >> 28) == 0x8);
    REQUIRE((at(4, 0, 1, 1).fpscr >> 28) == 0x8);
    REQUIRE((at(4, 1, 1, 0).fpscr >> 28) == 0xA);
    REQUIRE((at(4, 0, 0, 1).fpscr >> 28) == 0x0);
    REQUIRE(((at(4, 1, 1, 0).cr >> 24) & 0xF) == 0xA);
    // fadd without Rc clears the caller's summary and nothing else.
    REQUIRE(at(0, 0, 0, 9).fpscr == 0x0FFFFFF8);
    // Untouched, it reads back whole.
    REQUIRE(at(11, 0, 0, 9).fpscr == 0xFFFFFFF8);
    REQUIRE(at(11, 0, 0, 2).fpscr == 0x90000000);
  }
}

TEST_CASE("FPSCR_SUMMARY_REPEATED_CLEARS", "[instr]") {
  const bool saved = cvars::fpscr_summary_byte;
  cvars::fpscr_summary_byte = true;
  // The entry says nothing, so the first clear stays and the second goes.
  REQUIRE(StoresLeft([](HIRBuilder& b) {
            ClearSummary(b);
            StoreGPR(b, 3, LoadGPR(b, 4));
            ClearSummary(b);
            b.Return();
          }) == 1);
  // Across a branch and its join.
  REQUIRE(StoresLeft([](HIRBuilder& b) {
            ClearSummary(b);
            auto label = b.NewLabel();
            b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
            ClearSummary(b);
            b.MarkLabel(label);
            ClearSummary(b);
            b.Return();
          }) == 1);
  // Not when one path into the join has not cleared.
  REQUIRE(StoresLeft([](HIRBuilder& b) {
            auto label = b.NewLabel();
            b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
            ClearSummary(b);
            b.MarkLabel(label);
            ClearSummary(b);
            b.Return();
          }) == 2);
  // Not when one path stores something else.
  REQUIRE(StoresLeft([](HIRBuilder& b) {
            ClearSummary(b);
            auto label = b.NewLabel();
            b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
            b.StoreContext(offsetof(PPCContext, fpscr_summary),
                           b.Truncate(LoadGPR(b, 3), INT8_TYPE));
            b.MarkLabel(label);
            ClearSummary(b);
            b.Return();
          }) == 3);
  // Nor a constant that is not zero.
  REQUIRE(StoresLeft([](HIRBuilder& b) {
            ClearSummary(b);
            b.StoreContext(offsetof(PPCContext, fpscr_summary),
                           b.LoadConstantInt8(8));
            ClearSummary(b);
            b.Return();
          }) == 3);
  // Nor across a call, which may run an instruction with Rc.
  REQUIRE(StoresLeft([](HIRBuilder& b) {
            ClearSummary(b);
            b.Call(nullptr);
            ClearSummary(b);
            b.Return();
          }) == 2);
  // Around a loop that only clears, the clear in the body goes.
  REQUIRE(StoresLeft([](HIRBuilder& b) {
            ClearSummary(b);
            auto loop = b.NewLabel();
            b.MarkLabel(loop);
            ClearSummary(b);
            StoreGPR(b, 3, LoadGPR(b, 4));
            b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), loop);
            b.Return();
          }) == 1);
  // Around a loop whose body sets the summary after its clear, it stays.
  REQUIRE(StoresLeft([](HIRBuilder& b) {
            ClearSummary(b);
            auto loop = b.NewLabel();
            b.MarkLabel(loop);
            ClearSummary(b);
            b.StoreContext(offsetof(PPCContext, fpscr_summary),
                           b.Truncate(LoadGPR(b, 3), INT8_TYPE));
            b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), loop);
            b.Return();
          }) == 3);
  // A branch that leaves mid-block, before the clear after it, carries the
  // state at the branch.
  bool mid_block_branch = false;
  REQUIRE(StoresLeft(
              [](HIRBuilder& b) {
                b.StoreContext(offsetof(PPCContext, fpscr_summary),
                               b.Truncate(LoadGPR(b, 3), INT8_TYPE));
                auto label = b.NewLabel();
                b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
                ClearSummary(b);
                b.MarkLabel(label);
                ClearSummary(b);
                b.Return();
              },
              true, &mid_block_branch) == 3);
  REQUIRE(mid_block_branch);
  // With the cvar clear the pass leaves every store.
  cvars::fpscr_summary_byte = false;
  REQUIRE(StoresLeft([](HIRBuilder& b) {
            ClearSummary(b);
            ClearSummary(b);
            b.Return();
          }) == 2);
  cvars::fpscr_summary_byte = saved;
}
