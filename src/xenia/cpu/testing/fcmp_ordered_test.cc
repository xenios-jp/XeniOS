/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

using namespace xe::cpu;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

namespace {

constexpr uint32_t kCodeBase = 0x80000000;
constexpr uint32_t kSnippetStride = 0x100;

constexpr uint32_t Fcmpu(uint32_t crf, uint32_t a, uint32_t b) {
  return (63u << 26) | (crf << 23) | (a << 16) | (b << 11);
}
constexpr uint32_t Fcmpo(uint32_t crf, uint32_t a, uint32_t b) {
  return Fcmpu(crf, a, b) | (32u << 1);
}
// Branches two instructions ahead if CR bit `bit` is `set`.
constexpr uint32_t Bc(uint32_t bit, bool set) {
  return (16u << 26) | ((set ? 12u : 4u) << 21) | (bit << 16) | 8;
}
constexpr uint32_t Li(uint32_t reg, uint32_t value) {
  return (14u << 26) | (reg << 21) | value;
}
constexpr uint32_t Mfcr(uint32_t reg) {
  return (31u << 26) | (reg << 21) | (19u << 1);
}
constexpr uint32_t Vaddfp(uint32_t d, uint32_t a, uint32_t b) {
  return (4u << 26) | (d << 21) | (a << 16) | (b << 11) | 10;
}
constexpr uint32_t Fadd(uint32_t d, uint32_t a, uint32_t b) {
  return (63u << 26) | (d << 21) | (a << 16) | (b << 11) | (21u << 1);
}
constexpr uint32_t kBlr = 0x4E800020;

struct Snippet {
  std::vector<uint32_t> code;
  // The compare the CR field read at the end comes from: its field, and
  // whether its operands are f2, f1 rather than f1, f2.
  uint32_t crf;
  bool swapped;
  // The CR bit the branch tests and the sense it is taken on; 4 for none.
  uint32_t branch_bit;
  bool branch_set;
  // The branch tests the first compare, of f1 and f2 in that order.
};

const std::vector<Snippet>& Snippets() {
  static const std::vector<Snippet> snippets = [] {
    std::vector<Snippet> list;
    // All four bits read back, in a few fields, in both forms.
    for (uint32_t crf : {0u, 1u, 6u, 7u}) {
      list.push_back({{Fcmpu(crf, 1, 2), Mfcr(6), kBlr}, crf, false, 4, false});
      list.push_back({{Fcmpo(crf, 1, 2), Mfcr(6), kBlr}, crf, false, 4, false});
    }
    // A branch on each bit, in each sense, with the field read after it.
    for (uint32_t bit = 0; bit < 4; ++bit) {
      for (bool set : {false, true}) {
        list.push_back(
            {{Fcmpu(0, 1, 2), Li(5, 1), Bc(bit, set), Li(5, 2), Mfcr(6), kBlr},
             0,
             false,
             bit,
             set});
        list.push_back({{Fcmpo(5, 1, 2), Li(5, 1), Bc(20 + bit, set), Li(5, 2),
                         Mfcr(6), kBlr},
                        5,
                        false,
                        bit,
                        set});
        // The field is rewritten after the branch, so the bits the branch
        // does not read are dead.
        list.push_back({{Fcmpu(1, 1, 2), Li(5, 1), Bc(4 + bit, set), Li(5, 2),
                         Fcmpo(1, 2, 1), Mfcr(6), kBlr},
                        1,
                        true,
                        bit,
                        set});
        // After a vector float instruction and after a scalar one, which
        // leave the host in different float modes.
        list.push_back({{Vaddfp(3, 1, 2), Fcmpu(0, 1, 2), Li(5, 1),
                         Bc(bit, set), Li(5, 2), Mfcr(6), kBlr},
                        0,
                        false,
                        bit,
                        set});
        list.push_back({{Fadd(3, 1, 2), Fcmpu(0, 1, 2), Li(5, 1), Bc(bit, set),
                         Li(5, 2), Mfcr(6), kBlr},
                        0,
                        false,
                        bit,
                        set});
      }
    }
    // The same operand twice.
    list.push_back({{Fcmpu(0, 1, 1), Mfcr(6), kBlr}, 0, false, 4, false});
    return list;
  }();
  return snippets;
}

constexpr uint64_t kOperands[] = {
    0x0000000000000000ull,  // 0
    0x8000000000000000ull,  // -0
    0x3FF0000000000000ull,  // 1
    0xBFF0000000000000ull,  // -1
    0x3FF0000000000001ull,  // just over 1
    0x7FF0000000000000ull,  // inf
    0xFFF0000000000000ull,  // -inf
    0x7FF8000000000000ull,  // quiet NaN
    0xFFF8000000000001ull,  // negative quiet NaN with a payload
    0x7FF0000000000001ull,  // signalling NaN
    0xFFF4000000000000ull,  // negative signalling NaN
    0x0000000000000001ull,  // smallest denormal
    0x800FFFFFFFFFFFFFull,  // largest denormal, negative
    0x0010000000000000ull,  // smallest normal
    0x7FEFFFFFFFFFFFFFull,  // largest
    0xFFEFFFFFFFFFFFFFull,  // most negative
    0x36A0000000000000ull,  // the smallest single denormal, widened
    0x47EFFFFFE0000000ull,  // the largest single, widened
};
constexpr size_t kOperandCount = sizeof(kOperands) / sizeof(kOperands[0]);

double AsDouble(uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, sizeof(d));
  return d;
}

// The field fcmpu and fcmpo write: lt, gt, eq, unordered from the top.
uint32_t ExpectedField(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return 1;
  }
  return a < b ? 8 : a > b ? 4 : 2;
}

struct Outcome {
  uint64_t cr;
  uint64_t r5;
  uint64_t r6;
  bool operator==(const Outcome& other) const {
    return cr == other.cr && r5 == other.r5 && r6 == other.r6;
  }
};

std::vector<Outcome> RunMatrix(bool ordered_compares) {
  const bool saved = cvars::fcmp_ordered_compares;
  cvars::fcmp_ordered_compares = ordered_compares;
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
      for (uint32_t word : snippets[s].code) {
        xe::store_and_swap<uint32_t>(p, word);
        p += 4;
      }
    }
    auto module = std::make_unique<RawModule>(processor.get());
    module->set_name("fcmp");
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
      const Snippet& snippet = snippets[s];
      auto fn =
          processor->ResolveFunction(kCodeBase + uint32_t(s) * kSnippetStride);
      REQUIRE(fn != nullptr);
      for (size_t ai = 0; ai < kOperandCount; ++ai) {
        for (size_t bi = 0; bi < kOperandCount; ++bi) {
          // Every CR bit starts set or clear, so a bit left unwritten shows.
          for (uint64_t cr_fill : {uint64_t(0), uint64_t(0xFFFFFFFF)}) {
            const uint64_t a = kOperands[ai], b = kOperands[bi];
            std::memcpy(&ctx->f[1], &a, sizeof(a));
            std::memcpy(&ctx->f[2], &b, sizeof(b));
            std::memset(&ctx->v[1], 0, sizeof(ctx->v[1]) * 3);
            ctx->set_cr(cr_fill);
            ctx->r[5] = 0;
            ctx->r[6] = 0;
            ctx->lr = 0xBCBCBCBC;
            fn->Call(thread_state.get(), uint32_t(ctx->lr));
            const Outcome outcome = {ctx->cr(), ctx->r[5], ctx->r[6]};
            outcomes.push_back(outcome);

            CAPTURE(s, ai, bi, cr_fill, outcome.cr, outcome.r5, outcome.r6);
            const double da = AsDouble(a), db = AsDouble(b);
            const bool same = snippet.code[0] == Fcmpu(0, 1, 1);
            const uint32_t field = same              ? ExpectedField(da, da)
                                   : snippet.swapped ? ExpectedField(db, da)
                                                     : ExpectedField(da, db);
            const uint32_t shift = 4 * (7 - snippet.crf);
            const uint64_t expected_cr = (cr_fill & ~(uint64_t(0xF) << shift)) |
                                         (uint64_t(field) << shift);
            REQUIRE(outcome.cr == expected_cr);
            REQUIRE(uint32_t(outcome.r6) == uint32_t(expected_cr));
            if (snippet.branch_bit < 4) {
              const bool bit =
                  (ExpectedField(da, db) >> (3 - snippet.branch_bit)) & 1;
              REQUIRE(outcome.r5 == (bit == snippet.branch_set ? 1u : 2u));
            }
          }
        }
      }
    }
    thread_state.reset();
    memory->SystemHeapFree(stack_address);
  }
  cvars::fcmp_ordered_compares = saved;
  return outcomes;
}

}  // namespace

// fcmpu and fcmpo give the CR field PowerPC defines, and a branch on any of
// its bits goes the same way, whichever way the bits are built.
TEST_CASE("FCMP_ORDERED_COMPARES_MATRIX", "[instr]") {
  const auto off = RunMatrix(false);
  const auto on = RunMatrix(true);
  REQUIRE(off.size() == Snippets().size() * kOperandCount * kOperandCount * 2);
  REQUIRE(off.size() == on.size());
  size_t differing = 0;
  for (size_t i = 0; i < off.size(); ++i) {
    differing += !(off[i] == on[i]);
  }
  REQUIRE(differing == 0);
}
