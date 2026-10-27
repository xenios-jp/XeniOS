/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_MEMEXPORT_FORMAT_PROFILE_H_
#define XENIA_GPU_MEMEXPORT_FORMAT_PROFILE_H_

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace memexport_format_profile {

// Guest memory export writes its values to a stream whose format is described
// by a constant register (xe_gpu_memexport_stream_t), and the export address is
// built as `mad eA, index, scale, stream`, so the lane of eA that receives the
// format - component 2, holding the descriptor's dword_2 - is
//
//   eA.z = SM3Mul(index.z, scale.z) + stream.z
//
// The translated shader has to decode that format at runtime and branch into
// the generic packing machinery for whichever format it finds, at every export
// site. When scale.z is known on the CPU to be +0, however, eA.z is stream.z
// for every invocation of the draw, so the format is a per-draw constant and
// each site's packing can be specialized.
//
// The fold is per instruction and pointwise: it replaces the value that the
// matched `mad` computes with the constant that the guest computation provably
// produces, so instructions writing eA elsewhere in the shader are unaffected
// and need not be recognized.
//
// +0 rather than a general "x * 0 is zero" assumption is what makes this sound:
// the kMad translation emits the Shader Model 3 zero-multiply select
// (spirv_shader_translator_alu.cc), so a NaN or infinite index still multiplies
// to +0, and adding +0 to a positive normal reproduces its bits exactly. The
// guest shader itself validates `eA.z >> 20 == 0x4B0`, so the format word is
// always a positive normal.

// A pool slot denotes an immutable list of (instruction address, stream
// constant, format word) entries. The command processor owns the pool and
// retains each slot until all of its shader translations have been released.
// The translation's vertex modification carries it
// (SpirvShaderTranslator::Modification memexport_format_slot, 7 bits). Bounded
// so a title cannot grow the pool without limit.
inline constexpr uint32_t kMaxProfiles = 64;
inline constexpr uint32_t kMaxSites = 32;

// The immutable table of profiles selected during this session. Slots are
// allocated in draw order, so a specialization must NOT be persisted: a stored
// pipeline is replayed on the next launch by handing its recorded modification
// straight to GetOrCreateHostTranslation, with no guard that would reject a
// slot whose meaning has changed. Callers must therefore avoid recording
// pipelines with a specialized vertex modification.
class ProfilePool {
 public:
  // Accessed on the command thread. Clear only after releasing translations.
  void Clear() {
    entries_ = {};
    used_ = 0;
  }

  // Returns a slot in [1, kMaxProfiles] denoting exactly these facts, or 0 if
  // the pool is full. Identical specializations share a slot.
  uint32_t GetOrCreate(
      const Shader::Specialization& specialization,
      std::shared_ptr<const Shader::Specialization>& specialization_out) {
    if (specialization.memexport_formats.empty() ||
        specialization.memexport_formats.size() > kMaxSites) {
      return 0;
    }
    for (uint32_t slot = 0; slot < used_; ++slot) {
      const auto& entry = entries_[slot];
      if (*entry == specialization) {
        specialization_out = entry;
        return slot + 1;
      }
    }
    if (used_ >= kMaxProfiles) {
      return 0;
    }
    auto& entry = entries_[used_];
    entry = std::make_shared<Shader::Specialization>(specialization);
    specialization_out = entry;
    return ++used_;
  }

 private:
  std::array<std::shared_ptr<const Shader::Specialization>, kMaxProfiles>
      entries_ = {};
  uint32_t used_ = 0;
};

// Facts are attached to exact ALU instruction addresses, so an unrelated site
// using the same stream constant cannot consume them. Returns the pool slot of
// the provable streams, or 0 if none is provable or the pool is full.
inline uint32_t SelectStreams(
    ProfilePool& pool, const std::vector<Shader::MemExportStream>& streams,
    const RegisterFile& regs, uint32_t constant_base,
    std::shared_ptr<const Shader::Specialization>& specialization_out) {
  specialization_out.reset();
  Shader::Specialization selected;
  for (const Shader::MemExportStream& stream : streams) {
    if (stream.scale == Shader::MemExportStream::Scale::kUnknown ||
        constant_base + stream.stream_constant >= 512) {
      continue;
    }
    if (stream.scale == Shader::MemExportStream::Scale::kConstantRegister) {
      auto is_zero = [&](uint32_t index, uint8_t component) {
        return constant_base + index < 512 && component < 4 &&
               regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X +
                           (constant_base + index) * 4 + component] == 0;
      };
      if (!is_zero(stream.scale_constant, stream.scale_component) &&
          !(stream.has_alternate_scale &&
            is_zero(stream.alternate_scale_constant,
                    stream.alternate_scale_component))) {
        continue;
      }
    }
    auto descriptor =
        regs.GetMemExportStream(constant_base + stream.stream_constant);
    if (descriptor.const_0x1 != 0x1 || descriptor.const_0x4b0 != 0x4B0 ||
        descriptor.const_0x96 != 0x96 || !descriptor.index_count) {
      continue;
    }
    selected.memexport_formats.push_back(
        {stream.instruction_address, descriptor.dword_2});
  }
  return pool.GetOrCreate(selected, specialization_out);
}

inline uint32_t SelectSlot(
    ProfilePool& pool, const Shader* shader, const RegisterFile& regs,
    std::shared_ptr<const Shader::Specialization>& specialization_out) {
  if (!shader || shader->type() != xenos::ShaderType::kVertex ||
      !shader->memexport_eM_written()) {
    specialization_out.reset();
    return 0;
  }
  return SelectStreams(pool, shader->memexport_streams(), regs,
                       regs.Get<reg::SQ_VS_CONST>().base, specialization_out);
}

}  // namespace memexport_format_profile
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_MEMEXPORT_FORMAT_PROFILE_H_
