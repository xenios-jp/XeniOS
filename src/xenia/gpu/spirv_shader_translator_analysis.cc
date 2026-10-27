/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/spirv_shader_translator.h"

#include <algorithm>

namespace xe::gpu {
namespace {
using Translator = SpirvShaderTranslator;
constexpr uint64_t kUnknown = Translator::kFiniteMultiplyUnknown;

struct FiniteValues {
  uint64_t registers[xenos::kMaxShaderTempRegisters][4];
  uint64_t previous_scalar = kUnknown;
  FiniteValues() { Reset(); }
  void Reset() {
    for (auto& reg : registers) {
      for (auto& component : reg) {
        component = kUnknown;
      }
    }
    previous_scalar = kUnknown;
  }
  void Join(const FiniteValues& other) {
    for (uint32_t reg = 0; reg < xenos::kMaxShaderTempRegisters; ++reg) {
      for (uint32_t component = 0; component < 4; ++component) {
        registers[reg][component] |= other.registers[reg][component];
      }
    }
    previous_scalar |= other.previous_scalar;
  }
  void Read(const InstructionOperand& operand, uint64_t* out) const {
    for (uint32_t i = 0; i < 4; ++i) {
      out[i] = kUnknown;
      if (!operand.component_count) {
        continue;
      }
      auto component = operand.GetComponent(i);
      if (component == SwizzleSource::k0 || component == SwizzleSource::k1) {
        out[i] = 0;
        continue;
      }
      if (component > SwizzleSource::kW ||
          operand.storage_addressing_mode !=
              InstructionStorageAddressingMode::kAbsolute) {
        continue;
      }
      if (operand.storage_source == InstructionStorageSource::kRegister &&
          operand.storage_index < xenos::kMaxShaderTempRegisters) {
        out[i] = registers[operand.storage_index][uint32_t(component)];
      } else if (operand.storage_source ==
                     InstructionStorageSource::kConstantFloat &&
                 operand.storage_index < 256) {
        out[i] = Translator::kFiniteMultiplyFloatConstants;
      }
    }
  }
  void Write(const InstructionResult& result, const uint64_t* reasons,
             bool conditional, const InstructionResult* paired = nullptr) {
    if (result.storage_target != InstructionStorageTarget::kRegister) {
      return;
    }
    if (result.storage_addressing_mode !=
            InstructionStorageAddressingMode::kAbsolute ||
        result.storage_index >= xenos::kMaxShaderTempRegisters) {
      Reset();
      return;
    }
    uint32_t mask = result.GetUsedWriteMask();
    uint32_t overlap =
        paired &&
                paired->storage_target == InstructionStorageTarget::kRegister &&
                paired->storage_addressing_mode ==
                    InstructionStorageAddressingMode::kAbsolute &&
                paired->storage_index == result.storage_index
            ? paired->GetUsedWriteMask()
            : 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(mask & (1u << i))) {
        continue;
      }
      auto component = result.components[i];
      uint64_t value = result.is_clamped || component == SwizzleSource::k0 ||
                               component == SwizzleSource::k1
                           ? 0
                       : component <= SwizzleSource::kW
                           ? reasons[uint32_t(component)]
                           : kUnknown;
      auto& target = registers[result.storage_index][i];
      target = conditional || (overlap & (1u << i)) ? target | value : value;
    }
  }
};

uint64_t Any(const uint64_t* reasons) {
  return reasons[0] | reasons[1] | reasons[2] | reasons[3];
}

}  // namespace

void SpirvShaderTranslator::AnalyzeFiniteMultiplications(
    const Shader& shader, FiniteMultiplyAnalysis& analysis) {
  const auto& words = shader.ucode_data();
  analysis.operands.assign(words.size() / 3, FiniteMultiplyOperands());
  analysis.dynamic_inputs = 0;
  std::fill(std::begin(analysis.direct_constants),
            std::end(analysis.direct_constants), 0);
  FiniteValues state;
  auto pair = [&](uint64_t a, uint64_t b) {
    uint64_t reasons = a | b;
    if (!(reasons & kUnknown)) {
      analysis.dynamic_inputs |= reasons;
    }
  };
  const auto& labels = shader.label_addresses();
  for (uint32_t cf_index = 0; cf_index < shader.cf_pair_index_bound() * 2;
       ++cf_index) {
    if (labels.count(cf_index) != 0) {
      state.Reset();
    }
    ucode::ControlFlowInstruction packed[2];
    ucode::UnpackControlFlowInstructions(words.data() + (cf_index / 2) * 3,
                                         packed);
    const auto& cf = packed[cf_index & 1];
    ParsedExecInstruction exec;
    switch (cf.opcode()) {
      case ucode::ControlFlowOpcode::kExec:
      case ucode::ControlFlowOpcode::kExecEnd:
        ParseControlFlowExec(cf.exec, cf_index, exec);
        break;
      case ucode::ControlFlowOpcode::kCondExec:
      case ucode::ControlFlowOpcode::kCondExecEnd:
      case ucode::ControlFlowOpcode::kCondExecPredClean:
      case ucode::ControlFlowOpcode::kCondExecPredCleanEnd:
        ParseControlFlowCondExec(cf.cond_exec, cf_index, exec);
        break;
      case ucode::ControlFlowOpcode::kCondExecPred:
      case ucode::ControlFlowOpcode::kCondExecPredEnd:
        ParseControlFlowCondExecPred(cf.cond_exec_pred, cf_index, exec);
        break;
      default:
        // No proof survives a jump, loop, call, return or unsupported CF op.
        state.Reset();
        continue;
    }
    // A boolean exec condition is immutable within the draw. Inside its taken
    // path, unpredicated writes are definite; both paths rejoin afterward.
    bool boolean_scope = exec.type == ParsedExecInstruction::Type::kConditional;
    FiniteValues before_exec = state;
    uint32_t sequence = exec.sequence;
    ucode::VertexFetchInstruction previous_full = {};
    for (uint32_t i = 0; i < exec.instruction_count; ++i, sequence >>= 2) {
      uint32_t address = exec.instruction_address + i;
      if (address >= analysis.operands.size()) {
        break;
      }
      const uint32_t* data = words.data() + address * 3;
      bool conditional = exec.type == ParsedExecInstruction::Type::kPredicated;
      if (sequence & 1) {
        const auto& fetch =
            *reinterpret_cast<const ucode::FetchInstruction*>(data);
        uint64_t reasons[4] = {kUnknown, kUnknown, kUnknown, kUnknown};
        if (fetch.opcode() == ucode::FetchOpcode::kVertexFetch) {
          ParsedVertexFetchInstruction parsed;
          if (ParseVertexFetchInstruction(fetch.vertex_fetch(), previous_full,
                                          parsed)) {
            previous_full = fetch.vertex_fetch();
          }
          state.Write(parsed.result, reasons,
                      conditional || parsed.is_predicated);
        } else {
          ParsedTextureFetchInstruction parsed;
          ParseTextureFetchInstruction(fetch.texture_fetch(), parsed);
          if (parsed.opcode == ucode::FetchOpcode::kTextureFetch &&
              parsed.operands[1].storage_index < 32) {
            std::fill(std::begin(reasons), std::end(reasons),
                      UINT64_C(1) << (kFiniteMultiplyTextureShift +
                                      parsed.operands[1].storage_index));
          }
          state.Write(parsed.result, reasons,
                      conditional || parsed.is_predicated);
        }
        continue;
      }
      ParsedAluInstruction instr;
      ParseAluInstruction(*reinterpret_cast<const ucode::AluInstruction*>(data),
                          shader.type(), instr);
      conditional |= instr.is_predicated;
      auto record_constant = [&](const InstructionOperand& operand) {
        if (operand.storage_source ==
                InstructionStorageSource::kConstantFloat &&
            operand.storage_addressing_mode ==
                InstructionStorageAddressingMode::kAbsolute &&
            operand.storage_index < 256) {
          analysis.direct_constants[operand.storage_index >> 6] |=
              UINT64_C(1) << (operand.storage_index & 63);
        }
      };
      FiniteMultiplyOperands inputs;
      for (uint32_t j = 0; j < instr.vector_operand_count; ++j) {
        state.Read(instr.vector_operands[j], inputs.vector[j]);
        record_constant(instr.vector_operands[j]);
      }
      for (uint32_t j = 0; j < instr.scalar_operand_count; ++j) {
        state.Read(instr.scalar_operands[j], inputs.scalar[j]);
        record_constant(instr.scalar_operands[j]);
      }
      inputs.previous_scalar = state.previous_scalar;
      inputs.recorded = true;
      auto& record = analysis.operands[address];
      if (!record.recorded) {
        record = inputs;
      } else {
        for (uint32_t j = 0; j < 3; ++j) {
          for (uint32_t k = 0; k < 4; ++k) {
            record.vector[j][k] |= inputs.vector[j][k];
          }
        }
        for (uint32_t j = 0; j < 2; ++j) {
          for (uint32_t k = 0; k < 4; ++k) {
            record.scalar[j][k] |= inputs.scalar[j][k];
          }
        }
        record.previous_scalar |= inputs.previous_scalar;
      }
      switch (instr.vector_opcode) {
        case ucode::AluVectorOpcode::kMul:
        case ucode::AluVectorOpcode::kMad:
        case ucode::AluVectorOpcode::kDp4:
        case ucode::AluVectorOpcode::kDp3:
        case ucode::AluVectorOpcode::kDp2Add:
          for (uint32_t j = 0; j < 4; ++j) {
            pair(inputs.vector[0][j], inputs.vector[1][j]);
          }
          break;
        case ucode::AluVectorOpcode::kDst:
          pair(inputs.vector[0][1], inputs.vector[1][1]);
          break;
        default:
          break;
      }
      switch (instr.scalar_opcode) {
        case ucode::AluScalarOpcode::kMuls:
          pair(inputs.scalar[0][0], inputs.scalar[0][1]);
          break;
        case ucode::AluScalarOpcode::kMulsPrev:
          pair(inputs.scalar[0][0], inputs.previous_scalar);
          break;
        case ucode::AluScalarOpcode::kMulsc0:
        case ucode::AluScalarOpcode::kMulsc1:
          pair(inputs.scalar[0][0], inputs.scalar[1][0]);
          break;
        default:
          break;
      }
      uint64_t vector_result[4] = {kUnknown, kUnknown, kUnknown, kUnknown};
      switch (instr.vector_opcode) {
        case ucode::AluVectorOpcode::kSeq:
        case ucode::AluVectorOpcode::kSgt:
        case ucode::AluVectorOpcode::kSge:
        case ucode::AluVectorOpcode::kSne:
          std::fill(std::begin(vector_result), std::end(vector_result), 0);
          break;
        case ucode::AluVectorOpcode::kMax:
        case ucode::AluVectorOpcode::kMin:
        case ucode::AluVectorOpcode::kMaxA:
          for (uint32_t j = 0; j < 4; ++j) {
            vector_result[j] = inputs.vector[0][j] | inputs.vector[1][j];
          }
          break;
        case ucode::AluVectorOpcode::kFrc:
        case ucode::AluVectorOpcode::kFloor:
        case ucode::AluVectorOpcode::kTrunc:
          std::copy(std::begin(inputs.vector[0]), std::end(inputs.vector[0]),
                    vector_result);
          break;
        default:
          break;
      }
      uint64_t scalar_reason = kUnknown;
      switch (instr.scalar_opcode) {
        case ucode::AluScalarOpcode::kSeqs:
        case ucode::AluScalarOpcode::kSgts:
        case ucode::AluScalarOpcode::kSges:
        case ucode::AluScalarOpcode::kSnes:
          scalar_reason = 0;
          break;
        case ucode::AluScalarOpcode::kMaxs:
        case ucode::AluScalarOpcode::kMins:
          scalar_reason = Any(inputs.scalar[0]);
          break;
        case ucode::AluScalarOpcode::kFrcs:
        case ucode::AluScalarOpcode::kFloors:
        case ucode::AluScalarOpcode::kTruncs:
          scalar_reason = inputs.scalar[0][0];
          break;
        case ucode::AluScalarOpcode::kRetainPrev:
          scalar_reason = inputs.previous_scalar;
          break;
        default:
          break;
      }
      uint64_t scalar_result[4] = {scalar_reason, scalar_reason, scalar_reason,
                                   scalar_reason};
      state.Write(instr.vector_and_constant_result, vector_result, conditional);
      state.Write(instr.scalar_result, scalar_result, conditional,
                  &instr.vector_and_constant_result);
      if (instr.scalar_opcode != ucode::AluScalarOpcode::kRetainPrev) {
        state.previous_scalar =
            conditional ? state.previous_scalar | scalar_reason : scalar_reason;
      }
      // Relative destinations may alias either paired destination in either
      // order. Discard all local proofs after such a write.
      for (const auto* result :
           {&instr.vector_and_constant_result, &instr.scalar_result}) {
        if (result->storage_target == InstructionStorageTarget::kRegister &&
            result->storage_addressing_mode !=
                InstructionStorageAddressingMode::kAbsolute) {
          state.Reset();
        }
      }
    }
    if (boolean_scope) {
      state.Join(before_exec);
    }
  }
}

}  // namespace xe::gpu
