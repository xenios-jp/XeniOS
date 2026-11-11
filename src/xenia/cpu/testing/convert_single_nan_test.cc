/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include "xenia/cpu/cpu_flags.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace xe::cpu::hir;
using namespace xe::cpu;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

static void SetF64Bits(PPCContext* ctx, int reg, uint64_t bits) {
  std::memcpy(&ctx->f[reg], &bits, sizeof(bits));
}
static uint64_t GetF64Bits(PPCContext* ctx, int reg) {
  uint64_t bits;
  std::memcpy(&bits, &ctx->f[reg], sizeof(bits));
  return bits;
}
static bool IsDoubleNaN(uint64_t bits) {
  return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
         (bits & 0x000FFFFFFFFFFFFFull) != 0;
}

// double -> single (stfs). PowerPC keeps the signaling bit; the host convert
// would quiet it. The trailing inf case is the documented payload-truncation
// behavior (top 23 fraction bits zero -> single fraction zero -> infinity),
// which matches Xenon's stfs.
TEST_CASE("PACK_SINGLE_KEEP_NAN", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    Value* sbits = b.PackSingle(LoadFPR(b, 1));
    StoreGPR(b, 3, b.ZeroExtend(sbits, INT64_TYPE));
    b.Return();
  });

  // {double input bits, expected single bits}
  const std::vector<std::pair<uint64_t, uint32_t>> cases = {
      {0x3FF0000000000000ull, 0x3F800000u},  // 1.0
      {0xC000000000000000ull, 0xC0000000u},  // -2.0
      // Rounding carries into or out of the single's bit 22. A finite input
      // must not take the NaN fixup, which would restore that bit from the
      // unrounded double.
      {0x3FF7FFFFF0000000ull, 0x3FC00000u},  // tie rounds to +1.5
      {0xBFF7FFFFF0000000ull, 0xBFC00000u},  // tie rounds to -1.5
      {0x3FFFFFFFF0000000ull, 0x40000000u},  // tie rounds to +2.0
      {0xBFFFFFFFF0000000ull, 0xC0000000u},  // tie rounds to -2.0
      {0x7FF0000000000000ull, 0x7F800000u},  // +inf
      {0xFFF0000000000000ull, 0xFF800000u},  // -inf
      {0x7FF8000000000000ull, 0x7FC00000u},  // qnan stays quiet
      {0x7FF4000000000000ull, 0x7FA00000u},  // snan stays signaling
      {0xFFF4000000000000ull, 0xFFA00000u},  // -snan stays signaling
      {0x7FF0000000000001ull, 0x7F800000u},  // snan payload truncated -> inf
  };
  for (const auto& c : cases) {
    INFO("double 0x" << std::hex << c.first);
    test.Run([&](PPCContext* ctx) { SetF64Bits(ctx, 1, c.first); },
             [&](PPCContext* ctx) {
               REQUIRE(static_cast<uint32_t>(ctx->r[3]) == c.second);
             });
  }
}

// A constant operand is materialised in a scratch register first.
TEST_CASE("PACK_SINGLE_KEEP_NAN_CONSTANT", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    Value* sbits = b.PackSingle(
        b.Cast(b.LoadConstantUint64(0x7FF4000000000000ull), FLOAT64_TYPE));
    StoreGPR(b, 3, b.ZeroExtend(sbits, INT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) {},
           [](PPCContext* ctx) {
             REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x7FA00000u);
           });
}

// single -> double (lfs). Widening never drops NaN-ness, so the only fixup is
// keeping the signaling bit the host convert would otherwise quiet.
TEST_CASE("UNPACK_SINGLE_KEEP_NAN", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    Value* sbits = b.Truncate(LoadGPR(b, 1), INT32_TYPE);
    StoreFPR(b, 3, b.UnpackSingle(sbits));
    b.Return();
  });

  // {single input bits, expected double bits}
  const std::vector<std::pair<uint32_t, uint64_t>> cases = {
      {0x3F800000u, 0x3FF0000000000000ull},  // 1.0
      {0xC0000000u, 0xC000000000000000ull},  // -2.0
      {0x7F800000u, 0x7FF0000000000000ull},  // +inf
      {0xFF800000u, 0xFFF0000000000000ull},  // -inf
      {0x7FC00000u, 0x7FF8000000000000ull},  // qnan stays quiet
      {0x7FA00000u, 0x7FF4000000000000ull},  // snan stays signaling
      {0xFFA00000u, 0xFFF4000000000000ull},  // -snan stays signaling
  };
  for (const auto& c : cases) {
    INFO("single 0x" << std::hex << c.first);
    test.Run([&](PPCContext* ctx) { ctx->r[1] = c.first; },
             [&](PPCContext* ctx) { REQUIRE(GetF64Bits(ctx, 3) == c.second); });
  }
}

// frsp is arithmetic, not data movement: PowerPC quiets signaling NaNs. It
// lowers to a plain double->single->double convert, so the NaN-preserving fix
// must stay scoped to lfs/stfs and leave this path quieting.
TEST_CASE("FRSP_QUIETS_SNAN", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3,
             b.Convert(b.Convert(LoadFPR(b, 1), FLOAT32_TYPE), FLOAT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { SetF64Bits(ctx, 1, 0x7FF4000000000000ull); },
           [](PPCContext* ctx) {
             uint64_t out = GetF64Bits(ctx, 3);
             INFO("frsp(snan) -> 0x" << std::hex << out);
             REQUIRE(IsDoubleNaN(out));
             REQUIRE((out & (1ull << 51)) != 0);  // quieted
           });
}

// lfs then stfs of the same value stores the bits that were loaded, for every
// class of single: all 256 exponents with empty, lowest-bit, quiet-bit-only,
// all-but-quiet-bit, full and mixed mantissas, both signs. With the fold on
// the pair is an assign; with it off both conversions run, in the Fpu mode
// and, after a vector op, in a VMX mode.
TEST_CASE("UNPACK_THEN_PACK_SINGLE_IS_IDENTITY", "[instr]") {
  const bool fold_default = cvars::fold_single_round_trip;
  for (int variant = 0; variant < 4; ++variant) {
    cvars::fold_single_round_trip = (variant & 1) != 0;
    const bool after_vmx = (variant & 2) != 0;
    TestFunction test([after_vmx](HIRBuilder& b) {
      if (after_vmx) {
        StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
      }
      for (int n = 0; n < 6; ++n) {
        Value* single_bits = b.Truncate(LoadGPR(b, 4 + n), INT32_TYPE);
        Value* wide = b.UnpackSingle(single_bits);
        StoreFPR(b, 1 + n, wide);
        StoreGPR(b, 14 + n, b.ZeroExtend(b.PackSingle(wide), INT64_TYPE));
      }
      b.Return();
    });
    static const uint32_t kMantissas[6] = {0x000000, 0x000001, 0x400000,
                                           0x3FFFFF, 0x7FFFFF, 0x2AAAAA};
    for (uint32_t sign = 0; sign < 2; ++sign) {
      for (uint32_t exponent = 0; exponent < 256; ++exponent) {
        const uint32_t base = (sign << 31) | (exponent << 23);
        test.Run(
            [base](PPCContext* ctx) {
              for (int n = 0; n < 6; ++n) {
                ctx->r[4 + n] = base | kMantissas[n];
              }
            },
            [base, variant](PPCContext* ctx) {
              for (int n = 0; n < 6; ++n) {
                CAPTURE(variant, base | kMantissas[n]);
                REQUIRE(ctx->r[14 + n] == (base | kMantissas[n]));
              }
            });
      }
    }
  }
  cvars::fold_single_round_trip = fold_default;
}
DECLARE_bool(a64_lfs_quiet_unobserved);
DECLARE_bool(a64_stfs_skip_quiet_test);
  for (int variant = 0; variant < 4; ++variant) {
    const bool after_vmx = (variant & 2) != 0;
    TestFunction test([after_vmx](HIRBuilder& b) {
      if (after_vmx) {
        StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
      }
      for (int n = 0; n < 6; ++n) {
      }
      b.Return();
    });
    static const uint32_t kMantissas[6] = {0x000000, 0x000001, 0x400000,
                                           0x3FFFFF, 0x7FFFFF, 0x2AAAAA};
    for (uint32_t sign = 0; sign < 2; ++sign) {
      for (uint32_t exponent = 0; exponent < 256; ++exponent) {
        const uint32_t base = (sign << 31) | (exponent << 23);
        test.Run(
            [base](PPCContext* ctx) {
              for (int n = 0; n < 6; ++n) {
                ctx->r[4 + n] = base | kMantissas[n];
              }
            },
              for (int n = 0; n < 6; ++n) {
                CAPTURE(variant, base | kMantissas[n]);

// A loaded single read only by arithmetic, rounding, compares and NaN tests
// gives them the same answers whether or not its signalling bit was kept, and
// one that is also stored keeps the bit. Every reader, with the loaded value
// in each operand position, over singles and doubles of every class, in the
// Fpu mode and after a vector op; on a64 with the test skipped and kept.
TEST_CASE("UNPACK_SINGLE_READERS_MATRIX", "[instr]") {
  static const uint32_t kSingles[] = {
      0x00000000, 0x80000000, 0x3F800000, 0xC0400000, 0x00000001,
      0x807FFFFF, 0x00800000, 0x7F7FFFFF, 0x7F800000, 0xFF800000,
      0x7FC00000, 0xFFC12345, 0x7F800001, 0xFFA54321, 0x7FBFFFFF,
  };
  static const uint64_t kDoubles[] = {
      0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull,
      0xC008000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
      0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
      0x7FF8000000000000ull, 0xFFF8000000012345ull, 0x7FF0000000000001ull,
      0xFFF4000000054321ull, 0x36A0000000000000ull,
  };
  const auto run = [](bool after_vmx) {
    TestFunction test([after_vmx](HIRBuilder& b) {
      if (after_vmx) {
        StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
      }
      // A fresh load for each reader, so each has no other reader.
      const auto load = [&b] {
        return b.UnpackSingle(b.Truncate(LoadGPR(b, 4), INT32_TYPE));
      };
      Value* d = LoadFPR(b, 1);
      Value* e = LoadFPR(b, 2);
      int out = 3;
      const auto store = [&b, &out](Value* v) { StoreFPR(b, out++, v); };
      const auto flag = [&b, &out](Value* v) {
        StoreGPR(b, out++, b.ZeroExtend(v, INT64_TYPE));
      };
      store(b.Add(d, load()));
      store(b.Add(load(), d));
      store(b.Sub(d, load()));
      store(b.Sub(load(), d));
      store(b.Mul(d, load()));
      store(b.Mul(load(), d));
      store(b.Div(d, load()));
      store(b.Div(load(), d));
      store(b.MulAdd(load(), d, e));
      store(b.MulAdd(d, load(), e));
      store(b.MulAdd(d, e, load()));
      store(b.MulSub(load(), d, e));
      store(b.MulSub(d, e, load(), true));
      store(b.ToSingle(load()));
      store(b.ToSingle(b.Mul(d, load())));
      Value* twice = load();
      store(b.Add(twice, twice));
      Value* both = load();
      store(b.Mul(b.Add(both, d), both));
      // Two loads, so the first operand's reader sees a load in the second.
      store(b.Add(load(), load()));
      out = 10;
      flag(b.CompareSLT(d, load()));
      flag(b.CompareSGT(load(), d));
      flag(b.CompareEQ(d, load()));
      flag(b.IsNan(load()));
      Value* q = load();
      flag(b.DenormalQuirk(d, q, q));
      // Also stored: the register must hold the signalling bit, and the sum
      // must still be the same.
      Value* kept = load();
      StoreFPR(b, 30, kept);
      StoreFPR(b, 31, b.Add(d, kept));
      b.Return();
    });
    std::vector<uint64_t> results;
    for (uint32_t single : kSingles) {
      for (uint64_t d : kDoubles) {
        for (uint64_t e : {kDoubles[2], kDoubles[10], kDoubles[11]}) {
          test.Run(
              [single, d, e](PPCContext* ctx) {
                ctx->r[4] = single;
                SetF64Bits(ctx, 1, d);
                SetF64Bits(ctx, 2, e);
              },
              [&results, single](PPCContext* ctx) {
                for (int n = 3; n <= 20; ++n) {
                  results.push_back(GetF64Bits(ctx, n));
                }
                for (int n = 10; n <= 14; ++n) {
                  results.push_back(ctx->r[n]);
                }
                results.push_back(GetF64Bits(ctx, 31));
                // The stored copy is the exact widening.
                const uint64_t kept = GetF64Bits(ctx, 30);
                results.push_back(kept);
                if ((single & 0x7FFFFFFF) > 0x7F800000) {
                  REQUIRE(kept == ((uint64_t(single >> 31) << 63) |
                                   0x7FF0000000000000ull |
                                   (uint64_t(single & 0x7FFFFF) << 29)));
                }
              });
        }
      }
    }
    return results;
  };
  for (bool after_vmx : {false, true}) {
    const std::vector<uint64_t> results = run(after_vmx);
    REQUIRE(results.size() == 15u * 14u * 3u * 25u);
#if XE_ARCH_ARM64
    const bool saved = cvars::a64_lfs_quiet_unobserved;
    cvars::a64_lfs_quiet_unobserved = !saved;
    const std::vector<uint64_t> other = run(after_vmx);
    cvars::a64_lfs_quiet_unobserved = saved;
    size_t differing = 0;
    for (size_t n = 0; n < results.size(); ++n) {
      differing += results[n] != other[n];
    }
    REQUIRE(differing == 0);
#endif
  }
}

// stfs of a value that can never be a signalling NaN (an arithmetic result,
// a rounding to single) stores the same single with or without the NaN test,
// and stfs of a value that can be one keeps the test. Over doubles of every
// class, in the Fpu mode and after a vector op.
TEST_CASE("PACK_SINGLE_OF_ARITHMETIC_MATRIX", "[instr]") {
  static const uint64_t kDoubles[] = {
      0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull,
      0xC008000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
      0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
      0x7FF8000000000000ull, 0xFFF8000000012345ull, 0x7FF0000000000001ull,
      0xFFF4000000054321ull, 0x36A0000000000000ull, 0x3FF7FFFFF0000000ull,
      0x47EFFFFFF0000000ull, 0x7FFFFFFFFFFFFFFFull, 0x7FF7FFFFE0000000ull,
  };
  const auto run = [](bool after_vmx) {
    TestFunction test([after_vmx](HIRBuilder& b) {
      if (after_vmx) {
        StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
      }
      Value* d = LoadFPR(b, 1);
      Value* e = LoadFPR(b, 2);
      int out = 10;
      const auto pack = [&b, &out](Value* v) {
        StoreGPR(b, out++, b.ZeroExtend(b.PackSingle(v), INT64_TYPE));
      };
      pack(b.Add(d, e));
      pack(b.Sub(d, e));
      pack(b.Mul(d, e));
      pack(b.Div(d, e));
      pack(b.MulAdd(d, e, d));
      pack(b.MulSub(d, e, e, true));
      pack(b.ToSingle(d));
      pack(b.ToSingle(b.Mul(d, e)));
      pack(b.Neg(b.Add(d, e)));
      pack(b.Abs(b.Mul(d, e)));
      // These can be signalling and keep the test.
      pack(d);
      pack(b.Neg(e));
      pack(b.Select(b.Truncate(LoadGPR(b, 4), INT8_TYPE), d, b.Add(d, e)));
      b.Return();
    });
    std::vector<uint32_t> results;
    for (uint64_t d : kDoubles) {
      for (uint64_t e : kDoubles) {
        for (uint64_t pick = 0; pick < 2; ++pick) {
          test.Run(
              [d, e, pick](PPCContext* ctx) {
                SetF64Bits(ctx, 1, d);
                SetF64Bits(ctx, 2, e);
                ctx->r[4] = pick;
              },
              [&results, d, e](PPCContext* ctx) {
                for (int n = 10; n <= 22; ++n) {
                  results.push_back(uint32_t(ctx->r[n]));
                }
                // stfs of the register itself moves a NaN's bits: sign,
                // quiet bit and the top 22 payload bits.
                if (IsDoubleNaN(d)) {
                  REQUIRE(uint32_t(ctx->r[20]) ==
                          (uint32_t(d >> 63) << 31 | 0x7F800000u |
                           uint32_t((d >> 29) & 0x7FFFFF)));
                }
                if (IsDoubleNaN(e)) {
                  REQUIRE(uint32_t(ctx->r[21]) ==
                          (uint32_t(~e >> 63) << 31 | 0x7F800000u |
                           uint32_t((e >> 29) & 0x7FFFFF)));
                }
              });
        }
      }
    }
    return results;
  };
  for (bool after_vmx : {false, true}) {
    const std::vector<uint32_t> results = run(after_vmx);
    REQUIRE(results.size() == 18u * 18u * 2u * 13u);
#if XE_ARCH_ARM64
    const bool saved = cvars::a64_stfs_skip_quiet_test;
    cvars::a64_stfs_skip_quiet_test = !saved;
    const std::vector<uint32_t> other = run(after_vmx);
    cvars::a64_stfs_skip_quiet_test = saved;
    size_t differing = 0;
    for (size_t n = 0; n < results.size(); ++n) {
      differing += results[n] != other[n];
    }
    REQUIRE(differing == 0);
#endif
  }
}
