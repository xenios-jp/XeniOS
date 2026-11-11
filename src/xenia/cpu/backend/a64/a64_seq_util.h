/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_SEQ_UTIL_H_
#define XENIA_CPU_BACKEND_A64_A64_SEQ_UTIL_H_

#include <utility>

#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/vec128.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/cpu_flags.h"

#include "xbyak_aarch64.h"

DECLARE_bool(a64_stfs_skip_quiet_test);
DECLARE_bool(a64_lfs_quiet_unobserved);
DECLARE_bool(a64_vmx_nan_fixup_in_tail);
DECLARE_bool(a64_vmx_fma_minmax_nan_in_tail);

#if XE_COMPILER_MSVC
#include <intrin.h>
constexpr uint32_t DCZID_EL0 = ARM64_SYSREG(0b11, 0b011, 0b0000, 0b0000, 0b111);
#define xe_cpu_mrs(reg) _ReadStatusReg(reg)
#elif XE_COMPILER_CLANG || XE_COMPILER_GNUC
#include <arm_acle.h>
#define xe_cpu_mrs(reg) __arm_rsr64(#reg)
#else
#error "No MRS wrapper available for current compiler implemented."
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using Xbyak_aarch64::QReg;
using Xbyak_aarch64::SReg;
using Xbyak_aarch64::VReg;
using Xbyak_aarch64::WReg;
using Xbyak_aarch64::XReg;

template <typename Fn>
inline void EmitWithFpcrMode(A64Emitter& e, FPCRMode mode, Fn&& emit_op) {
  // Enter the requested FPCR mode using tracked lazy switching.  If the
  // emitter is already in that mode (e.g. consecutive VMX ops in the same
  // basic block) this is a no-op — no system register access at all.
  // FPU mode is re-established at transitions and by scalar FP sequences.
  e.ChangeFpcrMode(mode);
  emit_op();
}

template <typename Fn>
inline void EmitWithVmxFpcr(A64Emitter& e, Fn&& emit_op) {
  EmitWithFpcrMode(e, FPCRMode::Vmx, std::forward<Fn>(emit_op));
}

// The multiply-add family and the dot products flush denormals whatever NJM
// says. Pinning that on needs an FPCR of its own, which costs an msr wherever
// they interleave with the rest of VMX, so it is opt in. Without it they follow
// NJM like every other VMX op, which only differs once a title clears VSCR.NJ.
inline FPCRMode VmxDenormalFlushFpcrMode() {
  return cvars::accurate_vmx_denormal_flush ? FPCRMode::VmxDaz : FPCRMode::Vmx;
}

template <typename Fn>
inline void EmitWithVmxDenormalFlushFpcr(A64Emitter& e, Fn&& emit_op) {
  EmitWithFpcrMode(e, VmxDenormalFlushFpcrMode(), std::forward<Fn>(emit_op));
}

// True iff the 64-bit value is encodable as a MOVI Dn, #imm8 immediate.
// MOVI replicates the 8-bit immediate "a:b:c:d:e:f:g:h" into the bytes
// "aaaaaaaabbbbbbbbccccccccddddddddeeeeeeeeffffffffgggggggghhhhhhhh", so
// every byte of the input must be 0x00 or 0xFF.
inline bool IsMovi64Imm(uint64_t value) {
  if (value == 0 || value == ~uint64_t(0)) {
    return true;
  }
  for (int shift = 0; shift < 8; ++shift) {
    const uint8_t shift_u8 = static_cast<uint8_t>(value >> (shift * 8));
    if (shift_u8 != 0 && shift_u8 != 0xFF) {
      return false;
    }
  }
  return true;
}

// True iff `imm` is encodable as an AArch64 logical immediate (AND/ORR/EOR
// immediate forms) for the given register size (32 or 64). A logical
// immediate is a power-of-two-sized element (2..64 bits) containing a single
// circular run of ones, replicated to fill the register. All-zeros and
// all-ones are not encodable.
inline bool IsValidLogicalImm(uint64_t imm, unsigned reg_size) {
  if (reg_size == 32) {
    imm &= 0xFFFFFFFFull;
    imm |= imm << 32;
  }
  if (imm == 0 || imm == ~0ull) {
    return false;
  }
  // Find the smallest power-of-two period of the pattern.
  unsigned size = 64;
  do {
    const unsigned half = size / 2;
    const uint64_t mask = (1ull << half) - 1;
    if ((imm & mask) != ((imm >> half) & mask)) {
      break;
    }
    size = half;
  } while (size > 2);
  // The element must be a single circular run of ones: exactly two 0<->1
  // transitions when traversed circularly.
  const uint64_t elem_mask = (size == 64) ? ~0ull : ((1ull << size) - 1);
  const uint64_t elem = imm & elem_mask;
  const uint64_t rot1 = ((elem >> 1) | (elem << (size - 1))) & elem_mask;
  return xe::bit_count(elem ^ rot1) == 2;
}

// Try to see if the provided double value can be compressed into an 8-bit value
// for the fmov instruction. Returns false if the value cannot be represented
// abcdefgh
//    V
// aBbbbbbc defgh000 00000000 00000000
// B = NOT(b)
constexpr bool IsFmov32Imm(float f32) {
  const uint32_t u32 = std::bit_cast<uint32_t, float>(f32);
  const uint32_t sign = (u32 >> 31) & 1;
  int32_t exp = ((u32 >> 23) & 0xff) - 127;
  int64_t mantissa = u32 & 0x7fffff;

  // Too many mantissa bits
  if (mantissa & 0x7ffff) {
    return false;
  }
  // Too many exp bits
  if (exp < -3 || exp > 4) {
    return false;
  }

  // mantissa = (16 + e:f:g:h) / 16.
  mantissa >>= 19;
  if ((mantissa & 0b1111) != mantissa) {
    return false;
  }

  return true;
}

// Try to see if the provided double value can be compressed into an 8-bit value
// for the fmov instruction. Returns false if the value cannot be represented
// abcdefgh
//    V
// aBbbbbbb bbcdefgh 00000000 00000000 00000000 00000000 00000000 00000000
// B = NOT(b)
constexpr bool IsFmov64Imm(double f64) {
  const uint64_t u64 = std::bit_cast<uint64_t, double>(f64);
  int32_t exp = ((u64 >> 52) & 0x7ff) - 1023;
  int64_t mantissa = u64 & 0xfffffffffffffULL;

  // Too many mantissa bits
  if (mantissa & 0xffffffffffffULL) {
    return false;
  }
  // Too many exp bits
  if (exp < -3 || exp > 4) {
    return false;
  }

  // mantissa = (16 + e:f:g:h) / 16.
  mantissa >>= 48;
  if ((mantissa & 0b1111) != mantissa) {
    return false;
  }
  return true;
}

// Load a compile-time vec128_t constant into a NEON register.
// Values with no immediate form come from the literal pool, never a GPR.
inline void LoadV128Const(A64Emitter& e, int vreg_idx, const vec128_t& val) {
  // Fast common cases
  if (!val.low && !val.high) {
    // 0000...
    e.movi(VReg2D(vreg_idx), 0);
    return;
  } else if (val.low == ~uint64_t(0) && val.high == ~uint64_t(0)) {
    // 1111...
    e.movi(VReg2D(vreg_idx), ~0ULL);
    return;
  }

  // Element splats
  bool all_equal_u8 = true;
  const uint8_t splat_u8 = val.u8[0];
  for (unsigned i = 1; i < 16; ++i) {
    if (val.u8[i] != splat_u8) {
      all_equal_u8 = false;
      break;
    }
  }
  if (all_equal_u8) {
    e.movi(VReg(vreg_idx).b16, static_cast<uint8_t>(splat_u8));
    return;
  }

  bool all_equal_u16 = true;
  const uint16_t splat_u16 = val.u16[0];
  for (unsigned i = 1; i < 8; ++i) {
    if (val.u16[i] != splat_u16) {
      all_equal_u16 = false;
      break;
    }
  }
  if (all_equal_u16) {
    if ((splat_u16 & 0xFF'00) == 0) {
      e.movi(VReg(vreg_idx).h8, static_cast<uint8_t>(splat_u16 >> 0), LSL, 0);
    } else if ((splat_u16 & 0x00'FF) == 0) {
      e.movi(VReg(vreg_idx).h8, static_cast<uint8_t>(splat_u16 >> 8), LSL, 8);
    } else if ((splat_u16 & 0xFF'00) == 0xFF'00) {
      e.mvni(VReg(vreg_idx).h8, ~static_cast<uint8_t>(splat_u16 >> 0) & 0xFF,
             LSL, 0);
    } else if ((splat_u16 & 0x00'FF) == 0x00'FF) {
      e.mvni(VReg(vreg_idx).h8, ~static_cast<uint8_t>(splat_u16 >> 8) & 0xFF,
             LSL, 8);
    } else {
      e.ldr(QReg(vreg_idx), e.GetV128ConstLabel(val));
    }
    return;
  }

  bool all_equal_u32 = true;
  const uint32_t splat_u32 = val.u32[0];
  const float splat_f32 = val.f32[0];
  for (unsigned i = 1; i < 4; ++i) {
    if (val.u32[i] != splat_u32) {
      all_equal_u32 = false;
      break;
    }
  }
  if (all_equal_u32) {
    if ((splat_u32 & 0xFF'FF'FF'00) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 0), LSL, 0);
    } else if ((splat_u32 & 0xFF'FF'00'FF) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 8), LSL, 8);
    } else if ((splat_u32 & 0xFF'00'FF'FF) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 16), LSL, 16);
    } else if ((splat_u32 & 0x00'FF'FF'FF) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 24), LSL, 24);
    } else if ((splat_u32 & 0xFF'FF'FF'00) == 0xFF'FF'FF'00) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 0) & 0xFF,
             LSL, 0);
    } else if ((splat_u32 & 0xFF'FF'00'FF) == 0xFF'FF'00'FF) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 8) & 0xFF,
             LSL, 8);
    } else if ((splat_u32 & 0xFF'00'FF'FF) == 0xFF'00'FF'FF) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 16) & 0xFF,
             LSL, 16);
    } else if ((splat_u32 & 0x00'FF'FF'FF) == 0x00'FF'FF'FF) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 24) & 0xFF,
             LSL, 24);
    } else if (IsFmov32Imm(splat_f32)) {
      e.fmov(VReg(vreg_idx).s4, splat_f32);
    } else {
      e.ldr(QReg(vreg_idx), e.GetV128ConstLabel(val));
    }
    return;
  }

  const bool all_equal_u64 = val.low == val.high;
  const double splat_f64 = val.f64[0];
  if (all_equal_u64) {
    if (IsMovi64Imm(val.low)) {
      e.movi(VReg2D(vreg_idx), val.low);
    } else if (IsFmov64Imm(splat_f64)) {
      e.fmov(VReg(vreg_idx).d2, splat_f64);
    } else {
      e.ldr(QReg(vreg_idx), e.GetV128ConstLabel(val));
    }
    return;
  }

  e.ldr(QReg(vreg_idx), e.GetV128ConstLabel(val));
}

// Resolve a V128 operand to a register index, loading constants into
// scratch_idx if needed.
template <typename T>
inline int SrcVReg(A64Emitter& e, const T& op, int scratch_idx) {
  if (op.is_constant) {
    LoadV128Const(e, scratch_idx, op.constant());
    return scratch_idx;
  }
  return op.reg().getIdx();
}

inline bool NeedsPhysicalRemap() {
  return xe::memory::allocation_granularity() > 0x1000;
}

// w0 = src, plus 0x1000 when src >= 0xE0000000. The bound is not an
// immediate operand, so it lives in w7 (A64Emitter::ReloadPhysicalRemapBound).
inline void ApplyPhysicalRemapW0(A64Emitter& e,
                                 const Xbyak_aarch64::WReg& src) {
  using namespace Xbyak_aarch64;
  e.cmp(src, e.w7);
  e.add(e.w17, src, 1, 12);  // + 0x1000 via LSL #12
  e.csel(e.w0, e.w17, src, HS);
}

// Compute a guest memory address, returning the XReg for [x21, xN] addressing.
// For constants, loads the address into x0 (scratch).
inline XReg ComputeMemoryAddress(A64Emitter& e, const I64Op& guest) {
  using namespace Xbyak_aarch64;
  if (guest.is_constant) {
    uint32_t address = static_cast<uint32_t>(guest.constant());
    if (address >= 0xE0000000 && NeedsPhysicalRemap()) {
      address += 0x1000;
    }
    e.mov(e.x0, static_cast<uint64_t>(address));
    return e.x0;
  } else {
    auto src = guest.reg();
    // Guest addresses are always 32-bit. Clear any stale upper bits before
    // applying the host membase so guest pointers can't escape above 4 GB.
    // The remap writes w0 itself, which is the same truncation.
    int mask_src_reg;
    uint32_t mask;
    if (e.ConsumeFusedAddressMask(src.getIdx(), &mask_src_reg, &mask)) {
      // The producing AND emitted nothing; its mask and this truncation are
      // the same W-form instruction.
      e.and_(e.w0, WReg(mask_src_reg), static_cast<uint64_t>(mask));
      ApplyPhysicalRemapW0(e, e.w0);
    } else if (NeedsPhysicalRemap()) {
      ApplyPhysicalRemapW0(e, WReg(src.getIdx()));
    } else {
      e.mov(e.w0, WReg(src.getIdx()));
    }
    return e.x0;
  }
}

inline bool GuestMemDirectIndex(const I64Op& guest, int* out_w_idx) {
  if (guest.is_constant || NeedsPhysicalRemap()) {
    return false;
  }
  *out_w_idx = guest.reg().getIdx();
  return true;
}

// The 32-bit guest address: the guest register itself, or w0. A fused mask is
// applied here and returned in |out_mask|, else all ones.
inline WReg GuestAddressW(A64Emitter& e, const I64Op& guest,
                          uint32_t* out_mask) {
  *out_mask = UINT32_MAX;
  if (guest.is_constant) {
    e.mov(e.w0, static_cast<uint64_t>(static_cast<uint32_t>(guest.constant())));
    return e.w0;
  }
  int mask_src_reg;
  if (e.ConsumeFusedAddressMask(guest.reg().getIdx(), &mask_src_reg,
                                out_mask)) {
    e.and_(e.w0, WReg(mask_src_reg), static_cast<uint64_t>(*out_mask));
    return e.w0;
  }
  return WReg(guest.reg().getIdx());
}

// Loads the table entry for |address|'s 4 KB page into x16, so x16 plus
// |address| zero-extended is the host address. An access of |size| bytes that
// reaches the next page uses the membase instead, from the tail, and a |size|
// of 1 never does. Clobbers w17 and NZCV.
inline void EmitGuestTlbLookup(A64Emitter& e, const WReg& address,
                               bool is_write, uint32_t size) {
  using namespace Xbyak_aarch64;
  static_assert(offsetof(ppc::PPCContext, tlb_write) < 32768);
  const size_t table_offset = is_write ? offsetof(ppc::PPCContext, tlb_write)
                                       : offsetof(ppc::PPCContext, tlb_read);
  e.ldr(e.x16, ptr(e.GetContextReg(), static_cast<uint32_t>(table_offset)));
  e.lsr(e.w17, address, 12);
  e.ldr(e.x16, ptr(e.x16, e.x17, LSL, 3));
  if (size <= 1) {
    return;
  }
  auto& join = e.NewCachedLabel();
  const int address_idx = address.getIdx();
  auto& cross =
      e.AddToTail([address_idx, &join](A64Emitter& e, Xbyak_aarch64::Label&) {
        // x16 = membase + remapped address - address, so the remap wraps.
        const WReg address(address_idx);
        e.mov(e.w17, uint64_t(0xE0000000u));
        e.cmp(address, e.w17);
        e.add(e.w17, address, 1, 12);
        e.csel(e.w17, e.w17, address, HS);
        e.add(e.x16, e.GetMembaseReg(), e.x17);
        e.sub(e.x16, e.x16, address, UXTW);
        e.b(join);
      });
  // Carries out when the page offset plus size - 1 passes 0xFFF.
  e.orr(e.w17, address, uint64_t(0xFFFFF000u));
  e.cmn(e.w17, size - 1);
  e.b(HS, cross);
  e.L(join);
}

// The access size that can reach the next page, 1 when none can. A constant
// address is checked here, a mask that clears the low bits proves alignment.
inline uint32_t GuestTlbCrossSize(const I64Op& guest, uint32_t mask,
                                  uint32_t size) {
  if (guest.is_constant) {
    const uint32_t offset = static_cast<uint32_t>(guest.constant()) & 0xFFF;
    return offset + size <= 0x1000 ? 1 : size;
  }
  return (mask & (size - 1)) ? size : 1;
}

// emit_access gets the address of a |size| byte access, which can use x0 and on
// the TLB path has x16 as its base. emit_access must not write either first.
template <typename Fn>
inline void EmitGuestMemAccess(A64Emitter& e, const I64Op& guest, uint32_t size,
                               bool is_write, Fn&& emit_access) {
  if (e.guest_tlb()) {
    uint32_t mask;
    const WReg address = GuestAddressW(e, guest, &mask);
    EmitGuestTlbLookup(e, address, is_write,
                       GuestTlbCrossSize(guest, mask, size));
    emit_access(ptr(e.x16, address, Xbyak_aarch64::UXTW));
    return;
  }
  int w_idx;
  if (GuestMemDirectIndex(guest, &w_idx)) {
    emit_access(ptr(e.GetMembaseReg(), WReg(w_idx), Xbyak_aarch64::UXTW));
  } else {
    emit_access(ptr(e.GetMembaseReg(), ComputeMemoryAddress(e, guest)));
  }
}

// dst = the host address of a |size| byte access at |guest|.
inline void ComputeGuestHostAddress(A64Emitter& e, const XReg& dst,
                                    const I64Op& guest, uint32_t size,
                                    bool is_write) {
  if (e.guest_tlb()) {
    uint32_t mask;
    const WReg address = GuestAddressW(e, guest, &mask);
    EmitGuestTlbLookup(e, address, is_write,
                       GuestTlbCrossSize(guest, mask, size));
    e.add(dst, e.x16, address, Xbyak_aarch64::UXTW);
    return;
  }
  e.add(dst, e.GetMembaseReg(), ComputeMemoryAddress(e, guest));
}

// Adds a displacement to a guest address in W registers, so it wraps at 32
// bits. Returns base unchanged for a zero displacement, else x0.
template <typename OffsetOp>
inline XReg AddGuestMemoryOffset(A64Emitter& e, const XReg& base,
                                 const OffsetOp& offset) {
  const WReg src = WReg(base.getIdx());
  if (offset.is_constant) {
    const uint32_t imm = static_cast<uint32_t>(offset.constant());
    const uint32_t neg = 0u - imm;
    if (imm == 0) {
      return base;
    } else if (imm <= 0xFFF) {
      e.add(e.w0, src, imm);
    } else if (!(imm & 0xFFF) && (imm >> 12) <= 0xFFF) {
      e.add(e.w0, src, imm >> 12, 12);
    } else if (neg <= 0xFFF) {
      // Adding a small negative offset wraps identically to subtracting.
      e.sub(e.w0, src, neg);
    } else if (!(neg & 0xFFF) && (neg >> 12) <= 0xFFF) {
      e.sub(e.w0, src, neg >> 12, 12);
    } else {
      e.mov(e.w17, static_cast<uint64_t>(imm));
      e.add(e.w0, src, e.w17);
    }
  } else {
    e.add(e.w0, src, WReg(offset.reg().getIdx()));
  }
  return e.x0;
}

// The remap is decided on the effective address, not the base, as on x64.
template <typename OffsetOp>
inline XReg ComputeMemoryAddressOffset(A64Emitter& e, const I64Op& guest,
                                       const OffsetOp& offset) {
  using namespace Xbyak_aarch64;
  if (guest.is_constant && offset.is_constant) {
    uint32_t address = static_cast<uint32_t>(guest.constant()) +
                       static_cast<uint32_t>(offset.constant());
    if (address >= 0xE0000000 && NeedsPhysicalRemap()) {
      address += 0x1000;
    }
    e.mov(e.x0, static_cast<uint64_t>(address));
    return e.x0;
  }
  XReg address = e.x0;
  if (guest.is_constant) {
    e.mov(e.w0, static_cast<uint64_t>(static_cast<uint32_t>(guest.constant())));
    address = AddGuestMemoryOffset(e, e.x0, offset);
  } else {
    address = AddGuestMemoryOffset(e, guest.reg(), offset);
  }
  if (NeedsPhysicalRemap()) {
    ApplyPhysicalRemapW0(e, WReg(address.getIdx()));
  } else if (address.getIdx() != e.x0.getIdx()) {
    e.mov(e.w0, WReg(address.getIdx()));
  }
  return e.x0;
}

// Guest addresses wrap at 32 bits: a nonzero displacement is added in W first.
template <typename OffsetOp, typename Fn>
inline void EmitGuestMemAccessOffset(A64Emitter& e, const I64Op& guest,
                                     const OffsetOp& offset, uint32_t size,
                                     bool is_write, Fn&& emit_access) {
  if (e.guest_tlb()) {
    WReg address = e.w0;
    uint32_t cross_size = size;
    if (guest.is_constant && offset.is_constant) {
      const uint32_t constant = static_cast<uint32_t>(guest.constant()) +
                                static_cast<uint32_t>(offset.constant());
      e.mov(e.w0, static_cast<uint64_t>(constant));
      if ((constant & 0xFFF) + size <= 0x1000) {
        cross_size = 1;
      }
    } else if (guest.is_constant) {
      e.mov(e.w0,
            static_cast<uint64_t>(static_cast<uint32_t>(guest.constant())));
      AddGuestMemoryOffset(e, e.x0, offset);
    } else {
      address = WReg(AddGuestMemoryOffset(e, guest.reg(), offset).getIdx());
    }
    EmitGuestTlbLookup(e, address, is_write, cross_size);
    emit_access(ptr(e.x16, address, Xbyak_aarch64::UXTW));
    return;
  }
  int w_idx;
  if (offset.is_constant && offset.constant() == 0 &&
      GuestMemDirectIndex(guest, &w_idx)) {
    emit_access(ptr(e.GetMembaseReg(), WReg(w_idx), Xbyak_aarch64::UXTW));
  } else {
    emit_access(
        ptr(e.GetMembaseReg(), ComputeMemoryAddressOffset(e, guest, offset)));
  }
}

// Flush denormal float32 lanes to zero in a NEON register (in-place).
// A float32 is denormal when 0 < abs(val) < 0x00800000.
// vreg must not equal sa or sb.
// This is needed because FPCR.FZ may not flush denormal inputs on all ARM64
// implementations (the ARM spec says input flushing is implementation-defined).
inline void LoadDenormalThreshold_V128(A64Emitter& e, int k) {
  e.movi(VReg(k).s4, 0x1, LSL, 24);
}

// Flushes with the threshold already in k. vreg must equal neither k nor t.
inline void FlushDenormalsWithK_V128(A64Emitter& e, int vreg, int k, int t) {
  e.shl(VReg(t).s4, VReg(vreg).s4, 1);
  e.cmhi(VReg(t).s4, VReg(k).s4,
         VReg(t).s4);                 // mask: all-1s for denormal-or-zero lanes
  e.ushr(VReg(t).s4, VReg(t).s4, 1);  // clear bit 31: preserve the sign
  e.bic(VReg(vreg).b16, VReg(vreg).b16, VReg(t).b16);
}

// Hardware flushes vector denormals only with VSCR.NJ set, so the software
// flushes are skipped at run time when it is clear. w17 is free in every
// vector FP sequence.
inline void EmitSkipFlushUnlessNJM(A64Emitter& e, Xbyak_aarch64::Label& skip) {
  e.ldr(e.w17, Xbyak_aarch64::ptr(e.x19, static_cast<uint32_t>(offsetof(
                                             A64BackendContext, flags))));
  e.tbz(e.w17, kA64BackendNJMOn, skip);
}

// vreg must not equal sa or sb.
inline void FlushDenormals_V128(A64Emitter& e, int vreg, int sa = 2,
                                int sb = 3) {
  Xbyak_aarch64::Label no_flush;
  EmitSkipFlushUnlessNJM(e, no_flush);
  LoadDenormalThreshold_V128(e, sa);
  FlushDenormalsWithK_V128(e, vreg, sa, sb);
  e.L(no_flush);
}

// Fixup for vmaxfp/vminfp NaN lanes.
// ARM64 fmax/fmin do propagate a NaN, but by ARM's rules: an SNaN in either
// operand outranks a QNaN in the other. PPC is strictly positional, so a NaN
// lane takes src1 where src1 is NaN and src2 otherwise.
// Expects: v0=flushed src1, v1=flushed src2, v2=hardware fmax/fmin result.
// Modifies v2 in place. Clobbers v1, v3.
inline void FixupVmxMaxMinNan(A64Emitter& e) {
  // The NaN to return: src1 where src1 is NaN, else src2.
  e.fcmeq(VReg(3).s4, VReg(0).s4, VReg(0).s4);  // v3 = non-NaN mask for src1
  e.bsl(VReg(3).b16, VReg(1).b16, VReg(0).b16);
  // Quieted, which is what hardware returns.
  e.movi(VReg(1).s4, 0x40, LSL, 16);  // v1 = quiet bit
  e.orr(VReg(3).b16, VReg(3).b16, VReg(1).b16);
  // fmax/fmin only produce a NaN out of a NaN operand, so the arithmetic
  // result's own NaN lanes are exactly the ones to replace.
  e.mov(VReg(1).b16, VReg(2).b16);
  e.fcmeq(VReg(2).s4, VReg(1).s4, VReg(1).s4);
  e.bsl(VReg(2).b16, VReg(1).b16, VReg(3).b16);
}

// Prepare two V128 operands for a VMX FP operation: copy to scratch v0/v1
// and flush denormals. Returns the flushed register indices (always 0 and 1).
template <typename T1, typename T2>
inline void PrepareVmxFpSources(A64Emitter& e, const T1& op1, const T2& op2,
                                int& out_s1, int& out_s2) {
  int s1 = SrcVReg(e, op1, 0);
  int s2 = SrcVReg(e, op2, 1);
  // Copy to scratch v0/v1 so we don't modify live allocated registers.
  if (s1 != 0) {
    e.mov(VReg(0).b16, VReg(s1).b16);
  }
  if (s2 != 1) {
    e.mov(VReg(1).b16, VReg(s2).b16);
  }
  // Flush denormal inputs in software only if FPCR.FZ doesn't handle it.
  if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
    Xbyak_aarch64::Label no_flush;
    EmitSkipFlushUnlessNJM(e, no_flush);
    LoadDenormalThreshold_V128(e, 2);
    FlushDenormalsWithK_V128(e, 0, 2, 3);
    FlushDenormalsWithK_V128(e, 1, 2, 3);
    e.L(no_flush);
  }
  out_s1 = 0;
  out_s2 = 1;
}

// Fix PPC NaN propagation for V128 float32 lanes after a NEON FP operation.
// Hardware returns the first NaN by operand position, quieted. An invalid
// operation with no NaN operand needs nothing: ARM's default NaN is already
// the one PPC produces.
// Expects s1 and s2 holding the (flushed) sources and `result` the hardware
// FP result, which is modified in place. Clobbers v3 and `quiet`, which may
// be a source that is dead after this but not `result`.
inline void FixupVmxNan_V128(A64Emitter& e, int s1 = 0, int s2 = 1,
                             int result = 2, int quiet = 1) {
  using namespace Xbyak_aarch64;
  // Lowest priority first, so src1 overwrites src2. BIF inserts an operand
  // wherever its self-compare is false, which is exactly where it is NaN.
  e.fcmeq(VReg(3).s4, VReg(s2).s4, VReg(s2).s4);
  e.bif(VReg(result).b16, VReg(s2).b16, VReg(3).b16);
  e.fcmeq(VReg(3).s4, VReg(s1).s4, VReg(s1).s4);
  e.bif(VReg(result).b16, VReg(s1).b16, VReg(3).b16);

  // Quiet whatever NaN each lane ended up with. A lane still holding the
  // arithmetic result is either not NaN or already the default NaN, so this
  // only ever quiets an operand that was signalling.
  e.fcmeq(VReg(3).s4, VReg(result).s4, VReg(result).s4);
  e.movi(VReg(quiet).s4, 0x40, LSL, 16);
  e.bic(VReg(quiet).b16, VReg(quiet).b16, VReg(3).b16);
  e.orr(VReg(result).b16, VReg(result).b16, VReg(quiet).b16);
}

// Loads an FMA's three sources for FixupVmxNan_V128_Fma and writes the
// registers now holding A, C and B. Where FPCR.FZ flushes inputs the
// allocated registers are used directly; otherwise they are copied to v0, v1
// and v3 and flushed in software, using v2 and `tmp` as scratch.
template <typename T1, typename T2, typename T3>
inline void PrepareVmxFmaSources(A64Emitter& e, const T1& op1, const T2& op2,
                                 const T3& op3, int tmp, int* out_a, int* out_c,
                                 int* out_b) {
  const int s1 = SrcVReg(e, op1, 0);
  const int s2 = SrcVReg(e, op2, 1);
  const int s3 = SrcVReg(e, op3, 3);
  if (e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
    *out_a = s1;
    *out_c = s2;
    *out_b = s3;
    return;
  }
  if (s1 != 0) {
    e.mov(VReg(0).b16, VReg(s1).b16);
  }
  if (s2 != 1) {
    e.mov(VReg(1).b16, VReg(s2).b16);
  }
  if (s3 != 3) {
    e.mov(VReg(3).b16, VReg(s3).b16);
  }
  // accurate_vmx_denormal_flush pins the input flush on; the output flush
  // stays NJ-gated, which is harmless since VmxDaz flushes outputs.
  Xbyak_aarch64::Label no_flush;
  const bool gate_on_njm = !cvars::accurate_vmx_denormal_flush;
  if (gate_on_njm) {
    EmitSkipFlushUnlessNJM(e, no_flush);
  }
  LoadDenormalThreshold_V128(e, 2);
  FlushDenormalsWithK_V128(e, 0, 2, tmp);
  FlushDenormalsWithK_V128(e, 1, 2, tmp);
  FlushDenormalsWithK_V128(e, 3, 2, tmp);
  if (gate_on_njm) {
    e.L(no_flush);
  }
  *out_a = 0;
  *out_c = 1;
  *out_b = 3;
}

// Fix PPC NaN propagation for a V128 FMA result (3 source operands).
// Hardware returns the first NaN in A, B, C order, quieted, and the HIR
// operands are (A, C, B). An invalid operation with no NaN operand needs
// nothing: ARM's default NaN is already the one PPC produces.
// Expects v2 = the FMA result and a, c, b holding the (flushed) sources.
// A test of the result skips the fixup unless some lane is NaN. tmp must
// differ from a, c, b, v2 and every live allocated register; qs
// is written only after the sources die. Modifies v2 in place.
inline void FixupVmxNanLanes_V128_Fma(A64Emitter& e, int a, int c, int b,
                                      int tmp, int qs);
inline void FixupVmxNan_V128_Fma(A64Emitter& e, int a, int c, int b, int tmp,
                                 int qs) {
  using namespace Xbyak_aarch64;
  auto& done = e.NewCachedLabel();
  if (cvars::a64_vmx_fma_minmax_nan_in_tail) {
    // FMINV returns a NaN if any lane is one, which one compare of it with
    // itself tells, and the fixup, which changes nothing unless some lane is
    // a NaN, sits in the tail.
    auto& nan_lane = e.AddToTail(
        [a, c, b, tmp, qs, &done](A64Emitter& e, Xbyak_aarch64::Label&) {
          FixupVmxNanLanes_V128_Fma(e, a, c, b, tmp, qs);
          e.b(done);
        });
    e.fminv(SReg(tmp), VReg(2).s4);
    e.fcmp(SReg(tmp), SReg(tmp));
    e.b(VS, nan_lane);
    e.L(done);
    return;
  }
  e.fcmeq(VReg(tmp).s4, VReg(2).s4, VReg(2).s4);  // all-ones where not NaN
  e.uminv(SReg(tmp), VReg(tmp).s4);               // 0 iff any lane is NaN
  e.fmov(WReg(0), SReg(tmp));
  e.cbnz_near(WReg(0), done);
  FixupVmxNanLanes_V128_Fma(e, a, c, b, tmp, qs);
  e.L(done);
}

// The fixup itself, for a result with a NaN lane.
inline void FixupVmxNanLanes_V128_Fma(A64Emitter& e, int a, int c, int b,
                                      int tmp, int qs) {
  using namespace Xbyak_aarch64;
  // Lowest priority first (C, B, A) so an earlier operand overwrites. BIF
  // inserts an operand wherever its self-compare is false, which is exactly
  // where it is NaN.
  e.fcmeq(VReg(tmp).s4, VReg(c).s4, VReg(c).s4);
  e.bif(VReg(2).b16, VReg(c).b16, VReg(tmp).b16);
  e.fcmeq(VReg(tmp).s4, VReg(b).s4, VReg(b).s4);
  e.bif(VReg(2).b16, VReg(b).b16, VReg(tmp).b16);
  e.fcmeq(VReg(tmp).s4, VReg(a).s4, VReg(a).s4);
  e.bif(VReg(2).b16, VReg(a).b16, VReg(tmp).b16);

  // Quiet whatever NaN each lane ended up with. A lane still holding the
  // arithmetic result is either not NaN or already the default NaN, so this
  // only ever quiets an operand that was signalling.
  e.fcmeq(VReg(tmp).s4, VReg(2).s4, VReg(2).s4);
  e.movi(VReg(qs).s4, 0x40, LSL, 16);
  e.bic(VReg(qs).b16, VReg(qs).b16, VReg(tmp).b16);
  e.orr(VReg(2).b16, VReg(2).b16, VReg(qs).b16);
}

// VMX float32x4 binary operations with full PPC semantics.
enum class VmxFpBinOp { Add, Sub, Mul, Div };

// Execute a VMX float32x4 binary operation with denormal flushing and PPC NaN
// propagation.  Result goes into dest_idx.
// Clobbers v0-v3, w0.
template <typename T1, typename T2>
inline void EmitVmxFpBinOp_V128(A64Emitter& e, int dest_idx, const T1& src1,
                                const T2& src2, VmxFpBinOp op) {
  EmitWithVmxFpcr(e, [&] {
    const auto emit_op = [&e, op](int result, int s1, int s2) {
      switch (op) {
        case VmxFpBinOp::Add:
          e.fadd(VReg(result).s4, VReg(s1).s4, VReg(s2).s4);
          break;
        case VmxFpBinOp::Sub:
          e.fsub(VReg(result).s4, VReg(s1).s4, VReg(s2).s4);
          break;
        case VmxFpBinOp::Mul:
          e.fmul(VReg(result).s4, VReg(s1).s4, VReg(s2).s4);
          break;
        case VmxFpBinOp::Div:
          e.fdiv(VReg(result).s4, VReg(s1).s4, VReg(s2).s4);
          break;
      }
    };

    if (cvars::a64_vmx_nan_fixup_in_tail &&
        e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
      // FPCR.FZ flushes the inputs and the result, so the operands are used
      // where they are. A NaN operand always makes its lane of the result a
      // NaN, and the fixup changes nothing in a lane that is not one, so a
      // result with no NaN lane is final. FMINV returns a NaN if any lane is
      // one, which one compare of it with itself tells.
      const int s1 = SrcVReg(e, src1, 0);
      const int s2 = SrcVReg(e, src2, 1);
      const int result = dest_idx != s1 && dest_idx != s2 ? dest_idx : 2;
      emit_op(result, s1, s2);
      auto& done = e.NewCachedLabel();
      auto& nan_lane = e.AddToTail(
          [s1, s2, result, &done](A64Emitter& e, Xbyak_aarch64::Label&) {
            // The sources are dead once they are merged in, so v0 is free
            // even where it holds a constant operand.
            FixupVmxNan_V128(e, s1, s2, result, 0);
            e.b(done);
          });
      e.fminv(SReg(3), VReg(result).s4);
      e.fcmp(SReg(3), SReg(3));
      e.b(Xbyak_aarch64::VS, nan_lane);
      e.L(done);
      if (result != dest_idx) {
        e.mov(VReg(dest_idx).b16, VReg(result).b16);
      }
      return;
    }

    // Flush input denormals → v0=s1, v1=s2.
    int s1, s2;
    PrepareVmxFpSources(e, src1, src2, s1, s2);

    // Hardware FP op → v2.
    emit_op(2, s1, s2);

    // PPC NaN propagation fixup.
    FixupVmxNan_V128(e);

    // Flush output denormals. FPCR.FZ guarantees output flushing per the
    // ARM spec, so skip when FZ is known to also handle inputs (implying
    // the core fully supports FZ denormal handling).
    if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
      FlushDenormals_V128(e, 2, 0, 1);
    }

    // Move to dest.
    e.mov(VReg(dest_idx).b16, VReg(2).b16);
  });
}

// VMX float32x4 maximum or minimum with denormal flushing and PPC NaN
// propagation, inside EmitWithVmxFpcr. Result goes into dest_idx.
// Clobbers v0-v3, w17.
template <typename T1, typename T2>
inline void EmitVmxFpMaxMin_V128(A64Emitter& e, int dest_idx, const T1& src1,
                                 const T2& src2, bool is_min) {
  const auto emit_op = [&e, is_min](int result, int s1, int s2) {
    if (is_min) {
      e.fmin(VReg(result).s4, VReg(s1).s4, VReg(s2).s4);
    } else {
      e.fmax(VReg(result).s4, VReg(s1).s4, VReg(s2).s4);
    }
  };
  if (cvars::a64_vmx_fma_minmax_nan_in_tail &&
      e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
    // FPCR.FZ flushes the inputs and the result, so the operands are used
    // where they are. fmax and fmin make a NaN only out of a NaN operand, and
    // the fixup replaces only the result's NaN lanes, so a result with no NaN
    // lane is final.
    const int s1 = SrcVReg(e, src1, 0);
    const int s2 = SrcVReg(e, src2, 1);
    const int result = dest_idx != s1 && dest_idx != s2 ? dest_idx : 2;
    emit_op(result, s1, s2);
    auto& done = e.NewCachedLabel();
    auto& nan_lane = e.AddToTail(
        [s1, s2, result, &done](A64Emitter& e, Xbyak_aarch64::Label&) {
          // The fixup wants src1, src2 and the result in v0, v1 and v2. A
          // source is in its allocated register or already in its scratch.
          if (s1 != 0) {
            e.mov(VReg(0).b16, VReg(s1).b16);
          }
          if (s2 != 1) {
            e.mov(VReg(1).b16, VReg(s2).b16);
          }
          if (result != 2) {
            e.mov(VReg(2).b16, VReg(result).b16);
          }
          FixupVmxMaxMinNan(e);
          if (result != 2) {
            e.mov(VReg(result).b16, VReg(2).b16);
          }
          e.b(done);
        });
    e.fminv(SReg(3), VReg(result).s4);
    e.fcmp(SReg(3), SReg(3));
    e.b(Xbyak_aarch64::VS, nan_lane);
    e.L(done);
    if (result != dest_idx) {
      e.mov(VReg(dest_idx).b16, VReg(result).b16);
    }
    return;
  }
  int s1, s2;
  PrepareVmxFpSources(e, src1, src2, s1, s2);
  emit_op(2, s1, s2);
  // PPC vmaxfp and vminfp: a NaN input is returned quieted, vA before vB.
  FixupVmxMaxMinNan(e);
  if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
    FlushDenormals_V128(e, 2, 0, 1);
  }
  e.mov(VReg(dest_idx).b16, VReg(2).b16);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_SEQ_UTIL_H_
