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
#include <cmath>
#include <cstring>
#include <map>

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
  // Joins this state into target, returning whether target gained a reason.
  bool JoinInto(FiniteValues& target) const {
    uint64_t gained = 0;
    for (uint32_t reg = 0; reg < xenos::kMaxShaderTempRegisters; ++reg) {
      for (uint32_t component = 0; component < 4; ++component) {
        uint64_t& value = target.registers[reg][component];
        gained |= registers[reg][component] & ~value;
        value |= registers[reg][component];
      }
    }
    gained |= previous_scalar & ~target.previous_scalar;
    target.previous_scalar |= previous_scalar;
    return gained != 0;
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

bool VertexFormatIsFinite(xenos::VertexFormat format) {
  switch (format) {
    case xenos::VertexFormat::k_8_8_8_8:
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_16_16:
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32:
      return true;
    default:
      return false;
  }
}

uint64_t Any(const uint64_t* reasons) {
  return reasons[0] | reasons[1] | reasons[2] | reasons[3];
}

// One pass in program order. label_states holds the state on entry to each
// label, and gains the states of the edges into it found by this pass. Returns
// whether any label gained a reason.
bool AnalyzeFiniteMultiplicationsPass(
    const Shader& shader, Translator::FiniteMultiplyAnalysis& analysis,
    std::map<uint32_t, FiniteValues>& label_states, bool one_sided,
    bool join_control_flow, bool give_up) {
  const auto& words = shader.ucode_data();
  analysis.operands.assign(words.size() / 3,
                           Translator::FiniteMultiplyOperands());
  analysis.dynamic_inputs = 0;
  std::fill(std::begin(analysis.direct_constants),
            std::end(analysis.direct_constants), 0);
  FiniteValues state;
  // A product needs the flags of both operands, or with one_sided, of either
  // operand proven finite.
  auto pair = [&](uint64_t a, uint64_t b) {
    if (!one_sided) {
      a |= b;
      b = a;
    }
    for (uint64_t reasons : {a, b}) {
      if (!(reasons & kUnknown)) {
        analysis.dynamic_inputs |= reasons;
      }
    }
  };
  bool label_states_changed = false;
  auto add_edge = [&](uint32_t target) {
    auto it = label_states.find(target);
    if (!give_up && it != label_states.end() && state.JoinInto(it->second)) {
      label_states_changed = true;
    }
  };
  for (uint32_t cf_index = 0; cf_index < shader.cf_pair_index_bound() * 2;
       ++cf_index) {
    auto label_it = label_states.find(cf_index);
    if (label_it != label_states.end()) {
      if (!join_control_flow || give_up) {
        state.Reset();
      } else {
        state.Join(label_it->second);
      }
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
        // Without join_control_flow, no proof survives a jump, loop, call,
        // return or unsupported CF op.
        if (join_control_flow) {
          switch (cf.opcode()) {
            case ucode::ControlFlowOpcode::kNop:
            case ucode::ControlFlowOpcode::kAlloc:
            case ucode::ControlFlowOpcode::kMarkVsFetchDone:
              // No register is written.
              continue;
            case ucode::ControlFlowOpcode::kCondJmp:
              // The fallthrough is joined too, even past an unconditional
              // jump.
              add_edge(cf.cond_jmp.address());
              continue;
            case ucode::ControlFlowOpcode::kLoopStart:
              // The loop skip address.
              add_edge(cf.loop_start.address());
              continue;
            case ucode::ControlFlowOpcode::kLoopEnd:
              // The loop body address.
              add_edge(cf.loop_end.address());
              continue;
            case ucode::ControlFlowOpcode::kCondCall:
              // The subroutine may write any register before returning here.
              add_edge(cf.cond_call.address());
              break;
            default:
              // No proof survives a return or an unsupported CF op.
              break;
          }
        }
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
          if (one_sided &&
              VertexFormatIsFinite(parsed.attributes.data_format)) {
            // Integers converted to float, normalized and scaled by at most
            // 2^31, or 0 out of bounds.
            std::fill(std::begin(reasons), std::end(reasons), 0);
          }
          state.Write(parsed.result, reasons,
                      conditional || parsed.is_predicated);
        } else {
          ParsedTextureFetchInstruction parsed;
          ParseTextureFetchInstruction(fetch.texture_fetch(), parsed);
          if (parsed.opcode == ucode::FetchOpcode::kTextureFetch &&
              parsed.operands[1].storage_index < 32) {
            std::fill(std::begin(reasons), std::end(reasons),
                      UINT64_C(1) << (Translator::kFiniteMultiplyTextureShift +
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
      Translator::FiniteMultiplyOperands inputs;
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
  return label_states_changed;
}

}  // namespace

void SpirvShaderTranslator::AnalyzeFiniteMultiplications(
    const Shader& shader, FiniteMultiplyAnalysis& analysis, bool one_sided,
    bool join_control_flow) {
  // The state at each label joins every edge into it: the fallthrough, and the
  // jumps, loop ends, loop skips and calls targeting it. Edges from later in
  // the program are only known after a pass, so passes repeat until no label
  // gains a reason. States only gain reasons, so this terminates; past the cap,
  // a last pass treats every label as unknown. Without join_control_flow, the
  // only pass does that.
  constexpr uint32_t kMaxPasses = 32;
  std::map<uint32_t, FiniteValues> label_states;
  for (uint32_t label : shader.label_addresses()) {
    // No edge seen yet: empty, the identity of the join.
    FiniteValues& entry = label_states[label];
    for (auto& reg : entry.registers) {
      std::fill(std::begin(reg), std::end(reg), 0);
    }
    entry.previous_scalar = 0;
  }
  for (uint32_t pass = 0;; ++pass) {
    bool give_up = !join_control_flow || pass >= kMaxPasses;
    if (!AnalyzeFiniteMultiplicationsPass(shader, analysis, label_states,
                                          one_sided, join_control_flow,
                                          give_up) ||
        give_up) {
      break;
    }
  }
}

namespace {
using DrawExact = Translator::DrawExactAnalysis;

// Values traced through the vertex shader as nodes of DrawExactAnalysis, 0 for
// an unknown one.
struct DrawExactValues {
  uint16_t registers[xenos::kMaxShaderTempRegisters][4];
};

struct DrawExactBuilder {
  // Before the nodes no flag needs are dropped.
  static constexpr size_t kNodeLimit = 4096;
  std::vector<DrawExact::Node> nodes;
  std::map<uint64_t, uint16_t> lookup;
  DrawExactValues state;
  uint16_t vertex_index = 0;
  // Whether r0.x has been given anything but the vertex index so far, and
  // whether control only flows forward, so that it's the index everywhere
  // before that.
  bool vertex_index_register_written = false;
  bool forward_only = false;

  DrawExactBuilder() { nodes.emplace_back(); }

  uint16_t Get(DrawExact::Op op, uint16_t a = 0, uint16_t b = 0) {
    uint64_t key = (uint64_t(op) << 32) | (uint64_t(a) << 16) | b;
    auto found = lookup.find(key);
    if (found != lookup.end()) {
      return found->second;
    }
    if (nodes.size() >= kNodeLimit) {
      return 0;
    }
    DrawExact::Node node;
    node.op = op;
    node.a = a;
    node.b = b;
    nodes.push_back(node);
    lookup.emplace(key, uint16_t(nodes.size() - 1));
    return uint16_t(nodes.size() - 1);
  }
  uint16_t Unary(DrawExact::Op op, uint16_t a) { return a ? Get(op, a) : 0; }
  uint16_t Binary(DrawExact::Op op, uint16_t a, uint16_t b) {
    if (!a || !b) {
      return 0;
    }
    return Get(op, std::min(a, b), std::max(a, b));
  }
  uint16_t Integer(DrawExact::Op op, uint16_t a) {
    // An integer is its own floor and truncation.
    switch (nodes[a].op) {
      case DrawExact::Op::kZero:
      case DrawExact::Op::kOne:
      case DrawExact::Op::kVertexIndex:
      case DrawExact::Op::kInteger:
      case DrawExact::Op::kFloor:
      case DrawExact::Op::kTruncate:
        return a;
      default:
        return Unary(op, a);
    }
  }
  uint16_t MinMax(uint16_t a, uint16_t b) {
    // The maximum or the minimum of a value and itself is how it is moved.
    // The same class describes a value that is either of two.
    return a == b ? a : Binary(DrawExact::Op::kMinMax, a, b);
  }

  // Leaves in a state what is true on either of two paths.
  void Join(DrawExactValues& into, const DrawExactValues& other) {
    for (uint32_t reg = 0; reg < xenos::kMaxShaderTempRegisters; ++reg) {
      for (uint32_t component = 0; component < 4; ++component) {
        uint16_t& value = into.registers[reg][component];
        value = MinMax(value, other.registers[reg][component]);
      }
    }
  }

  void Reset() {
    std::memset(state.registers, 0, sizeof(state.registers));
    if (forward_only && !vertex_index_register_written) {
      state.registers[0][0] = vertex_index;
    }
  }

  void Read(const InstructionOperand& operand, uint16_t* out) {
    for (uint32_t i = 0; i < 4; ++i) {
      out[i] = 0;
      if (!operand.component_count ||
          operand.storage_addressing_mode !=
              InstructionStorageAddressingMode::kAbsolute) {
        continue;
      }
      auto component = operand.GetComponent(i);
      if (component > SwizzleSource::kW) {
        continue;
      }
      uint16_t value = 0;
      if (operand.storage_source == InstructionStorageSource::kRegister &&
          operand.storage_index < xenos::kMaxShaderTempRegisters) {
        value = state.registers[operand.storage_index][uint32_t(component)];
      } else if (operand.storage_source ==
                     InstructionStorageSource::kConstantFloat &&
                 operand.storage_index < 256) {
        value = Get(DrawExact::Op::kConstant, uint16_t(operand.storage_index),
                    uint16_t(component));
      }
      if (operand.is_absolute_value) {
        value = Unary(DrawExact::Op::kAbsolute, value);
      }
      if (operand.is_negated) {
        value = Unary(DrawExact::Op::kNegate, value);
      }
      out[i] = value;
    }
  }

  void Write(const InstructionResult& result, const uint16_t* values,
             bool conditional, const InstructionResult* paired = nullptr) {
    if (result.storage_target != InstructionStorageTarget::kRegister) {
      return;
    }
    if (result.storage_addressing_mode !=
            InstructionStorageAddressingMode::kAbsolute ||
        result.storage_index >= xenos::kMaxShaderTempRegisters) {
      vertex_index_register_written = true;
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
      uint16_t value = 0;
      if (!result.is_clamped) {
        if (component == SwizzleSource::k0) {
          value = Get(DrawExact::Op::kZero);
        } else if (component == SwizzleSource::k1) {
          value = Get(DrawExact::Op::kOne);
        } else if (component <= SwizzleSource::kW) {
          value = values[uint32_t(component)];
        }
      }
      auto& target = state.registers[result.storage_index][i];
      if (overlap & (1u << i)) {
        if (target != value) {
          value = 0;
        }
      } else if (conditional) {
        // Either the old value or the new one.
        value = MinMax(target, value);
      }
      if (!result.storage_index && !i && value != vertex_index) {
        // Something other than the vertex index itself, as its truncation is.
        vertex_index_register_written = true;
      }
      target = value;
    }
  }
};

// The most bits an unnormalized integer vertex element has, 0 if its values
// aren't known to be small integers.
uint32_t DrawExactIntegerBits(
    const ParsedVertexFetchInstruction::Attributes& attributes) {
  if (!attributes.is_integer || attributes.exp_adjust) {
    return 0;
  }
  switch (attributes.data_format) {
    case xenos::VertexFormat::k_8_8_8_8:
      return 8;
    case xenos::VertexFormat::k_2_10_10_10:
      return 10;
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
      return 11;
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_16_16:
      return 16;
    default:
      return 0;
  }
}

}  // namespace

void SpirvShaderTranslator::AnalyzeDrawExactness(const Shader& shader,
                                                 DrawExactAnalysis& analysis) {
  using Op = DrawExactAnalysis::Op;
  using Node = DrawExactAnalysis::Node;
  using Sites = DrawExactAnalysis::Sites;
  constexpr uint8_t kNoFlag = DrawExactAnalysis::kNoFlag;
  constexpr uint32_t kMaxFlags = DrawExactAnalysis::kMaxFlags;
  constexpr uint32_t kMaxEstimates = DrawExactAnalysis::kMaxEstimates;
  constexpr uint32_t kMaxNodes = DrawExactAnalysis::kMaxNodes;
  analysis = {};
  if (shader.type() != xenos::ShaderType::kVertex) {
    return;
  }
  const auto& words = shader.ucode_data();
  size_t instruction_count = words.size() / 3;
  // The nodes of the sites of each instruction, 0 if there's none or if they
  // differ between the times the instruction is reached.
  struct SiteNodes {
    uint16_t vector_multiply[4] = {};
    uint16_t vector_add[4] = {};
    uint16_t scalar = 0;
    uint16_t estimate = 0;
    bool recorded = false;
  };
  std::vector<SiteNodes> site_nodes;
  DrawExactBuilder builder;
  builder.vertex_index = builder.Get(Op::kVertexIndex);
  // What a comparison gives.
  uint16_t zero_or_one =
      builder.MinMax(builder.Get(Op::kZero), builder.Get(Op::kOne));
  const auto& labels = shader.label_addresses();
  builder.forward_only = true;
  for (uint32_t cf_index = 0; cf_index < shader.cf_pair_index_bound() * 2;
       ++cf_index) {
    ucode::ControlFlowInstruction packed[2];
    ucode::UnpackControlFlowInstructions(words.data() + (cf_index / 2) * 3,
                                         packed);
    const auto& cf = packed[cf_index & 1];
    switch (cf.opcode()) {
      case ucode::ControlFlowOpcode::kLoopStart:
      case ucode::ControlFlowOpcode::kLoopEnd:
      case ucode::ControlFlowOpcode::kCondCall:
      case ucode::ControlFlowOpcode::kReturn:
        builder.forward_only = false;
        break;
      case ucode::ControlFlowOpcode::kCondJmp:
        if (cf.cond_jmp.address() <= cf_index) {
          builder.forward_only = false;
        }
        break;
      default:
        break;
    }
  }
  {
    site_nodes.assign(instruction_count, SiteNodes());
    builder.Reset();
    builder.state.registers[0][0] = builder.vertex_index;
    // Where control only flows forward, a label is reached with what holds on
    // every path to it: the states at the jumps to it, and the one before it
    // unless the shader has ended or jumped away there.
    std::map<uint32_t, DrawExactValues> jump_states;
    bool reachable = true;
    for (uint32_t cf_index = 0; cf_index < shader.cf_pair_index_bound() * 2;
         ++cf_index) {
      if (labels.count(cf_index) != 0) {
        auto jump_state = jump_states.find(cf_index);
        if (!builder.forward_only ||
            (!reachable && jump_state == jump_states.end())) {
          builder.Reset();
        } else if (jump_state != jump_states.end()) {
          if (reachable) {
            builder.Join(builder.state, jump_state->second);
          } else {
            builder.state = jump_state->second;
          }
        }
        reachable = true;
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
        case ucode::ControlFlowOpcode::kNop:
        case ucode::ControlFlowOpcode::kAlloc:
        case ucode::ControlFlowOpcode::kMarkVsFetchDone:
          // Control goes on to the next instruction.
          continue;
        case ucode::ControlFlowOpcode::kCondJmp:
          if (builder.forward_only) {
            if (reachable) {
              auto emplaced =
                  jump_states.emplace(cf.cond_jmp.address(), builder.state);
              if (!emplaced.second) {
                builder.Join(emplaced.first->second, builder.state);
              }
            }
            if (cf.cond_jmp.is_unconditional()) {
              reachable = false;
              builder.Reset();
            }
            continue;
          }
          builder.Reset();
          continue;
        default:
          // Nothing is known after a jump, loop, call, return or unsupported
          // control flow instruction.
          builder.Reset();
          continue;
      }
      // Like in AnalyzeFiniteMultiplications, writes in an exec conditional on
      // a boolean constant are definite inside it, and both paths rejoin
      // after it.
      bool boolean_scope =
          exec.type == ParsedExecInstruction::Type::kConditional;
      DrawExactValues before_exec = builder.state;
      uint32_t sequence = exec.sequence;
      ucode::VertexFetchInstruction previous_full = {};
      for (uint32_t i = 0; i < exec.instruction_count; ++i, sequence >>= 2) {
        uint32_t address = exec.instruction_address + i;
        if (address >= instruction_count) {
          break;
        }
        const uint32_t* data = words.data() + address * 3;
        bool conditional =
            exec.type == ParsedExecInstruction::Type::kPredicated;
        if (sequence & 1) {
          const auto& fetch =
              *reinterpret_cast<const ucode::FetchInstruction*>(data);
          uint16_t values[4] = {};
          if (fetch.opcode() == ucode::FetchOpcode::kVertexFetch) {
            ParsedVertexFetchInstruction parsed;
            if (ParseVertexFetchInstruction(fetch.vertex_fetch(), previous_full,
                                            parsed)) {
              previous_full = fetch.vertex_fetch();
            }
            uint32_t integer_bits = DrawExactIntegerBits(parsed.attributes);
            if (integer_bits) {
              std::fill(std::begin(values), std::end(values),
                        builder.Get(Op::kInteger, uint16_t(integer_bits)));
            }
            builder.Write(parsed.result, values,
                          conditional || parsed.is_predicated);
          } else {
            ParsedTextureFetchInstruction parsed;
            ParseTextureFetchInstruction(fetch.texture_fetch(), parsed);
            builder.Write(parsed.result, values,
                          conditional || parsed.is_predicated);
          }
          continue;
        }
        ParsedAluInstruction instr;
        ParseAluInstruction(
            *reinterpret_cast<const ucode::AluInstruction*>(data),
            shader.type(), instr);
        conditional |= instr.is_predicated;
        uint16_t vector_inputs[3][4] = {};
        uint16_t scalar_inputs[2][4] = {};
        for (uint32_t j = 0; j < instr.vector_operand_count; ++j) {
          builder.Read(instr.vector_operands[j], vector_inputs[j]);
        }
        for (uint32_t j = 0; j < instr.scalar_operand_count; ++j) {
          builder.Read(instr.scalar_operands[j], scalar_inputs[j]);
        }
        SiteNodes sites;
        sites.recorded = true;
        uint16_t vector_result[4] = {};
        switch (instr.vector_opcode) {
          case ucode::AluVectorOpcode::kAdd:
            for (uint32_t j = 0; j < 4; ++j) {
              vector_result[j] = sites.vector_add[j] = builder.Binary(
                  Op::kAdd, vector_inputs[0][j], vector_inputs[1][j]);
            }
            break;
          case ucode::AluVectorOpcode::kMul:
            for (uint32_t j = 0; j < 4; ++j) {
              vector_result[j] = sites.vector_multiply[j] = builder.Binary(
                  Op::kMultiply, vector_inputs[0][j], vector_inputs[1][j]);
            }
            break;
          case ucode::AluVectorOpcode::kMad:
            for (uint32_t j = 0; j < 4; ++j) {
              sites.vector_multiply[j] = builder.Binary(
                  Op::kMultiply, vector_inputs[0][j], vector_inputs[1][j]);
              vector_result[j] = sites.vector_add[j] = builder.Binary(
                  Op::kAdd, sites.vector_multiply[j], vector_inputs[2][j]);
            }
            break;
          case ucode::AluVectorOpcode::kMax:
          case ucode::AluVectorOpcode::kMin:
            for (uint32_t j = 0; j < 4; ++j) {
              vector_result[j] =
                  builder.MinMax(vector_inputs[0][j], vector_inputs[1][j]);
            }
            break;
          case ucode::AluVectorOpcode::kCndEq:
          case ucode::AluVectorOpcode::kCndGe:
          case ucode::AluVectorOpcode::kCndGt:
            for (uint32_t j = 0; j < 4; ++j) {
              vector_result[j] =
                  builder.MinMax(vector_inputs[1][j], vector_inputs[2][j]);
            }
            break;
          case ucode::AluVectorOpcode::kSeq:
          case ucode::AluVectorOpcode::kSgt:
          case ucode::AluVectorOpcode::kSge:
          case ucode::AluVectorOpcode::kSne:
            for (uint32_t j = 0; j < 4; ++j) {
              vector_result[j] = zero_or_one;
            }
            break;
          case ucode::AluVectorOpcode::kFrc:
            // Translated as the operand minus its floor.
            for (uint32_t j = 0; j < 4; ++j) {
              vector_result[j] = sites.vector_add[j] =
                  builder.Unary(Op::kFraction, vector_inputs[0][j]);
            }
            break;
          case ucode::AluVectorOpcode::kFloor:
          case ucode::AluVectorOpcode::kTrunc:
            for (uint32_t j = 0; j < 4; ++j) {
              vector_result[j] = builder.Integer(
                  instr.vector_opcode == ucode::AluVectorOpcode::kFloor
                      ? Op::kFloor
                      : Op::kTruncate,
                  vector_inputs[0][j]);
            }
            break;
          default:
            break;
        }
        uint16_t scalar_value = 0;
        auto estimate = [&](ScalarEstimateOp op, bool is_result) {
          sites.estimate = scalar_inputs[0][0]
                               ? builder.Get(Op::kEstimate, scalar_inputs[0][0],
                                             uint16_t(op))
                               : 0;
          if (is_result) {
            scalar_value = sites.estimate;
          }
        };
        switch (instr.scalar_opcode) {
          case ucode::AluScalarOpcode::kAdds:
            scalar_value = sites.scalar = builder.Binary(
                Op::kAdd, scalar_inputs[0][0], scalar_inputs[0][1]);
            break;
          case ucode::AluScalarOpcode::kSubs:
            scalar_value = sites.scalar =
                builder.Binary(Op::kAdd, scalar_inputs[0][0],
                               builder.Unary(Op::kNegate, scalar_inputs[0][1]));
            break;
          case ucode::AluScalarOpcode::kAddsc0:
          case ucode::AluScalarOpcode::kAddsc1:
            scalar_value = sites.scalar = builder.Binary(
                Op::kAdd, scalar_inputs[0][0], scalar_inputs[1][0]);
            break;
          case ucode::AluScalarOpcode::kSubsc0:
          case ucode::AluScalarOpcode::kSubsc1:
            scalar_value = sites.scalar =
                builder.Binary(Op::kAdd, scalar_inputs[0][0],
                               builder.Unary(Op::kNegate, scalar_inputs[1][0]));
            break;
          case ucode::AluScalarOpcode::kMuls:
            scalar_value = sites.scalar = builder.Binary(
                Op::kMultiply, scalar_inputs[0][0], scalar_inputs[0][1]);
            break;
          case ucode::AluScalarOpcode::kMulsc0:
          case ucode::AluScalarOpcode::kMulsc1:
            scalar_value = sites.scalar = builder.Binary(
                Op::kMultiply, scalar_inputs[0][0], scalar_inputs[1][0]);
            break;
          case ucode::AluScalarOpcode::kMaxs:
          case ucode::AluScalarOpcode::kMins:
            scalar_value =
                builder.MinMax(scalar_inputs[0][0], scalar_inputs[0][1]);
            break;
          case ucode::AluScalarOpcode::kSeqs:
          case ucode::AluScalarOpcode::kSgts:
          case ucode::AluScalarOpcode::kSges:
          case ucode::AluScalarOpcode::kSnes:
            scalar_value = zero_or_one;
            break;
          case ucode::AluScalarOpcode::kFrcs:
            scalar_value = sites.scalar =
                builder.Unary(Op::kFraction, scalar_inputs[0][0]);
            break;
          case ucode::AluScalarOpcode::kFloors:
            scalar_value = builder.Integer(Op::kFloor, scalar_inputs[0][0]);
            break;
          case ucode::AluScalarOpcode::kTruncs:
            scalar_value = builder.Integer(Op::kTruncate, scalar_inputs[0][0]);
            break;
          case ucode::AluScalarOpcode::kExp:
            estimate(ScalarEstimateOp::kExp, true);
            break;
          case ucode::AluScalarOpcode::kLog:
            estimate(ScalarEstimateOp::kLog, true);
            break;
          case ucode::AluScalarOpcode::kLogc:
            estimate(ScalarEstimateOp::kLog, false);
            break;
          case ucode::AluScalarOpcode::kSqrt:
            estimate(ScalarEstimateOp::kSqrt, true);
            break;
          case ucode::AluScalarOpcode::kRsq:
            estimate(ScalarEstimateOp::kRsq, true);
            break;
          case ucode::AluScalarOpcode::kRsqc:
          case ucode::AluScalarOpcode::kRsqf:
            estimate(ScalarEstimateOp::kRsq, false);
            break;
          case ucode::AluScalarOpcode::kRcp:
            estimate(ScalarEstimateOp::kRcp, true);
            break;
          case ucode::AluScalarOpcode::kRcpc:
          case ucode::AluScalarOpcode::kRcpf:
            estimate(ScalarEstimateOp::kRcp, false);
            break;
          default:
            break;
        }
        // Only a rounding-sensitive estimate is the console's, and only a sum
        // or a product with nothing to round is the same either way.
        if (!shader.IsAluScalarResultRoundingSensitive(address)) {
          if (sites.estimate && scalar_value == sites.estimate) {
            scalar_value = 0;
          }
          sites.estimate = 0;
        }
        auto& recorded = site_nodes[address];
        if (!recorded.recorded) {
          recorded = sites;
        } else {
          for (uint32_t j = 0; j < 4; ++j) {
            if (recorded.vector_multiply[j] != sites.vector_multiply[j]) {
              recorded.vector_multiply[j] = 0;
            }
            if (recorded.vector_add[j] != sites.vector_add[j]) {
              recorded.vector_add[j] = 0;
            }
          }
          if (recorded.scalar != sites.scalar) {
            recorded.scalar = 0;
          }
          if (recorded.estimate != sites.estimate) {
            recorded.estimate = 0;
          }
        }
        uint16_t scalar_result[4] = {scalar_value, scalar_value, scalar_value,
                                     scalar_value};
        builder.Write(instr.vector_and_constant_result, vector_result,
                      conditional);
        builder.Write(instr.scalar_result, scalar_result, conditional,
                      &instr.vector_and_constant_result);
      }
      if (boolean_scope) {
        builder.Join(builder.state, before_exec);
      }
      if (exec.is_end &&
          exec.type == ParsedExecInstruction::Type::kUnconditional) {
        reachable = false;
        builder.Reset();
      }
    }
  }

  // Flags for the rounding-sensitive sites, the estimates first since they
  // are the longest.
  auto& nodes = builder.nodes;
  // Whether the value may be known from the constants alone.
  std::vector<bool> constant(nodes.size(), false);
  for (size_t i = 1; i < nodes.size(); ++i) {
    const Node& node = nodes[i];
    switch (node.op) {
      case Op::kZero:
      case Op::kOne:
      case Op::kConstant:
        constant[i] = true;
        break;
      case Op::kNegate:
      case Op::kAbsolute:
      case Op::kFloor:
      case Op::kTruncate:
      case Op::kEstimate:
        constant[i] = constant[node.a];
        break;
      case Op::kAdd:
      case Op::kMultiply:
        constant[i] = constant[node.a] && constant[node.b];
        break;
      default:
        break;
    }
  }
  uint32_t flag_count = 0, estimate_count = 0;
  for (size_t address = 0; address < instruction_count; ++address) {
    uint16_t estimate = site_nodes[address].estimate;
    if (!estimate || nodes[estimate].flag != kNoFlag ||
        !constant[nodes[estimate].a] || flag_count >= kMaxFlags ||
        estimate_count >= kMaxEstimates) {
      continue;
    }
    nodes[estimate].flag = uint8_t(flag_count++);
    nodes[estimate].estimate = uint8_t(estimate_count++);
  }
  auto flag_site = [&](uint16_t node) {
    if (node && nodes[node].flag == kNoFlag && flag_count < kMaxFlags) {
      nodes[node].flag = uint8_t(flag_count++);
    }
  };
  for (size_t address = 0; address < instruction_count; ++address) {
    const SiteNodes& sites = site_nodes[address];
    if (shader.IsAluVectorResultRoundingSensitive(uint32_t(address))) {
      for (uint32_t j = 0; j < 4; ++j) {
        flag_site(sites.vector_multiply[j]);
        flag_site(sites.vector_add[j]);
      }
    }
    if (shader.IsAluScalarResultRoundingSensitive(uint32_t(address))) {
      flag_site(sites.scalar);
    }
  }
  if (!flag_count) {
    return;
  }
  // Only the nodes the flagged ones are made of are evaluated in the draw.
  std::vector<bool> needed(nodes.size(), false);
  for (size_t i = nodes.size() - 1; i; --i) {
    const Node& node = nodes[i];
    if (node.flag == kNoFlag && !needed[i]) {
      continue;
    }
    needed[i] = true;
    switch (node.op) {
      case Op::kAdd:
      case Op::kMultiply:
      case Op::kMinMax:
        needed[node.b] = true;
        [[fallthrough]];
      case Op::kNegate:
      case Op::kAbsolute:
      case Op::kFloor:
      case Op::kTruncate:
      case Op::kFraction:
      case Op::kEstimate:
        needed[node.a] = true;
        break;
      default:
        break;
    }
  }
  std::vector<uint16_t> renumbered(nodes.size(), 0);
  analysis.nodes.emplace_back();
  for (size_t i = 1; i < nodes.size(); ++i) {
    if (!needed[i]) {
      continue;
    }
    Node node = nodes[i];
    switch (node.op) {
      case Op::kAdd:
      case Op::kMultiply:
      case Op::kMinMax:
        node.b = renumbered[node.b];
        [[fallthrough]];
      case Op::kNegate:
      case Op::kAbsolute:
      case Op::kFloor:
      case Op::kTruncate:
      case Op::kFraction:
      case Op::kEstimate:
        node.a = renumbered[node.a];
        break;
      default:
        break;
    }
    analysis.uses_vertex_index |= node.op == Op::kVertexIndex;
    renumbered[i] = uint16_t(analysis.nodes.size());
    analysis.nodes.push_back(node);
  }
  if (analysis.nodes.size() > kMaxNodes) {
    analysis = {};
    return;
  }
  analysis.flag_count = flag_count;
  analysis.estimate_count = estimate_count;
  analysis.sites.assign(instruction_count, Sites());
  for (size_t address = 0; address < instruction_count; ++address) {
    const SiteNodes& from = site_nodes[address];
    Sites& to = analysis.sites[address];
    if (shader.IsAluVectorResultRoundingSensitive(uint32_t(address))) {
      for (uint32_t j = 0; j < 4; ++j) {
        to.vector_multiply[j] = nodes[from.vector_multiply[j]].flag;
        to.vector_add[j] = nodes[from.vector_add[j]].flag;
      }
    }
    if (shader.IsAluScalarResultRoundingSensitive(uint32_t(address))) {
      to.scalar = nodes[from.scalar].flag;
    }
    to.estimate = nodes[from.estimate].flag;
    to.estimate_index = nodes[from.estimate].estimate;
  }
}

uint32_t SpirvShaderTranslator::EvaluateDrawExactness(
    const DrawExactAnalysis& analysis, const uint32_t* float_constants,
    double vertex_index_bound, float* estimates_out) {
  std::fill(estimates_out, estimates_out + DrawExactAnalysis::kMaxEstimates,
            0.0f);
  if (!analysis.flag_count) {
    return 0;
  }
  // A value known to be finite is a multiple of 2^granularity of at most the
  // bound in magnitude. It's exact where 24 bits hold that. A concrete one is
  // the same in every vertex, its bits known.
  constexpr int32_t kAnyGranularity = INT32_MIN;
  constexpr int32_t kZeroGranularity = 4096;
  constexpr int32_t kMinGranularity = -100;
  constexpr double kMaxBound = 0x1p100;
  struct Value {
    bool known = false;
    bool concrete = false;
    int32_t granularity = kAnyGranularity;
    double bound = 0.0;
    float value = 0.0f;
  };
  auto from_concrete = [&](float concrete) {
    Value value;
    value.concrete = true;
    value.value = concrete;
    uint32_t bits = xe::memory::Reinterpret<uint32_t>(concrete);
    uint32_t exponent = (bits >> 23) & 0xFF;
    uint32_t mantissa = bits & 0x7FFFFF;
    if (!exponent && !mantissa) {
      value.known = true;
      value.granularity = kZeroGranularity;
      return value;
    }
    if (!exponent || exponent == 0xFF) {
      // Denormals may be flushed, and non-finite values are not bounded.
      return value;
    }
    uint32_t trailing_zeros = 23;
    if (mantissa) {
      xe::bit_scan_forward(mantissa, &trailing_zeros);
    }
    value.granularity = int32_t(exponent) - 127 - 23 + int32_t(trailing_zeros);
    value.bound = std::abs(double(concrete));
    value.known =
        value.granularity >= kMinGranularity && value.bound <= kMaxBound;
    if (!value.known) {
      value.granularity = kAnyGranularity;
    }
    return value;
  };
  // Whether a sum or a product of known values is exact, and what it is.
  auto combine = [&](const Value& a, const Value& b, bool multiply,
                     bool& exact) {
    Value value;
    exact = false;
    if (!a.known || !b.known) {
      return value;
    }
    bool a_zero = a.granularity == kZeroGranularity;
    bool b_zero = b.granularity == kZeroGranularity;
    if (multiply ? (a_zero || b_zero) : (a_zero && b_zero)) {
      value.known = true;
      value.granularity = kZeroGranularity;
      exact = true;
      // A product is stored as +0 if an operand is zero. The sign of a zero
      // sum is not followed.
      value.concrete = multiply;
      return value;
    }
    double bound = multiply ? a.bound * b.bound : a.bound + b.bound;
    // The bound itself may have been rounded down.
    bound = std::nextafter(bound, HUGE_VAL);
    if (!(bound <= kMaxBound)) {
      return value;
    }
    value.known = true;
    value.bound = bound;
    if (a.granularity != kAnyGranularity && b.granularity != kAnyGranularity) {
      int32_t granularity = multiply ? a.granularity + b.granularity
                                     : std::min(a.granularity, b.granularity);
      if (granularity >= kMinGranularity &&
          bound <= std::ldexp(1.0, granularity + 24)) {
        exact = true;
        value.granularity = granularity;
        if (a.concrete && b.concrete) {
          float concrete = multiply ? a.value * b.value : a.value + b.value;
          // The sign of a zero is not followed.
          if (concrete != 0.0f) {
            value.concrete = true;
            value.value = concrete;
          } else {
            value.granularity = kZeroGranularity;
            value.bound = 0.0;
          }
        }
        return value;
      }
    }
    // Rounded to the nearest where it's not rounding-sensitive, which may be
    // farther from zero, by whole steps of a denormal result.
    value.bound = bound * (1.0 + 0x1p-22) + 0x1p-126;
    return value;
  };
  Value values[DrawExactAnalysis::kMaxNodes + 1];
  uint32_t flags = 0;
  for (size_t i = 1; i < analysis.nodes.size(); ++i) {
    const DrawExactAnalysis::Node& node = analysis.nodes[i];
    Value& value = values[i];
    // For kConstant and kInteger, node.a is not a node index and may be past
    // the array: don't form a reference from it (it's not read for those).
    const Value& a =
        values[node.a <= DrawExactAnalysis::kMaxNodes ? node.a : 0];
    switch (node.op) {
      case DrawExactAnalysis::Op::kZero:
        value = from_concrete(0.0f);
        break;
      case DrawExactAnalysis::Op::kOne:
        value = from_concrete(1.0f);
        break;
      case DrawExactAnalysis::Op::kConstant:
        value = from_concrete(xe::memory::Reinterpret<float>(
            float_constants[node.a * 4 + node.b]));
        break;
      case DrawExactAnalysis::Op::kVertexIndex:
        if (vertex_index_bound >= 0.0 && vertex_index_bound <= 0x1p24) {
          value.known = true;
          value.granularity = 0;
          value.bound = vertex_index_bound;
        }
        break;
      case DrawExactAnalysis::Op::kInteger:
        value.known = true;
        value.granularity = 0;
        value.bound = std::ldexp(1.0, node.a);
        break;
      case DrawExactAnalysis::Op::kNegate:
        value = values[node.a];
        value.value = -value.value;
        break;
      case DrawExactAnalysis::Op::kAbsolute:
        value = values[node.a];
        value.value = std::abs(value.value);
        break;
      case DrawExactAnalysis::Op::kAdd:
      case DrawExactAnalysis::Op::kMultiply: {
        bool exact;
        value = combine(a, values[node.b],
                        node.op == DrawExactAnalysis::Op::kMultiply, exact);
        if (exact && node.flag != DrawExactAnalysis::kNoFlag) {
          flags |= UINT32_C(1) << node.flag;
        }
      } break;
      case DrawExactAnalysis::Op::kFloor:
      case DrawExactAnalysis::Op::kTruncate:
        if (!a.known) {
          break;
        }
        if (a.granularity != kAnyGranularity && a.granularity >= 0) {
          // Already an integer.
          value = a;
          break;
        }
        value.known = true;
        value.granularity = 0;
        value.bound = std::ceil(a.bound);
        if (a.concrete) {
          float concrete = node.op == DrawExactAnalysis::Op::kFloor
                               ? std::floor(a.value)
                               : std::trunc(a.value);
          // The sign of a zero is not followed.
          if (concrete != 0.0f) {
            value.concrete = true;
            value.value = concrete;
          }
        }
        break;
      case DrawExactAnalysis::Op::kFraction:
        // The operand minus its floor: in [0, 1], and with nothing to round
        // if the operand has no bits below 2^-24.
        if (!a.known) {
          break;
        }
        value.known = true;
        if (a.granularity != kAnyGranularity && a.granularity >= -24) {
          if (a.granularity >= 0) {
            value.granularity = kZeroGranularity;
          } else {
            value.granularity = a.granularity;
            value.bound = 1.0;
          }
          if (node.flag != DrawExactAnalysis::kNoFlag) {
            flags |= UINT32_C(1) << node.flag;
          }
        } else {
          value.bound = 1.0;
        }
        break;
      case DrawExactAnalysis::Op::kMinMax: {
        const Value& b = values[node.b];
        if (!a.known || !b.known) {
          break;
        }
        value.known = true;
        value.bound = std::max(a.bound, b.bound);
        value.granularity =
            a.granularity == kAnyGranularity || b.granularity == kAnyGranularity
                ? kAnyGranularity
                : std::min(a.granularity, b.granularity);
      } break;
      case DrawExactAnalysis::Op::kEstimate: {
        // The bits of a known concrete value are certain, a zero being a
        // constant itself.
        if (!a.concrete || !a.known) {
          break;
        }
        float estimate;
        switch (ScalarEstimateOp(node.b)) {
          case ScalarEstimateOp::kRcp:
            estimate = xenos::Rcp(a.value);
            break;
          case ScalarEstimateOp::kRsq:
            estimate = xenos::Rsq(a.value);
            break;
          case ScalarEstimateOp::kSqrt:
            estimate = xenos::Sqrt(a.value);
            break;
          case ScalarEstimateOp::kExp:
            estimate = xenos::Exp(a.value);
            break;
          default:
            estimate = xenos::Log(a.value);
            break;
        }
        value = from_concrete(estimate);
        if (estimate == 0.0f) {
          // Not followed further, but folded with its sign.
          value.concrete = false;
        }
        if (node.flag != DrawExactAnalysis::kNoFlag) {
          flags |= UINT32_C(1) << node.flag;
          estimates_out[node.estimate] = estimate;
        }
      } break;
      default:
        break;
    }
  }
  return flags;
}

}  // namespace xe::gpu
