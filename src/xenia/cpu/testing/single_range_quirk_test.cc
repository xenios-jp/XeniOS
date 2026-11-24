/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "xenia/cpu/compiler/passes/control_flow_analysis_pass.h"
#include "xenia/cpu/compiler/passes/control_flow_simplification_pass.h"
#include "xenia/cpu/compiler/passes/simplification_pass.h"

using namespace xe::cpu::hir;
using namespace xe::cpu;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

namespace {

constexpr uint64_t kOne = 0x3FF0000000000000ull;
constexpr uint64_t kZero = 0x0000000000000000ull;
constexpr uint64_t kDenormal = 0x0000000000000001ull;
constexpr uint64_t kNegDenormal = 0x800FFFFFFFFFFFFFull;
constexpr uint64_t kMinNormal = 0x0010000000000000ull;
constexpr uint64_t kBelowSingle = 0x3690000000000000ull;     // 2^-150
constexpr uint64_t kSmallestSingle = 0x36A0000000000000ull;  // 2^-149
constexpr uint64_t kBelow128 = 0x47EFFFFFFFFFFFFFull;
constexpr uint64_t k128 = 0x47F0000000000000ull;      // 2^128
constexpr uint64_t kNeg1000 = 0xFE70000000000000ull;  // -2^1000
constexpr uint64_t kInf = 0x7FF0000000000000ull;
constexpr uint64_t kNaN = 0x7FF8000000000000ull;

void SetF64Bits(PPCContext* ctx, int reg, uint64_t bits) {
  std::memcpy(&ctx->f[reg], &bits, sizeof(bits));
}

Value* F64Constant(HIRBuilder& b, uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, sizeof(d));
  return b.LoadConstantFloat64(d);
}

bool HasSingleRangeQuirk(HIRBuilder& b) {
  for (Block* block = b.first_block(); block; block = block->next) {
    for (Instr* i = block->instr_head; i; i = i->next) {
      if (i->GetOpcodeNum() == OPCODE_SINGLE_RANGE_QUIRK) {
        return true;
      }
    }
  }
  return false;
}

bool SimplifiesAway(void (*emit)(HIRBuilder& b)) {
  HIRBuilder b;
  b.MakeCurrent();
  emit(b);
  b.Finalize();
  compiler::passes::ControlFlowAnalysisPass cfa;
  cfa.Run(&b);
  compiler::passes::SimplificationPass pass;
  bool changed = false;
  pass.Run(&b, changed);
  const bool folded = !HasSingleRangeQuirk(b);
  b.RemoveCurrent();
  return folded;
}

// Merges blocks first, as the translator does, which can leave a branch in the
// middle of a block where the state differs from the block's end.
bool SimplifiesAwayAfterMerging(void (*emit)(HIRBuilder& b),
                                bool* mid_block_branch) {
  HIRBuilder b;
  b.MakeCurrent();
  emit(b);
  b.Finalize();
  compiler::passes::ControlFlowAnalysisPass cfa;
  cfa.Run(&b);
  compiler::passes::ControlFlowSimplificationPass cfs;
  cfs.Run(&b);
  cfa.Run(&b);
  *mid_block_branch = false;
  for (Block* block = b.first_block(); block; block = block->next) {
    for (Instr* i = block->instr_head; i; i = i->next) {
      if (i->BranchLabel() && i != block->instr_tail) {
        *mid_block_branch = true;
      }
    }
  }
  compiler::passes::SimplificationPass pass;
  bool changed = false;
  pass.Run(&b, changed);
  const bool folded = !HasSingleRangeQuirk(b);
  b.RemoveCurrent();
  return folded;
}

// Whether the ADD the emitter builds still has ARITHMETIC_SINGLE_RANGE after
// the simplification pass.
bool KeepsSingleRangeFlag(void (*emit)(HIRBuilder& b)) {
  HIRBuilder b;
  b.MakeCurrent();
  emit(b);
  b.Finalize();
  compiler::passes::ControlFlowAnalysisPass cfa;
  cfa.Run(&b);
  compiler::passes::SimplificationPass pass;
  bool changed = false;
  pass.Run(&b, changed);
  bool kept = false;
  for (Block* block = b.first_block(); block; block = block->next) {
    for (Instr* i = block->instr_head; i; i = i->next) {
      if (i->GetOpcodeNum() == OPCODE_ADD &&
          (i->flags & ARITHMETIC_SINGLE_RANGE)) {
        kept = true;
      }
    }
  }
  b.RemoveCurrent();
  return kept;
}

}  // namespace

// quirk = any operand nonzero and outside [2^-149, 2^128) && every operand
// finite.
TEST_CASE("SINGLE_RANGE_QUIRK", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(b.SingleRangeQuirk(LoadFPR(b, 1), LoadFPR(b, 2),
                                             LoadFPR(b, 3)),
                          INT64_TYPE));
    b.Return();
  });
  struct Case {
    uint64_t a, b, c;
    uint64_t quirk;
  };
  const Case cases[] = {
      {kOne, kOne, kOne, 0},
      {kZero, kZero, kZero, 0},
      {kMinNormal, kOne, kZero, 1},
      {kBelowSingle, kOne, kOne, 1},
      {kSmallestSingle, kOne, kZero, 0},
      {kOne, kBelow128, kOne, 0},
      {kOne, kOne, k128, 1},
      {kNeg1000, kOne, kOne, 1},
      {k128, kInf, kOne, 0},
      {kOne, k128, kNaN, 0},
      {kDenormal, kOne, kOne, 1},
      {kOne, kNegDenormal, kOne, 1},
      {kOne, kOne, kDenormal, 1},
      {kDenormal, kDenormal, kDenormal, 1},
      {kDenormal, kInf, kOne, 0},
      {kOne, kDenormal, kNaN, 0},
      {kInf, kInf, kInf, 0},
  };
  for (const Case& c : cases) {
    INFO("operands 0x" << std::hex << c.a << " 0x" << c.b << " 0x" << c.c);
    test.Run(
        [&](PPCContext* ctx) {
          SetF64Bits(ctx, 1, c.a);
          SetF64Bits(ctx, 2, c.b);
          SetF64Bits(ctx, 3, c.c);
        },
        [&](PPCContext* ctx) { REQUIRE(ctx->r[3] == c.quirk); });
  }
}

// Constant operands take the backends' materialisation path.
TEST_CASE("SINGLE_RANGE_QUIRK_CONSTANT_OPERAND", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(
                 b.SingleRangeQuirk(LoadFPR(b, 1), F64Constant(b, kDenormal),
                                    LoadFPR(b, 1)),
                 INT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { SetF64Bits(ctx, 1, kOne); },
           [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 1); });
  test.Run([](PPCContext* ctx) { SetF64Bits(ctx, 1, kInf); },
           [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0); });
}

TEST_CASE("SINGLE_RANGE_QUIRK_FOLD", "[instr]") {
  // Widened singles and constants in the single range never set it off.
  REQUIRE(SimplifiesAway([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreGPR(b, 3,
             b.ZeroExtend(b.SingleRangeQuirk(single, b.Neg(single),
                                             F64Constant(b, kOne)),
                          INT64_TYPE));
    b.Return();
  }));
  REQUIRE(SimplifiesAway([](HIRBuilder& b) {
    Value* single =
        b.Convert(b.Convert(LoadFPR(b, 1), FLOAT32_TYPE), FLOAT64_TYPE);
    StoreGPR(
        b, 3,
        b.ZeroExtend(b.SingleRangeQuirk(single, single, single), INT64_TYPE));
    b.Return();
  }));
  // A guest FPR or a constant outside the range may.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreGPR(
        b, 3,
        b.ZeroExtend(b.SingleRangeQuirk(LoadFPR(b, 1), F64Constant(b, kOne),
                                        F64Constant(b, kOne)),
                     INT64_TYPE));
    b.Return();
  }));
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreGPR(b, 3,
             b.ZeroExtend(
                 b.SingleRangeQuirk(single, F64Constant(b, kDenormal), single),
                 INT64_TYPE));
    b.Return();
  }));
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreGPR(
        b, 3,
        b.ZeroExtend(b.SingleRangeQuirk(single, F64Constant(b, k128), single),
                     INT64_TYPE));
    b.Return();
  }));
}

// The same proof drops ARITHMETIC_SINGLE_RANGE from the operation.
TEST_CASE("SINGLE_RANGE_FLAG_FOLD", "[instr]") {
  REQUIRE_FALSE(KeepsSingleRangeFlag([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreFPR(b, 3,
             b.Add(single, F64Constant(b, kOne), ARITHMETIC_SINGLE_RANGE));
    b.Return();
  }));
  REQUIRE(KeepsSingleRangeFlag([](HIRBuilder& b) {
    StoreFPR(
        b, 3,
        b.Add(LoadFPR(b, 1), F64Constant(b, kOne), ARITHMETIC_SINGLE_RANGE));
    b.Return();
  }));
  REQUIRE(KeepsSingleRangeFlag([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreFPR(b, 3,
             b.Add(single, F64Constant(b, k128), ARITHMETIC_SINGLE_RANGE));
    b.Return();
  }));
}

// The operations' own check: the default QNaN for an operand outside the range,
// unless an operand is infinite or NaN, and the plain result otherwise.
TEST_CASE("SINGLE_RANGE_FLAG", "[instr]") {
  struct Case {
    uint64_t a, b;
    uint64_t sum, product, fused;  // fused = a * b + 1
  };
  const Case cases[] = {
      {kOne, kOne, 0x4000000000000000ull, kOne, 0x4000000000000000ull},
      {kSmallestSingle, kOne, kOne, kSmallestSingle, kOne},
      {kBelowSingle, kOne, kNaN, kNaN, kNaN},
      {kOne, k128, kNaN, kNaN, kNaN},
      {kMinNormal, kZero, kNaN, kNaN, kNaN},
      {kZero, kZero, kZero, kZero, kOne},
      {k128, kInf, kInf, kInf, kInf},
  };
  struct Op {
    void (*build)(HIRBuilder& b);
    uint64_t Case::* expected;
  };
  const Op ops[] = {
      {[](HIRBuilder& b) {
         StoreFPR(b, 3,
                  b.Add(LoadFPR(b, 1), LoadFPR(b, 2), ARITHMETIC_SINGLE_RANGE));
         b.Return();
       },
       &Case::sum},
      {[](HIRBuilder& b) {
         StoreFPR(b, 3,
                  b.Mul(LoadFPR(b, 1), LoadFPR(b, 2), ARITHMETIC_SINGLE_RANGE));
         b.Return();
       },
       &Case::product},
      {[](HIRBuilder& b) {
         StoreFPR(b, 3,
                  b.MulAdd(LoadFPR(b, 1), LoadFPR(b, 2), LoadFPR(b, 0), false,
                           ARITHMETIC_SINGLE_RANGE));
         b.Return();
       },
       &Case::fused},
  };
  // One function at a time: each installs the process-wide MMIO handler.
  for (const Op& op : ops) {
    TestFunction test(op.build);
    for (const Case& c : cases) {
      INFO("operands 0x" << std::hex << c.a << " 0x" << c.b);
      test.Run(
          [&](PPCContext* ctx) {
            SetF64Bits(ctx, 0, kOne);
            SetF64Bits(ctx, 1, c.a);
            SetF64Bits(ctx, 2, c.b);
          },
          [&](PPCContext* ctx) {
            uint64_t bits;
            std::memcpy(&bits, &ctx->f[3], sizeof(bits));
            REQUIRE(bits == c.*op.expected);
          });
    }
  }
}

// Operands known to be in the range, here rounded to single, aren't tested, but
// an infinite or NaN one still takes precedence over another out of range.
TEST_CASE("SINGLE_RANGE_FLAG_KNOWN_OPERANDS", "[instr]") {
  struct Case {
    uint64_t f1, f2, f3;
    uint64_t sum, fused;  // sum = single(f1) + f2, fused = f2 * f3 + single(f1)
  };
  const Case cases[] = {
      {kOne, kOne, kOne, 0x4000000000000000ull, 0x4000000000000000ull},
      {kOne, k128, kOne, kNaN, kNaN},
      {kOne, kOne, k128, 0x4000000000000000ull, kNaN},
      {kOne, kBelowSingle, kOne, kNaN, kNaN},
      {kOne, kSmallestSingle, kOne, kOne, kOne},
      {kInf, k128, kOne, kInf, kInf},
      {kNaN, k128, kOne, kNaN, kNaN},
  };
  struct Op {
    void (*build)(HIRBuilder& b);
    uint64_t Case::* expected;
  };
  const Op ops[] = {
      {[](HIRBuilder& b) {
         StoreFPR(b, 0,
                  b.Add(b.ToSingle(LoadFPR(b, 1)), LoadFPR(b, 2),
                        ARITHMETIC_SINGLE_RANGE));
         b.Return();
       },
       &Case::sum},
      {[](HIRBuilder& b) {
         StoreFPR(
             b, 0,
             b.MulAdd(LoadFPR(b, 2), LoadFPR(b, 3), b.ToSingle(LoadFPR(b, 1)),
                      false, ARITHMETIC_SINGLE_RANGE));
         b.Return();
       },
       &Case::fused},
  };
  // One function at a time: each installs the process-wide MMIO handler.
  for (const Op& op : ops) {
    TestFunction test(op.build);
    for (const Case& c : cases) {
      INFO("operands 0x" << std::hex << c.f1 << " 0x" << c.f2 << " 0x" << c.f3);
      test.Run(
          [&](PPCContext* ctx) {
            SetF64Bits(ctx, 1, c.f1);
            SetF64Bits(ctx, 2, c.f2);
            SetF64Bits(ctx, 3, c.f3);
          },
          [&](PPCContext* ctx) {
            uint64_t bits;
            std::memcpy(&bits, &ctx->f[0], sizeof(bits));
            REQUIRE(bits == c.*op.expected);
          });
    }
  }
}

// The operations' own check gives what SINGLE_RANGE_QUIRK and a select do, in
// every rounding mode, for operands on and around the edges of the range, the
// doubles that are no single, and infinite and NaN ones.
TEST_CASE("SINGLE_RANGE_FLAG_MATCHES_QUIRK", "[instr]") {
  std::vector<uint64_t> operands;
  for (uint64_t magnitude :
       {kZero, kDenormal, uint64_t(0x000FFFFFFFFFFFFFull), kMinNormal,
        uint64_t(0x3680000000000000ull), uint64_t(0x368FFFFFFFFFFFFFull),
        kBelowSingle, uint64_t(0x3690000000000001ull),
        uint64_t(0x369FFFFFFFFFFFFFull), kSmallestSingle,
        uint64_t(0x36A0000000000001ull), uint64_t(0x36A8000000000000ull),
        uint64_t(0x380FFFFFC0000000ull), uint64_t(0x3810000000000000ull),
        uint64_t(0x3FEFFFFFFFFFFFFFull), kOne, uint64_t(0x3FF0000010000000ull),
        uint64_t(0x3FF0000020000000ull), uint64_t(0x47EFFFFFE0000000ull),
        uint64_t(0x47EFFFFFEFFFFFFFull), uint64_t(0x47EFFFFFF0000000ull),
        kBelow128, k128, uint64_t(0x47F0000000000001ull),
        uint64_t(0x7FEFFFFFFFFFFFFFull), kInf, uint64_t(0x7FF0000000000001ull),
        kNaN}) {
    operands.push_back(magnitude);
    operands.push_back(magnitude | 0x8000000000000000ull);
  }
  using Build = Value* (*)(HIRBuilder & b, Value* x, Value* y, Value* z,
                           uint32_t flags);
  const Build builds[] = {
      [](HIRBuilder& b, Value* x, Value* y, Value*, uint32_t flags) {
        return b.Add(x, y, flags);
      },
      [](HIRBuilder& b, Value* x, Value* y, Value*, uint32_t flags) {
        return b.Sub(x, y, flags);
      },
      [](HIRBuilder& b, Value* x, Value* y, Value*, uint32_t flags) {
        return b.Mul(x, y, flags);
      },
      [](HIRBuilder& b, Value* x, Value* y, Value* z, uint32_t flags) {
        return b.MulAdd(x, y, z, false, flags);
      },
      [](HIRBuilder& b, Value* x, Value* y, Value* z, uint32_t flags) {
        return b.MulSub(x, y, z, true, flags);
      },
  };
  int build_index = 0;
  for (Build build : builds) {
    const bool fused = build_index++ >= 3;
    // One function at a time: each installs the process-wide MMIO handler.
    TestFunction test([build, fused](HIRBuilder& b) {
      b.SetRoundingMode(b.Truncate(LoadGPR(b, 4), INT32_TYPE));
      Value* x = LoadFPR(b, 1);
      Value* y = LoadFPR(b, 2);
      Value* z = LoadFPR(b, 3);
      Value* quirk = b.SingleRangeQuirk(x, y, fused ? z : y);
      StoreFPR(b, 5,
               b.Select(quirk, F64Constant(b, kNaN), build(b, x, y, z, 0)));
      StoreFPR(b, 4, build(b, x, y, z, ARITHMETIC_SINGLE_RANGE));
      b.SetRoundingMode(b.LoadConstantInt32(0));
      b.Return();
    });
    const uint64_t thirds[] = {kOne, kZero, kBelowSingle, k128, kInf, kNaN};
    for (uint64_t mode = 0; mode < 4; ++mode) {
      for (uint64_t x : operands) {
        for (uint64_t y : operands) {
          for (uint64_t z : thirds) {
            test.Run(
                [&](PPCContext* ctx) {
                  ctx->r[4] = mode;
                  SetF64Bits(ctx, 1, x);
                  SetF64Bits(ctx, 2, y);
                  SetF64Bits(ctx, 3, z);
                },
                [&](PPCContext* ctx) {
                  uint64_t flag_bits, quirk_bits;
                  std::memcpy(&flag_bits, &ctx->f[4], sizeof(flag_bits));
                  std::memcpy(&quirk_bits, &ctx->f[5], sizeof(quirk_bits));
                  INFO("operation " << build_index << " mode " << mode
                                    << " operands 0x" << std::hex << x << " 0x"
                                    << y << " 0x" << z);
                  REQUIRE(flag_bits == quirk_bits);
                });
            if (!fused || (mode && z == kZero)) {
              break;
            }
          }
        }
      }
    }
  }
}

// Values that a block uses again keep answering for themselves, whichever of
// the uses is the first that fails, and so does the next call, after one in
// which a test failed.
TEST_CASE("SINGLE_RANGE_FLAG_REPEATED_OPERANDS", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    Value* x = LoadFPR(b, 1);
    Value* y = LoadFPR(b, 2);
    Value* z = LoadFPR(b, 3);
    Value* results[][2] = {
        {b.Add(x, y, ARITHMETIC_SINGLE_RANGE),
         b.Select(b.SingleRangeQuirk(x, y, y), F64Constant(b, kNaN),
                  b.Add(x, y))},
        {b.Mul(x, x, ARITHMETIC_SINGLE_RANGE),
         b.Select(b.SingleRangeQuirk(x, x, x), F64Constant(b, kNaN),
                  b.Mul(x, x))},
        {b.Sub(z, x, ARITHMETIC_SINGLE_RANGE),
         b.Select(b.SingleRangeQuirk(z, x, x), F64Constant(b, kNaN),
                  b.Sub(z, x))},
        {b.MulAdd(y, z, x, false, ARITHMETIC_SINGLE_RANGE),
         b.Select(b.SingleRangeQuirk(y, z, x), F64Constant(b, kNaN),
                  b.MulAdd(y, z, x))},
        {b.Mul(z, y, ARITHMETIC_SINGLE_RANGE),
         b.Select(b.SingleRangeQuirk(z, y, y), F64Constant(b, kNaN),
                  b.Mul(z, y))},
    };
    int reg = 10;
    for (auto& result : results) {
      StoreFPR(b, reg++, result[0]);
      StoreFPR(b, reg++, result[1]);
    }
    b.Return();
  });
  const uint64_t operands[] = {kZero,
                               kOne,
                               kSmallestSingle,
                               uint64_t(0x3FF0000000000001ull),
                               kDenormal,
                               kBelowSingle,
                               k128,
                               k128 | 0x8000000000000000ull,
                               kInf,
                               uint64_t(0x7FF0000000000001ull),
                               kNaN};
  for (uint64_t x : operands) {
    for (uint64_t y : operands) {
      for (uint64_t z : operands) {
        test.Run(
            [&](PPCContext* ctx) {
              SetF64Bits(ctx, 1, x);
              SetF64Bits(ctx, 2, y);
              SetF64Bits(ctx, 3, z);
            },
            [&](PPCContext* ctx) {
              for (int reg = 10; reg < 20; reg += 2) {
                uint64_t flag_bits, quirk_bits;
                std::memcpy(&flag_bits, &ctx->f[reg], sizeof(flag_bits));
                std::memcpy(&quirk_bits, &ctx->f[reg + 1], sizeof(quirk_bits));
                INFO("operation " << (reg - 10) / 2 << " operands 0x"
                                  << std::hex << x << " 0x" << y << " 0x"
                                  << z);
                REQUIRE(flag_bits == quirk_bits);
              }
            });
      }
    }
  }
}

// Rounded to single as the PPC front end does, with operands that are out of
// range, NaNs a single can't hold, or both.
TEST_CASE("SINGLE_RANGE_FLAG_TO_SINGLE", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    Value* x = LoadFPR(b, 1);
    Value* y = LoadFPR(b, 2);
    Value* n = b.UnpackSingle(b.Truncate(LoadGPR(b, 3), INT32_TYPE));
    Value* wide = b.ToSingle(x, TO_SINGLE_KEEP_NAN);
    const uint32_t flag = ARITHMETIC_SINGLE_RANGE;
    const auto expect = [&](Value* a1, Value* a2, Value* result) {
      return b.ToSingle(b.Select(b.SingleRangeQuirk(a1, a2, a2),
                                 F64Constant(b, kNaN), result),
                        TO_SINGLE_KEEP_NAN);
    };
    Value* results[][2] = {
        {b.ToSingle(b.Mul(x, n, flag), TO_SINGLE_KEEP_NAN),
         expect(x, n, b.Mul(x, n))},
        {b.ToSingle(b.Sub(x, n, flag), TO_SINGLE_KEEP_NAN),
         expect(x, n, b.Sub(x, n))},
        {b.ToSingle(b.Add(y, n, flag), TO_SINGLE_KEEP_NAN),
         expect(y, n, b.Add(y, n))},
        {b.ToSingle(b.Mul(y, y, flag), TO_SINGLE_KEEP_NAN),
         expect(y, y, b.Mul(y, y))},
        {b.ToSingle(b.Add(x, x, flag), TO_SINGLE_KEEP_NAN),
         expect(x, x, b.Add(x, x))},
        {b.ToSingle(b.Mul(x, y, flag), TO_SINGLE_KEEP_NAN),
         expect(x, y, b.Mul(x, y))},
        // In the range, but can be a wide NaN
        {b.ToSingle(b.Mul(y, wide, flag), TO_SINGLE_KEEP_NAN),
         expect(y, wide, b.Mul(y, wide))},
    };
    int reg = 10;
    for (auto& result : results) {
      StoreFPR(b, reg++, result[0]);
      StoreFPR(b, reg++, result[1]);
    }
    b.Return();
  });
  const uint64_t operands[] = {kZero,
                               0x8000000000000000ull,
                               kOne,
                               0x3FF0000000000001ull,
                               kDenormal,
                               kBelowSingle,
                               kSmallestSingle,
                               kBelow128,
                               k128,
                               kNeg1000,
                               kInf,
                               0xFFF0000000000000ull,
                               kNaN,
                               0x7FF8000000000001ull,
                               0xFFFC000000000003ull,
                               0x7FF0000000000001ull,
                               0xFFF4000020000001ull};
  const uint32_t singles[] = {0,          0x3F800000, 0x80000001, 0x7F7FFFFF,
                              0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001,
                              0x7F800001, 0xFFA00003};
  for (uint64_t x : operands) {
    for (uint64_t y : operands) {
      for (uint32_t n : singles) {
        test.Run(
            [&](PPCContext* ctx) {
              SetF64Bits(ctx, 1, x);
              SetF64Bits(ctx, 2, y);
              ctx->r[3] = n;
            },
            [&](PPCContext* ctx) {
              for (int reg = 10; reg < 24; reg += 2) {
                uint64_t flag_bits, quirk_bits;
                std::memcpy(&flag_bits, &ctx->f[reg], sizeof(flag_bits));
                std::memcpy(&quirk_bits, &ctx->f[reg + 1], sizeof(quirk_bits));
                INFO("operation " << (reg - 10) / 2 << " operands 0x"
                                  << std::hex << x << " 0x" << y << " 0x" << n);
                REQUIRE(flag_bits == quirk_bits);
              }
            });
      }
    }
  }
}

// A block's last reuse forgets its failures, so each time around a loop has to
// find its own, whatever the one before left.
TEST_CASE("SINGLE_RANGE_FLAG_REPEATED_OPERANDS_LOOP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto loop = b.NewLabel();
    auto second = b.NewLabel();
    b.MarkLabel(loop);
    const auto operations = [&](Value* x, Value* y, Value* z, bool flagged) {
      const uint32_t flag = flagged ? ARITHMETIC_SINGLE_RANGE : 0;
      const auto quirk = [&](Value* a1, Value* a2, Value* result) {
        return flagged ? result
                       : b.Select(b.SingleRangeQuirk(a1, a2, a2),
                                  F64Constant(b, kNaN), result);
      };
      Value* bits = b.LoadConstantUint64(0);
      for (Value* result :
           {quirk(x, y, b.Add(x, y, flag)), quirk(x, z, b.Mul(x, z, flag)),
            quirk(z, y, b.Sub(z, y, flag)), quirk(y, y, b.Mul(y, y, flag))}) {
        bits = b.Add(b.Mul(bits, b.LoadConstantUint64(31)),
                     b.Cast(result, INT64_TYPE));
      }
      return bits;
    };
    const auto block = [&](int reg) {
      Value* x = LoadFPR(b, 1);
      Value* y = LoadFPR(b, 2);
      Value* z = LoadFPR(b, 3);
      for (int k = 0; k < 2; ++k) {
        StoreGPR(b, reg + k,
                 b.Add(b.Mul(LoadGPR(b, reg + k), b.LoadConstantUint64(33)),
                       operations(x, y, z, k == 0)));
      }
      // Other operands the next time
      StoreFPR(b, 1, LoadFPR(b, 4));
      StoreFPR(b, 4, y);
      StoreFPR(b, 2, z);
      StoreFPR(b, 3, x);
    };
    block(10);
    b.BranchTrue(b.Truncate(LoadGPR(b, 4), INT8_TYPE), second);
    b.MarkLabel(second);
    block(12);
    Value* left = b.Sub(LoadGPR(b, 5), b.LoadConstantUint64(1));
    StoreGPR(b, 5, left);
    b.BranchTrue(b.IsTrue(left), loop);
    b.Return();
  });
  const uint64_t operands[] = {kZero, kOne, kBelowSingle, kInf, kNaN};
  for (size_t code = 0; code < 5 * 5 * 5 * 5; ++code) {
    test.Run(
        [&](PPCContext* ctx) {
          size_t rest = code;
          for (int reg = 1; reg <= 4; ++reg, rest /= 5) {
            SetF64Bits(ctx, reg, operands[rest % 5]);
          }
          ctx->r[4] = code & 1;
          ctx->r[5] = 9;
          for (int reg = 10; reg < 14; ++reg) {
            ctx->r[reg] = 0;
          }
        },
        [&](PPCContext* ctx) {
          INFO("case " << code);
          REQUIRE(ctx->r[10] == ctx->r[11]);
          REQUIRE(ctx->r[12] == ctx->r[13]);
        });
  }
}

// The proof follows a widened single through a guest FPR across blocks.
TEST_CASE("SINGLE_RANGE_QUIRK_FOLD_ACROSS_BLOCKS", "[instr]") {
  REQUIRE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto label = b.NewLabel();
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
    StoreFPR(b, 2, LoadFPR(b, 1));
    b.MarkLabel(label);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    b.Return();
  }));
  // Not when one path stores something unproven.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto label = b.NewLabel();
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
    StoreFPR(b, 1, LoadFPR(b, 2));
    b.MarkLabel(label);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    b.Return();
  }));
  // Nor across a call, which may write any FPR.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    b.Call(nullptr);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    b.Return();
  }));
  // Nor when the branch leaves mid-block, before a later store proves it.
  bool mid_block_branch = false;
  REQUIRE_FALSE(SimplifiesAwayAfterMerging(
      [](HIRBuilder& b) {
        StoreFPR(b, 1, LoadFPR(b, 2));
        auto label = b.NewLabel();
        b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
        StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
        b.MarkLabel(label);
        Value* f1 = LoadFPR(b, 1);
        StoreGPR(b, 3,
                 b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
        b.Return();
      },
      &mid_block_branch));
  REQUIRE(mid_block_branch);
  // Nor around a loop whose body stores something unproven.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto loop = b.NewLabel();
    b.MarkLabel(loop);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    StoreFPR(b, 1, LoadFPR(b, 2));
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), loop);
    b.Return();
  }));
  // A loop's own sign flip keeps it: every value the loop stores is in range
  // as long as the one it loads is.
  REQUIRE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto loop = b.NewLabel();
    b.MarkLabel(loop);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    StoreFPR(b, 1, b.Neg(f1));
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), loop);
    b.Return();
  }));
  // Unless the value comes into the loop unproven,
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    auto loop = b.NewLabel();
    b.MarkLabel(loop);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    StoreFPR(b, 1, b.Neg(f1));
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), loop);
    b.Return();
  }));
  // or is doubled, which leaves the range,
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto loop = b.NewLabel();
    b.MarkLabel(loop);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    StoreFPR(b, 1, b.Add(f1, f1));
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), loop);
    b.Return();
  }));
  // or swaps places around the loop with one that comes in unproven: the first
  // sweeps tag the loads, and the tags have to go again.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto loop = b.NewLabel();
    b.MarkLabel(loop);
    Value* f1 = LoadFPR(b, 1);
    Value* f2 = LoadFPR(b, 2);
    Value* f3 = LoadFPR(b, 3);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    StoreFPR(b, 1, b.Neg(f2));
    StoreFPR(b, 2, b.Neg(f3));
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), loop);
    b.Return();
  }));
  // Nor when the loop's value goes through a call.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto loop = b.NewLabel();
    b.MarkLabel(loop);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    StoreFPR(b, 1, b.Neg(f1));
    b.Call(nullptr);
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), loop);
    b.Return();
  }));
}
