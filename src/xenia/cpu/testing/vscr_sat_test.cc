/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// VSCR[SAT] after the saturating packs and after vctsxs and vaddsbs, as mfvscr
// reads it, and that it stays set.

#include "xenia/cpu/testing/util.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

using namespace xe::cpu;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

namespace {

constexpr uint32_t kCodeBase = 0x82000000;
constexpr uint32_t kStride = 0x10;

// v3 = op v1, v2; v4 = VSCR
struct SatOp {
  const char* name;
  uint32_t extended;
  int lane_bits;
  int64_t min, max;
  bool is_signed;
};
const SatOp kPacks[] = {
    {"vpkshss", 0x18E, 16, -128, 127, true},
    {"vpkshus", 0x10E, 16, 0, 255, true},
    {"vpkuhus", 0x08E, 16, 0, 255, false},
    {"vpkswss", 0x1CE, 32, -32768, 32767, true},
    {"vpkswus", 0x14E, 32, 0, 65535, true},
    {"vpkuwus", 0x0CE, 32, 0, 65535, false},
};

std::vector<uint32_t> Lanes(const SatOp& op) {
  std::vector<uint32_t> lanes;
  if (op.lane_bits == 16) {
    for (uint32_t value = 0; value < 0x10000; ++value) {
      lanes.push_back(value);
    }
    return lanes;
  }
  for (int bit = 0; bit < 32; ++bit) {
    for (int32_t delta = -2; delta <= 2; ++delta) {
      lanes.push_back((uint32_t(1) << bit) + delta);
      lanes.push_back(~(uint32_t(1) << bit) + delta);
      // The low half that a wrong compare of halves would take for a match
      lanes.push_back((uint32_t(1) << bit) * 3 + delta);
      lanes.push_back(((uint32_t(1) << bit) | 0x7FFF) + delta);
    }
  }
  return lanes;
}

}  // namespace

TEST_CASE("VSCR_SAT", "[instr]") {
  auto memory = std::make_unique<xe::Memory>();
  memory->Initialize();
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(CreateBackend());
  std::vector<uint32_t> extended;
  for (const SatOp& op : kPacks) {
    extended.push_back(op.extended);
  }
  extended.push_back(0x3CA);  // vctsxs v3, v2, 1
  extended.push_back(0x300);  // vaddsbs
  memory->LookupHeap(kCodeBase)->AllocFixed(
      kCodeBase, 0x1000, 0,
      xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
      xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
  for (size_t n = 0; n < extended.size(); ++n) {
    uint8_t* p = memory->TranslateVirtual(kCodeBase + uint32_t(n) * kStride);
    xe::store_and_swap<uint32_t>(
        p, 0x10000000 | (3 << 21) | (1 << 16) | (2 << 11) | extended[n]);
    xe::store_and_swap<uint32_t>(p + 4, 0x10000604 | (4 << 21));  // mfvscr v4
    xe::store_and_swap<uint32_t>(p + 8, 0x4E800020);              // blr
  }
  // Four packs in one block, each of its own input: vpkshss v3, vN, vN
  const uint32_t run_address = kCodeBase + 0x800;
  uint8_t* code = memory->TranslateVirtual(run_address);
  for (uint32_t vr = 5; vr < 9; ++vr, code += 4) {
    xe::store_and_swap<uint32_t>(
        code, 0x10000000 | (3 << 21) | (vr << 16) | (vr << 11) | 0x18E);
  }
  xe::store_and_swap<uint32_t>(code, 0x10000604 | (4 << 21));  // mfvscr v4
  xe::store_and_swap<uint32_t>(code + 4, 0x4E800020);          // blr
  auto module = std::make_unique<RawModule>(processor.get());
  module->set_name("sat");
  module->SetAddressRange(kCodeBase, 0x1000);
  module->set_executable(true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(kCodeBase, kCodeBase + 0x10000);
  const uint32_t stack_size = 64 * 1024;
  const uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(
      processor.get(), 0x100, stack_address + stack_size);
  PPCContext* ctx = thread_state->context();

  // Whether SAT reads as set by mfvscr and by the host
  const auto run = [&](size_t n, const xe::vec128_t& a, const xe::vec128_t& b,
                       bool sat) {
    auto fn = processor->ResolveFunction(kCodeBase + uint32_t(n) * kStride);
    REQUIRE(fn != nullptr);
    ctx->v[1] = a;
    ctx->v[2] = b;
    ctx->vscr_vec = xe::vec128i(0, 0, 0, 0x00010000);
    ctx->vscr_sat = sat;
    ctx->lr = 0xBCBCBCBC;
    fn->Call(thread_state.get(), uint32_t(ctx->lr));
    REQUIRE((ctx->v[4].u32[3] & ~1u) == 0x00010000);
    REQUIRE((ctx->v[4].u32[3] & 1) == ctx->vscr_sat);
    return (ctx->v[4].u32[3] & 1) != 0;
  };

  for (size_t n = 0; n < std::size(kPacks); ++n) {
    const SatOp& op = kPacks[n];
    size_t rotation = 0;
    for (uint32_t lane : Lanes(op)) {
      const int64_t value =
          !op.is_signed          ? int64_t(lane)
          : op.lane_bits == 16   ? int64_t(int16_t(lane))
                                 : int64_t(int32_t(lane));
      const bool saturates = value < op.min || value > op.max;
      xe::vec128_t in[2] = {};
      const size_t lanes = 128 / op.lane_bits;
      const size_t at = rotation++ % (2 * lanes);
      if (op.lane_bits == 16) {
        in[at / lanes].u16[at % lanes] = uint16_t(lane);
      } else {
        in[at / lanes].u32[at % lanes] = lane;
      }
      INFO(op.name << " lane 0x" << std::hex << lane << " at " << at);
      if (run(n, in[0], in[1], false) != saturates ||
          run(n, in[at / lanes], in[at / lanes], false) != saturates ||
          !run(n, in[0], in[1], true)) {
        REQUIRE(false);
      }
    }
  }
  const size_t vctsxs = std::size(kPacks);
  const struct {
    uint32_t bits;
    bool saturates;
  } floats[] = {{0, false},          {0x4E7FFFFF, false}, {0x4E800000, true},
                {0xCE800000, false}, {0xCE800001, true},  {0x7F800000, true},
                {0xFF800000, true},  {0x7FC00000, false}, {0xFFC00000, false}};
  for (const auto& f : floats) {
    for (int lane = 0; lane < 4; ++lane) {
      xe::vec128_t b = {};
      b.u32[lane] = f.bits;
      INFO("vctsxs 0x" << std::hex << f.bits);
      REQUIRE(run(vctsxs, {}, b, false) == f.saturates);
    }
  }
  for (uint32_t x = 0; x < 256; ++x) {
    for (uint32_t y = 0; y < 256; ++y) {
      xe::vec128_t a = {}, b = {};
      a.u8[(x + y) % 16] = uint8_t(x);
      b.u8[(x + y) % 16] = uint8_t(y);
      const int sum = int8_t(x) + int8_t(y);
      if (run(vctsxs + 1, a, b, false) != (sum < -128 || sum > 127)) {
        INFO("vaddsbs " << x << " " << y);
        REQUIRE(false);
      }
    }
  }
  // The converted values too, scaled and not: NaNs give 0
  const uint32_t convert_address = kCodeBase + 0xC00;
  code = memory->TranslateVirtual(convert_address);
  for (uint32_t k = 0; k < 4; ++k, code += 4) {  // vctsxs/vctuxs v(9+k), v2, n
    xe::store_and_swap<uint32_t>(
        code, 0x10000000 | ((9 + k) << 21) | ((k & 1 ? 5 : 0) << 16) |
                  (2 << 11) | (k & 2 ? 0x38A : 0x3CA));
  }
  xe::store_and_swap<uint32_t>(code, 0x4E800020);  // blr
  auto convert = processor->ResolveFunction(convert_address);
  REQUIRE(convert != nullptr);
  std::vector<uint32_t> inputs = {0x7F800001, 0xFF800001, 0x7FC00000,
                                  0xFFFFFFFF, 0x00000001, 0x80000001,
                                  0x007FFFFF, 0x7F7FFFFF, 0xFF7FFFFF};
  for (uint32_t exponent = 0; exponent < 256; ++exponent) {
    for (uint32_t mantissa : {0u, 1u, 0x400000u, 0x7FFFFFu}) {
      inputs.push_back((exponent << 23) | mantissa);
      inputs.push_back(0x80000000 | (exponent << 23) | mantissa);
    }
  }
  for (uint32_t bits : inputs) {
    ctx->v[2] = xe::vec128i(0x3F800000, bits, bits ^ 0x80000000, 0x7FC00000);
    ctx->lr = 0xBCBCBCBC;
    convert->Call(thread_state.get(), uint32_t(ctx->lr));
    for (int k = 0; k < 4; ++k) {
      for (int lane = 0; lane < 4; ++lane) {
        float value;
        std::memcpy(&value, &ctx->v[2].u32[lane], sizeof(value));
        const double scaled = double(value) * (k & 1 ? 32 : 1);
        const double low = k & 2 ? 0.0 : -2147483648.0;
        const double high = k & 2 ? 4294967295.0 : 2147483647.0;
        const int64_t expected =
            scaled != scaled
                ? 0
                : int64_t(scaled < low ? low : scaled > high ? high : scaled);
        INFO("input 0x" << std::hex << bits << " conversion " << k);
        REQUIRE(ctx->v[9 + k].u32[lane] == uint32_t(expected));
      }
    }
  }

  auto fn = processor->ResolveFunction(run_address);
  REQUIRE(fn != nullptr);
  for (uint32_t mask = 0; mask < 32; ++mask) {
    for (int vr = 0; vr < 4; ++vr) {
      ctx->v[5 + vr] = {};
      ctx->v[5 + vr].u16[vr] = mask & (1 << vr) ? 0x80 : 0x7F;
    }
    ctx->vscr_vec = xe::vec128i(0, 0, 0, 0x00010000);
    ctx->vscr_sat = (mask >> 4) & 1;
    ctx->lr = 0xBCBCBCBC;
    fn->Call(thread_state.get(), uint32_t(ctx->lr));
    INFO("mask " << mask);
    REQUIRE((ctx->v[4].u32[3] & 1) == (mask != 0));
    REQUIRE(ctx->vscr_sat == (mask != 0));
  }
}

// Two tests of one mask, or of constant masks, aren't merged: there's no new
// OR of the masks to place before the one into vscr_sat.
TEST_CASE("VSCR_SAT_SAME_MASK", "[instr]") {
  TestFunction test([](hir::HIRBuilder& b) {
    hir::Value* mask =
        b.VectorAdd(LoadVR(b, 1), LoadVR(b, 2), hir::INT32_TYPE);
    hir::Value* saturated =
        b.Or(b.LoadContext(offsetof(PPCContext, vscr_sat), hir::INT8_TYPE),
             b.DidSaturate(mask));
    StoreVR(b, 3, b.Not(mask));
    b.StoreContext(offsetof(PPCContext, vscr_sat),
                   b.Or(saturated, b.DidSaturate(mask)));
    b.Return();
  });
  for (uint32_t lane : {0u, 2u}) {
    test.Run(
        [&](PPCContext* ctx) {
          ctx->v[1] = xe::vec128i(lane);
          ctx->v[2] = xe::vec128i(lane / 2);
          ctx->vscr_sat = 0;
        },
        [&](PPCContext* ctx) {
          REQUIRE(ctx->v[3].u32[0] == ~(lane + lane / 2));
          REQUIRE(ctx->vscr_sat == (lane != 0));
        });
  }
}

TEST_CASE("VSCR_SAT_CONSTANT_MASK", "[instr]") {
  TestFunction test([](hir::HIRBuilder& b) {
    hir::Value* saturated =
        b.Or(b.LoadContext(offsetof(PPCContext, vscr_sat), hir::INT8_TYPE),
             b.DidSaturate(b.LoadZeroVec128()));
    b.StoreContext(offsetof(PPCContext, vscr_sat),
                   b.Or(saturated, b.DidSaturate(b.LoadZeroVec128())));
    b.Return();
  });
  for (uint8_t before : {0, 1}) {
    test.Run([&](PPCContext* ctx) { ctx->vscr_sat = before; },
             [&](PPCContext* ctx) { REQUIRE(ctx->vscr_sat == before); });
  }
}
