/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <vector>

#include "xenia/cpu/testing/util.h"

#if XE_ARCH_ARM64
DECLARE_bool(a64_vmx_nan_fixup_in_tail);
DECLARE_bool(a64_vmx_fma_minmax_nan_in_tail);
#endif

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// =============================================================================
// PPC NaN propagation rules for VMX vector float operations:
//
//   1. If src1 is NaN, result = src1 with bit 22 set (quieted).
//   2. Else if src2 is NaN, result = src2 with bit 22 set (quieted).
//   3. Else if the operation itself produces NaN (e.g., inf - inf),
//      result = PPC default QNaN = 0x7FC00000. x86's indefinite is negative.
//
// These tests verify that behavior for VECTOR_ADD with FLOAT32_TYPE.
// =============================================================================

// src1 has QNaN in lane 0, src2 is normal → result lane 0 = src1's NaN quieted.
TEST_CASE("VECTOR_ADD_F32_NAN_SRC1_PROPAGATES", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        // QNaN with payload 0x1234 in lane 0; normal values elsewhere.
        ctx->v[4] = vec128i(0x7FC01234, 0x3F800000, 0x40000000, 0x40400000);
        ctx->v[5] = vec128i(0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Lane 0: src1 NaN wins, bit 22 set, payload preserved.
        REQUIRE((result.u32[0] & 0xFFC00000) == 0x7FC00000);
        REQUIRE((result.u32[0] & 0x003FFFFF) == 0x1234);
        // Lane 1: 1.0 + 1.0 = 2.0
        REQUIRE(result.u32[1] == 0x40000000);
      });
}

// src1 is normal, src2 has QNaN → result = src2's NaN quieted.
TEST_CASE("VECTOR_ADD_F32_NAN_SRC2_PROPAGATES", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000);
        // QNaN with payload 0x5678 in lane 0.
        ctx->v[5] = vec128i(0x7FC05678, 0x3F800000, 0x3F800000, 0x3F800000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Lane 0: src2 NaN wins (src1 was not NaN), payload preserved.
        REQUIRE((result.u32[0] & 0xFFC00000) == 0x7FC00000);
        REQUIRE((result.u32[0] & 0x003FFFFF) == 0x5678);
      });
}

// Both src1 and src2 have NaN in the same lane → src1 wins.
TEST_CASE("VECTOR_ADD_F32_NAN_BOTH_SRC1_WINS", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        // src1: QNaN with payload 0xAAAA
        ctx->v[4] = vec128i(0x7FC0AAAA, 0x3F800000, 0x3F800000, 0x3F800000);
        // src2: QNaN with payload 0xBBBB
        ctx->v[5] = vec128i(0x7FC0BBBB, 0x3F800000, 0x3F800000, 0x3F800000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // src1's NaN wins when both are NaN.
        REQUIRE((result.u32[0] & 0xFFC00000) == 0x7FC00000);
        REQUIRE((result.u32[0] & 0x003FFFFF) == 0x0AAAA);
      });
}

// SNaN in src1 → result is src1 with bit 22 SET (quieted to QNaN).
TEST_CASE("VECTOR_ADD_F32_SNAN_QUIETED", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        // SNaN: exponent all 1s, bit 22 = 0, mantissa != 0.
        // 0x7F812345 = SNaN with payload.
        ctx->v[4] = vec128i(0x7F812345, 0x3F800000, 0x3F800000, 0x3F800000);
        ctx->v[5] = vec128i(0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // SNaN quieted: bit 22 forced on, rest of payload preserved.
        REQUIRE(result.u32[0] == (0x7F812345 | 0x00400000));
      });
}

// Neither input is NaN, but operation produces NaN (inf + (-inf)).
// Result should be PPC default QNaN = 0x7FC00000.
TEST_CASE("VECTOR_ADD_F32_INF_MINUS_INF_DEFAULT_NAN", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        // +inf + (-inf) → NaN
        ctx->v[4] = vec128i(0x7F800000, 0x3F800000, 0x3F800000, 0x3F800000);
        ctx->v[5] = vec128i(0xFF800000, 0x3F800000, 0x3F800000, 0x3F800000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // PPC default QNaN is positive. x86's indefinite is the negative one.
        REQUIRE(result.u32[0] == 0x7FC00000);
        // Other lanes: normal 1.0 + 1.0 = 2.0
        REQUIRE(result.u32[1] == 0x40000000);
      });
}

// NaN in different lanes — verify per-lane independence.
TEST_CASE("VECTOR_ADD_F32_NAN_PER_LANE", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        // Lane 0: normal + normal
        // Lane 1: NaN in src1
        // Lane 2: NaN in src2
        // Lane 3: inf + (-inf)
        ctx->v[4] = vec128i(0x3F800000, 0x7FC00001, 0x3F800000, 0x7F800000);
        ctx->v[5] = vec128i(0x3F800000, 0x3F800000, 0x7FC00002, 0xFF800000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Lane 0: 1.0 + 1.0 = 2.0
        REQUIRE(result.u32[0] == 0x40000000);
        // Lane 1: src1 NaN propagated, quieted.
        REQUIRE((result.u32[1] & 0xFFC00000) == 0x7FC00000);
        REQUIRE((result.u32[1] & 0x003FFFFF) == 0x00001);
        // Lane 2: src2 NaN propagated, quieted.
        REQUIRE((result.u32[2] & 0xFFC00000) == 0x7FC00000);
        REQUIRE((result.u32[2] & 0x003FFFFF) == 0x00002);
        // Lane 3: generated NaN → PPC default.
        REQUIRE(result.u32[3] == 0x7FC00000);
      });
}

// vsubfp follows the same rules. inf - inf is the generated-NaN case here.
TEST_CASE("VECTOR_SUB_F32_NAN_RULES", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorSub(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        // Lane 0: src1 NaN wins over src2 NaN.
        // Lane 1: src2 NaN propagates.
        // Lane 2: SNaN quieted, payload kept.
        // Lane 3: inf - inf, neither operand NaN.
        ctx->v[4] = vec128i(0x7FC00001, 0x3F800000, 0x7F812345, 0x7F800000);
        ctx->v[5] = vec128i(0x7FC00002, 0x7FC00003, 0x3F800000, 0x7F800000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result.u32[0] == 0x7FC00001);
        REQUIRE(result.u32[1] == 0x7FC00003);
        REQUIRE(result.u32[2] == (0x7F812345 | 0x00400000));
        REQUIRE(result.u32[3] == 0x7FC00000);
      });
}

// vmulfp is OPCODE_MUL on VEC128. inf * 0 is its generated-NaN case.
TEST_CASE("MUL_V128_F32_NAN_RULES", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Mul(LoadVR(b, 4), LoadVR(b, 5)));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        // Lane 0: src1 NaN wins over src2 NaN.
        // Lane 1: src2 NaN propagates.
        // Lane 2: SNaN quieted, payload kept.
        // Lane 3: inf * 0, neither operand NaN.
        ctx->v[4] = vec128i(0x7FC00001, 0x3F800000, 0x7F812345, 0x7F800000);
        ctx->v[5] = vec128i(0x7FC00002, 0x7FC00003, 0x3F800000, 0x00000000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result.u32[0] == 0x7FC00001);
        REQUIRE(result.u32[1] == 0x7FC00003);
        REQUIRE(result.u32[2] == (0x7F812345 | 0x00400000));
        REQUIRE(result.u32[3] == 0x7FC00000);
      });
}

// =============================================================================
// Value matrix: every ordered pair of the lane values below through add, sub,
// mul and div, against the rule stated at the top of this file. On a64 it runs
// with the NaN fixup in the function tail and with it applied to every result,
// and the two must also agree bit for bit in the lanes with no NaN operand.
// =============================================================================

namespace {

const uint32_t kNanMatrixLanes[] = {
    0x00000000, 0x80000000,  // zeros
    0x3F800000, 0xBF800000,  // +-1
    0x40400000, 0x7F7FFFFF,  // 3, largest normal
    0x00800000, 0x80800000,  // smallest normals
    0x00000001, 0x807FFFFF,  // denormals
    0x7F800000, 0xFF800000,  // infinities
    0x7FC00000, 0xFFC00000,  // default QNaN and its negative
    0x7FC12345, 0xFFE00001,  // QNaNs with payloads
    0x7F800001, 0xFFBFFFFF,  // SNaNs
    0x7FA54321, 0xFF812345,
};
constexpr size_t kNanMatrixLaneCount =
    sizeof(kNanMatrixLanes) / sizeof(kNanMatrixLanes[0]);

bool IsNanBits(uint32_t bits) { return (bits & 0x7FFFFFFF) > 0x7F800000; }

using NanMatrixOp = Value* (*)(HIRBuilder& b, Value* a, Value* c);

// Results of `op` over the whole matrix, four pairs to a vector.
std::vector<uint32_t> RunNanMatrix(NanMatrixOp op) {
  TestFunction test([op](HIRBuilder& b) {
    StoreVR(b, 3, op(b, LoadVR(b, 4), LoadVR(b, 5)));
    b.Return();
  });
  std::vector<uint32_t> results;
  const size_t pairs = kNanMatrixLaneCount * kNanMatrixLaneCount;
  for (size_t first = 0; first < pairs; first += 4) {
    test.Run(
        [first, pairs](PPCContext* ctx) {
          for (size_t lane = 0; lane < 4; ++lane) {
            const size_t pair = (first + lane) % pairs;
            ctx->v[4].u32[lane] = kNanMatrixLanes[pair / kNanMatrixLaneCount];
            ctx->v[5].u32[lane] = kNanMatrixLanes[pair % kNanMatrixLaneCount];
          }
        },
        [&results](PPCContext* ctx) {
          for (size_t lane = 0; lane < 4; ++lane) {
            results.push_back(ctx->v[3].u32[lane]);
          }
        });
  }
  return results;
}

void CheckNanMatrix(NanMatrixOp op) {
  std::vector<uint32_t> results = RunNanMatrix(op);
  const size_t pairs = kNanMatrixLaneCount * kNanMatrixLaneCount;
  REQUIRE(results.size() >= pairs);
  for (size_t pair = 0; pair < pairs; ++pair) {
    const uint32_t a = kNanMatrixLanes[pair / kNanMatrixLaneCount];
    const uint32_t c = kNanMatrixLanes[pair % kNanMatrixLaneCount];
    CAPTURE(a, c);
    if (IsNanBits(a)) {
      REQUIRE(results[pair] == (a | 0x00400000));
    } else if (IsNanBits(c)) {
      REQUIRE(results[pair] == (c | 0x00400000));
    } else if (IsNanBits(results[pair])) {
      REQUIRE(results[pair] == 0x7FC00000);
    }
  }
#if XE_ARCH_ARM64
  const bool in_tail = cvars::a64_vmx_nan_fixup_in_tail;
  cvars::a64_vmx_nan_fixup_in_tail = !in_tail;
  std::vector<uint32_t> other = RunNanMatrix(op);
  cvars::a64_vmx_nan_fixup_in_tail = in_tail;
  REQUIRE(other == results);
#endif
}

}  // namespace

TEST_CASE("VMX_FP_BINOP_NAN_MATRIX", "[instr]") {
  SECTION("vaddfp") {
    CheckNanMatrix([](HIRBuilder& b, Value* a, Value* c) {
      return b.VectorAdd(a, c, FLOAT32_TYPE);
    });
  }
  SECTION("vsubfp") {
    CheckNanMatrix([](HIRBuilder& b, Value* a, Value* c) {
      return b.VectorSub(a, c, FLOAT32_TYPE);
    });
  }
  SECTION("mul") {
    CheckNanMatrix(
        [](HIRBuilder& b, Value* a, Value* c) { return b.Mul(a, c); });
  }
  SECTION("div") {
    CheckNanMatrix(
        [](HIRBuilder& b, Value* a, Value* c) { return b.Div(a, c); });
  }
  SECTION("add") {
    CheckNanMatrix(
        [](HIRBuilder& b, Value* a, Value* c) { return b.Add(a, c); });
  }
  SECTION("sub") {
    CheckNanMatrix(
        [](HIRBuilder& b, Value* a, Value* c) { return b.Sub(a, c); });
  }
}

// The same operand twice, a constant operand on either side, and a chain whose
// middle result is both a result and an operand, so the fixup runs with the
// destination aliasing a source and with a source in a scratch register.
TEST_CASE("VMX_FP_BINOP_NAN_OPERAND_FORMS", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    Value* a = LoadVR(b, 4);
    Value* c = LoadVR(b, 5);
    Value* k = b.LoadConstantVec128(
        vec128i(0x7FA00007, 0x3F800000, 0x7FC00009, 0x40000000));
    StoreVR(b, 6, b.Mul(a, a));
    StoreVR(b, 7, b.Mul(a, k));
    StoreVR(b, 8, b.Mul(k, c));
    Value* sum = b.VectorAdd(a, c, FLOAT32_TYPE);
    sum = b.Mul(sum, c);
    sum = b.VectorSub(sum, a, FLOAT32_TYPE);
    StoreVR(b, 9, sum);
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x7F800001, 0x40000000, 0x3F800000, 0x7FC00002);
        ctx->v[5] = vec128i(0x7FC00003, 0x40400000, 0x7F900004, 0x7F800005);
      },
      [](PPCContext* ctx) {
        // a * a.
        REQUIRE(ctx->v[6] ==
                vec128i(0x7FC00001, 0x40800000, 0x3F800000, 0x7FC00002));
        // a * k: a's NaN first, then k's.
        REQUIRE(ctx->v[7] ==
                vec128i(0x7FC00001, 0x40000000, 0x7FC00009, 0x7FC00002));
        // k * c: k's NaN first, then c's.
        REQUIRE(ctx->v[8] ==
                vec128i(0x7FE00007, 0x40400000, 0x7FC00009, 0x7FC00005));
        // ((a + c) * c) - a: lane 0 a, lane 1 13, lane 2 c, lane 3 a.
        REQUIRE(ctx->v[9] ==
                vec128i(0x7FC00001, 0x41500000, 0x7FD00004, 0x7FC00002));
      });
}

// =============================================================================
// vmaxfp, vminfp, vmaddfp and vnmsubfp over the same lane values. Maximum and
// minimum return a NaN operand quieted, vA before vB; the multiply-adds return
// the first NaN in A, B, C order quieted, and the default NaN where the
// operation is invalid without one. On a64 each also runs with the fixup
// behind a result test in the function tail and with the fixup inline, and the
// two must agree bit for bit in every lane.
// =============================================================================

namespace {

void CheckMaxMinNanMatrix(NanMatrixOp op) {
  std::vector<uint32_t> results = RunNanMatrix(op);
  const size_t pairs = kNanMatrixLaneCount * kNanMatrixLaneCount;
  REQUIRE(results.size() >= pairs);
  for (size_t pair = 0; pair < pairs; ++pair) {
    const uint32_t a = kNanMatrixLanes[pair / kNanMatrixLaneCount];
    const uint32_t c = kNanMatrixLanes[pair % kNanMatrixLaneCount];
    CAPTURE(a, c);
    if (IsNanBits(a)) {
      REQUIRE(results[pair] == (a | 0x00400000));
    } else if (IsNanBits(c)) {
      REQUIRE(results[pair] == (c | 0x00400000));
    } else {
      REQUIRE_FALSE(IsNanBits(results[pair]));
    }
  }
#if XE_ARCH_ARM64
  const bool in_tail = cvars::a64_vmx_fma_minmax_nan_in_tail;
  cvars::a64_vmx_fma_minmax_nan_in_tail = !in_tail;
  std::vector<uint32_t> other = RunNanMatrix(op);
  cvars::a64_vmx_fma_minmax_nan_in_tail = in_tail;
  REQUIRE(other == results);
#endif
}

using NanMatrixOp3 = Value* (*)(HIRBuilder& b, Value* a, Value* c, Value* bb);

// Results of `op` over every ordered triple, four to a vector. The HIR
// operands are (A, C, B) for A * C + B.
std::vector<uint32_t> RunNanMatrix3(NanMatrixOp3 op) {
  TestFunction test([op](HIRBuilder& b) {
    StoreVR(b, 3, op(b, LoadVR(b, 4), LoadVR(b, 5), LoadVR(b, 6)));
    b.Return();
  });
  std::vector<uint32_t> results;
  const size_t n = kNanMatrixLaneCount;
  const size_t triples = n * n * n;
  for (size_t first = 0; first < triples; first += 4) {
    test.Run(
        [first, triples, n](PPCContext* ctx) {
          for (size_t lane = 0; lane < 4; ++lane) {
            const size_t triple = (first + lane) % triples;
            ctx->v[4].u32[lane] = kNanMatrixLanes[triple / (n * n)];
            ctx->v[5].u32[lane] = kNanMatrixLanes[(triple / n) % n];
            ctx->v[6].u32[lane] = kNanMatrixLanes[triple % n];
          }
        },
        [&results](PPCContext* ctx) {
          for (size_t lane = 0; lane < 4; ++lane) {
            results.push_back(ctx->v[3].u32[lane]);
          }
        });
  }
  return results;
}

void CheckFmaNanMatrix(NanMatrixOp3 op) {
  std::vector<uint32_t> results = RunNanMatrix3(op);
  const size_t n = kNanMatrixLaneCount;
  const size_t triples = n * n * n;
  REQUIRE(results.size() >= triples);
  size_t wrong = 0;
  uint32_t first_wrong[4] = {};
  for (size_t triple = 0; triple < triples; ++triple) {
    const uint32_t a = kNanMatrixLanes[triple / (n * n)];
    const uint32_t c = kNanMatrixLanes[(triple / n) % n];
    const uint32_t bb = kNanMatrixLanes[triple % n];
    uint32_t expected = results[triple];
    if (IsNanBits(a)) {
      expected = a | 0x00400000;
    } else if (IsNanBits(bb)) {
      expected = bb | 0x00400000;
    } else if (IsNanBits(c)) {
      expected = c | 0x00400000;
    } else if (IsNanBits(results[triple])) {
      // The default NaN, with the sign the negated forms give it.
      expected = 0x7FC00000 | (results[triple] & 0x80000000);
    }
    if (results[triple] != expected && !wrong++) {
      first_wrong[0] = a;
      first_wrong[1] = c;
      first_wrong[2] = bb;
      first_wrong[3] = results[triple];
    }
  }
  CAPTURE(first_wrong[0], first_wrong[1], first_wrong[2], first_wrong[3]);
  REQUIRE(wrong == 0);
#if XE_ARCH_ARM64
  const bool in_tail = cvars::a64_vmx_fma_minmax_nan_in_tail;
  cvars::a64_vmx_fma_minmax_nan_in_tail = !in_tail;
  std::vector<uint32_t> other = RunNanMatrix3(op);
  cvars::a64_vmx_fma_minmax_nan_in_tail = in_tail;
  REQUIRE(other == results);
#endif
}

}  // namespace

TEST_CASE("VMX_FP_MAXMIN_NAN_MATRIX", "[instr]") {
  SECTION("max") {
    CheckMaxMinNanMatrix(
        [](HIRBuilder& b, Value* a, Value* c) { return b.Max(a, c); });
  }
  SECTION("min") {
    CheckMaxMinNanMatrix(
        [](HIRBuilder& b, Value* a, Value* c) { return b.Min(a, c); });
  }
  SECTION("vector_max") {
    CheckMaxMinNanMatrix([](HIRBuilder& b, Value* a, Value* c) {
      return b.VectorMax(a, c, FLOAT32_TYPE);
    });
  }
  SECTION("vector_min") {
    CheckMaxMinNanMatrix([](HIRBuilder& b, Value* a, Value* c) {
      return b.VectorMin(a, c, FLOAT32_TYPE);
    });
  }
}

TEST_CASE("VMX_FMA_NAN_MATRIX", "[instr]") {
  SECTION("mul_add") {
    CheckFmaNanMatrix([](HIRBuilder& b, Value* a, Value* c, Value* bb) {
      return b.MulAdd(a, c, bb);
    });
  }
  SECTION("mul_sub_negated") {
    CheckFmaNanMatrix([](HIRBuilder& b, Value* a, Value* c, Value* bb) {
      return b.MulSub(a, c, bb, /*negate_result=*/true);
    });
  }
  SECTION("mul_sub") {
    CheckFmaNanMatrix([](HIRBuilder& b, Value* a, Value* c, Value* bb) {
      return b.MulSub(a, c, bb);
    });
  }
}

// The same operand twice, a constant on either side and chains, so the tail
// runs with the destination aliasing a source and a source in a scratch
// register. Both forms of the sequence must give the same vectors.
TEST_CASE("VMX_FP_MAXMIN_FMA_NAN_OPERAND_FORMS", "[instr]") {
  const auto run = [] {
    TestFunction test([](HIRBuilder& b) {
      Value* a = LoadVR(b, 4);
      Value* c = LoadVR(b, 5);
      Value* k = b.LoadConstantVec128(
          vec128i(0x7FA00007, 0x3F800000, 0x7FC00009, 0x40000000));
      StoreVR(b, 6, b.Max(a, a));
      StoreVR(b, 7, b.Max(a, k));
      StoreVR(b, 8, b.Min(k, c));
      Value* m = b.Max(a, c);
      m = b.Min(m, c);
      m = b.Max(a, m);
      StoreVR(b, 9, m);
      StoreVR(b, 10, b.MulAdd(a, a, a));
      StoreVR(b, 11, b.MulAdd(a, k, c));
      StoreVR(b, 12, b.MulSub(k, c, a, true));
      Value* f = b.MulAdd(a, c, a);
      f = b.MulAdd(f, c, f);
      f = b.MulSub(c, f, f, true);
      StoreVR(b, 13, f);
      b.Return();
    });
    std::vector<vec128_t> out;
    test.Run(
        [](PPCContext* ctx) {
          ctx->v[4] = vec128i(0x7F800001, 0x40000000, 0x3F800000, 0x7FC00002);
          ctx->v[5] = vec128i(0x7FC00003, 0x40400000, 0x7F900004, 0x7F800005);
        },
        [&out](PPCContext* ctx) {
          for (int n = 6; n <= 13; ++n) {
            out.push_back(ctx->v[n]);
          }
        });
    return out;
  };
  const std::vector<vec128_t> results = run();
  // max(a, a), max(a, k), min(k, c): the first NaN by position, quieted.
  REQUIRE(results[0] ==
          vec128i(0x7FC00001, 0x40000000, 0x3F800000, 0x7FC00002));
  REQUIRE(results[1] ==
          vec128i(0x7FC00001, 0x40000000, 0x7FC00009, 0x7FC00002));
  REQUIRE(results[2] ==
          vec128i(0x7FE00007, 0x3F800000, 0x7FC00009, 0x7FC00005));
  // max(a, min(max(a, c), c)): lane 0 a, lane 1 3, lane 2 c, lane 3 a.
  REQUIRE(results[3] ==
          vec128i(0x7FC00001, 0x40400000, 0x7FD00004, 0x7FC00002));
#if XE_ARCH_ARM64
  const bool in_tail = cvars::a64_vmx_fma_minmax_nan_in_tail;
  cvars::a64_vmx_fma_minmax_nan_in_tail = !in_tail;
  const std::vector<vec128_t> other = run();
  cvars::a64_vmx_fma_minmax_nan_in_tail = in_tail;
  REQUIRE(other == results);
#endif
}
