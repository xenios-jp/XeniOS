/** ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 * ******************************************************************************
 * Native Metal Shading Language production-path translator.
 * ******************************************************************************
 */
#include "xenia/gpu/msl_shader_translator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_shader.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace {

std::string MslFloatLiteral(float value) {
  if (std::isnan(value)) {
    return "as_type<float>(0x7FC00000u)";
  }
  if (std::isinf(value)) {
    return value < 0.0f ? "-as_type<float>(0x7F800000u)"
                        : "as_type<float>(0x7F800000u)";
  }
  std::string text = fmt::format("{:.9g}", value);
  if (text.find_first_of(".eE") == std::string::npos) {
    text += ".0";
  }
  text += "f";
  return text;
}

std::string MslZeroLiteral(uint32_t component_count) {
  return component_count <= 1
             ? "0.0f"
             : "float" + std::to_string(component_count) + "(0.0f)";
}

bool IsMslZeroLiteral(const std::string& expression, uint32_t component_count) {
  std::string expr = expression;
  while (expr.size() > 3 && expr.rfind("-(", 0) == 0 && expr.back() == ')') {
    expr = expr.substr(2, expr.size() - 3);
  }
  if (component_count <= 1 && (expr == "0.0f" || expr == "-0.0f")) {
    return true;
  }
  if (component_count > 1 && expr == MslZeroLiteral(component_count)) {
    return true;
  }
  const std::string swizzled_zero_prefix = "float4(0.0f).";
  if (expr.rfind(swizzled_zero_prefix, 0) == 0) {
    std::string swizzle = expr.substr(swizzled_zero_prefix.size());
    if (swizzle.size() != component_count) {
      return false;
    }
    return swizzle.find_first_not_of("xyzw") == std::string::npos;
  }
  if (component_count > 1) {
    std::string zero_constructor =
        "float" + std::to_string(component_count) + "(";
    for (uint32_t i = 0; i < component_count; ++i) {
      if (i) {
        zero_constructor += ", ";
      }
      zero_constructor += "0.0f";
    }
    zero_constructor += ")";
    if (expr == zero_constructor) {
      return true;
    }
  }
  return false;
}

bool NativeMslTextureSignsNeedSigned(uint8_t signs, uint8_t component_mask) {
  for (uint32_t i = 0; i < 4; ++i) {
    if ((component_mask & (uint8_t(1) << i)) &&
        (((signs >> (i * 2)) & 0x3u) ==
         uint32_t(xenos::TextureSign::kSigned))) {
      return true;
    }
  }
  return false;
}

bool NativeMslTextureSignsNeedUnsigned(uint8_t signs, uint8_t component_mask) {
  for (uint32_t i = 0; i < 4; ++i) {
    if ((component_mask & (uint8_t(1) << i)) &&
        (((signs >> (i * 2)) & 0x3u) !=
         uint32_t(xenos::TextureSign::kSigned))) {
      return true;
    }
  }
  return false;
}

bool NativeMslTextureSignsNeedGamma(uint8_t signs, uint8_t component_mask) {
  for (uint32_t i = 0; i < 4; ++i) {
    if ((component_mask & (uint8_t(1) << i)) &&
        (((signs >> (i * 2)) & 0x3u) == uint32_t(xenos::TextureSign::kGamma))) {
      return true;
    }
  }
  return false;
}

bool NativeMslVertexFetchNeedsSignExtend(
    const ParsedVertexFetchInstruction& instr) {
  if (!instr.attributes.is_signed) {
    return false;
  }
  switch (instr.attributes.data_format) {
    case xenos::VertexFormat::k_8_8_8_8:
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_16_16:
      return true;
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32:
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      return false;
    default:
      return true;
  }
}

constexpr uint8_t kNativeMslFloat1Width = 1u << 0;
constexpr uint8_t kNativeMslFloat2Width = 1u << 1;
constexpr uint8_t kNativeMslFloat3Width = 1u << 2;
constexpr uint8_t kNativeMslFloat4Width = 1u << 3;
constexpr uint8_t kNativeMslAllFloatWidths =
    kNativeMslFloat1Width | kNativeMslFloat2Width | kNativeMslFloat3Width |
    kNativeMslFloat4Width;

uint8_t NativeMslFloatWidthBit(uint32_t component_count) {
  assert_true(component_count >= 1u && component_count <= 4u);
  return uint8_t(1u << (component_count - 1u));
}

uint8_t NativeMslTexturePwlGammaWidths(uint8_t signs,
                                       uint8_t used_component_mask) {
  auto sign = [&](uint32_t component_index) {
    return uint32_t((signs >> (component_index * 2u)) & 0x3u);
  };
  const uint32_t sign_x = sign(0);
  const uint32_t sign_y = sign(1);
  const uint32_t sign_z = sign(2);
  const uint32_t sign_w = sign(3);
  if (used_component_mask == 0b1111u && sign_x == sign_y && sign_x == sign_z &&
      sign_x == sign_w) {
    return sign_x == uint32_t(xenos::TextureSign::kGamma)
               ? kNativeMslFloat4Width
               : uint8_t(0);
  }
  if ((used_component_mask == 0b0111u || used_component_mask == 0b1111u) &&
      sign_x == sign_y && sign_x == sign_z &&
      sign_w == uint32_t(xenos::TextureSign::kUnsigned)) {
    return sign_x == uint32_t(xenos::TextureSign::kGamma)
               ? kNativeMslFloat3Width
               : uint8_t(0);
  }
  uint8_t widths = 0;
  if ((used_component_mask & 0b0011u) == 0b0011u && sign_x == sign_y) {
    if (sign_x == uint32_t(xenos::TextureSign::kGamma)) {
      widths |= kNativeMslFloat2Width;
    }
    if ((used_component_mask & 0b0100u) &&
        sign_z == uint32_t(xenos::TextureSign::kGamma)) {
      widths |= kNativeMslFloat1Width;
    }
    if ((used_component_mask & 0b1000u) &&
        sign_w == uint32_t(xenos::TextureSign::kGamma)) {
      widths |= kNativeMslFloat1Width;
    }
    return widths;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if ((used_component_mask & (uint8_t(1) << i)) &&
        sign(i) == uint32_t(xenos::TextureSign::kGamma)) {
      widths |= kNativeMslFloat1Width;
    }
  }
  return widths;
}

std::string NativeMslApplyTextureSignExpr(const std::string& unsigned_value,
                                          const std::string& signed_value,
                                          uint32_t sign) {
  switch (xenos::TextureSign(sign)) {
    case xenos::TextureSign::kSigned:
      return signed_value;
    case xenos::TextureSign::kUnsignedBiased:
      return "fma(" + unsigned_value + ", 2.0f, -1.0f)";
    case xenos::TextureSign::kGamma:
      return "XePWLGammaToLinear(" + unsigned_value + ")";
    case xenos::TextureSign::kUnsigned:
    default:
      return unsigned_value;
  }
}

std::string MslMulSM3Expr(const std::string& op0, const std::string& op1,
                          uint32_t component_count) {
  if (IsMslZeroLiteral(op0, component_count) ||
      IsMslZeroLiteral(op1, component_count)) {
    return MslZeroLiteral(component_count);
  }
  if (op0 == op1) {
    return "(" + op0 + " * " + op1 + ")";
  }
  return "XeMulSM3(" + op0 + ", " + op1 + ")";
}

enum TextureFetchDerivedMask : uint32_t {
  kTextureFetchDerivedDimension = 1u << 0,
  kTextureFetchDerivedWidth1D = 1u << 1,
  kTextureFetchDerivedSize2D = 1u << 2,
  kTextureFetchDerivedSize3D = 1u << 3,
  kTextureFetchDerivedSizeStacked = 1u << 4,
  kTextureFetchDerivedLodBias = 1u << 5,
  kTextureFetchDerivedExpAdjust = 1u << 6,
};

bool IsMemExportTarget(const InstructionResult& result) {
  return result.storage_target == InstructionStorageTarget::kExportAddress ||
         result.storage_target == InstructionStorageTarget::kExportData;
}

const char* GetMemExportTargetName(const InstructionResult& result) {
  return result.storage_target == InstructionStorageTarget::kExportAddress
             ? "eA"
             : "eM";
}

bool IsVectorKillOpcode(ucode::AluVectorOpcode opcode) {
  switch (opcode) {
    case ucode::AluVectorOpcode::kKillEq:
    case ucode::AluVectorOpcode::kKillGt:
    case ucode::AluVectorOpcode::kKillGe:
    case ucode::AluVectorOpcode::kKillNe:
      return true;
    default:
      return false;
  }
}

bool IsScalarKillOpcode(ucode::AluScalarOpcode opcode) {
  switch (opcode) {
    case ucode::AluScalarOpcode::kKillsEq:
    case ucode::AluScalarOpcode::kKillsGt:
    case ucode::AluScalarOpcode::kKillsGe:
    case ucode::AluScalarOpcode::kKillsNe:
    case ucode::AluScalarOpcode::kKillsOne:
      return true;
    default:
      return false;
  }
}

std::string SelectExpr(const std::string& condition, const std::string& if_true,
                       const std::string& if_false) {
  // MSL select(false_value, true_value, condition).
  return "select((" + if_false + "), (" + if_true + "), (" + condition + "))";
}

std::string MslByteOffsetTerm(int32_t offset_dwords) {
  int64_t bytes = int64_t(offset_dwords) * 4;
  if (bytes >= 0) {
    return " + " + std::to_string(uint64_t(bytes)) + "u";
  }
  return " - " + std::to_string(uint64_t(-bytes)) + "u";
}

using NativeMslSystemConstants = ShaderSystemConstants;
static_assert(sizeof(NativeMslSystemConstants) == 464,
              "Native MSL XeSystemConstants must mirror SystemConstants");
static_assert(offsetof(NativeMslSystemConstants, flags) == 0);
static_assert(offsetof(NativeMslSystemConstants, tessellation_factor_range) ==
              4);
static_assert(offsetof(NativeMslSystemConstants, line_loop_closing_index) ==
              12);
static_assert(offsetof(NativeMslSystemConstants, vertex_index_endian) == 16);
static_assert(offsetof(NativeMslSystemConstants, vertex_index_offset) == 20);
static_assert(offsetof(NativeMslSystemConstants, vertex_index_min_max) == 24);
static_assert(offsetof(NativeMslSystemConstants, user_clip_planes) == 32);
static_assert(offsetof(NativeMslSystemConstants, ndc_scale) == 128);
static_assert(offsetof(NativeMslSystemConstants, point_vertex_diameter_min) ==
              140);
static_assert(offsetof(NativeMslSystemConstants, ndc_offset) == 144);
static_assert(offsetof(NativeMslSystemConstants, point_vertex_diameter_max) ==
              156);
static_assert(offsetof(NativeMslSystemConstants, point_constant_diameter) ==
              160);
static_assert(offsetof(NativeMslSystemConstants,
                       point_screen_diameter_to_ndc_radius) == 168);
static_assert(offsetof(NativeMslSystemConstants, texture_swizzled_signs) ==
              176);
static_assert(offsetof(NativeMslSystemConstants, textures_resolution_scaled) ==
              208);
static_assert(offsetof(NativeMslSystemConstants, sample_count_log2) == 212);
static_assert(offsetof(NativeMslSystemConstants, alpha_test_reference) == 220);
static_assert(offsetof(NativeMslSystemConstants, alpha_to_mask) == 224);
static_assert(offsetof(NativeMslSystemConstants,
                       edram_32bpp_tile_pitch_dwords_scaled) == 228);
static_assert(offsetof(NativeMslSystemConstants,
                       edram_depth_base_dwords_scaled) == 232);
static_assert(offsetof(NativeMslSystemConstants, zpd_rov_counter_index) == 236);
static_assert(offsetof(NativeMslSystemConstants, color_exp_bias) == 240);
static_assert(offsetof(NativeMslSystemConstants, edram_poly_offset_front) ==
              256);
static_assert(offsetof(NativeMslSystemConstants, edram_poly_offset_back) ==
              264);
static_assert(offsetof(NativeMslSystemConstants, edram_stencil) == 272);
static_assert(offsetof(NativeMslSystemConstants, edram_rt_base_dwords_scaled) ==
              304);
static_assert(offsetof(NativeMslSystemConstants, edram_rt_format_flags) == 320);
static_assert(offsetof(NativeMslSystemConstants, edram_rt_clamp) == 336);
static_assert(offsetof(NativeMslSystemConstants, edram_rt_keep_mask) == 400);
static_assert(offsetof(NativeMslSystemConstants, edram_rt_blend_factors_ops) ==
              432);
static_assert(offsetof(NativeMslSystemConstants, edram_blend_constant) == 448);

}  // namespace

MslShaderTranslator::MslShaderTranslator(
    ui::GraphicsProvider::GpuVendorID vendor_id, bool bindless_resources_used,
    bool edram_rov_used, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y)
    : vendor_id_(vendor_id),
      bindless_resources_used_(bindless_resources_used),
      edram_rov_used_(edram_rov_used),
      draw_resolution_scale_x_(draw_resolution_scale_x ? draw_resolution_scale_x
                                                       : UINT32_C(1)),
      draw_resolution_scale_y_(draw_resolution_scale_y ? draw_resolution_scale_y
                                                       : UINT32_C(1)) {}

MslShaderTranslator::~MslShaderTranslator() = default;

std::string MslShaderTranslator::GetEntryPointName() const {
  if (IsPointListAsMesh()) {
    return "main_point_mesh";
  }
  if (IsRectangleListAsMesh()) {
    return "main_rect_mesh";
  }
  if (IsQuadListAsMesh()) {
    return "main_quad_mesh";
  }
  return is_vertex_shader() ? "main_vs" : "main_ps";
}

MslShaderTranslator::Modification
MslShaderTranslator::GetMslShaderModification() const {
  Modification modification;
  modification.value = current_translation().modification();
  const auto* metal_translation =
      dynamic_cast<const metal::MetalShader::MetalTranslation*>(
          &current_translation());
  if (metal_translation) {
    modification.value = metal_translation->shader_modification();
  }
  return modification;
}

bool MslShaderTranslator::GetNativeTextureSignSpecialization(
    uint32_t fetch_constant, uint8_t component_mask,
    uint8_t& sign_values_out) const {
  const auto* metal_translation =
      dynamic_cast<const metal::MetalShader::MetalTranslation*>(
          &current_translation());
  if (!metal_translation || !metal_translation->native_msl_texture_sign_key() ||
      fetch_constant >= xenos::kTextureFetchConstantCount || !component_mask) {
    return false;
  }
  const auto& component_masks =
      metal_translation->native_msl_texture_sign_component_masks();
  if ((component_masks[fetch_constant] & component_mask) != component_mask) {
    return false;
  }
  sign_values_out =
      metal_translation->native_msl_texture_sign_values()[fetch_constant];
  return true;
}

uint64_t MslShaderTranslator::GetDefaultVertexShaderModification(
    uint32_t dynamic_addressable_register_count,
    Shader::HostVertexShaderType host_vertex_shader_type) const {
  Modification modification;
  modification.vertex.dynamic_addressable_register_count =
      dynamic_addressable_register_count;
  modification.vertex.host_vertex_shader_type = host_vertex_shader_type;
  modification.vertex.interpolator_mask =
      (UINT32_C(1) << xenos::kMaxInterpolators) - 1;
  return modification.value;
}

uint64_t MslShaderTranslator::GetDefaultPixelShaderModification(
    uint32_t dynamic_addressable_register_count) const {
  Modification modification;
  modification.pixel.dynamic_addressable_register_count =
      dynamic_addressable_register_count;
  modification.pixel.interpolator_mask =
      (UINT32_C(1) << xenos::kMaxInterpolators) - 1;
  modification.pixel.depth_stencil_mode =
      Modification::DepthStencilMode::kNoModifiers;
  return modification.value;
}

uint32_t MslShaderTranslator::GetModificationRegisterCount() const {
  Modification modification = GetMslShaderModification();
  return is_vertex_shader()
             ? modification.vertex.dynamic_addressable_register_count
             : modification.pixel.dynamic_addressable_register_count;
}

void MslShaderTranslator::Reset() {
  ShaderTranslator::Reset();
  msl_stream_.str("");
  msl_stream_.clear();
  msl_source_.clear();
  indent_level_ = 0;
  indent_string_.clear();
  cf_exec_predicated_ = false;
  cf_exec_predicate_condition_ = false;
  cf_exec_bool_constant_ = UINT32_MAX;
  cf_exec_bool_constant_condition_ = false;
  has_main_switch_ = false;
  control_flow_has_call_return_ = false;
  synthetic_label_addresses_.clear();
  emitted_cf_case_indices_.clear();
  texture_bindings_.clear();
  sampler_bindings_.clear();
  native_metadata_ = {};
  uses_memexport_ = false;
  uses_staged_vector_result_ = false;
  uses_staged_scalar_result_ = false;
  alu_kill_memexport_flush_mask_ = 0;
}

void MslShaderTranslator::EmitLine(const std::string& line) {
  msl_stream_ << indent_string_ << line << "\n";
}

void MslShaderTranslator::Emit(const std::string& text) { msl_stream_ << text; }

void MslShaderTranslator::EmitNativeMslUnsupported(const std::string& feature) {
  XELOGW("Metal native MSL unsupported/invalid path: {}", feature);
  EmitLine("// Native MSL unsupported/invalid path: " + feature);
  std::string message = "native MSL unsupported/invalid path: " + feature;
  EmitTranslationError(message.c_str());
}

bool MslShaderTranslator::RejectUnsupportedResultTarget(
    const char* operation, const InstructionResult& result) {
  if (IsMemExportTarget(result)) {
    if (!uses_memexport_) {
      std::string message = fmt::format(
          "native MSL {} writes {}{} but this shader has no analyzed "
          "memexport side effects",
          operation, GetMemExportTargetName(result), result.storage_index);
      EmitTranslationError(message.c_str());
    } else if (result.storage_index >= kNativeMslMemExportSlots) {
      std::string message = fmt::format(
          "native MSL {} writes {}{} outside the supported memexport slot "
          "range",
          operation, GetMemExportTargetName(result), result.storage_index);
      EmitTranslationError(message.c_str());
    }
    return true;
  }
  return false;
}

void MslShaderTranslator::EmitFetchPredicationBegin(
    bool is_predicated, bool predicate_condition, bool& predication_open_out) {
  predication_open_out = false;
  if (!is_predicated) {
    return;
  }
  EmitLine("if (xe_p0 " + std::string(predicate_condition ? "==" : "!=") +
           " true) {");
  Indent();
  predication_open_out = true;
}

void MslShaderTranslator::EmitFetchPredicationEnd(bool predication_open) {
  if (!predication_open) {
    return;
  }
  Outdent();
  EmitLine("}");
}

std::string MslShaderTranslator::TextureArgumentName(
    uint32_t texture_slot) const {
  return "xe_descriptor_indices[" + std::to_string(texture_slot) + "]";
}

std::string MslShaderTranslator::TextureArgumentName2DArray(
    uint32_t texture_slot) const {
  return "xe_texture_2d_array_heap.textures[" +
         TextureArgumentName(texture_slot) + "]";
}

std::string MslShaderTranslator::TextureArgumentName3D(
    uint32_t texture_slot) const {
  return "xe_texture_3d_heap.textures[" + TextureArgumentName(texture_slot) +
         "]";
}

std::string MslShaderTranslator::TextureArgumentNameCube(
    uint32_t texture_slot) const {
  return "xe_texture_cube_heap.textures[" + TextureArgumentName(texture_slot) +
         "]";
}

std::string MslShaderTranslator::SamplerArgumentName(
    uint32_t sampler_slot) const {
  return "xe_sampler_heap.samplers[xe_descriptor_indices[" +
         std::to_string(uint32_t(texture_bindings_.size()) + sampler_slot) +
         "]]";
}

uint32_t MslShaderTranslator::GetTextureArgumentSlotCount(
    xenos::FetchOpDimension dimension) const {
  return 1u;
}

bool MslShaderTranslator::UsesNativeSystemConstants() const {
  // Generated vertex/pixel finalization code references system constants even
  // when the guest shader itself doesn't read cbuffer b0. Keep this conservative.
  return true;
}

bool MslShaderTranslator::UsesNativeFloatConstants() const {
  return current_shader().constant_register_map().float_count != 0;
}

bool MslShaderTranslator::UsesNativeBoolLoopConstants() const {
  const Shader::ConstantRegisterMap& constant_map =
      current_shader().constant_register_map();
  if (constant_map.loop_bitmap) {
    return true;
  }
  for (uint32_t bits : constant_map.bool_bitmap) {
    if (bits) {
      return true;
    }
  }
  NativeMslHelperUsage usage = GetNativeMslHelperUsage();
  return usage.uses_bool_constant || usage.uses_loop_constant;
}

bool MslShaderTranslator::UsesNativeFetchConstants() const {
  const Shader::ConstantRegisterMap& constant_map =
      current_shader().constant_register_map();
  for (uint32_t bits : constant_map.vertex_fetch_bitmap) {
    if (bits) {
      return true;
    }
  }
  return !current_shader().vertex_bindings().empty() ||
         !current_shader().texture_bindings().empty() ||
         !texture_bindings_.empty();
}

bool MslShaderTranslator::UsesNativeSharedMemory() const {
  NativeMslHelperUsage usage = GetNativeMslHelperUsage();
  return usage.uses_shared_memory_load || usage.uses_memexport;
}

bool MslShaderTranslator::UsesNativePrimitiveIndexConstants() const {
  return GetNativeMslHelperUsage().uses_primitive_index_load;
}

bool MslShaderTranslator::UsesTextureRuntimeInfo() const {
  for (const TextureBinding& binding : texture_bindings_) {
    if (binding.dimension == xenos::FetchOpDimension::k3DOrStacked) {
      return true;
    }
  }
  return false;
}

bool MslShaderTranslator::UsesNativeDescriptorIndices() const {
  return !texture_bindings_.empty() || !sampler_bindings_.empty();
}

bool MslShaderTranslator::UsesNativeDrawConstants() const {
  return UsesNativeSystemConstants() || UsesNativeFloatConstants() ||
         UsesNativeBoolLoopConstants() || UsesNativeFetchConstants() ||
         UsesNativeDescriptorIndices() ||
         UsesNativePrimitiveIndexConstants();
}

bool MslShaderTranslator::UsesNativeTexture2DArrayHeap() const {
  for (const TextureBinding& binding : texture_bindings_) {
    switch (binding.dimension) {
      case xenos::FetchOpDimension::kCube:
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
      default:
        return true;
    }
  }
  return false;
}

bool MslShaderTranslator::UsesNativeTexture3DHeap() const {
  for (const TextureBinding& binding : texture_bindings_) {
    if (binding.dimension == xenos::FetchOpDimension::k3DOrStacked) {
      return true;
    }
  }
  return false;
}

bool MslShaderTranslator::UsesNativeTextureCubeHeap() const {
  for (const TextureBinding& binding : texture_bindings_) {
    if (binding.dimension == xenos::FetchOpDimension::kCube) {
      return true;
    }
  }
  return false;
}

bool MslShaderTranslator::CurrentShaderUsesStagedVectorResult() const {
  const std::vector<uint32_t>& ucode_dwords = current_shader().ucode_data();
  const uint32_t ucode_instruction_count = uint32_t(ucode_dwords.size() / 3u);
  auto exec_uses_staged_vector_result =
      [&](const ParsedExecInstruction& instr) {
        uint32_t sequence = instr.sequence;
        const uint32_t end =
            instr.instruction_address + instr.instruction_count;
        if (end > ucode_instruction_count) {
          return true;
        }
        for (uint32_t instr_offset = instr.instruction_address;
             instr_offset < end; ++instr_offset, sequence >>= 2) {
          if (sequence & 0b01) {
            continue;
          }
          auto& op = *reinterpret_cast<const ucode::AluInstruction*>(
              ucode_dwords.data() + instr_offset * 3u);
          ParsedAluInstruction alu_instr;
          ParseAluInstruction(op, current_shader().type(), alu_instr);
          if (alu_instr.vector_and_constant_result.GetUsedResultComponents() ||
              ucode::GetAluVectorOpcodeInfo(alu_instr.vector_opcode)
                  .changed_state) {
            return true;
          }
        }
        return false;
      };

  const uint32_t cf_pair_index_bound = current_shader().cf_pair_index_bound();
  for (uint32_t i = 0; i < cf_pair_index_bound; ++i) {
    ucode::ControlFlowInstruction cf_ab[2];
    ucode::UnpackControlFlowInstructions(ucode_dwords.data() + i * 3u, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      const uint32_t cf_index = i * 2u + j;
      const ucode::ControlFlowInstruction& cf = cf_ab[j];
      ParsedExecInstruction instr;
      switch (cf.opcode()) {
        case ucode::ControlFlowOpcode::kExec:
        case ucode::ControlFlowOpcode::kExecEnd:
          ParseControlFlowExec(cf.exec, cf_index, instr);
          break;
        case ucode::ControlFlowOpcode::kCondExec:
        case ucode::ControlFlowOpcode::kCondExecEnd:
        case ucode::ControlFlowOpcode::kCondExecPredClean:
        case ucode::ControlFlowOpcode::kCondExecPredCleanEnd:
          ParseControlFlowCondExec(cf.cond_exec, cf_index, instr);
          break;
        case ucode::ControlFlowOpcode::kCondExecPred:
        case ucode::ControlFlowOpcode::kCondExecPredEnd:
          ParseControlFlowCondExecPred(cf.cond_exec_pred, cf_index, instr);
          break;
        default:
          continue;
      }
      if (exec_uses_staged_vector_result(instr)) {
        return true;
      }
    }
  }
  return false;
}

bool MslShaderTranslator::CurrentShaderUsesStagedScalarResult() const {
  const std::vector<uint32_t>& ucode_dwords = current_shader().ucode_data();
  const uint32_t ucode_instruction_count = uint32_t(ucode_dwords.size() / 3u);
  auto exec_uses_staged_scalar_result =
      [&](const ParsedExecInstruction& instr) {
        uint32_t sequence = instr.sequence;
        const uint32_t end =
            instr.instruction_address + instr.instruction_count;
        if (end > ucode_instruction_count) {
          return true;
        }
        for (uint32_t instr_offset = instr.instruction_address;
             instr_offset < end; ++instr_offset, sequence >>= 2) {
          if (sequence & 0b01) {
            continue;
          }
          auto& op = *reinterpret_cast<const ucode::AluInstruction*>(
              ucode_dwords.data() + instr_offset * 3u);
          if (op.scalar_opcode() != ucode::AluScalarOpcode::kRetainPrev) {
            return true;
          }
        }
        return false;
      };

  const uint32_t cf_pair_index_bound = current_shader().cf_pair_index_bound();
  for (uint32_t i = 0; i < cf_pair_index_bound; ++i) {
    ucode::ControlFlowInstruction cf_ab[2];
    ucode::UnpackControlFlowInstructions(ucode_dwords.data() + i * 3u, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      const uint32_t cf_index = i * 2u + j;
      const ucode::ControlFlowInstruction& cf = cf_ab[j];
      ParsedExecInstruction instr;
      switch (cf.opcode()) {
        case ucode::ControlFlowOpcode::kExec:
        case ucode::ControlFlowOpcode::kExecEnd:
          ParseControlFlowExec(cf.exec, cf_index, instr);
          break;
        case ucode::ControlFlowOpcode::kCondExec:
        case ucode::ControlFlowOpcode::kCondExecEnd:
        case ucode::ControlFlowOpcode::kCondExecPredClean:
        case ucode::ControlFlowOpcode::kCondExecPredCleanEnd:
          ParseControlFlowCondExec(cf.cond_exec, cf_index, instr);
          break;
        case ucode::ControlFlowOpcode::kCondExecPred:
        case ucode::ControlFlowOpcode::kCondExecPredEnd:
          ParseControlFlowCondExecPred(cf.cond_exec_pred, cf_index, instr);
          break;
        default:
          continue;
      }
      if (exec_uses_staged_scalar_result(instr)) {
        return true;
      }
    }
  }
  return false;
}

bool MslShaderTranslator::UseScalarGprLocals() const {
  return register_count() != 0 &&
         !current_shader().uses_register_dynamic_addressing();
}

MslShaderTranslator::NativeMslHelperUsage
MslShaderTranslator::GetNativeMslHelperUsage() const {
  NativeMslHelperUsage usage = {};
  const Modification modification = GetMslShaderModification();
  auto mark_saturate_width = [&usage](uint32_t component_count) {
    usage.uses_saturate = true;
    usage.saturate_widths |= NativeMslFloatWidthBit(component_count);
  };
  auto mark_saturate_no_nan_width = [&usage](uint32_t component_count) {
    usage.uses_saturate_no_nan = true;
    usage.saturate_no_nan_widths |= NativeMslFloatWidthBit(component_count);
  };
  auto mark_texture_pwl_gamma_widths =
      [&usage, &mark_saturate_no_nan_width](uint8_t widths) {
        if (!widths) {
          return;
        }
        usage.uses_texture_pwl_gamma = true;
        usage.texture_pwl_gamma_widths |= widths;
        for (uint32_t component_count = 1; component_count <= 4;
             ++component_count) {
          if (widths & NativeMslFloatWidthBit(component_count)) {
            mark_saturate_no_nan_width(component_count);
          }
        }
      };

  usage.uses_memexport = CurrentShaderUsesMemExport();
  usage.uses_vertex_position_inf_to_nan = is_vertex_shader();
  usage.uses_point_size_clamp = VertexShaderEmitsPointSizeOutput();
  usage.uses_endian_swap = is_vertex_shader();

  const bool pixel_param_gen =
      is_pixel_shader() && modification.pixel.param_gen_enable &&
      modification.pixel.param_gen_interpolator < register_count();
  usage.uses_set_float_sign_bit = pixel_param_gen;
  if (pixel_param_gen && modification.pixel.param_gen_point) {
    mark_saturate_width(2);
  }
  usage.uses_depth_float24 = PixelShaderNeedsFloat24DepthOutput();
  usage.uses_color_pwl_gamma =
      is_pixel_shader() && current_shader().writes_color_targets() != 0;
  if (usage.uses_depth_float24 || PixelShaderNeedsCoverageOutput()) {
    mark_saturate_no_nan_width(1);
  }
  if (usage.uses_color_pwl_gamma) {
    mark_saturate_no_nan_width(3);
  }
  if (current_shader().writes_depth()) {
    // Depth writes can be produced by vector or scalar instructions. Keep both
    // overloads available unless the instruction scan below proves the exact
    // producer shape.
    mark_saturate_no_nan_width(1);
    mark_saturate_no_nan_width(4);
  }
  usage.uses_first_bit_low = usage.uses_memexport && is_pixel_shader() &&
                             current_shader().memexport_eM_written() &&
                             IsPixelShaderSampleRate();

  auto scan_texture_fetch_helper_usage =
      [&usage](const ParsedTextureFetchInstruction& fetch) {
        using FetchOpcode = ucode::FetchOpcode;
        const bool needs_texture_sampler =
            fetch.opcode == FetchOpcode::kTextureFetch ||
            fetch.opcode == FetchOpcode::kGetTextureComputedLod;
        if (needs_texture_sampler &&
            fetch.dimension == xenos::FetchOpDimension::k1D) {
          usage.uses_frac = true;
        }
        if (fetch.opcode == FetchOpcode::kGetTextureWeights) {
          usage.uses_texture_weights = true;
          usage.uses_frac = true;
        }
      };

  const bool point_list_as_triangle_strip =
      is_vertex_shader() &&
      modification.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip;
  usage.uses_primitive_index_load =
      is_vertex_shader() &&
      (point_list_as_triangle_strip || IsRectangleListAsTriangleStrip() ||
       IsPointListAsMesh() || IsRectangleListAsMesh() || IsQuadListAsMesh() ||
       IsDomainShader());

  for (const Shader::TextureBinding& binding :
       current_shader().texture_bindings()) {
    const ParsedTextureFetchInstruction& fetch = binding.fetch_instr;
    scan_texture_fetch_helper_usage(fetch);
    const bool texture_fetch =
        fetch.opcode == ucode::FetchOpcode::kTextureFetch;
    usage.uses_texture_fetch = true;
    usage.uses_cube_texture |=
        fetch.dimension == xenos::FetchOpDimension::kCube;
    usage.uses_texture_weights |=
        fetch.opcode == ucode::FetchOpcode::kGetTextureWeights;
    if (fetch.result.GetUsedWriteMask() &&
        ResultNeedsSaturation(fetch.result)) {
      mark_saturate_no_nan_width(4);
    }
    if (texture_fetch) {
      const uint8_t texture_sign_component_mask =
          uint8_t(fetch.result.GetUsedResultComponents() &
                  fetch.GetNonZeroResultComponents());
      if (texture_sign_component_mask) {
        uint8_t specialized_texture_signs = 0;
        if (GetNativeTextureSignSpecialization(binding.fetch_constant,
                                               texture_sign_component_mask,
                                               specialized_texture_signs)) {
          mark_texture_pwl_gamma_widths(NativeMslTexturePwlGammaWidths(
              specialized_texture_signs, texture_sign_component_mask));
        } else {
          usage.uses_texture_sign_helpers = true;
          mark_texture_pwl_gamma_widths(kNativeMslFloat1Width);
        }
      }
    }
  }
  usage.uses_frac |= usage.uses_texture_weights;
  usage.uses_sign_extend |= usage.uses_texture_fetch;

  auto mark_conservative_exec_usage = [&usage]() {
    usage.uses_finite_helper = true;
    usage.uses_saturate_no_nan = true;
    usage.saturate_no_nan_widths |= kNativeMslAllFloatWidths;
    usage.uses_frac = true;
    usage.uses_address_index_round = true;
    usage.uses_address_index_floor = true;
    usage.uses_mul_sm3 = true;
    usage.uses_clamp_inf_to_max = true;
    usage.uses_flush_inf_to_signed_zero = true;
    usage.uses_logc = true;
    usage.uses_rcpc = true;
    usage.uses_rcpf = true;
    usage.uses_rsqc = true;
    usage.uses_rsqf = true;
    usage.uses_vertex_fetch = true;
    usage.uses_sign_extend = true;
  };

  auto scan_alu = [this, &usage, &mark_saturate_no_nan_width](
                      const ParsedAluInstruction& instr) {
    auto mark_saturate_if_needed = [this, &mark_saturate_no_nan_width](
                                       const InstructionResult& result,
                                       uint32_t component_count) {
      if (result.GetUsedWriteMask() && ResultNeedsSaturation(result)) {
        mark_saturate_no_nan_width(component_count);
      }
    };
    mark_saturate_if_needed(instr.vector_and_constant_result, 4);
    mark_saturate_if_needed(instr.scalar_result, 1);

    using AluVectorOpcode = ucode::AluVectorOpcode;
    switch (instr.vector_opcode) {
      case AluVectorOpcode::kMul:
      case AluVectorOpcode::kMad:
      case AluVectorOpcode::kDp4:
      case AluVectorOpcode::kDp3:
      case AluVectorOpcode::kDp2Add:
      case AluVectorOpcode::kDst:
        usage.uses_mul_sm3 = true;
        break;
      case AluVectorOpcode::kFrc:
        usage.uses_frac = true;
        break;
      case AluVectorOpcode::kMaxA:
        usage.uses_address_index_round = true;
        break;
      default:
        break;
    }

    using AluScalarOpcode = ucode::AluScalarOpcode;
    switch (instr.scalar_opcode) {
      case AluScalarOpcode::kMuls:
      case AluScalarOpcode::kMulsPrev:
      case AluScalarOpcode::kMulsPrev2:
      case AluScalarOpcode::kMulsc0:
      case AluScalarOpcode::kMulsc1:
        usage.uses_mul_sm3 = true;
        break;
      default:
        break;
    }
    switch (instr.scalar_opcode) {
      case AluScalarOpcode::kMulsPrev2:
        usage.uses_finite_helper = true;
        break;
      case AluScalarOpcode::kFrcs:
        usage.uses_frac = true;
        break;
      case AluScalarOpcode::kLogc:
        usage.uses_logc = true;
        break;
      case AluScalarOpcode::kRcpc:
        usage.uses_rcpc = true;
        usage.uses_clamp_inf_to_max = true;
        break;
      case AluScalarOpcode::kRcpf:
        usage.uses_rcpf = true;
        usage.uses_flush_inf_to_signed_zero = true;
        break;
      case AluScalarOpcode::kRsqc:
        usage.uses_rsqc = true;
        usage.uses_clamp_inf_to_max = true;
        break;
      case AluScalarOpcode::kRsqf:
        usage.uses_rsqf = true;
        usage.uses_flush_inf_to_signed_zero = true;
        break;
      case AluScalarOpcode::kMaxAs:
        usage.uses_address_index_round = true;
        break;
      case AluScalarOpcode::kMaxAsf:
        usage.uses_address_index_floor = true;
        break;
      default:
        break;
    }
  };

  const std::vector<uint32_t>& ucode_dwords = current_shader().ucode_data();
  const uint32_t ucode_instruction_count = uint32_t(ucode_dwords.size() / 3u);
  auto scan_exec = [&](const ParsedExecInstruction& instr) {
    uint32_t sequence = instr.sequence;
    const uint32_t end = instr.instruction_address + instr.instruction_count;
    if (end > ucode_instruction_count) {
      mark_conservative_exec_usage();
      return;
    }
    for (uint32_t instr_offset = instr.instruction_address; instr_offset < end;
         ++instr_offset, sequence >>= 2) {
      const uint32_t* op_ptr = ucode_dwords.data() + instr_offset * 3u;
      if (sequence & 0b01) {
        auto& op = *reinterpret_cast<const ucode::FetchInstruction*>(op_ptr);
        if (op.opcode() == ucode::FetchOpcode::kVertexFetch) {
          usage.uses_vertex_fetch = true;
        } else {
          ParsedTextureFetchInstruction texture_instr;
          ParseTextureFetchInstruction(op.texture_fetch(), texture_instr);
          scan_texture_fetch_helper_usage(texture_instr);
        }
        continue;
      }
      auto& op = *reinterpret_cast<const ucode::AluInstruction*>(op_ptr);
      ParsedAluInstruction alu_instr;
      ParseAluInstruction(op, current_shader().type(), alu_instr);
      scan_alu(alu_instr);
    }
  };

  const uint32_t cf_pair_index_bound = current_shader().cf_pair_index_bound();
  for (uint32_t i = 0; i < cf_pair_index_bound; ++i) {
    ucode::ControlFlowInstruction cf_ab[2];
    ucode::UnpackControlFlowInstructions(ucode_dwords.data() + i * 3u, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      const uint32_t cf_index = i * 2u + j;
      const ucode::ControlFlowInstruction& cf = cf_ab[j];
      ParsedExecInstruction exec_instr;
      switch (cf.opcode()) {
        case ucode::ControlFlowOpcode::kExec:
        case ucode::ControlFlowOpcode::kExecEnd:
          ParseControlFlowExec(cf.exec, cf_index, exec_instr);
          scan_exec(exec_instr);
          break;
        case ucode::ControlFlowOpcode::kCondExec:
        case ucode::ControlFlowOpcode::kCondExecEnd:
        case ucode::ControlFlowOpcode::kCondExecPredClean:
        case ucode::ControlFlowOpcode::kCondExecPredCleanEnd:
          usage.uses_bool_constant = true;
          ParseControlFlowCondExec(cf.cond_exec, cf_index, exec_instr);
          scan_exec(exec_instr);
          break;
        case ucode::ControlFlowOpcode::kCondExecPred:
        case ucode::ControlFlowOpcode::kCondExecPredEnd:
          ParseControlFlowCondExecPred(cf.cond_exec_pred, cf_index, exec_instr);
          scan_exec(exec_instr);
          break;
        case ucode::ControlFlowOpcode::kLoopStart:
        case ucode::ControlFlowOpcode::kLoopEnd:
          usage.uses_loop_constant = true;
          break;
        case ucode::ControlFlowOpcode::kCondCall:
          if (!cf.cond_call.is_unconditional() &&
              !cf.cond_call.is_predicated()) {
            usage.uses_bool_constant = true;
          }
          break;
        case ucode::ControlFlowOpcode::kCondJmp:
          if (!cf.cond_jmp.is_unconditional() && !cf.cond_jmp.is_predicated()) {
            usage.uses_bool_constant = true;
          }
          break;
        default:
          break;
      }
    }
  }

  usage.uses_vertex_fetch |= !current_shader().vertex_bindings().empty();
  for (const Shader::VertexBinding& binding :
       current_shader().vertex_bindings()) {
    for (const Shader::VertexBinding::Attribute& attribute :
         binding.attributes) {
      usage.uses_sign_extend |=
          NativeMslVertexFetchNeedsSignExtend(attribute.fetch_instr);
    }
  }
  usage.uses_shared_memory_load =
      usage.uses_vertex_fetch || usage.uses_primitive_index_load;
  usage.uses_vertex_fetch_unpack = usage.uses_vertex_fetch;
  usage.uses_nan_helpers =
      usage.uses_saturate || usage.uses_saturate_no_nan ||
      usage.uses_address_index_round || usage.uses_address_index_floor ||
      usage.uses_memexport || IsRectangleListAsTriangleStrip() ||
      IsPrimitiveListAsMesh() || IsDomainShader();
  return usage;
}

std::string MslShaderTranslator::TextureRuntimeInfoExpression(
    uint32_t texture_slot) const {
  return "xe_texture_runtime_info[" + std::to_string(texture_slot) + "]";
}

std::string MslShaderTranslator::TextureSizeExpression(
    uint32_t texture_slot, xenos::FetchOpDimension dimension) const {
  const std::string info = TextureRuntimeInfoExpression(texture_slot);
  switch (dimension) {
    case xenos::FetchOpDimension::k1D:
    case xenos::FetchOpDimension::k2D: {
      const std::string t = TextureArgumentName2DArray(texture_slot);
      return "float3(float(" + t + ".get_width()), float(" + t +
             ".get_height()), float(max(" + t + ".get_array_size(), 1u)))";
    }
    case xenos::FetchOpDimension::k3DOrStacked: {
      const std::string t2 = TextureArgumentName2DArray(texture_slot);
      const std::string t3 = TextureArgumentName3D(texture_slot);
      return "((" + info +
             ".x == " + std::to_string(kNativeTextureRuntimeType3D) +
             "u) ? float3(float(" + t3 + ".get_width()), float(" + t3 +
             ".get_height()), float(" + t3 + ".get_depth())) : float3(float(" +
             t2 + ".get_width()), float(" + t2 + ".get_height()), float(max(" +
             t2 + ".get_array_size(), 1u))))";
    }
    case xenos::FetchOpDimension::kCube: {
      const std::string t = TextureArgumentNameCube(texture_slot);
      return "float3(float(" + t + ".get_width()), float(" + t +
             ".get_width()), 1.0f)";
    }
    default:
      return "float3(1.0f)";
  }
}

uint8_t MslShaderTranslator::GetTextureFetchWordMask(
    const ParsedTextureFetchInstruction& instr) const {
  using FetchOpcode = ucode::FetchOpcode;
  const bool get_texture_weights =
      instr.opcode == FetchOpcode::kGetTextureWeights;
  uint32_t used_result_nonzero_components = instr.GetNonZeroResultComponents();
  if (get_texture_weights) {
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
        used_result_nonzero_components &= 0b0001;
        break;
      case xenos::FetchOpDimension::k2D:
      case xenos::FetchOpDimension::kCube:
        used_result_nonzero_components &= 0b0011;
        break;
      case xenos::FetchOpDimension::k3DOrStacked: {
        used_result_nonzero_components &= 0b0111;
        break;
      }
    }
  }
  if (!used_result_nonzero_components ||
      instr.opcode == FetchOpcode::kGetTextureBorderColorFrac ||
      instr.opcode == FetchOpcode::kGetTextureGradients) {
    return 0;
  }

  const bool texture_fetch = instr.opcode == FetchOpcode::kTextureFetch;
  const bool needs_texture_sampler =
      texture_fetch || instr.opcode == FetchOpcode::kGetTextureComputedLod;
  const bool dimension_1d = instr.dimension == xenos::FetchOpDimension::k1D;
  const bool dimension_2d = instr.dimension == xenos::FetchOpDimension::k2D;
  const bool dimension_3d_or_stacked =
      instr.dimension == xenos::FetchOpDimension::k3DOrStacked;
  const bool dimension_cube = instr.dimension == xenos::FetchOpDimension::kCube;
  const bool needs_uv = needs_texture_sampler && (dimension_1d || dimension_2d);
  const bool needs_1d_row_remap = needs_uv && dimension_1d;
  const bool needs_width =
      dimension_1d &&
      (needs_texture_sampler || !instr.attributes.unnormalized_coordinates);
  const bool needs_size2 =
      (dimension_1d && needs_uv) ||
      ((dimension_2d || dimension_cube) &&
       (needs_texture_sampler || !instr.attributes.unnormalized_coordinates));
  const bool needs_size3 =
      dimension_3d_or_stacked &&
      (needs_texture_sampler || !instr.attributes.unnormalized_coordinates);
  const bool needs_is_3d = needs_size3;
  const bool needs_fetch_word2 = needs_width || needs_size2 || needs_size3;
  const bool needs_fetch_word5 =
      (needs_is_3d && !needs_texture_sampler) || needs_1d_row_remap;

  uint8_t mask = 0;
  if (needs_fetch_word2) {
    mask |= uint8_t(1u << 2);
  }
  if (texture_fetch) {
    mask |= uint8_t((1u << 3) | (1u << 4));
  }
  if (needs_fetch_word5) {
    mask |= uint8_t(1u << 5);
  }
  return mask;
}

uint32_t MslShaderTranslator::GetTextureFetchDerivedMask(
    const ParsedTextureFetchInstruction& instr) const {
  const uint8_t word_mask = GetTextureFetchWordMask(instr);
  if (!word_mask) {
    return 0;
  }

  using FetchOpcode = ucode::FetchOpcode;
  const bool texture_fetch = instr.opcode == FetchOpcode::kTextureFetch;
  const bool needs_texture_sampler =
      texture_fetch || instr.opcode == FetchOpcode::kGetTextureComputedLod;
  const bool dimension_1d = instr.dimension == xenos::FetchOpDimension::k1D;
  const bool dimension_2d = instr.dimension == xenos::FetchOpDimension::k2D;
  const bool dimension_3d_or_stacked =
      instr.dimension == xenos::FetchOpDimension::k3DOrStacked;
  const bool dimension_cube = instr.dimension == xenos::FetchOpDimension::kCube;
  const bool needs_uv = needs_texture_sampler && (dimension_1d || dimension_2d);
  const bool needs_width =
      dimension_1d &&
      (needs_texture_sampler || !instr.attributes.unnormalized_coordinates);
  const bool needs_size2 =
      (dimension_1d && needs_uv) ||
      ((dimension_2d || dimension_cube) &&
       (needs_texture_sampler || !instr.attributes.unnormalized_coordinates));
  const bool needs_size3 =
      dimension_3d_or_stacked &&
      (needs_texture_sampler || !instr.attributes.unnormalized_coordinates);

  uint32_t mask = 0;
  if (word_mask & (1u << 5)) {
    mask |= kTextureFetchDerivedDimension;
  }
  if (needs_width) {
    mask |= kTextureFetchDerivedWidth1D;
  }
  if (needs_size2 && !dimension_1d) {
    mask |= kTextureFetchDerivedSize2D;
  }
  if (needs_size3) {
    mask |= kTextureFetchDerivedSize3D | kTextureFetchDerivedSizeStacked;
  }
  if (texture_fetch) {
    mask |= kTextureFetchDerivedLodBias | kTextureFetchDerivedExpAdjust;
  }
  return mask;
}

std::string MslShaderTranslator::TextureFetchWordName(uint32_t fetch_constant,
                                                      uint32_t word) const {
  return "xe_tfc_" + std::to_string(fetch_constant) + "_word" +
         std::to_string(word);
}

std::string MslShaderTranslator::TextureFetchDerivedName(
    uint32_t fetch_constant, const char* name) const {
  return "xe_tfc_" + std::to_string(fetch_constant) + "_" + name;
}

void MslShaderTranslator::EmitTextureFetchWordCache() {
  uint8_t fetch_word_masks[xenos::kTextureFetchConstantCount] = {};
  for (const Shader::TextureBinding& binding :
       current_shader().texture_bindings()) {
    if (binding.fetch_constant >= xenos::kTextureFetchConstantCount) {
      continue;
    }
    fetch_word_masks[binding.fetch_constant] |=
        GetTextureFetchWordMask(binding.fetch_instr);
  }

  bool emitted = false;
  for (uint32_t fetch_constant = 0;
       fetch_constant < xenos::kTextureFetchConstantCount; ++fetch_constant) {
    const uint8_t word_mask = fetch_word_masks[fetch_constant];
    for (uint32_t word = 2; word <= 5; ++word) {
      if ((word_mask & (1u << word)) == 0) {
        continue;
      }
      EmitLine("uint " + TextureFetchWordName(fetch_constant, word) +
               " = XeGetTextureFetchConstantWord(xe_fetch_constants_data, " +
               std::to_string(fetch_constant) + "u, " + std::to_string(word) +
               "u);");
      emitted = true;
    }
  }
  if (emitted) {
    EmitLine("");
  }
}

void MslShaderTranslator::EmitTextureFetchDerivedConstantCache() {
  uint32_t derived_masks[xenos::kTextureFetchConstantCount] = {};
  for (const Shader::TextureBinding& binding :
       current_shader().texture_bindings()) {
    if (binding.fetch_constant >= xenos::kTextureFetchConstantCount) {
      continue;
    }
    derived_masks[binding.fetch_constant] |=
        GetTextureFetchDerivedMask(binding.fetch_instr);
  }

  bool emitted = false;
  for (uint32_t fetch_constant = 0;
       fetch_constant < xenos::kTextureFetchConstantCount; ++fetch_constant) {
    const uint32_t mask = derived_masks[fetch_constant];
    if (!mask) {
      continue;
    }
    const std::string word2 = TextureFetchWordName(fetch_constant, 2u);
    const std::string word3 = TextureFetchWordName(fetch_constant, 3u);
    const std::string word4 = TextureFetchWordName(fetch_constant, 4u);
    const std::string word5 = TextureFetchWordName(fetch_constant, 5u);
    auto emit_if = [&](uint32_t bit, const std::string& line) {
      if ((mask & bit) == 0) {
        return;
      }
      EmitLine(line);
      emitted = true;
    };
    emit_if(kTextureFetchDerivedDimension,
            "uint " + TextureFetchDerivedName(fetch_constant, "dimension") +
                " = ((" + word5 + " >> 9u) & 3u);");
    emit_if(kTextureFetchDerivedWidth1D,
            "float " + TextureFetchDerivedName(fetch_constant, "width_1d") +
                " = float((" + word2 + " & 0xFFFFFFu) + 1u);");
    emit_if(kTextureFetchDerivedSize2D,
            "float2 " + TextureFetchDerivedName(fetch_constant, "size_2d") +
                " = float2(float((" + word2 + " & 0x1FFFu) + 1u), float(((" +
                word2 + " >> 13u) & 0x1FFFu) + 1u));");
    emit_if(kTextureFetchDerivedSize3D,
            "float3 " + TextureFetchDerivedName(fetch_constant, "size_3d") +
                " = float3(float((" + word2 + " & 0x7FFu) + 1u), float(((" +
                word2 + " >> 11u) & 0x7FFu) + 1u), float(((" + word2 +
                " >> 22u) & 0x3FFu) + 1u));");
    emit_if(kTextureFetchDerivedSizeStacked,
            "float3 " +
                TextureFetchDerivedName(fetch_constant, "size_stacked") +
                " = float3(float((" + word2 + " & 0x1FFFu) + 1u), float(((" +
                word2 + " >> 13u) & 0x1FFFu) + 1u), float(((" + word2 +
                " >> 26u) & 0x3Fu) + 1u));");
    emit_if(kTextureFetchDerivedLodBias,
            "float " + TextureFetchDerivedName(fetch_constant, "lod_bias") +
                " = (float(XeSignExtend((" + word4 +
                " >> 12u) & 0x3FFu, 10u)) * (1.0f / 32.0f));");
    emit_if(kTextureFetchDerivedExpAdjust,
            "float " + TextureFetchDerivedName(fetch_constant, "exp_adjust") +
                " = exp2(float(XeSignExtend((" + word3 +
                " >> 13u) & 0x3Fu, 6u)));");
  }
  if (emitted) {
    EmitLine("");
  }
}

void MslShaderTranslator::Indent() {
  ++indent_level_;
  indent_string_ = std::string(indent_level_ * 2, ' ');
}

void MslShaderTranslator::Outdent() {
  if (indent_level_) {
    --indent_level_;
  }
  indent_string_ = std::string(indent_level_ * 2, ' ');
}

void MslShaderTranslator::PreProcessControlFlowInstructions(
    std::vector<ucode::ControlFlowInstruction> instrs) {
  ShaderTranslator::PreProcessControlFlowInstructions(instrs);
  synthetic_label_addresses_.clear();
  control_flow_has_call_return_ = false;
  for (uint32_t cf_index = 0; cf_index < uint32_t(instrs.size()); ++cf_index) {
    const ucode::ControlFlowInstruction& cf = instrs[cf_index];
    if (cf.opcode() == ucode::ControlFlowOpcode::kCondCall) {
      control_flow_has_call_return_ = true;
      synthetic_label_addresses_.insert(cf.cond_call.address());
      // The return target of a call is the next control-flow slot. It may not
      // be a branch label in the analyzed shader, but a switch case must exist
      // for the call stack to resume execution there.
      if (cf_index + 1u < uint32_t(instrs.size())) {
        synthetic_label_addresses_.insert(cf_index + 1u);
      }
    } else if (cf.opcode() == ucode::ControlFlowOpcode::kReturn) {
      control_flow_has_call_return_ = true;
    }
  }

  const bool needs_main_switch = !current_shader().label_addresses().empty() ||
                                 !synthetic_label_addresses_.empty() ||
                                 control_flow_has_call_return_;
  const int32_t max_control_flow_cases =
      cvars::metal_native_msl_max_control_flow_cases;
  if (needs_main_switch && max_control_flow_cases > 0) {
    std::set<uint32_t> control_flow_cases = current_shader().label_addresses();
    control_flow_cases.insert(0u);
    control_flow_cases.insert(synthetic_label_addresses_.begin(),
                              synthetic_label_addresses_.end());
    if (control_flow_cases.size() > uint32_t(max_control_flow_cases)) {
      EmitNativeMslUnsupported(
          fmt::format("flattened control flow has {} switch cases, exceeding "
                      "metal_native_msl_max_control_flow_cases={}",
                      control_flow_cases.size(), max_control_flow_cases));
    }
  }
}

void MslShaderTranslator::EmitControlFlowCase(uint32_t cf_index) {
  if (!has_main_switch_ || emitted_cf_case_indices_.count(cf_index)) {
    return;
  }
  if (!emitted_cf_case_indices_.empty()) {
    CloseExecConditionals();
    EmitLine("xe_pc = " + std::to_string(cf_index) + "u;");
    EmitLine("continue;");
    Outdent();
  }
  EmitLine("case " + std::to_string(cf_index) + "u:");
  Indent();
  emitted_cf_case_indices_.insert(cf_index);
}

void MslShaderTranslator::ProcessControlFlowInstructionBegin(
    uint32_t cf_index) {
  if (synthetic_label_addresses_.count(cf_index) != 0) {
    EmitControlFlowCase(cf_index);
  }
}

uint32_t MslShaderTranslator::FindOrAddTextureBinding(
    uint32_t fetch_constant, xenos::FetchOpDimension dimension,
    bool is_signed) {
  for (uint32_t i = 0; i < uint32_t(texture_bindings_.size()); ++i) {
    const TextureBinding& binding = texture_bindings_[i];
    if (binding.fetch_constant == fetch_constant &&
        binding.dimension == dimension && binding.is_signed == is_signed) {
      return i;
    }
  }
  uint32_t index = uint32_t(texture_bindings_.size());
  TextureBinding& binding = texture_bindings_.emplace_back();
  binding.bindful_srv_index = index;
  binding.bindless_descriptor_index = index;
  binding.texture_runtime_info_index = index;
  binding.fetch_constant = fetch_constant;
  binding.dimension = dimension;
  binding.is_signed = is_signed;
  return index;
}

uint32_t MslShaderTranslator::FindOrAddSamplerBinding(
    uint32_t fetch_constant, xenos::TextureFilter mag_filter,
    xenos::TextureFilter min_filter, xenos::TextureFilter mip_filter,
    xenos::AnisoFilter aniso_filter) {
  if (aniso_filter != xenos::AnisoFilter::kDisabled &&
      aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    mag_filter = xenos::TextureFilter::kLinear;
    min_filter = xenos::TextureFilter::kLinear;
    mip_filter = xenos::TextureFilter::kLinear;
    aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  }
  for (uint32_t i = 0; i < uint32_t(sampler_bindings_.size()); ++i) {
    const SamplerBinding& binding = sampler_bindings_[i];
    if (binding.fetch_constant == fetch_constant &&
        binding.mag_filter == mag_filter && binding.min_filter == min_filter &&
        binding.mip_filter == mip_filter &&
        binding.aniso_filter == aniso_filter) {
      return i;
    }
  }
  uint32_t index = uint32_t(sampler_bindings_.size());
  SamplerBinding& binding = sampler_bindings_.emplace_back();
  binding.bindful_sampler_index = index;
  // Reuse this metadata field as the descriptor-index word for native MSL.
  binding.bindless_descriptor_index = index;
  binding.fetch_constant = fetch_constant;
  binding.mag_filter = mag_filter;
  binding.min_filter = min_filter;
  binding.mip_filter = mip_filter;
  binding.aniso_filter = aniso_filter;
  return index;
}

bool MslShaderTranslator::CurrentShaderUsesMemExport() const {
  return current_shader().memexport_eM_written() != 0 ||
         current_shader().memexport_eM_potentially_written_before_end() != 0;
}

bool MslShaderTranslator::CollectStaticResourceBindings() {
  texture_bindings_.clear();
  sampler_bindings_.clear();
  texture_sign_component_masks_.fill(0);
  uses_memexport_ = CurrentShaderUsesMemExport();

  if (uses_memexport_) {
    uint32_t used_memexport_slots =
        uint32_t(current_shader().memexport_eM_written()) |
        uint32_t(
            current_shader().memexport_eM_potentially_written_before_end());
    if (used_memexport_slots >> kNativeMslMemExportSlots) {
      EmitNativeMslUnsupported(fmt::format(
          "memexport eM mask 0x{:02X} exceeds native MSL slot capacity {}",
          used_memexport_slots, kNativeMslMemExportSlots));
      return false;
    }
  }

  if (is_vertex_shader()) {
    const Modification& modification = GetMslShaderModification();
    const bool helper_can_cull_distances = IsPrimitiveListAsMesh();
    Shader::HostVertexShaderType host_vertex_shader_type =
        modification.vertex.host_vertex_shader_type;
    if (host_vertex_shader_type ==
        Shader::HostVertexShaderType::kMemExportCompute) {
      EmitNativeMslUnsupported(
          "native MSL memexport compute host vertex shaders are not "
          "implemented");
      return false;
    }
    if (modification.vertex.user_clip_plane_cull &&
        !helper_can_cull_distances) {
      EmitNativeMslUnsupported(
          "native MSL cannot emit true Xenos user cull distances");
      return false;
    }
    if (modification.vertex.vertex_kill_and && !helper_can_cull_distances) {
      EmitNativeMslUnsupported(
          "native MSL cannot emit true Xenos vertex-kill cull distance");
      return false;
    }
    if (VertexShaderCullDistanceCount() && !helper_can_cull_distances) {
      EmitNativeMslUnsupported(
          "SV_CullDistance has no native Metal output equivalent");
      return false;
    }
  }

  for (const Shader::TextureBinding& shader_binding :
       current_shader().texture_bindings()) {
    const ParsedTextureFetchInstruction& fetch = shader_binding.fetch_instr;
    const uint32_t fetch_constant = shader_binding.fetch_constant;

    if (fetch_constant >= 32u) {
      EmitNativeMslUnsupported(
          fmt::format("texture fetch constant {} outside Xenos texture range",
                      fetch_constant));
      return false;
    }
    const bool needs_texture_sampler =
        fetch.opcode == ucode::FetchOpcode::kTextureFetch ||
        fetch.opcode == ucode::FetchOpcode::kGetTextureComputedLod;
    if (!needs_texture_sampler) {
      continue;
    }
    const bool texture_fetch =
        fetch.opcode == ucode::FetchOpcode::kTextureFetch;
    const uint8_t texture_sign_component_mask =
        texture_fetch ? uint8_t(fetch.result.GetUsedResultComponents() &
                                fetch.GetNonZeroResultComponents())
                      : uint8_t(0);
    uint8_t specialized_texture_signs = 0;
    const bool texture_signs_specialized =
        texture_fetch && GetNativeTextureSignSpecialization(
                             fetch_constant, texture_sign_component_mask,
                             specialized_texture_signs);
    if (texture_fetch) {
      texture_sign_component_masks_[fetch_constant] |=
          texture_sign_component_mask;
    }
    const bool use_computed_lod =
        fetch.attributes.use_computed_lod &&
        (is_pixel_shader() || fetch.attributes.use_register_gradients);
    const bool use_sample_level = !use_computed_lod;
    const xenos::TextureFilter sampler_mip_filter =
        fetch.opcode == ucode::FetchOpcode::kGetTextureComputedLod
            ? xenos::TextureFilter::kLinear
            : fetch.attributes.mip_filter;
    const xenos::AnisoFilter sampler_aniso_filter =
        use_sample_level ? xenos::AnisoFilter::kDisabled
                         : fetch.attributes.aniso_filter;

    uint32_t texture_slot = UINT32_MAX;
    uint32_t texture_slot_signed = UINT32_MAX;
    if (!texture_fetch || !texture_signs_specialized ||
        NativeMslTextureSignsNeedUnsigned(specialized_texture_signs,
                                          texture_sign_component_mask)) {
      texture_slot =
          FindOrAddTextureBinding(fetch_constant, fetch.dimension, false);
    }
    if (texture_fetch &&
        (!texture_signs_specialized ||
         NativeMslTextureSignsNeedSigned(specialized_texture_signs,
                                         texture_sign_component_mask))) {
      texture_slot_signed =
          FindOrAddTextureBinding(fetch_constant, fetch.dimension, true);
    }
    const uint32_t sampler_slot = FindOrAddSamplerBinding(
        fetch_constant, fetch.attributes.mag_filter,
        fetch.attributes.min_filter, sampler_mip_filter, sampler_aniso_filter);

    if ((texture_slot != UINT32_MAX &&
         texture_slot >= kNativeMaxTextureBindings) ||
        (texture_slot_signed != UINT32_MAX &&
         texture_slot_signed >= kNativeMaxTextureBindings) ||
        sampler_slot >= kNativeMaxSamplerBindings) {
      EmitNativeMslUnsupported(
          fmt::format("shader uses more than {} native texture bindings or "
                      "{} native sampler bindings",
                      kNativeMaxTextureBindings, kNativeMaxSamplerBindings));
      return false;
    }
  }

  return true;
}

ShaderTranslationMetadata MslShaderTranslator::BuildNativeMetadata() const {
  ShaderTranslationMetadata metadata = {};
  metadata.texture_bindings.clear();
  metadata.texture_bindings.reserve(texture_bindings_.size());
  metadata.used_texture_mask = 0;
  metadata.texture_sign_component_masks = texture_sign_component_masks_;
  auto mark_fetch_constant_dword = [&](uint32_t dword_index) {
    if (dword_index < metadata.fetch_constant_dword_mask.size() * 32u) {
      metadata.fetch_constant_dword_mask[dword_index >> 5u] |=
          uint32_t(1) << (dword_index & 31u);
    }
  };
  auto mark_vertex_fetch_constant = [&](uint32_t fetch_constant) {
    if (fetch_constant >= xenos::kVertexFetchConstantCount) {
      return;
    }
    uint32_t first_fetch_dword = fetch_constant * 2u;
    mark_fetch_constant_dword(first_fetch_dword);
    mark_fetch_constant_dword(first_fetch_dword + 1u);
  };
  auto mark_texture_fetch_constant = [&](uint32_t fetch_constant) {
    if (fetch_constant >= xenos::kTextureFetchConstantCount) {
      return;
    }
    uint32_t first_fetch_dword = fetch_constant * 6u;
    for (uint32_t i = 0; i < 6u; ++i) {
      mark_fetch_constant_dword(first_fetch_dword + i);
    }
  };
  const Shader::ConstantRegisterMap& constant_map =
      current_shader().constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map.vertex_fetch_bitmap[i];
    uint32_t bit_index;
    while (xe::bit_scan_forward(vfetch_bits_remaining, &bit_index)) {
      vfetch_bits_remaining = xe::clear_lowest_bit(vfetch_bits_remaining);
      mark_vertex_fetch_constant(i * 32u + bit_index);
    }
  }
  for (const Shader::VertexBinding& binding :
       current_shader().vertex_bindings()) {
    mark_vertex_fetch_constant(binding.fetch_constant);
  }
  for (const Shader::TextureBinding& binding :
       current_shader().texture_bindings()) {
    mark_texture_fetch_constant(binding.fetch_constant);
  }
  for (const TextureBinding& translator_binding : texture_bindings_) {
    ShaderTextureBinding binding = {};
    binding.bindless_descriptor_index =
        translator_binding.bindless_descriptor_index;
    binding.fetch_constant = translator_binding.fetch_constant;
    binding.dimension = translator_binding.dimension;
    binding.is_signed = translator_binding.is_signed;
    metadata.texture_bindings.emplace_back(binding);
    if (translator_binding.fetch_constant < 32u) {
      metadata.used_texture_mask |= 1u << translator_binding.fetch_constant;
    }
  }
  metadata.sampler_bindings.clear();
  metadata.sampler_bindings.reserve(sampler_bindings_.size());
  for (const SamplerBinding& translator_binding : sampler_bindings_) {
    ShaderSamplerBinding binding = {};
    binding.bindless_descriptor_index =
        uint32_t(texture_bindings_.size()) +
        translator_binding.bindless_descriptor_index;
    binding.fetch_constant = translator_binding.fetch_constant;
    binding.mag_filter = translator_binding.mag_filter;
    binding.min_filter = translator_binding.min_filter;
    binding.mip_filter = translator_binding.mip_filter;
    binding.aniso_filter = translator_binding.aniso_filter;
    metadata.sampler_bindings.emplace_back(binding);
  }
  metadata.used_cbuffer_mask = 0;
  auto mark_used_cbuffer = [&](ShaderCbufferRegister cbuffer) {
    metadata.used_cbuffer_mask |= 1u << uint32_t(cbuffer);
  };
  if (UsesNativeSystemConstants()) {
    mark_used_cbuffer(ShaderCbufferRegister::kSystemConstants);
  }
  if (UsesNativeFloatConstants()) {
    mark_used_cbuffer(ShaderCbufferRegister::kFloatConstants);
  }
  if (UsesNativeBoolLoopConstants()) {
    mark_used_cbuffer(ShaderCbufferRegister::kBoolLoopConstants);
  }
  if (UsesNativeFetchConstants()) {
    mark_used_cbuffer(ShaderCbufferRegister::kFetchConstants);
  }
  if (!metadata.texture_bindings.empty() || !metadata.sampler_bindings.empty()) {
    mark_used_cbuffer(ShaderCbufferRegister::kDescriptorIndices);
  }
  metadata.uses_shared_memory = UsesNativeSharedMemory();
  metadata.uses_primitive_index_constants =
      UsesNativePrimitiveIndexConstants();
  return metadata;
}

void MslShaderTranslator::PostTranslation() {
  Shader::Translation& translation = current_translation();
  if (!translation.is_valid()) {
    return;
  }
  native_metadata_ = BuildNativeMetadata();
}

void MslShaderTranslator::EmitSystemConstants() {
  EmitLine("struct XeSystemConstants {");
  Indent();
  EmitLine("uint xe_flags;");
  EmitLine("float xe_tessellation_factor_range[2];");
  EmitLine("uint xe_line_loop_closing_index;");
  EmitLine("uint xe_vertex_index_endian;");
  EmitLine("uint xe_vertex_index_offset;");
  EmitLine("uint xe_vertex_index_min_max[2];");
  EmitLine("float xe_user_clip_planes[6][4];");
  EmitLine("float xe_ndc_scale[3];");
  EmitLine("float xe_point_vertex_diameter_min;");
  EmitLine("float xe_ndc_offset[3];");
  EmitLine("float xe_point_vertex_diameter_max;");
  EmitLine("float xe_point_constant_diameter[2];");
  EmitLine("float xe_point_screen_diameter_to_ndc_radius[2];");
  EmitLine("uint xe_texture_swizzled_signs[8];");
  EmitLine("uint xe_textures_resolution_scaled;");
  EmitLine("uint xe_sample_count_log2[2];");
  EmitLine("float xe_alpha_test_reference;");
  EmitLine("uint xe_alpha_to_mask;");
  EmitLine("uint xe_edram_32bpp_tile_pitch_dwords_scaled;");
  EmitLine("uint xe_edram_depth_base_dwords_scaled;");
  EmitLine("uint xe_zpd_rov_counter_index;");
  EmitLine("float xe_color_exp_bias[4];");
  EmitLine("float xe_edram_poly_offset_front[2];");
  EmitLine("float xe_edram_poly_offset_back[2];");
  EmitLine("uint xe_edram_stencil[2][4];");
  EmitLine("uint xe_edram_rt_base_dwords_scaled[4];");
  EmitLine("uint xe_edram_rt_format_flags[4];");
  EmitLine("float xe_edram_rt_clamp[4][4];");
  EmitLine("uint xe_edram_rt_keep_mask[4][2];");
  EmitLine("uint xe_edram_rt_blend_factors_ops[4];");
  EmitLine("float xe_edram_blend_constant[4];");
  Outdent();
  EmitLine("};");
  EmitLine("");
  EmitLine("struct XeNativeDrawConstants {");
  Indent();
  EmitLine("constant XeSystemConstants* system [[id(0)]];");
  EmitLine("constant float4* float_constants_data [[id(1)]];");
  EmitLine("constant uint4* bool_loop_constants_data [[id(2)]];");
  EmitLine("constant uint4* fetch_constants_data [[id(3)]];");
  EmitLine("constant uint* descriptor_indices [[id(4)]];");
  EmitLine("constant uint4* primitive_index [[id(5)]];");
  Outdent();
  EmitLine("};");
  EmitLine("");
}

void MslShaderTranslator::EmitInputOutputDeclarations() {
  EmitLine("struct XeVertexOutput {");
  Indent();
  EmitLine("float4 xe_position [[position]];");
  if (VertexShaderEmitsPointSizeOutput()) {
    EmitLine("float xe_point_size [[point_size]];");
  }
  uint32_t clip_distance_count = VertexShaderClipDistanceCount();
  if (clip_distance_count) {
    EmitLine("float xe_clip_distance [[clip_distance]] [" +
             std::to_string(clip_distance_count) + "];");
  }
  for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
    EmitLine("float4 xe_interpolator_" + std::to_string(i) +
             " [[user(xe_interpolator_" + std::to_string(i) + ")]];");
  }
  EmitLine("float4 xe_point_parameters [[user(xe_point_parameters)]];");
  Outdent();
  EmitLine("};");
  EmitLine("");

  if (is_vertex_shader() && (IsPrimitiveListAsMesh() || IsDomainShader())) {
    EmitLine("struct XeTriangleMeshVertexOutput {");
    Indent();
    EmitLine("float4 xe_position [[position]];");
    if (clip_distance_count) {
      EmitLine("float xe_clip_distance [[clip_distance]] [" +
               std::to_string(clip_distance_count) + "];");
    }
    for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
      EmitLine("float4 xe_interpolator_" + std::to_string(i) +
               " [[user(xe_interpolator_" + std::to_string(i) + ")]];");
    }
    EmitLine("float4 xe_point_parameters [[user(xe_point_parameters)]];");
    Outdent();
    EmitLine("};");
    EmitLine("");
  }

  EmitLine("struct XeVertexInvocationResult {");
  Indent();
  EmitLine("XeVertexOutput output;");
  uint32_t cull_distance_count = VertexShaderCullDistanceCount();
  if (cull_distance_count) {
    EmitLine("float xe_cull_distance[" + std::to_string(cull_distance_count) +
             "];");
  }
  EmitLine("float4 xe_point_size_edge_flag_kill_vertex;");
  EmitLine("uint xe_point_sprite_vertex;");
  Outdent();
  EmitLine("};");
  EmitLine("");

  EmitLine("struct XePixelInput {");
  Indent();
  EmitLine("float4 xe_position [[position]];");
  EmitLine("bool xe_is_front_face [[front_facing]];");
  Modification modification = GetMslShaderModification();
  uint32_t centroid_interpolators =
      is_pixel_shader() ? modification.pixel.interpolators_centroid : 0;
  for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
    bool centroid = (centroid_interpolators & (UINT32_C(1) << i)) != 0;
    EmitLine("float4 xe_interpolator_" + std::to_string(i) +
             " [[user(xe_interpolator_" + std::to_string(i) + ")" +
             (centroid ? ", centroid_perspective" : "") + "]];");
  }
  EmitLine("float4 xe_point_parameters [[user(xe_point_parameters)]];");
  Outdent();
  EmitLine("};");
  EmitLine("");

  EmitLine("struct XePixelOutput {");
  Indent();
  bool emitted_pixel_output = false;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_shader().writes_color_target(i)) {
      EmitLine("float4 xe_color_" + std::to_string(i) + " [[color(" +
               std::to_string(i) + ")]];");
      emitted_pixel_output = true;
    }
  }
  if (PixelShaderWritesDepthOutput()) {
    const bool converted_raster_depth = PixelShaderNeedsFloat24DepthOutput() &&
                                        !current_shader().writes_depth();
    const bool truncating_converted_depth =
        converted_raster_depth &&
        modification.pixel.depth_stencil_mode ==
            Modification::DepthStencilMode::kFloat24Truncating;
    EmitLine("float xe_depth [[depth(" +
             std::string(truncating_converted_depth ? "less" : "any") + ")]];");
    emitted_pixel_output = true;
  }
  if (PixelShaderNeedsCoverageOutput()) {
    EmitLine("uint xe_coverage [[sample_mask]];");
    emitted_pixel_output = true;
  }
  if (!emitted_pixel_output) {
    // Metal fragment functions need a concrete return type. This path is rare
    // for guest pixel shaders, but keeps the native source valid.
    EmitLine("float4 xe_dummy_color [[color(0)]];");
  }
  Outdent();
  EmitLine("};");
  EmitLine("");
}

void MslShaderTranslator::EmitHelperFunctions() {
  const NativeMslHelperUsage helper_usage = GetNativeMslHelperUsage();
  const bool shader_uses_memexport = helper_usage.uses_memexport;
  const bool shader_has_texture_fetch = helper_usage.uses_texture_fetch;
  const bool shader_has_cube_texture = helper_usage.uses_cube_texture;
  const bool needs_depth_float24 = helper_usage.uses_depth_float24;
  const bool pixel_writes_color = helper_usage.uses_color_pwl_gamma;
  auto uses_width = [](uint8_t widths, uint32_t component_count) {
    return (widths & NativeMslFloatWidthBit(component_count)) != 0;
  };

  if (helper_usage.uses_nan_helpers) {
    EmitLine(
        "inline bool XeIsNaN(float v) { return (as_type<uint>(v) & "
        "0x7FFFFFFFu) > 0x7F800000u; }");
    EmitLine(
        "inline bool2 XeIsNaN(float2 v) { return (as_type<uint2>(v) & "
        "uint2(0x7FFFFFFFu)) > uint2(0x7F800000u); }");
    EmitLine(
        "inline bool3 XeIsNaN(float3 v) { return (as_type<uint3>(v) & "
        "uint3(0x7FFFFFFFu)) > uint3(0x7F800000u); }");
    EmitLine(
        "inline bool4 XeIsNaN(float4 v) { return (as_type<uint4>(v) & "
        "uint4(0x7FFFFFFFu)) > uint4(0x7F800000u); }");
  }
  if (helper_usage.uses_finite_helper) {
    EmitLine(
        "inline bool XeIsFinite(float v) { return (as_type<uint>(v) & "
        "0x7FFFFFFFu) < 0x7F800000u; }");
  }
  if (uses_width(helper_usage.saturate_widths, 1)) {
    EmitLine(
        "inline float XeSaturate(float v) { float sanitized = XeIsNaN(v) ? "
        "0.0f : v; return clamp(sanitized, 0.0f, 1.0f); }");
  }
  if (uses_width(helper_usage.saturate_widths, 2)) {
    EmitLine(
        "inline float2 XeSaturate(float2 v) { float2 sanitized = "
        "select(v, float2(0.0f), XeIsNaN(v)); return clamp(sanitized, "
        "float2(0.0f), float2(1.0f)); }");
  }
  if (uses_width(helper_usage.saturate_widths, 3)) {
    EmitLine(
        "inline float3 XeSaturate(float3 v) { float3 sanitized = "
        "select(v, float3(0.0f), XeIsNaN(v)); return clamp(sanitized, "
        "float3(0.0f), float3(1.0f)); }");
  }
  if (uses_width(helper_usage.saturate_widths, 4)) {
    EmitLine(
        "inline float4 XeSaturate(float4 v) { float4 sanitized = "
        "select(v, float4(0.0f), XeIsNaN(v)); return clamp(sanitized, "
        "float4(0.0f), float4(1.0f)); }");
  }
  if (helper_usage.uses_frac) {
    EmitLine("inline float frac(float v) { return fract(v); }");
    EmitLine("inline float2 frac(float2 v) { return fract(v); }");
    EmitLine("inline float3 frac(float3 v) { return fract(v); }");
    EmitLine("inline float4 frac(float4 v) { return fract(v); }");
  }
  EmitLine("inline float asfloat(uint v) { return as_type<float>(v); }");
  EmitLine("inline float2 asfloat(uint2 v) { return as_type<float2>(v); }");
  EmitLine("inline float3 asfloat(uint3 v) { return as_type<float3>(v); }");
  EmitLine("inline float4 asfloat(uint4 v) { return as_type<float4>(v); }");
  EmitLine("inline uint asuint(float v) { return as_type<uint>(v); }");
  EmitLine("inline uint2 asuint(float2 v) { return as_type<uint2>(v); }");
  EmitLine("inline uint3 asuint(float3 v) { return as_type<uint3>(v); }");
  EmitLine("inline uint4 asuint(float4 v) { return as_type<uint4>(v); }");
  if (helper_usage.uses_vertex_position_inf_to_nan) {
    EmitLine(
        "inline float4 XeVertexPositionInfToNaN(float4 v) { uint4 bits = "
        "asuint(v); bool4 is_inf = (bits & uint4(0x7FFFFFFFu)) == "
        "uint4(0x7F800000u); return select(v, float4(asfloat(0x7FC00000u)), "
        "is_inf); }");
  }
  if (helper_usage.uses_set_float_sign_bit) {
    EmitLine(
        "inline float XeSetFloatSignBit(float v) { return "
        "as_type<float>(as_type<uint>(v) | 0x80000000u); }");
  }
  if (helper_usage.uses_first_bit_low) {
    EmitLine(
        "inline uint XeFirstBitLow(uint v) { uint r = ctz(v); return r == "
        "32u ? 0xFFFFFFFFu : r; }");
  }
  if (helper_usage.uses_address_index_round ||
      helper_usage.uses_address_index_floor) {
    EmitLine(
        "inline float XeClampAddressFloat(float value) { float sanitized = "
        "XeIsNaN(value) ? 0.0f : value; return clamp(sanitized, -256.0f, "
        "255.0f); }");
  }
  if (helper_usage.uses_address_index_round) {
    EmitLine(
        "inline int XeAddressIndexRound(float value) { return "
        "int(floor(XeClampAddressFloat(value + 0.5f))); }");
  }
  if (helper_usage.uses_address_index_floor) {
    EmitLine(
        "inline int XeAddressIndexFloor(float value) { return "
        "int(floor(XeClampAddressFloat(value))); }");
  }
  if (helper_usage.uses_point_size_clamp) {
    EmitLine(
        "inline float XeClampPointSize(float v, float min_v, float max_v) {");
    Indent();
    EmitLine("int bits = as_type<int>(v);");
    EmitLine("bits = max(bits, as_type<int>(min_v));");
    EmitLine("bits = min(bits, as_type<int>(max_v));");
    EmitLine("return as_type<float>(bits);");
    Outdent();
    EmitLine("}");
  }
  if (helper_usage.uses_mul_sm3) {
    EmitLine(
        "inline float XeMulSM3(float a, float b) { return (a == 0.0f || b == "
        "0.0f) ? 0.0f : a * b; }");
    EmitLine(
        "inline float2 XeMulSM3(float2 a, float2 b) { return select(a * b, "
        "float2(0.0f), (a == float2(0.0f)) | (b == float2(0.0f))); }");
    EmitLine(
        "inline float3 XeMulSM3(float3 a, float3 b) { return select(a * b, "
        "float3(0.0f), (a == float3(0.0f)) | (b == float3(0.0f))); }");
    EmitLine(
        "inline float4 XeMulSM3(float4 a, float4 b) { return select(a * b, "
        "float4(0.0f), (a == float4(0.0f)) | (b == float4(0.0f))); }");
  }
  if (helper_usage.uses_clamp_inf_to_max) {
    EmitLine(
        "inline float XeClampInfToMax(float v) { uint bits = as_type<uint>(v); "
        "return ((bits & 0x7FFFFFFFu) == 0x7F800000u) ? (((bits & "
        "0x80000000u) != 0u) ? -3.402823466e+38f : 3.402823466e+38f) : v; }");
  }
  if (helper_usage.uses_flush_inf_to_signed_zero) {
    EmitLine(
        "inline float XeFlushInfToSignedZero(float v) { uint bits = "
        "as_type<uint>(v); return ((bits & 0x7FFFFFFFu) == 0x7F800000u) ? "
        "as_type<float>(bits & 0x80000000u) : v; }");
  }
  if (helper_usage.uses_logc) {
    EmitLine(
        "inline float XeLogc(float v) { float r = log2(v); return "
        "as_type<uint>(r) == 0xFF800000u ? -3.402823466e+38f : r; }");
  }
  if (helper_usage.uses_rcpc) {
    EmitLine(
        "inline float XeRcpc(float v) { return XeClampInfToMax(1.0f / v); }");
  }
  if (helper_usage.uses_rcpf) {
    EmitLine(
        "inline float XeRcpf(float v) { return XeFlushInfToSignedZero(1.0f / "
        "v); }");
  }
  if (helper_usage.uses_rsqc) {
    EmitLine(
        "inline float XeRsqc(float v) { return XeClampInfToMax(rsqrt(v)); }");
  }
  if (helper_usage.uses_rsqf) {
    EmitLine(
        "inline float XeRsqf(float v) { return "
        "XeFlushInfToSignedZero(rsqrt(v)); }");
  }
  if (helper_usage.uses_endian_swap) {
    EmitLine("inline uint XeEndianSwap(uint v, uint endian) {");
    Indent();
    EmitLine("if (endian == 0u) return v;");
    EmitLine(
        "if (endian == 1u) return ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & "
        "0x00FF00FFu);");
    EmitLine(
        "if (endian == 2u) return ((v & 0x000000FFu) << 24u) | ((v & "
        "0x0000FF00u) << 8u) | ((v >> "
        "8u) & 0x0000FF00u) | ((v >> 24u) & 0x000000FFu);");
    EmitLine("return (v << 16u) | (v >> 16u);");
    Outdent();
    EmitLine("}");
  }
  if (helper_usage.uses_shared_memory_load) {
    EmitLine(
        "inline uint XeSharedMemoryLoad(const device uint* mem, uint "
        "byte_address) { return mem[byte_address >> 2u]; }");
    EmitLine(
        "inline uint2 XeSharedMemoryLoad2(const device uint* mem, uint "
        "byte_address) { uint i = byte_address >> 2u; return uint2(mem[i], "
        "mem[i + 1u]); }");
  }
  if (shader_uses_memexport && helper_usage.uses_shared_memory_load) {
    EmitLine(
        "inline uint XeSharedMemoryLoad(device atomic_uint* mem, uint "
        "byte_address) { return atomic_load_explicit(&mem[byte_address >> 2u], "
        "memory_order_relaxed); }");
    EmitLine(
        "inline uint2 XeSharedMemoryLoad2(device atomic_uint* mem, uint "
        "byte_address) { uint i = byte_address >> 2u; return "
        "uint2(atomic_load_explicit(&mem[i], memory_order_relaxed), "
        "atomic_load_explicit(&mem[i + 1u], memory_order_relaxed)); }");
  }
  if (helper_usage.uses_primitive_index_load) {
    EmitLine(
        "inline uint XeLoadPrimitiveIndex(constant uint4& primitive_index, "
        "const device uint* mem, uint index) { if ((primitive_index.x & 1u) == "
        "0u) return index; if (index >= primitive_index.z) return 0u; bool "
        "is_32bit = (primitive_index.x & 2u) != 0u; uint byte_address = "
        "primitive_index.y + (index << (is_32bit ? 2u : 1u)); uint raw = "
        "XeSharedMemoryLoad(mem, byte_address & ~3u); return is_32bit ? raw : "
        "((raw >> ((byte_address & 2u) << 3u)) & 0xFFFFu); }");
  }
  if (shader_uses_memexport && helper_usage.uses_primitive_index_load) {
    EmitLine(
        "inline uint XeLoadPrimitiveIndex(constant uint4& primitive_index, "
        "device atomic_uint* mem, uint index) { if ((primitive_index.x & 1u) "
        "== "
        "0u) return index; if (index >= primitive_index.z) return 0u; bool "
        "is_32bit = (primitive_index.x & 2u) != 0u; uint byte_address = "
        "primitive_index.y + (index << (is_32bit ? 2u : 1u)); uint raw = "
        "XeSharedMemoryLoad(mem, byte_address & ~3u); return is_32bit ? raw : "
        "((raw >> ((byte_address & 2u) << 3u)) & 0xFFFFu); }");
  }
  if (shader_uses_memexport) {
    EmitLine(
        "inline void XeSharedMemoryStore(device atomic_uint* mem, uint "
        "byte_address, uint value) { atomic_store_explicit(&mem[byte_address "
        ">> "
        "2u], value, memory_order_relaxed); }");
    EmitLine(
        "inline void XeSharedMemoryStore2(device atomic_uint* mem, uint "
        "byte_address, uint2 value) { uint i = byte_address >> 2u; "
        "atomic_store_explicit(&mem[i], value.x, memory_order_relaxed); "
        "atomic_store_explicit(&mem[i + 1u], value.y, memory_order_relaxed); "
        "}");
    EmitLine(
        "inline void XeSharedMemoryStore4(device atomic_uint* mem, uint "
        "byte_address, uint4 value) { uint i = byte_address >> 2u; "
        "atomic_store_explicit(&mem[i], value.x, memory_order_relaxed); "
        "atomic_store_explicit(&mem[i + 1u], value.y, memory_order_relaxed); "
        "atomic_store_explicit(&mem[i + 2u], value.z, memory_order_relaxed); "
        "atomic_store_explicit(&mem[i + 3u], value.w, memory_order_relaxed); "
        "}");
    EmitLine(
        "inline void XeSharedMemoryStoreSubDword(device atomic_uint* mem, uint "
        "byte_address, uint value, uint width) { uint shift = (byte_address & "
        "3u) * 8u; uint mask = ((1u << width) - 1u) << shift; device "
        "atomic_uint* word = &mem[byte_address >> 2u]; "
        "atomic_fetch_and_explicit(word, ~mask, memory_order_relaxed); "
        "atomic_fetch_or_explicit(word, (value << shift) & mask, "
        "memory_order_relaxed); }");
  }
  if (helper_usage.uses_sign_extend) {
    EmitLine(
        "inline int XeSignExtend(uint value, uint bits) { if (bits >= 32u) "
        "return int(value); uint sign = 1u << (bits - 1u); uint mask = (1u << "
        "bits) - 1u; value &= mask; return int((value ^ sign) - sign); }");
  }
  if (helper_usage.uses_vertex_fetch_unpack) {
    EmitLine(
        "inline float XeNormalizeUnsigned(uint value, uint bits) { if (bits >= "
        "32u) return float(value) * (1.0f / 4294967295.0f); return "
        "float(value) / float((1u << bits) - 1u); }");
    EmitLine(
        "inline float XeNormalizeSignedZeroClampMinusOne(int value, uint bits) "
        "{ float scale = (bits >= 32u) ? (1.0f / 2147483647.0f) : (1.0f / "
        "float((1u << (bits - 1u)) - 1u)); return max(float(value) * scale, "
        "-1.0f); }");
    EmitLine(
        "inline float XeNormalizeSignedNoZero(int value, uint bits) { if (bits "
        ">= 32u) return float(value) * (1.0f / 2147483647.5f) + (0.5f / "
        "2147483647.5f); float denom = float((1u << bits) - 1u); return "
        "float(value) * (2.0f / denom) + (1.0f / denom); }");
    EmitLine(
        "inline float XeF16(uint bits) { return float(as_type<half>(ushort("
        "bits & 0xFFFFu))); }");
    EmitLine(
        "inline float2 XeUnpackFloat16x2(uint bits) { return "
        "float2(XeF16(bits), XeF16(bits >> 16u)); }");
  }
  if (helper_usage.uses_bool_constant) {
    EmitLine(
        "inline bool XeGetBoolConstant(constant uint4* bool_loop, uint index) "
        "{ "
        "uint vec_index = index >> 5u; uint word = bool_loop[vec_index >> "
        "2u][vec_index & 3u]; return ((word >> (index & 31u)) & 1u) != 0u; }");
  }
  if (helper_usage.uses_loop_constant) {
    EmitLine(
        "inline uint XeGetLoopConstant(constant uint4* bool_loop, uint index) "
        "{ "
        "uint vec_index = 8u + index; return bool_loop[vec_index >> "
        "2u][vec_index & 3u]; }");
  }
  if (shader_has_texture_fetch) {
    EmitLine(
        "inline uint XeGetFetchConstantWord(constant uint4* fetch, uint "
        "fetch_constant, uint word) { uint d = fetch_constant * 6u + word; "
        "return fetch[d >> 2u][d & 3u]; }");
    EmitLine(
        "inline uint XeGetTextureFetchConstantWord(constant uint4* fetch, uint "
        "fetch_constant, uint word) { return XeGetFetchConstantWord(fetch, "
        "fetch_constant, word); }");
  }
  if (shader_has_cube_texture) {
    EmitLine(
        "inline float3 XeTextureCubeDirection(float3 coord) { float2 st = "
        "coord.xy * 2.0f - 3.0f; uint face = uint(clamp(floor(coord.z), 0.0f, "
        "5.0f)); uint axis = "
        "face >> 1u; bool negative = (face & 1u) != 0u; if (axis == 0u) return "
        "float3(negative ? -1.0f : 1.0f, -st.y, negative ? st.x : -st.x); if "
        "(axis == 1u) return float3(st.x, negative ? -1.0f : 1.0f, negative ? "
        "-st.y : st.y); return float3(negative ? -st.x : st.x, -st.y, negative "
        "? "
        "-1.0f : 1.0f); }");
  }
  if (uses_width(helper_usage.saturate_no_nan_widths, 1)) {
    EmitLine(
        "inline float XeSaturateNoNaN(float value) { float sanitized = "
        "XeIsNaN(value) ? 0.0f : value; return clamp(sanitized, 0.0f, 1.0f); "
        "}");
  }
  if (uses_width(helper_usage.saturate_no_nan_widths, 2)) {
    EmitLine(
        "inline float2 XeSaturateNoNaN(float2 value) { float2 sanitized = "
        "select(value, float2(0.0f), XeIsNaN(value)); return clamp(sanitized, "
        "float2(0.0f), float2(1.0f)); }");
  }
  if (uses_width(helper_usage.saturate_no_nan_widths, 3)) {
    EmitLine(
        "inline float3 XeSaturateNoNaN(float3 value) { float3 sanitized = "
        "select(value, float3(0.0f), XeIsNaN(value)); return clamp(sanitized, "
        "float3(0.0f), float3(1.0f)); }");
  }
  if (uses_width(helper_usage.saturate_no_nan_widths, 4)) {
    EmitLine(
        "inline float4 XeSaturateNoNaN(float4 value) { float4 sanitized = "
        "select(value, float4(0.0f), XeIsNaN(value)); return clamp(sanitized, "
        "float4(0.0f), float4(1.0f)); }");
  }
  if (needs_depth_float24) {
    EmitLine(
        "inline uint XePreClampedDepthTo20e4(float depth, bool "
        "round_to_nearest_even, bool remap_from_0_to_0_5) { uint f32 = "
        "asuint(depth); uint remap_bias = remap_from_0_to_0_5 ? 1u : 0u; uint "
        "biased_f32; if (f32 < (0x38800000u - (remap_bias << 23u))) { uint "
        "shift "
        "= min((113u - remap_bias) - (f32 >> 23u), 24u); biased_f32 = (((f32 & "
        "0x7FFFFFu) | 0x800000u) >> shift); } else { biased_f32 = f32 + "
        "(0xC8000000u + (remap_bias << 23u)); } if (round_to_nearest_even) "
        "biased_f32 += 3u + ((biased_f32 >> 3u) & 1u); return (biased_f32 >> "
        "3u) "
        "& 0xFFFFFFu; }");
    EmitLine(
        "inline float XeDepth20e4To32(uint f24, bool remap_to_0_to_0_5) { uint "
        "remap_bias = remap_to_0_to_0_5 ? 1u : 0u; uint exponent = (f24 >> "
        "20u) "
        "& 0xFu; uint mantissa = f24 & 0xFFFFFu; int exponent_signed = "
        "int(exponent); if (exponent == 0u) { if (mantissa != 0u) { int shift "
        "= "
        "20 - int(31u - clz(mantissa)); mantissa <<= uint(shift); "
        "exponent_signed = 1 - shift; } else { exponent_signed = -int(112u - "
        "remap_bias); } } uint exponent_bits = uint(exponent_signed + int(112u "
        "- "
        "remap_bias)) << 23u; return asfloat(exponent_bits | ((mantissa & "
        "0xFFFFFu) << 3u)); }");
    EmitLine(
        "inline float XeDepthFloat24TruncateToHost(float depth) { depth = "
        "XeSaturateNoNaN(depth); uint depth_uint = asuint(depth); if "
        "(depth_uint "
        "< 0x2E800000u) return 0.0f; uint exponent = (depth_uint >> 23u) & "
        "0xFFu; int trunc_bits_signed = max(116 - int(exponent), 3); uint "
        "trunc_bits = uint(trunc_bits_signed); uint trunc_mask = ~((1u << "
        "trunc_bits) - 1u); return asfloat(depth_uint & trunc_mask) * 0.5f; }");
    EmitLine(
        "inline float XeDepthFloat24RoundToHost(float depth) { depth = "
        "XeSaturateNoNaN(depth); uint f24 = XePreClampedDepthTo20e4(depth, "
        "true, "
        "false); return XeDepth20e4To32(f24, true); }");
  }
  if (uses_width(helper_usage.texture_pwl_gamma_widths, 1)) {
    EmitLine(
        "inline float XePWLGammaToLinear(float value) { float clamped = "
        "XeSaturateNoNaN(value); bool piece_at_least_2 = clamped >= (96.0f / "
        "255.0f); bool piece_at_least_3 = clamped >= (192.0f / 255.0f); bool "
        "piece_at_least_1 = clamped >= (64.0f / 255.0f); float scale = "
        "piece_at_least_2 ? (piece_at_least_3 ? (8.0f / 1024.0f) : (4.0f / "
        "1024.0f)) : (piece_at_least_1 ? (2.0f / 1024.0f) : (1.0f / 1024.0f)); "
        "float offset = piece_at_least_2 ? (piece_at_least_3 ? -1024.0f : "
        "-256.0f) : (piece_at_least_1 ? -64.0f : 0.0f); float linear_value = "
        "fma(clamped * (255.0f * 1024.0f), scale, offset); linear_value += "
        "trunc(linear_value * scale); return linear_value * (1.0f / 1023.0f); "
        "}");
  }
  if (uses_width(helper_usage.texture_pwl_gamma_widths, 2)) {
    EmitLine(
        "inline float2 XePWLGammaToLinear(float2 value) { float2 clamped = "
        "XeSaturateNoNaN(value); bool2 piece_at_least_2 = clamped >= "
        "float2(96.0f / 255.0f); bool2 piece_at_least_3 = clamped >= "
        "float2(192.0f / 255.0f); bool2 piece_at_least_1 = clamped >= "
        "float2(64.0f / 255.0f); float2 scale = select(select(float2(1.0f / "
        "1024.0f), float2(2.0f / 1024.0f), piece_at_least_1), "
        "select(float2(4.0f / 1024.0f), float2(8.0f / 1024.0f), "
        "piece_at_least_3), piece_at_least_2); float2 offset = "
        "select(select(float2(0.0f), float2(-64.0f), piece_at_least_1), "
        "select(float2(-256.0f), float2(-1024.0f), piece_at_least_3), "
        "piece_at_least_2); float2 linear_value = fma(clamped * float2(255.0f * "
        "1024.0f), scale, offset); linear_value += trunc(linear_value * "
        "scale); return linear_value * float2(1.0f / 1023.0f); }");
  }
  if (uses_width(helper_usage.texture_pwl_gamma_widths, 3)) {
    EmitLine(
        "inline float3 XePWLGammaToLinear(float3 value) { float3 clamped = "
        "XeSaturateNoNaN(value); bool3 piece_at_least_2 = clamped >= "
        "float3(96.0f / 255.0f); bool3 piece_at_least_3 = clamped >= "
        "float3(192.0f / 255.0f); bool3 piece_at_least_1 = clamped >= "
        "float3(64.0f / 255.0f); float3 scale = select(select(float3(1.0f / "
        "1024.0f), float3(2.0f / 1024.0f), piece_at_least_1), "
        "select(float3(4.0f / 1024.0f), float3(8.0f / 1024.0f), "
        "piece_at_least_3), piece_at_least_2); float3 offset = "
        "select(select(float3(0.0f), float3(-64.0f), piece_at_least_1), "
        "select(float3(-256.0f), float3(-1024.0f), piece_at_least_3), "
        "piece_at_least_2); float3 linear_value = fma(clamped * float3(255.0f * "
        "1024.0f), scale, offset); linear_value += trunc(linear_value * "
        "scale); return linear_value * float3(1.0f / 1023.0f); }");
  }
  if (uses_width(helper_usage.texture_pwl_gamma_widths, 4)) {
    EmitLine(
        "inline float4 XePWLGammaToLinear(float4 value) { float4 clamped = "
        "XeSaturateNoNaN(value); bool4 piece_at_least_2 = clamped >= "
        "float4(96.0f / 255.0f); bool4 piece_at_least_3 = clamped >= "
        "float4(192.0f / 255.0f); bool4 piece_at_least_1 = clamped >= "
        "float4(64.0f / 255.0f); float4 scale = select(select(float4(1.0f / "
        "1024.0f), float4(2.0f / 1024.0f), piece_at_least_1), "
        "select(float4(4.0f / 1024.0f), float4(8.0f / 1024.0f), "
        "piece_at_least_3), piece_at_least_2); float4 offset = "
        "select(select(float4(0.0f), float4(-64.0f), piece_at_least_1), "
        "select(float4(-256.0f), float4(-1024.0f), piece_at_least_3), "
        "piece_at_least_2); float4 linear_value = fma(clamped * float4(255.0f * "
        "1024.0f), scale, offset); linear_value += trunc(linear_value * "
        "scale); return linear_value * float4(1.0f / 1023.0f); }");
  }
  if (helper_usage.uses_texture_sign_helpers) {
    EmitLine(
        "inline float XeApplyTextureSign(float unsigned_value, float "
        "signed_value, uint sign) { if (sign == 1u) return signed_value; if "
        "(sign == 2u) return fma(unsigned_value, 2.0f, -1.0f); if (sign == 3u) "
        "return XePWLGammaToLinear(unsigned_value); return unsigned_value; }");
  }
  if (pixel_writes_color) {
    EmitLine(
        "inline float3 XeLinearToPWLGamma3(float3 value) { float3 clamped = "
        "XeSaturateNoNaN(value); bool3 piece_at_least_2 = clamped >= "
        "float3(128.0f / 1023.0f); bool3 piece_at_least_3 = clamped >= "
        "float3(512.0f / 1023.0f); bool3 piece_at_least_1 = clamped >= "
        "float3(64.0f / 1023.0f); float3 scale = "
        "select(select(float3(1023.0f), float3(1023.0f / 2.0f), "
        "piece_at_least_1), select(float3(1023.0f / 4.0f), "
        "float3(1023.0f / 8.0f), piece_at_least_3), piece_at_least_2); "
        "float3 offset = select(select(float3(0.0f), float3(32.0f / "
        "255.0f), piece_at_least_1), select(float3(64.0f / 255.0f), "
        "float3(128.0f / 255.0f), piece_at_least_3), "
        "piece_at_least_2); return fma(trunc(clamped * scale), "
        "float3(1.0f / 255.0f), offset); }");
  }
  if (!shader_uses_memexport) {
    return;
  }
  EmitLine(
      "inline float4 XeMemExportFlushNaN(float4 value) { return select(value, "
      "float4(0.0f), XeIsNaN(value)); }");
  EmitLine(
      "inline uint4 XeMemExportPackFixedComponents(float4 value, uint4 widths, "
      "bool num_signed, bool num_integer) {");
  Indent();
  EmitLine("value = XeMemExportFlushNaN(value);");
  EmitLine("uint4 safe_widths = max(widths, uint4(1u));");
  EmitLine(
      "uint4 masks = select(uint4(0u), (uint4(1u) << safe_widths) - uint4(1u), "
      "widths != uint4(0u));");
  EmitLine(
      "uint4 signed_max_bits = (uint4(1u) << (safe_widths - uint4(1u))) - "
      "uint4(1u);");
  EmitLine("float4 packed_value;");
  EmitLine("if (num_signed) {");
  Indent();
  EmitLine("if (num_integer) {");
  Indent();
  EmitLine("float4 max_value = float4(signed_max_bits);");
  EmitLine(
      "packed_value = clamp(value, -float4(1.0f) - max_value, max_value);");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine(
      "float4 max_value = select(float4(1.0f), float4(signed_max_bits), "
      "safe_widths > uint4(2u));");
  EmitLine(
      "packed_value = clamp(value, float4(-1.0f), float4(1.0f)) * max_value;");
  Outdent();
  EmitLine("}");
  EmitLine(
      "packed_value += select(float4(0.5f), float4(-0.5f), "
      "(as_type<uint4>(packed_value) & uint4(0x80000000u)) != uint4(0u));");
  EmitLine("return as_type<uint4>(int4(packed_value)) & masks;");
  Outdent();
  EmitLine("}");
  EmitLine("if (num_integer) {");
  Indent();
  EmitLine("packed_value = clamp(value, float4(0.0f), float4(masks));");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine(
      "float4 max_value = select(float4(1.0f), float4(masks), safe_widths > "
      "uint4(1u));");
  EmitLine(
      "packed_value = clamp(value, float4(0.0f), float4(1.0f)) * max_value;");
  Outdent();
  EmitLine("}");
  EmitLine("return uint4(packed_value + float4(0.5f)) & masks;");
  Outdent();
  EmitLine("}");
  EmitLine(
      "inline uint XeMemExportPackFixed(float4 value, uint4 widths, bool "
      "num_signed, bool num_integer) {");
  Indent();
  EmitLine(
      "uint4 components = XeMemExportPackFixedComponents(value, widths, "
      "num_signed, num_integer);");
  EmitLine(
      "uint4 offsets = uint4(0u, widths.x, widths.x + widths.y, widths.x + "
      "widths.y + widths.z);");
  EmitLine(
      "return (components.x << offsets.x) | (components.y << offsets.y) | "
      "(components.z << offsets.z) | (components.w << offsets.w);");
  Outdent();
  EmitLine("}");
  EmitLine(
      "inline uint XeMemExportF32ToF16(float value) { ushort standard_bits = "
      "as_type<ushort>(half(value)); bool overflowed = (standard_bits & "
      "0x7C00u) == 0x7C00u; float clamped = clamp(value, -131008.0f, "
      "131008.0f) * 0.5f; ushort extended_bits = "
      "as_type<ushort>(half(clamped)) "
      "+ ushort(0x0400u); return uint(overflowed ? extended_bits : "
      "standard_bits); }");
  EmitLine(
      "inline uint4 XeMemExportPack(float4 value, uint color_format, bool "
      "num_signed, bool num_integer, thread uint& element_size) {");
  Indent();
  EmitLine("element_size = 0xFFFFFFFFu;");
  EmitLine("switch (color_format) {");
  Indent();
  auto emit_memexport_case = [this](xenos::ColorFormat format) {
    EmitLine("case " + std::to_string(uint32_t(format)) + "u:");
  };
  emit_memexport_case(xenos::ColorFormat::k_8);
  emit_memexport_case(xenos::ColorFormat::k_8_A);
  emit_memexport_case(xenos::ColorFormat::k_8_B);
  EmitLine(
      "element_size = 0u; return uint4(XeMemExportPackFixed(value, uint4(8u, "
      "0u, 0u, 0u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_1_5_5_5);
  EmitLine(
      "element_size = 1u; return uint4(XeMemExportPackFixed(value, uint4(5u, "
      "5u, 5u, 1u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_5_6_5);
  EmitLine(
      "element_size = 1u; return uint4(XeMemExportPackFixed(value, uint4(5u, "
      "6u, 5u, 0u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_6_5_5);
  EmitLine(
      "element_size = 1u; return uint4(XeMemExportPackFixed(value, uint4(5u, "
      "5u, 6u, 0u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_8_8_8_8);
  emit_memexport_case(xenos::ColorFormat::k_8_8_8_8_A);
  emit_memexport_case(xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16);
  EmitLine(
      "element_size = 2u; return uint4(XeMemExportPackFixed(value, uint4(8u, "
      "8u, 8u, 8u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_2_10_10_10);
  emit_memexport_case(xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16);
  EmitLine(
      "element_size = 2u; return uint4(XeMemExportPackFixed(value, uint4(10u, "
      "10u, 10u, 2u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_8_8);
  EmitLine(
      "element_size = 1u; return uint4(XeMemExportPackFixed(value, uint4(8u, "
      "8u, 0u, 0u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_4_4_4_4);
  EmitLine(
      "element_size = 1u; return uint4(XeMemExportPackFixed(value, uint4(4u, "
      "4u, 4u, 4u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_10_11_11);
  emit_memexport_case(xenos::ColorFormat::k_10_11_11_AS_16_16_16_16);
  EmitLine(
      "element_size = 2u; return uint4(XeMemExportPackFixed(value, uint4(11u, "
      "11u, 10u, 0u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_11_11_10);
  emit_memexport_case(xenos::ColorFormat::k_11_11_10_AS_16_16_16_16);
  EmitLine(
      "element_size = 2u; return uint4(XeMemExportPackFixed(value, uint4(10u, "
      "11u, 11u, 0u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_16);
  EmitLine(
      "element_size = 1u; return uint4(XeMemExportPackFixed(value, uint4(16u, "
      "0u, 0u, 0u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_16_16);
  EmitLine(
      "element_size = 2u; return uint4(XeMemExportPackFixed(value, uint4(16u, "
      "16u, 0u, 0u), num_signed, num_integer), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_16_16_16_16);
  EmitLine(
      "{ uint4 c = XeMemExportPackFixedComponents(value, uint4(16u), "
      "num_signed, num_integer); element_size = 3u; return uint4(c.x | (c.y << "
      "16u), c.z | (c.w << 16u), 0u, 0u); }");
  emit_memexport_case(xenos::ColorFormat::k_16_FLOAT);
  EmitLine(
      "element_size = 1u; return uint4(XeMemExportF32ToF16(value.x), 0u, 0u, "
      "0u);");
  emit_memexport_case(xenos::ColorFormat::k_16_16_FLOAT);
  EmitLine(
      "element_size = 2u; return uint4(XeMemExportF32ToF16(value.x) | "
      "(XeMemExportF32ToF16(value.y) << 16u), 0u, 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_16_16_16_16_FLOAT);
  EmitLine(
      "element_size = 3u; return uint4(XeMemExportF32ToF16(value.x) | "
      "(XeMemExportF32ToF16(value.y) << 16u), XeMemExportF32ToF16(value.z) | "
      "(XeMemExportF32ToF16(value.w) << 16u), 0u, 0u);");
  emit_memexport_case(xenos::ColorFormat::k_32_FLOAT);
  EmitLine("element_size = 2u; return as_type<uint4>(value);");
  emit_memexport_case(xenos::ColorFormat::k_32_32_FLOAT);
  EmitLine("element_size = 3u; return as_type<uint4>(value);");
  emit_memexport_case(xenos::ColorFormat::k_32_32_32_32_FLOAT);
  EmitLine("element_size = 4u; return as_type<uint4>(value);");
  EmitLine("default: return uint4(0u);");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  EmitLine("");
}

void MslShaderTranslator::EmitNativeResourceHeapDeclarations() {
  if (texture_bindings_.empty() && sampler_bindings_.empty()) {
    return;
  }
  if (UsesNativeTexture2DArrayHeap()) {
    EmitLine("struct XeNativeTexture2DArrayHeap {");
    Indent();
    EmitLine("array<texture2d_array<float>, " +
             std::to_string(kNativeMslTextureHeapSize) +
             "> textures [[id(0)]];");
    Outdent();
    EmitLine("};");
  }
  if (UsesNativeTexture3DHeap()) {
    EmitLine("struct XeNativeTexture3DHeap {");
    Indent();
    EmitLine("array<texture3d<float>, " +
             std::to_string(kNativeMslTextureHeapSize) +
             "> textures [[id(0)]];");
    Outdent();
    EmitLine("};");
  }
  if (UsesNativeTextureCubeHeap()) {
    EmitLine("struct XeNativeTextureCubeHeap {");
    Indent();
    EmitLine("array<texturecube<float>, " +
             std::to_string(kNativeMslTextureHeapSize) +
             "> textures [[id(0)]];");
    Outdent();
    EmitLine("};");
  }
  if (!sampler_bindings_.empty()) {
    EmitLine("struct XeNativeSamplerHeap {");
    Indent();
    EmitLine("array<sampler, " + std::to_string(kNativeMslSamplerHeapSize) +
             "> samplers [[id(0)]];");
    Outdent();
    EmitLine("};");
  }
  EmitLine("");
}

void MslShaderTranslator::EmitDirectResourceArguments(
    bool first_argument_written, bool emit_attributes) {
  auto buffer_attribute = [&](uint32_t slot) {
    return emit_attributes ? " [[buffer(" + std::to_string(slot) + ")]]" : "";
  };
  auto comma = [&]() {
    Emit(first_argument_written ? ",\n" : "\n");
    first_argument_written = true;
  };
  if (UsesNativeDrawConstants()) {
    comma();
    Emit(indent_string_ +
         "const device XeNativeDrawConstants& xe_draw_constants" +
         buffer_attribute(kNativeBufferDrawConstants));
  }
  if (UsesNativeSharedMemory()) {
    comma();
    // Memexport shaders write shared memory through atomic stores. Metal's
    // offline compiler warns about writable resources in non-void vertex
    // functions, but downgrading this binding to read-only would break those
    // stores.
    Emit(indent_string_ +
         (uses_memexport_ ? "device atomic_uint* " : "const device uint* ") +
         "xe_shared_memory" + buffer_attribute(kNativeBufferSharedMemory));
  }
  if (UsesTextureRuntimeInfo()) {
    comma();
    Emit(indent_string_ + "constant uint4* xe_texture_runtime_info" +
         buffer_attribute(kNativeBufferTextureRuntimeInfo));
  }
  if (UsesNativeTexture2DArrayHeap()) {
    comma();
    Emit(indent_string_ +
         "const device XeNativeTexture2DArrayHeap& xe_texture_2d_array_heap" +
         buffer_attribute(kNativeBufferTexture2DArrayHeap));
  }
  if (UsesNativeTexture3DHeap()) {
    comma();
    Emit(indent_string_ +
         "const device XeNativeTexture3DHeap& xe_texture_3d_heap" +
         buffer_attribute(kNativeBufferTexture3DHeap));
  }
  if (UsesNativeTextureCubeHeap()) {
    comma();
    Emit(indent_string_ +
         "const device XeNativeTextureCubeHeap& xe_texture_cube_heap" +
         buffer_attribute(kNativeBufferTextureCubeHeap));
  }

  if (!sampler_bindings_.empty()) {
    comma();
    Emit(indent_string_ + "const device XeNativeSamplerHeap& xe_sampler_heap" +
         buffer_attribute(kNativeBufferSamplerHeap));
  }
}

void MslShaderTranslator::EmitDirectResourceArgumentNames(
    bool first_argument_written) {
  auto comma = [&]() {
    Emit(first_argument_written ? ",\n" : "\n");
    first_argument_written = true;
  };
  auto emit_name = [&](const std::string& name) {
    comma();
    Emit(indent_string_ + name);
  };
  if (UsesNativeDrawConstants()) {
    emit_name("xe_draw_constants");
  }
  if (UsesNativeSharedMemory()) {
    emit_name("xe_shared_memory");
  }
  if (UsesTextureRuntimeInfo()) {
    emit_name("xe_texture_runtime_info");
  }
  if (UsesNativeTexture2DArrayHeap()) {
    emit_name("xe_texture_2d_array_heap");
  }
  if (UsesNativeTexture3DHeap()) {
    emit_name("xe_texture_3d_heap");
  }
  if (UsesNativeTextureCubeHeap()) {
    emit_name("xe_texture_cube_heap");
  }
  if (!sampler_bindings_.empty()) {
    emit_name("xe_sampler_heap");
  }
}

void MslShaderTranslator::EmitNativeDrawConstantAliases() {
  if (!UsesNativeDrawConstants()) {
    return;
  }
  if (UsesNativeSystemConstants()) {
    EmitLine(
        "constant XeSystemConstants& xe_system = *xe_draw_constants.system;");
  }
  if (UsesNativeFloatConstants()) {
    EmitLine(
        "constant float4* xe_float_constants_data = "
        "xe_draw_constants.float_constants_data;");
  }
  if (UsesNativeBoolLoopConstants()) {
    EmitLine(
        "constant uint4* xe_bool_loop_constants_data = "
        "xe_draw_constants.bool_loop_constants_data;");
  }
  if (UsesNativeFetchConstants()) {
    EmitLine(
        "constant uint4* xe_fetch_constants_data = "
        "xe_draw_constants.fetch_constants_data;");
  }
  if (UsesNativeDescriptorIndices()) {
    EmitLine(
        "constant uint* xe_descriptor_indices = "
        "xe_draw_constants.descriptor_indices;");
  }
  if (UsesNativePrimitiveIndexConstants()) {
    EmitLine(
        "constant uint4& xe_primitive_index = "
        "*xe_draw_constants.primitive_index;");
  }
  EmitLine("");
}

bool MslShaderTranslator::IsRectangleListAsTriangleStrip() const {
  if (!is_vertex_shader()) {
    return false;
  }
  Modification modification = GetMslShaderModification();
  return modification.vertex.host_vertex_shader_type ==
         Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
}

bool MslShaderTranslator::IsDomainShader() const {
  if (!is_vertex_shader()) {
    return false;
  }
  Modification modification = GetMslShaderModification();
  return Shader::IsHostVertexShaderTypeDomain(
      modification.vertex.host_vertex_shader_type);
}

bool MslShaderTranslator::IsPointListAsMesh() const {
  if (!is_vertex_shader()) {
    return false;
  }
  Modification modification = GetMslShaderModification();
  return modification.vertex.host_vertex_shader_type ==
         Shader::HostVertexShaderType::kPointListAsMesh;
}

bool MslShaderTranslator::IsRectangleListAsMesh() const {
  if (!is_vertex_shader()) {
    return false;
  }
  Modification modification = GetMslShaderModification();
  return modification.vertex.host_vertex_shader_type ==
         Shader::HostVertexShaderType::kRectangleListAsMesh;
}

bool MslShaderTranslator::IsQuadListAsMesh() const {
  if (!is_vertex_shader()) {
    return false;
  }
  Modification modification = GetMslShaderModification();
  return modification.vertex.host_vertex_shader_type ==
         Shader::HostVertexShaderType::kQuadListAsMesh;
}

bool MslShaderTranslator::IsPrimitiveListAsMesh() const {
  return IsPointListAsMesh() || IsRectangleListAsMesh() || IsQuadListAsMesh();
}

void MslShaderTranslator::EmitInvocationStateInitialization() {
  if (UseScalarGprLocals()) {
    for (uint32_t i = 0; i < register_count(); ++i) {
      EmitLine("xe_gpr_" + std::to_string(i) + " = float4(0.0f);");
    }
  } else {
    EmitLine("for (uint xe_i = 0u; xe_i < " +
             std::to_string(std::max(register_count(), 1u)) +
             "u; ++xe_i) xe_gprs[xe_i] = float4(0.0f);");
  }
  EmitLine("xe_a0 = 0;");
  EmitLine("xe_aL = int4(0);");
  EmitLine("xe_loop_count = uint4(0u);");
  EmitLine("xe_p0 = false;");
  EmitLine("xe_ps = float4(0.0f);");
  if (uses_staged_vector_result_) {
    EmitLine("float4 xe_pv = float4(0.0f);");
  }
  if (uses_staged_scalar_result_) {
    EmitLine("float xe_ps_result = 0.0f;");
  }
  EmitLine("xe_texture_lod = 0.0f;");
  EmitLine("xe_texture_grad_h = float3(0.0f);");
  EmitLine("xe_texture_grad_v = float3(0.0f);");
  EmitLine("xe_pc = 0u;");
  if (control_flow_has_call_return_) {
    EmitLine("xe_call_stack_depth = 0u;");
  }
  EmitLine("xe_vfetch_address = 0u;");
  if (uses_memexport_) {
    EmitLine("xe_killed = false;");
    EmitLine("xe_memexport_allowed = true;");
    if (is_pixel_shader() && current_shader().memexport_eM_written()) {
      if (draw_resolution_scale_x_ > 1u || draw_resolution_scale_y_ > 1u) {
        EmitLine("uint2 xe_me_host_pixel = uint2(input.xe_position.xy);");
        if (draw_resolution_scale_x_ > 1u) {
          EmitLine(
              "xe_memexport_allowed = xe_memexport_allowed && "
              "((xe_me_host_pixel.x % " +
              std::to_string(draw_resolution_scale_x_) + "u) == " +
              std::to_string(draw_resolution_scale_x_ >> 1u) + "u);");
        }
        if (draw_resolution_scale_y_ > 1u) {
          EmitLine(
              "xe_memexport_allowed = xe_memexport_allowed && "
              "((xe_me_host_pixel.y % " +
              std::to_string(draw_resolution_scale_y_) + "u) == " +
              std::to_string(draw_resolution_scale_y_ >> 1u) + "u);");
        }
      }
      if (IsPixelShaderSampleRate()) {
        EmitLine(
            "xe_memexport_allowed = xe_memexport_allowed && "
            "(xe_sample_id == XeFirstBitLow(xe_sample_mask));");
      }
    }
    EmitLine("xe_memexport_address = float4(0.0f);");
    EmitLine("for (uint xe_me_i = 0u; xe_me_i < " +
             std::to_string(kNativeMslMemExportSlots) + "u; ++xe_me_i) {");
    Indent();
    EmitLine("xe_memexport_data[xe_me_i] = float4(0.0f);");
    Outdent();
    EmitLine("}");
    EmitLine("xe_memexport_data_valid_mask = 0u;");
  }
}

void MslShaderTranslator::EmitVertexOutputInitialization() {
  EmitLine("output.xe_position = float4(0.0f, 0.0f, 0.0f, 1.0f);");
  if (VertexShaderEmitsPointSizeOutput()) {
    EmitLine("output.xe_point_size = xe_system.xe_point_constant_diameter[0];");
  }
  for (uint32_t i = 0; i < VertexShaderClipDistanceCount(); ++i) {
    EmitLine("output.xe_clip_distance[" + std::to_string(i) + "] = 0.0f;");
  }
  for (uint32_t i = 0; i < VertexShaderCullDistanceCount(); ++i) {
    EmitLine("xe_cull_distance[" + std::to_string(i) + "] = 0.0f;");
  }
  for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
    EmitLine("output.xe_interpolator_" + std::to_string(i) +
             " = float4(0.0f);");
  }
  EmitLine("output.xe_point_parameters = float4(0.0f);");
  EmitLine(
      "xe_point_size_edge_flag_kill_vertex = "
      "float4(-1.0f, 0.0f, 0.0f, 0.0f);");
}

void MslShaderTranslator::EmitEntryPointBegin() {
  if (is_vertex_shader()) {
    EmitLine("inline XeVertexInvocationResult XeRunGuestVertex(");
    Indent();
    Emit(indent_string_ + "uint xe_vertex_id,\n");
    Emit(indent_string_ + "uint xe_instance_id");
    if (IsDomainShader()) {
      Emit(",\n" + indent_string_ + "float3 xe_domain_location,\n");
      Emit(indent_string_ + "uint4 xe_control_point_indices,\n");
      Emit(indent_string_ + "uint xe_patch_index");
    }
    EmitDirectResourceArguments(true, false);
    Emit(") {\n");
  } else {
    EmitLine(std::string(IsForceEarlyDepthStencilEnabled()
                             ? "[[early_fragment_tests]] "
                             : "") +
             "fragment XePixelOutput main_ps(");
    Indent();
    Emit(indent_string_ + "XePixelInput input [[stage_in]]");
    if (IsPixelShaderSampleRate()) {
      Emit(",\n" + indent_string_ + "uint xe_sample_id [[sample_id]]");
      if (current_shader().memexport_eM_written()) {
        Emit(",\n" + indent_string_ + "uint xe_sample_mask [[sample_mask]]");
      }
    }
    EmitDirectResourceArguments(true, true);
    Emit(") {\n");
  }
  Outdent();
  Indent();

  if (UseScalarGprLocals()) {
    for (uint32_t i = 0; i < register_count(); ++i) {
      EmitLine("float4 xe_gpr_" + std::to_string(i) + ";");
    }
  } else {
    EmitLine("float4 xe_gprs[" +
             std::to_string(std::max(register_count(), 1u)) + "];");
  }
  EmitLine("int xe_a0;");
  EmitLine("int4 xe_aL;");
  EmitLine("uint4 xe_loop_count;");
  EmitLine("bool xe_p0;");
  EmitLine("float4 xe_ps;");
  EmitLine("float xe_texture_lod;");
  EmitLine("float3 xe_texture_grad_h;");
  EmitLine("float3 xe_texture_grad_v;");
  EmitLine("uint xe_pc;");
  if (control_flow_has_call_return_) {
    EmitLine("uint xe_call_stack[" + std::to_string(kNativeMslCallStackDepth) +
             "]; ");
    EmitLine("(void)xe_call_stack;");
    EmitLine("uint xe_call_stack_depth;");
  }
  EmitLine("uint xe_vfetch_address;");
  if (uses_memexport_) {
    EmitLine("bool xe_killed;");
    EmitLine("bool xe_memexport_allowed;");
    EmitLine("float4 xe_memexport_address;");
    EmitLine("float4 xe_memexport_data[" +
             std::to_string(kNativeMslMemExportSlots) + "];");
    EmitLine("uint xe_memexport_data_valid_mask;");
  }
  EmitNativeDrawConstantAliases();
  EmitTextureFetchWordCache();
  EmitTextureFetchDerivedConstantCache();

  if (is_vertex_shader()) {
    Modification modification = GetMslShaderModification();
    const bool point_list_as_triangle_strip =
        modification.vertex.host_vertex_shader_type ==
        Shader::HostVertexShaderType::kPointListAsTriangleStrip;
    const bool rectangle_list_as_triangle_strip =
        modification.vertex.host_vertex_shader_type ==
        Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
    const bool domain_shader = IsDomainShader();
    EmitLine("XeVertexOutput output;");
    if (VertexShaderCullDistanceCount()) {
      EmitLine("float xe_cull_distance[" +
               std::to_string(VertexShaderCullDistanceCount()) + "];");
    }
    EmitLine("float4 xe_point_size_edge_flag_kill_vertex;");
    EmitLine("uint xe_point_sprite_vertex = 0u;");
    EmitLine("(void)xe_instance_id;");
    if (domain_shader) {
      EmitLine("(void)xe_vertex_id;");
    }
    if (rectangle_list_as_triangle_strip) {
      EmitLine("uint xe_rect_strip_vertex = xe_vertex_id & 3u;");
      EmitLine("uint xe_rect_index = xe_vertex_id >> 2u;");
      EmitLine("uint xe_rect_guest_indices[3];");
      EmitLine("for (uint xe_rect_i = 0u; xe_rect_i < 3u; ++xe_rect_i) {");
      Indent();
      EmitLine(
          "uint xe_rect_guest_index = XeLoadPrimitiveIndex("
          "xe_primitive_index, xe_shared_memory, "
          "xe_rect_index * 3u + xe_rect_i);");
      EmitLine("if ((xe_primitive_index.x & 1u) != 0u) {");
      Indent();
      EmitLine(
          "xe_rect_guest_index = XeEndianSwap("
          "xe_rect_guest_index, xe_system.xe_vertex_index_endian);");
      Outdent();
      EmitLine("}");
      EmitLine(
          "xe_rect_guest_index = (xe_rect_guest_index + "
          "xe_system.xe_vertex_index_offset) & 0x00FFFFFFu;");
      EmitLine(
          "xe_rect_guest_indices[xe_rect_i] = clamp("
          "xe_rect_guest_index, xe_system.xe_vertex_index_min_max[0], "
          "xe_system.xe_vertex_index_min_max[1]);");
      Outdent();
      EmitLine("}");
      EmitLine("float4 xe_rect_guest_positions[3];");
      uint32_t interpolator_mask = modification.vertex.interpolator_mask;
      for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
        if (interpolator_mask & (1u << i)) {
          EmitLine("float4 xe_rect_guest_interpolator_" + std::to_string(i) +
                   "[3];");
        }
      }
      EmitLine(
          "for (uint xe_rect_corner = 0u; xe_rect_corner < 3u; "
          "++xe_rect_corner) {");
      Indent();
    }
    EmitInvocationStateInitialization();
    if (rectangle_list_as_triangle_strip && uses_memexport_) {
      EmitLine("xe_memexport_allowed = xe_rect_strip_vertex == 0u;");
    }
    EmitVertexOutputInitialization();
    if (domain_shader) {
      EmitDomainRegisterInitialization();
    } else {
      EmitLine("uint xe_vertex_index = xe_vertex_id;");
      if (point_list_as_triangle_strip) {
        EmitLine("xe_point_sprite_vertex = xe_vertex_id & 3u;");
        EmitLine(
            "xe_vertex_index = XeLoadPrimitiveIndex("
            "xe_primitive_index, xe_shared_memory, xe_vertex_id >> 2u);");
        if (uses_memexport_) {
          EmitLine("xe_memexport_allowed = xe_point_sprite_vertex == 0u;");
        }
      } else if (rectangle_list_as_triangle_strip) {
        EmitLine("xe_vertex_index = xe_rect_guest_indices[xe_rect_corner];");
      } else {
        EmitLine(
            "xe_vertex_index = "
            "(xe_vertex_index != xe_system.xe_line_loop_closing_index) ? "
            "xe_vertex_index : 0u;");
      }
    }
    if (!rectangle_list_as_triangle_strip && !domain_shader) {
      EmitLine(
          "xe_vertex_index = XeEndianSwap(xe_vertex_index, "
          "xe_system.xe_vertex_index_endian);");
      EmitLine(
          "xe_vertex_index = (xe_vertex_index + "
          "xe_system.xe_vertex_index_offset) & 0x00FFFFFFu;");
      EmitLine(
          "xe_vertex_index = clamp(xe_vertex_index, "
          "xe_system.xe_vertex_index_min_max[0], "
          "xe_system.xe_vertex_index_min_max[1]);");
    }
    if (!domain_shader && register_count()) {
      EmitLine(RegisterToMsl(0, InstructionStorageAddressingMode::kAbsolute) +
               ".x = float(xe_vertex_index);");
    }
  } else {
    EmitInvocationStateInitialization();
    EmitLine("XePixelOutput output;");
    bool emitted_pixel_output = false;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_shader().writes_color_target(i)) {
        EmitLine("output.xe_color_" + std::to_string(i) + " = float4(0.0f);");
        emitted_pixel_output = true;
      }
    }
    if (PixelShaderWritesDepthOutput()) {
      EmitLine("output.xe_depth = input.xe_position.z;");
      emitted_pixel_output = true;
    }
    if (PixelShaderNeedsCoverageOutput()) {
      EmitLine("output.xe_coverage = 0xFFFFFFFFu;");
      emitted_pixel_output = true;
    }
    if (!emitted_pixel_output) {
      EmitLine("output.xe_dummy_color = float4(0.0f);");
    }
    Modification modification = GetMslShaderModification();
    uint32_t interpolator_mask = modification.pixel.interpolator_mask;
    uint32_t param_gen_interpolator =
        (modification.pixel.param_gen_enable &&
         modification.pixel.param_gen_interpolator < register_count())
            ? modification.pixel.param_gen_interpolator
            : UINT32_MAX;
    for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
      if ((interpolator_mask & (1u << i)) && i < register_count() &&
          i != param_gen_interpolator) {
        EmitLine(RegisterToMsl(i, InstructionStorageAddressingMode::kAbsolute) +
                 " = input.xe_interpolator_" + std::to_string(i) + ";");
      }
    }
    EmitPixelShaderParamGen();
  }
  EmitLine("");

  has_main_switch_ = !current_shader().label_addresses().empty() ||
                     !synthetic_label_addresses_.empty() ||
                     control_flow_has_call_return_;
  if (has_main_switch_) {
    EmitLine("while (true) {");
    Indent();
    EmitLine("switch (xe_pc) {");
    Indent();
    EmitLine("case 0u:");
    Indent();
    emitted_cf_case_indices_.insert(0u);
  }
}

void MslShaderTranslator::EmitMemExportMarkDirty(
    const InstructionResult& result) {
  if (!uses_memexport_ || result.storage_index >= kNativeMslMemExportSlots) {
    return;
  }
  uint32_t bit = 1u << result.storage_index;
  switch (result.storage_target) {
    case InstructionStorageTarget::kExportAddress:
      break;
    case InstructionStorageTarget::kExportData:
      EmitLine("xe_memexport_data_valid_mask |= " + std::to_string(bit) + "u;");
      break;
    default:
      break;
  }
}

void MslShaderTranslator::EmitMemExportFlush(const char* reason,
                                             uint32_t eM_mask) {
  if (!uses_memexport_) {
    return;
  }
  EmitLine(std::string("// Memexport flush: ") + reason);
  EmitLine("{");
  Indent();
  const uint32_t active_mask =
      eM_mask & ((UINT32_C(1) << kNativeMslMemExportSlots) - 1u);
  EmitLine("uint xe_me_active_mask = xe_memexport_data_valid_mask & " +
           std::to_string(active_mask) + "u;");
  EmitLine(
      "if (xe_me_active_mask != 0u && xe_memexport_allowed && "
      "!xe_killed && (xe_system.xe_flags & 1u) != 0u) {");
  Indent();
  EmitLine("uint4 xe_me_a = as_type<uint4>(xe_memexport_address);");
  EmitLine("uint4 xe_me_address_check = xe_me_a >> uint4(30u, 23u, 23u, 23u);");
  EmitLine("if (all(xe_me_address_check == uint4(1u, 0x96u, 0x96u, 0x96u))) {");
  Indent();
  EmitLine("bool xe_me_red_blue_swap = ((xe_me_a.z >> 19u) & 1u) != 0u;");
  EmitLine("uint xe_me_color_format = (xe_me_a.z >> 8u) & 0x3Fu;");
  EmitLine("bool xe_me_num_signed = ((xe_me_a.z >> 16u) & 1u) != 0u;");
  EmitLine("bool xe_me_num_integer = ((xe_me_a.z >> 17u) & 1u) != 0u;");
  EmitLine("uint xe_me_endian = xe_me_a.z & 7u;");
  EmitLine("uint xe_me_base_index = xe_me_a.y & 0x7FFFFFu;");
  EmitLine("uint xe_me_index_count = xe_me_a.w & 0x7FFFFFu;");
  EmitLine("uint xe_me_base_byte_address = xe_me_a.x << 2u;");
  EmitLine("for (uint xe_me_slot = 0u; xe_me_slot < " +
           std::to_string(kNativeMslMemExportSlots) + "u; ++xe_me_slot) {");
  Indent();
  EmitLine("uint xe_me_slot_bit = 1u << xe_me_slot;");
  EmitLine("if ((xe_me_active_mask & xe_me_slot_bit) == 0u) continue;");
  EmitLine("uint xe_me_element_index = xe_me_base_index + xe_me_slot;");
  EmitLine("if (xe_me_element_index >= xe_me_index_count) continue;");
  EmitLine("float4 xe_me_value = xe_memexport_data[xe_me_slot];");
  EmitLine("if (xe_me_red_blue_swap) xe_me_value = xe_me_value.zyxw;");
  EmitLine("uint xe_me_element_size = 0xFFFFFFFFu;");
  EmitLine(
      "uint4 xe_me_packed = XeMemExportPack("
      "xe_me_value, xe_me_color_format, xe_me_num_signed, "
      "xe_me_num_integer, xe_me_element_size);");
  EmitLine("if (xe_me_element_size == 0xFFFFFFFFu) continue;");
  EmitLine("if (xe_me_endian == " +
           std::to_string(uint32_t(xenos::Endian128::k8in64)) +
           "u) { xe_me_packed = xe_me_packed.yxwz; xe_me_endian = " +
           std::to_string(uint32_t(xenos::Endian128::k8in32)) + "u; }");
  EmitLine("if (xe_me_endian == " +
           std::to_string(uint32_t(xenos::Endian128::k8in128)) +
           "u) { xe_me_packed = xe_me_packed.wzyx; xe_me_endian = " +
           std::to_string(uint32_t(xenos::Endian128::k8in32)) + "u; }");
  EmitLine("if (xe_me_endian == " +
           std::to_string(uint32_t(xenos::Endian128::k8in16)) +
           "u || xe_me_endian == " +
           std::to_string(uint32_t(xenos::Endian128::k8in32)) +
           "u) { xe_me_packed = ((xe_me_packed & 0x00FF00FFu) << 8u) | "
           "((xe_me_packed >> 8u) & 0x00FF00FFu); }");
  EmitLine("if (xe_me_endian == " +
           std::to_string(uint32_t(xenos::Endian128::k8in32)) +
           "u || xe_me_endian == " +
           std::to_string(uint32_t(xenos::Endian128::k16in32)) +
           "u) { xe_me_packed = (xe_me_packed << 16u) | "
           "(xe_me_packed >> 16u); }");
  EmitLine(
      "uint xe_me_byte_address = xe_me_base_byte_address + "
      "(xe_me_element_index << xe_me_element_size);");
  EmitLine("switch (xe_me_element_size) {");
  Indent();
  EmitLine("case 0u:");
  Indent();
  EmitLine(
      "XeSharedMemoryStoreSubDword(xe_shared_memory, "
      "xe_me_byte_address, xe_me_packed.x, 8u);");
  EmitLine("break;");
  Outdent();
  EmitLine("case 1u:");
  Indent();
  EmitLine(
      "XeSharedMemoryStoreSubDword(xe_shared_memory, "
      "xe_me_byte_address, xe_me_packed.x, 16u);");
  EmitLine("break;");
  Outdent();
  EmitLine("case 2u:");
  Indent();
  EmitLine(
      "XeSharedMemoryStore(xe_shared_memory, xe_me_byte_address, "
      "xe_me_packed.x);");
  EmitLine("break;");
  Outdent();
  EmitLine("case 3u:");
  Indent();
  EmitLine(
      "XeSharedMemoryStore2(xe_shared_memory, xe_me_byte_address, "
      "xe_me_packed.xy);");
  EmitLine("break;");
  Outdent();
  EmitLine("case 4u:");
  Indent();
  EmitLine(
      "XeSharedMemoryStore4(xe_shared_memory, xe_me_byte_address, "
      "xe_me_packed);");
  EmitLine("break;");
  Outdent();
  EmitLine("default:");
  Indent();
  EmitLine("break;");
  Outdent();
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  EmitLine("xe_memexport_data_valid_mask &= ~xe_me_active_mask;");
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitRectangleListGuestLoopEnd() {
  if (!IsRectangleListAsTriangleStrip()) {
    return;
  }
  Modification modification = GetMslShaderModification();
  EmitLine("// Save this guest rectangle corner before running the next one");
  EmitLine("xe_rect_guest_positions[xe_rect_corner] = output.xe_position;");
  uint32_t interpolator_mask = modification.vertex.interpolator_mask;
  for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
    if (interpolator_mask & (1u << i)) {
      EmitLine("xe_rect_guest_interpolator_" + std::to_string(i) +
               "[xe_rect_corner] = output.xe_interpolator_" +
               std::to_string(i) + ";");
    }
  }
  if (uses_memexport_) {
    EmitMemExportFlush(
        "rectangle guest corner",
        current_shader().memexport_eM_potentially_written_before_end()
            ? current_shader().memexport_eM_potentially_written_before_end()
            : 0xFFu);
    EmitLine("xe_memexport_data_valid_mask = 0u;");
  }
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitRectangleListOutputSynthesis() {
  if (!IsRectangleListAsTriangleStrip()) {
    return;
  }
  Modification modification = GetMslShaderModification();
  EmitLine("// Synthesize the host rectangle-list triangle-strip vertex");
  EmitLine("{");
  Indent();
  EmitLine(
      "bool xe_rect_positions_have_nan = "
      "any(XeIsNaN(xe_rect_guest_positions[0])) || "
      "any(XeIsNaN(xe_rect_guest_positions[1])) || "
      "any(XeIsNaN(xe_rect_guest_positions[2]));");
  EmitLine("float2 xe_rect_position_xy_converted[3];");
  EmitLine("for (uint xe_rect_i = 0u; xe_rect_i < 3u; ++xe_rect_i) {");
  Indent();
  EmitLine("float4 xe_rect_p = xe_rect_guest_positions[xe_rect_i];");
  EmitLine(
      "float xe_rect_w = ((xe_system.xe_flags & 8u) != 0u) ? "
      "xe_rect_p.w : (1.0f / xe_rect_p.w);");
  EmitLine("float2 xe_rect_xy = xe_rect_p.xy;");
  EmitLine("if ((xe_system.xe_flags & 2u) != 0u) {");
  Indent();
  EmitLine("xe_rect_xy *= xe_rect_w;");
  Outdent();
  EmitLine("}");
  EmitLine(
      "xe_rect_position_xy_converted[xe_rect_i] = "
      "xe_rect_xy * float2(xe_system.xe_ndc_scale[0], "
      "xe_system.xe_ndc_scale[1]) + float2(xe_system.xe_ndc_offset[0], "
      "xe_system.xe_ndc_offset[1]) * xe_rect_w;");
  Outdent();
  EmitLine("}");
  EmitLine("float xe_rect_edge_lengths[3];");
  EmitLine("for (uint xe_rect_i = 0u; xe_rect_i < 3u; ++xe_rect_i) {");
  Indent();
  EmitLine(
      "float2 xe_rect_edge = "
      "xe_rect_position_xy_converted[(xe_rect_i + 2u) % 3u] - "
      "xe_rect_position_xy_converted[(xe_rect_i + 1u) % 3u];");
  EmitLine(
      "xe_rect_edge_lengths[xe_rect_i] = dot(xe_rect_edge, xe_rect_edge);");
  Outdent();
  EmitLine("}");
  EmitLine(
      "uint xe_rect_i0 = "
      "(xe_rect_edge_lengths[0] > xe_rect_edge_lengths[1] && "
      "xe_rect_edge_lengths[0] > xe_rect_edge_lengths[2]) ? 0u : "
      "((xe_rect_edge_lengths[1] > xe_rect_edge_lengths[2]) ? 1u : 2u);");
  EmitLine("uint xe_rect_i1 = (xe_rect_i0 + 1u) % 3u;");
  EmitLine("uint xe_rect_i2 = (xe_rect_i0 + 2u) % 3u;");
  EmitLine("float4 xe_rect_v0 = xe_rect_guest_positions[xe_rect_i0];");
  EmitLine("float4 xe_rect_v1 = xe_rect_guest_positions[xe_rect_i1];");
  EmitLine("float4 xe_rect_v2 = xe_rect_guest_positions[xe_rect_i2];");
  EmitLine("float4 xe_rect_v3 = xe_rect_v1 - xe_rect_v0 + xe_rect_v2;");
  EmitLine("if (xe_rect_strip_vertex == 0u) {");
  Indent();
  EmitLine("output.xe_position = xe_rect_v0;");
  Outdent();
  EmitLine("} else if (xe_rect_strip_vertex == 1u) {");
  Indent();
  EmitLine("output.xe_position = xe_rect_v1;");
  Outdent();
  EmitLine("} else if (xe_rect_strip_vertex == 2u) {");
  Indent();
  EmitLine("output.xe_position = xe_rect_v2;");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine("output.xe_position = xe_rect_v3;");
  Outdent();
  EmitLine("}");
  EmitLine("if (xe_rect_positions_have_nan) {");
  Indent();
  EmitLine("output.xe_position = float4(0.0f / 0.0f);");
  Outdent();
  EmitLine("}");
  uint32_t interpolator_mask = modification.vertex.interpolator_mask;
  for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
    if (!(interpolator_mask & (1u << i))) {
      continue;
    }
    std::string name = "xe_rect_guest_interpolator_" + std::to_string(i);
    std::string out = "output.xe_interpolator_" + std::to_string(i);
    EmitLine("{");
    Indent();
    EmitLine("float4 xe_rect_attr0 = " + name + "[xe_rect_i0];");
    EmitLine("float4 xe_rect_attr1 = " + name + "[xe_rect_i1];");
    EmitLine("float4 xe_rect_attr2 = " + name + "[xe_rect_i2];");
    EmitLine(
        "float4 xe_rect_attr3 = xe_rect_attr1 - xe_rect_attr0 + "
        "xe_rect_attr2;");
    EmitLine("if (xe_rect_strip_vertex == 0u) {");
    Indent();
    EmitLine(out + " = xe_rect_attr0;");
    Outdent();
    EmitLine("} else if (xe_rect_strip_vertex == 1u) {");
    Indent();
    EmitLine(out + " = xe_rect_attr1;");
    Outdent();
    EmitLine("} else if (xe_rect_strip_vertex == 2u) {");
    Indent();
    EmitLine(out + " = xe_rect_attr2;");
    Outdent();
    EmitLine("} else {");
    Indent();
    EmitLine(out + " = xe_rect_attr3;");
    Outdent();
    EmitLine("}");
    Outdent();
    EmitLine("}");
  }
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitDomainRegisterInitialization() {
  if (!IsDomainShader() || !register_count()) {
    return;
  }
  Modification modification = GetMslShaderModification();
  Shader::HostVertexShaderType host_vertex_shader_type =
      modification.vertex.host_vertex_shader_type;
  auto reg = [&](uint32_t index) {
    return RegisterToMsl(index, InstructionStorageAddressingMode::kAbsolute);
  };
  switch (host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      EmitLine(reg(0) + ".xyz = xe_domain_location.zyx;");
      if (register_count() >= 2) {
        EmitLine(reg(1) + ".xyz = float3(xe_control_point_indices.xyz);");
      }
      break;
    case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      EmitLine(reg(0) + ".xyz = xe_domain_location.zyx;");
      if (register_count() >= 2) {
        EmitLine(reg(1) + ".x = float(xe_patch_index);");
        EmitLine(reg(1) + ".y = 0.0f;");
      }
      break;
    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      EmitLine(reg(0) + ".xy = xe_domain_location.xy;");
      EmitLine(reg(0) + ".z = float(xe_control_point_indices.x);");
      if (register_count() >= 2) {
        EmitLine(reg(1) + ".xyz = float3(xe_control_point_indices.yzw);");
      }
      break;
    case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      EmitLine(reg(0) + ".x = float(xe_patch_index);");
      EmitLine(reg(0) + ".yz = xe_domain_location.xy;");
      if (register_count() >= 2) {
        EmitLine(reg(1) + ".x = 0.0f;");
      }
      break;
    case Shader::HostVertexShaderType::kLineDomainCPIndexed:
    case Shader::HostVertexShaderType::kLineDomainPatchIndexed:
      EmitNativeMslUnsupported(
          "native MSL line-domain tessellation is not implemented");
      break;
    default:
      EmitNativeMslUnsupported(fmt::format(
          "native MSL unsupported domain host vertex shader type {}",
          uint32_t(host_vertex_shader_type)));
      break;
  }
}

void MslShaderTranslator::EmitVertexFinalizerFunction() {
  if (!is_vertex_shader()) {
    return;
  }
  EmitLine("");
  EmitLine("inline XeVertexOutput XeFinalizeVertexOutput(");
  Indent();
  EmitLine("XeVertexInvocationResult invocation,");
  EmitLine("constant XeSystemConstants& xe_system) {");
  Outdent();
  Indent();
  EmitLine("XeVertexOutput output = invocation.output;");
  EmitLine(
      "float4 xe_point_size_edge_flag_kill_vertex = "
      "invocation.xe_point_size_edge_flag_kill_vertex;");
  EmitLine("uint xe_point_sprite_vertex = invocation.xe_point_sprite_vertex;");
  EmitLine("(void)xe_point_size_edge_flag_kill_vertex;");
  EmitLine("(void)xe_point_sprite_vertex;");
  EmitVertexShaderEpilogue();
  EmitLine("return output;");
  Outdent();
  EmitLine("}");
  if (IsPrimitiveListAsMesh() || IsDomainShader()) {
    EmitLine("");
    EmitLine(
        "inline XeTriangleMeshVertexOutput XeToTriangleMeshVertexOutput("
        "XeVertexOutput output) {");
    Indent();
    EmitLine("XeTriangleMeshVertexOutput mesh_output;");
    EmitLine("mesh_output.xe_position = output.xe_position;");
    for (uint32_t i = 0; i < VertexShaderClipDistanceCount(); ++i) {
      EmitLine("mesh_output.xe_clip_distance[" + std::to_string(i) +
               "] = output.xe_clip_distance[" + std::to_string(i) + "];");
    }
    for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
      const std::string interpolator = "xe_interpolator_" + std::to_string(i);
      EmitLine("mesh_output." + interpolator + " = output." + interpolator +
               ";");
    }
    EmitLine("mesh_output.xe_point_parameters = output.xe_point_parameters;");
    EmitLine("return mesh_output;");
    Outdent();
    EmitLine("}");
  }
}

void MslShaderTranslator::EmitPointMeshEntryPoint() {
  if (!is_vertex_shader()) {
    return;
  }
  EmitLine("");
  EmitLine(
      "using XePointMesh = mesh<XeTriangleMeshVertexOutput, void, 4, 2, "
      "topology::triangle>;");
  EmitLine("[[mesh]] void main_point_mesh(");
  Indent();
  EmitLine("uint xe_thread_index [[thread_index_in_threadgroup]],");
  EmitLine("uint3 xe_point_mesh_id [[threadgroup_position_in_grid]],");
  Emit(indent_string_ + "XePointMesh xe_mesh");
  EmitDirectResourceArguments(true, true);
  Emit(") {\n");
  Outdent();
  Indent();
  EmitNativeDrawConstantAliases();
  EmitLine("if (xe_thread_index != 0u) {");
  Indent();
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine(
      "uint xe_point_guest_index = XeLoadPrimitiveIndex("
      "xe_primitive_index, xe_shared_memory, xe_point_mesh_id.x);");
  EmitLine("XeVertexInvocationResult xe_point_guest = XeRunGuestVertex(");
  Indent();
  Emit(indent_string_ + "xe_point_guest_index,\n");
  Emit(indent_string_ + "0u");
  EmitDirectResourceArgumentNames(true);
  Emit(");\n");
  Outdent();
  if (VertexShaderCullDistanceCount()) {
    EmitLine("bool xe_point_culled = false;");
    for (uint32_t i = 0; i < VertexShaderCullDistanceCount(); ++i) {
      EmitLine(
          "xe_point_culled = xe_point_culled || "
          "(xe_point_guest.xe_cull_distance[" +
          std::to_string(i) + "] < 0.0f);");
    }
    EmitLine("if (xe_point_culled) {");
    Indent();
    EmitLine("xe_mesh.set_primitive_count(0u);");
    EmitLine("return;");
    Outdent();
    EmitLine("}");
  }
  EmitLine(
      "XeVertexOutput xe_point_base = "
      "XeFinalizeVertexOutput(xe_point_guest, xe_system);");
  EmitLine("if (any(XeIsNaN(xe_point_base.xe_position))) {");
  Indent();
  EmitLine("xe_mesh.set_primitive_count(0u);");
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine(
      "float2 xe_point_diameter = float2("
      "xe_system.xe_point_constant_diameter[0], "
      "xe_system.xe_point_constant_diameter[1]);");
  if (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b001) {
    EmitLine(
        "if (xe_point_guest.xe_point_size_edge_flag_kill_vertex.x >= 0.0f) {");
    Indent();
    EmitLine(
        "xe_point_diameter = "
        "float2(xe_point_guest.xe_point_size_edge_flag_kill_vertex.x);");
    Outdent();
    EmitLine("}");
  }
  EmitLine(
      "float2 xe_point_radius = xe_point_diameter * "
      "float2(xe_system.xe_point_screen_diameter_to_ndc_radius[0], "
      "xe_system.xe_point_screen_diameter_to_ndc_radius[1]) * "
      "xe_point_base.xe_position.w;");
  EmitLine("float2 xe_point_positive[4] = {");
  Indent();
  EmitLine("float2(0.0f, 0.0f), float2(0.0f, 1.0f),");
  EmitLine("float2(1.0f, 0.0f), float2(1.0f, 1.0f)");
  Outdent();
  EmitLine("};");
  EmitLine("for (uint xe_point_i = 0u; xe_point_i < 4u; ++xe_point_i) {");
  Indent();
  EmitLine("XeVertexOutput xe_point_output = xe_point_base;");
  EmitLine(
      "float2 xe_point_offset = select(-xe_point_radius, xe_point_radius, "
      "xe_point_positive[xe_point_i] != float2(0.0f));");
  EmitLine("xe_point_output.xe_position.xy += xe_point_offset;");
  EmitLine(
      "xe_point_output.xe_point_parameters.xy = "
      "xe_point_positive[xe_point_i];");
  EmitLine(
      "xe_mesh.set_vertex(xe_point_i, "
      "XeToTriangleMeshVertexOutput(xe_point_output));");
  Outdent();
  EmitLine("}");
  EmitLine("xe_mesh.set_primitive_count(2u);");
  EmitLine("xe_mesh.set_index(0u, 0u);");
  EmitLine("xe_mesh.set_index(1u, 1u);");
  EmitLine("xe_mesh.set_index(2u, 2u);");
  EmitLine("xe_mesh.set_index(3u, 2u);");
  EmitLine("xe_mesh.set_index(4u, 1u);");
  EmitLine("xe_mesh.set_index(5u, 3u);");
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitRectangleMeshEntryPoint() {
  if (!is_vertex_shader()) {
    return;
  }
  Modification modification = GetMslShaderModification();
  const uint32_t interpolator_mask = modification.vertex.interpolator_mask;
  EmitLine("");
  EmitLine(
      "using XeRectangleMesh = mesh<XeTriangleMeshVertexOutput, void, 4, 2, "
      "topology::triangle>;");
  EmitLine("[[mesh]] void main_rect_mesh(");
  Indent();
  EmitLine("uint xe_thread_index [[thread_index_in_threadgroup]],");
  EmitLine("uint3 xe_rect_mesh_id [[threadgroup_position_in_grid]],");
  Emit(indent_string_ + "XeRectangleMesh xe_mesh");
  EmitDirectResourceArguments(true, true);
  Emit(") {\n");
  Outdent();
  Indent();
  EmitNativeDrawConstantAliases();
  EmitLine("if (xe_thread_index != 0u) {");
  Indent();
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine("uint xe_rect_base_vertex = xe_rect_mesh_id.x * 3u;");
  EmitLine("uint xe_rect_guest_indices[3];");
  EmitLine("for (uint xe_rect_i = 0u; xe_rect_i < 3u; ++xe_rect_i) {");
  Indent();
  EmitLine(
      "xe_rect_guest_indices[xe_rect_i] = XeLoadPrimitiveIndex("
      "xe_primitive_index, xe_shared_memory, "
      "xe_rect_base_vertex + xe_rect_i);");
  Outdent();
  EmitLine("}");
  EmitLine("XeVertexInvocationResult xe_rect_guest[3];");
  EmitLine("for (uint xe_rect_i = 0u; xe_rect_i < 3u; ++xe_rect_i) {");
  Indent();
  EmitLine("xe_rect_guest[xe_rect_i] = XeRunGuestVertex(");
  Indent();
  Emit(indent_string_ + "xe_rect_guest_indices[xe_rect_i],\n");
  Emit(indent_string_ + "0u");
  EmitDirectResourceArgumentNames(true);
  Emit(");\n");
  Outdent();
  Outdent();
  EmitLine("}");
  EmitLine(
      "bool xe_rect_positions_have_nan = "
      "any(XeIsNaN(xe_rect_guest[0].output.xe_position)) || "
      "any(XeIsNaN(xe_rect_guest[1].output.xe_position)) || "
      "any(XeIsNaN(xe_rect_guest[2].output.xe_position));");
  if (VertexShaderCullDistanceCount()) {
    EmitLine("bool xe_rect_culled = false;");
    for (uint32_t i = 0; i < VertexShaderCullDistanceCount(); ++i) {
      EmitLine(
          "xe_rect_culled = xe_rect_culled || "
          "(xe_rect_guest[0].xe_cull_distance[" +
          std::to_string(i) +
          "] < 0.0f && "
          "xe_rect_guest[1].xe_cull_distance[" +
          std::to_string(i) +
          "] < 0.0f && "
          "xe_rect_guest[2].xe_cull_distance[" +
          std::to_string(i) + "] < 0.0f);");
    }
    EmitLine("if (xe_rect_culled) {");
    Indent();
    EmitLine("xe_mesh.set_primitive_count(0u);");
    EmitLine("return;");
    Outdent();
    EmitLine("}");
  }
  EmitLine("float2 xe_rect_position_xy_converted[3];");
  EmitLine("for (uint xe_rect_i = 0u; xe_rect_i < 3u; ++xe_rect_i) {");
  Indent();
  EmitLine("float4 xe_rect_p = xe_rect_guest[xe_rect_i].output.xe_position;");
  EmitLine(
      "float xe_rect_w = ((xe_system.xe_flags & 8u) != 0u) ? "
      "xe_rect_p.w : (1.0f / xe_rect_p.w);");
  EmitLine("float2 xe_rect_xy = xe_rect_p.xy;");
  EmitLine("if ((xe_system.xe_flags & 2u) != 0u) {");
  Indent();
  EmitLine("xe_rect_xy *= xe_rect_w;");
  Outdent();
  EmitLine("}");
  EmitLine(
      "xe_rect_position_xy_converted[xe_rect_i] = "
      "xe_rect_xy * float2(xe_system.xe_ndc_scale[0], "
      "xe_system.xe_ndc_scale[1]) + float2(xe_system.xe_ndc_offset[0], "
      "xe_system.xe_ndc_offset[1]) * xe_rect_w;");
  Outdent();
  EmitLine("}");
  EmitLine("float xe_rect_edge_lengths[3];");
  EmitLine("for (uint xe_rect_i = 0u; xe_rect_i < 3u; ++xe_rect_i) {");
  Indent();
  EmitLine(
      "float2 xe_rect_edge = "
      "xe_rect_position_xy_converted[(xe_rect_i + 2u) % 3u] - "
      "xe_rect_position_xy_converted[(xe_rect_i + 1u) % 3u];");
  EmitLine(
      "xe_rect_edge_lengths[xe_rect_i] = dot(xe_rect_edge, xe_rect_edge);");
  Outdent();
  EmitLine("}");
  EmitLine(
      "uint xe_rect_i0 = "
      "(xe_rect_edge_lengths[0] > xe_rect_edge_lengths[1] && "
      "xe_rect_edge_lengths[0] > xe_rect_edge_lengths[2]) ? 0u : "
      "((xe_rect_edge_lengths[1] > xe_rect_edge_lengths[2]) ? 1u : 2u);");
  EmitLine("uint xe_rect_i1 = (xe_rect_i0 + 1u) % 3u;");
  EmitLine("uint xe_rect_i2 = (xe_rect_i0 + 2u) % 3u;");
  EmitLine(
      "XeVertexInvocationResult xe_rect_out0 = xe_rect_guest[xe_rect_i0];");
  EmitLine(
      "XeVertexInvocationResult xe_rect_out1 = xe_rect_guest[xe_rect_i1];");
  EmitLine(
      "XeVertexInvocationResult xe_rect_out2 = xe_rect_guest[xe_rect_i2];");
  EmitLine(
      "XeVertexInvocationResult xe_rect_out3 = xe_rect_guest[xe_rect_i2];");
  EmitLine(
      "xe_rect_out3.output.xe_position = "
      "xe_rect_out1.output.xe_position - "
      "xe_rect_out0.output.xe_position + "
      "xe_rect_out2.output.xe_position;");
  for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
    if (!(interpolator_mask & (1u << i))) {
      continue;
    }
    const std::string attr = "xe_interpolator_" + std::to_string(i);
    EmitLine("xe_rect_out3.output." + attr + " = xe_rect_out1.output." + attr +
             " - xe_rect_out0.output." + attr + " + xe_rect_out2.output." +
             attr + ";");
  }
  EmitLine(
      "XeTriangleMeshVertexOutput xe_rect_v0 = "
      "XeToTriangleMeshVertexOutput("
      "XeFinalizeVertexOutput(xe_rect_out0, xe_system));");
  EmitLine(
      "XeTriangleMeshVertexOutput xe_rect_v1 = "
      "XeToTriangleMeshVertexOutput("
      "XeFinalizeVertexOutput(xe_rect_out1, xe_system));");
  EmitLine(
      "XeTriangleMeshVertexOutput xe_rect_v2 = "
      "XeToTriangleMeshVertexOutput("
      "XeFinalizeVertexOutput(xe_rect_out2, xe_system));");
  EmitLine(
      "XeTriangleMeshVertexOutput xe_rect_v3 = "
      "XeToTriangleMeshVertexOutput("
      "XeFinalizeVertexOutput(xe_rect_out3, xe_system));");
  EmitLine(
      "if (xe_rect_positions_have_nan || any(XeIsNaN(xe_rect_v0.xe_position)) "
      "|| any(XeIsNaN(xe_rect_v1.xe_position)) || "
      "any(XeIsNaN(xe_rect_v2.xe_position)) || "
      "any(XeIsNaN(xe_rect_v3.xe_position))) {");
  Indent();
  EmitLine("xe_mesh.set_primitive_count(0u);");
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine("xe_mesh.set_primitive_count(2u);");
  EmitLine("xe_mesh.set_vertex(0u, xe_rect_v0);");
  EmitLine("xe_mesh.set_vertex(1u, xe_rect_v1);");
  EmitLine("xe_mesh.set_vertex(2u, xe_rect_v2);");
  EmitLine("xe_mesh.set_vertex(3u, xe_rect_v3);");
  EmitLine("xe_mesh.set_index(0u, 0u);");
  EmitLine("xe_mesh.set_index(1u, 1u);");
  EmitLine("xe_mesh.set_index(2u, 2u);");
  EmitLine("xe_mesh.set_index(3u, 2u);");
  EmitLine("xe_mesh.set_index(4u, 1u);");
  EmitLine("xe_mesh.set_index(5u, 3u);");
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitQuadMeshEntryPoint() {
  if (!is_vertex_shader()) {
    return;
  }
  EmitLine("");
  EmitLine(
      "using XeQuadMesh = mesh<XeTriangleMeshVertexOutput, void, 4, 2, "
      "topology::triangle>;");
  EmitLine("[[mesh]] void main_quad_mesh(");
  Indent();
  EmitLine("uint xe_thread_index [[thread_index_in_threadgroup]],");
  EmitLine("uint3 xe_quad_mesh_id [[threadgroup_position_in_grid]],");
  Emit(indent_string_ + "XeQuadMesh xe_mesh");
  EmitDirectResourceArguments(true, true);
  Emit(") {\n");
  Outdent();
  Indent();
  EmitNativeDrawConstantAliases();
  EmitLine("if (xe_thread_index != 0u) {");
  Indent();
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine("uint xe_quad_base_vertex = xe_quad_mesh_id.x * 4u;");
  EmitLine("uint xe_quad_guest_indices[4];");
  EmitLine("for (uint xe_quad_i = 0u; xe_quad_i < 4u; ++xe_quad_i) {");
  Indent();
  EmitLine(
      "xe_quad_guest_indices[xe_quad_i] = XeLoadPrimitiveIndex("
      "xe_primitive_index, xe_shared_memory, "
      "xe_quad_base_vertex + xe_quad_i);");
  Outdent();
  EmitLine("}");
  EmitLine("XeVertexInvocationResult xe_quad_guest[4];");
  EmitLine("for (uint xe_quad_i = 0u; xe_quad_i < 4u; ++xe_quad_i) {");
  Indent();
  EmitLine("xe_quad_guest[xe_quad_i] = XeRunGuestVertex(");
  Indent();
  Emit(indent_string_ + "xe_quad_guest_indices[xe_quad_i],\n");
  Emit(indent_string_ + "0u");
  EmitDirectResourceArgumentNames(true);
  Emit(");\n");
  Outdent();
  Outdent();
  EmitLine("}");
  if (VertexShaderCullDistanceCount()) {
    EmitLine("bool xe_quad_culled = false;");
    for (uint32_t i = 0; i < VertexShaderCullDistanceCount(); ++i) {
      EmitLine(
          "xe_quad_culled = xe_quad_culled || "
          "(xe_quad_guest[0].xe_cull_distance[" +
          std::to_string(i) +
          "] < 0.0f && "
          "xe_quad_guest[1].xe_cull_distance[" +
          std::to_string(i) +
          "] < 0.0f && "
          "xe_quad_guest[2].xe_cull_distance[" +
          std::to_string(i) +
          "] < 0.0f && "
          "xe_quad_guest[3].xe_cull_distance[" +
          std::to_string(i) + "] < 0.0f);");
    }
    EmitLine("if (xe_quad_culled) {");
    Indent();
    EmitLine("xe_mesh.set_primitive_count(0u);");
    EmitLine("return;");
    Outdent();
    EmitLine("}");
  }
  EmitLine("XeVertexOutput xe_quad_output[4];");
  EmitLine("bool xe_quad_positions_have_nan = false;");
  EmitLine("for (uint xe_quad_i = 0u; xe_quad_i < 4u; ++xe_quad_i) {");
  Indent();
  EmitLine(
      "xe_quad_positions_have_nan = xe_quad_positions_have_nan || "
      "any(XeIsNaN(xe_quad_guest[xe_quad_i].output.xe_position));");
  EmitLine(
      "xe_quad_output[xe_quad_i] = "
      "XeFinalizeVertexOutput(xe_quad_guest[xe_quad_i], xe_system);");
  EmitLine(
      "xe_quad_positions_have_nan = xe_quad_positions_have_nan || "
      "any(XeIsNaN(xe_quad_output[xe_quad_i].xe_position));");
  Outdent();
  EmitLine("}");
  EmitLine("if (xe_quad_positions_have_nan) {");
  Indent();
  EmitLine("xe_mesh.set_primitive_count(0u);");
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine("uint xe_quad_order[4] = {0u, 1u, 3u, 2u};");
  EmitLine("for (uint xe_quad_i = 0u; xe_quad_i < 4u; ++xe_quad_i) {");
  Indent();
  EmitLine(
      "xe_mesh.set_vertex(xe_quad_i, XeToTriangleMeshVertexOutput("
      "xe_quad_output[xe_quad_order[xe_quad_i]]));");
  Outdent();
  EmitLine("}");
  EmitLine("xe_mesh.set_primitive_count(2u);");
  EmitLine("xe_mesh.set_index(0u, 0u);");
  EmitLine("xe_mesh.set_index(1u, 1u);");
  EmitLine("xe_mesh.set_index(2u, 2u);");
  EmitLine("xe_mesh.set_index(3u, 2u);");
  EmitLine("xe_mesh.set_index(4u, 1u);");
  EmitLine("xe_mesh.set_index(5u, 3u);");
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitTessellationMeshEntryPoints() {
  if (!is_vertex_shader() || !IsDomainShader()) {
    return;
  }
  Modification modification = GetMslShaderModification();
  Shader::HostVertexShaderType host_vertex_shader_type =
      modification.vertex.host_vertex_shader_type;
  const bool is_triangle =
      host_vertex_shader_type ==
          Shader::HostVertexShaderType::kTriangleDomainCPIndexed ||
      host_vertex_shader_type ==
          Shader::HostVertexShaderType::kTriangleDomainPatchIndexed;
  const bool is_quad =
      host_vertex_shader_type ==
          Shader::HostVertexShaderType::kQuadDomainCPIndexed ||
      host_vertex_shader_type ==
          Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
  const bool cp_indexed =
      host_vertex_shader_type ==
          Shader::HostVertexShaderType::kTriangleDomainCPIndexed ||
      host_vertex_shader_type ==
          Shader::HostVertexShaderType::kQuadDomainCPIndexed;
  if (!is_triangle && !is_quad) {
    return;
  }

  const uint32_t control_point_count = is_quad ? 4u : 3u;

  EmitLine("");
  EmitLine("struct XeTessellationPayload {");
  Indent();
  EmitLine("uint xe_patch_index;");
  EmitLine("uint4 xe_control_point_indices;");
  EmitLine("uint xe_tess_factor;");
  EmitLine("uint xe_is_quad;");
  Outdent();
  EmitLine("};");
  EmitLine("");
  EmitLine(
      "inline uint XeNativeTessControlIndex(uint raw_index, "
      "constant XeSystemConstants& xe_system) {");
  Indent();
  EmitLine(
      "uint swapped = XeEndianSwap(raw_index, "
      "xe_system.xe_vertex_index_endian);");
  EmitLine(
      "uint biased = (swapped + xe_system.xe_vertex_index_offset) & "
      "0xFFFFFFu;");
  EmitLine(
      "return clamp(biased, xe_system.xe_vertex_index_min_max[0], "
      "xe_system.xe_vertex_index_min_max[1]);");
  Outdent();
  EmitLine("}");
  EmitLine("");
  EmitLine(
      "inline float XeNativeTessAdaptiveFactor(uint raw_factor, "
      "constant XeSystemConstants& xe_system) {");
  Indent();
  EmitLine(
      "float factor = asfloat(XeEndianSwap(raw_factor, "
      "xe_system.xe_vertex_index_endian)) + 1.0f;");
  EmitLine(
      "return clamp(factor, xe_system.xe_tessellation_factor_range[0], "
      "xe_system.xe_tessellation_factor_range[1]);");
  Outdent();
  EmitLine("}");
  EmitLine("");
  EmitLine(
      "inline uint XeNativeTessFactorToSubdivisions(float factor, "
      "uint tess_mode) {");
  Indent();
  EmitLine("float sanitized = XeIsNaN(factor) ? 1.0f : factor;");
  EmitLine("uint subdivisions = uint(clamp(ceil(sanitized), 1.0f, 64.0f));");
  EmitLine("if (tess_mode == 1u) {");
  Indent();
  EmitLine("subdivisions = max(2u, (subdivisions + 1u) & ~1u);");
  Outdent();
  EmitLine("}");
  EmitLine("return min(subdivisions, 64u);");
  Outdent();
  EmitLine("}");
  EmitLine("");
  EmitLine(
      "[[object, max_total_threads_per_threadgroup(1), "
      "max_total_threadgroups_per_mesh_grid(8192)]]");
  EmitLine("void main_tess_object(");
  Indent();
  EmitLine("uint xe_thread_index [[thread_index_in_threadgroup]],");
  EmitLine("uint3 xe_patch_grid_id [[threadgroup_position_in_grid]],");
  EmitLine("object_data XeTessellationPayload& xe_payload [[payload]],");
  Emit(indent_string_ + "mesh_grid_properties xe_mesh_grid");
  EmitDirectResourceArguments(true, true);
  Emit(") {\n");
  Outdent();
  Indent();
  EmitNativeDrawConstantAliases();
  EmitLine("if (xe_thread_index != 0u) {");
  Indent();
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine("constexpr uint xe_tess_control_point_count = " +
           std::to_string(control_point_count) + "u;");
  EmitLine("uint xe_tess_mode = xe_primitive_index.w & 3u;");
  EmitLine("uint xe_patch_id = xe_patch_grid_id.x;");
  EmitLine("uint xe_patch_base = xe_patch_id * xe_tess_control_point_count;");
  EmitLine("float xe_edge_factors[4];");
  EmitLine("for (uint xe_i = 0u; xe_i < 4u; ++xe_i) {");
  Indent();
  EmitLine(
      "xe_edge_factors[xe_i] = "
      "xe_system.xe_tessellation_factor_range[1];");
  Outdent();
  EmitLine("}");
  EmitLine("uint4 xe_control_indices = uint4(0u);");
  if (cp_indexed) {
    EmitLine(
        "for (uint xe_i = 0u; xe_i < xe_tess_control_point_count; ++xe_i) {");
    Indent();
    EmitLine(
        "uint xe_raw_index = XeLoadPrimitiveIndex(xe_primitive_index, "
        "xe_shared_memory, xe_patch_base + xe_i);");
    EmitLine(
        "xe_control_indices[xe_i] = "
        "XeNativeTessControlIndex(xe_raw_index, xe_system);");
    EmitLine("if (xe_tess_mode == 2u) {");
    Indent();
    EmitLine(
        "xe_edge_factors[xe_i] = "
        "XeNativeTessAdaptiveFactor(xe_raw_index, xe_system);");
    Outdent();
    EmitLine("}");
    Outdent();
    EmitLine("}");
  } else {
    EmitLine("if (xe_tess_mode == 2u) {");
    Indent();
    EmitLine(
        "for (uint xe_i = 0u; xe_i < xe_tess_control_point_count; ++xe_i) {");
    Indent();
    EmitLine(
        "uint xe_raw_factor = XeLoadPrimitiveIndex(xe_primitive_index, "
        "xe_shared_memory, xe_patch_base + xe_i);");
    EmitLine(
        "xe_edge_factors[xe_i] = "
        "XeNativeTessAdaptiveFactor(xe_raw_factor, xe_system);");
    Outdent();
    EmitLine("}");
    Outdent();
    EmitLine("}");
  }
  EmitLine("float xe_tess_factor = xe_edge_factors[0];");
  EmitLine(
      "for (uint xe_i = 1u; xe_i < xe_tess_control_point_count; ++xe_i) {");
  Indent();
  EmitLine("xe_tess_factor = max(xe_tess_factor, xe_edge_factors[xe_i]);");
  Outdent();
  EmitLine("}");
  EmitLine(
      "uint xe_tess_factor_u = XeNativeTessFactorToSubdivisions("
      "xe_tess_factor, xe_tess_mode);");
  if (cp_indexed) {
    EmitLine("uint xe_patch_index = xe_patch_id;");
  } else {
    EmitLine("uint xe_patch_index = 0u;");
    EmitLine("if (xe_tess_mode == 2u) {");
    Indent();
    EmitLine(
        "xe_patch_index = clamp((xe_patch_id + "
        "xe_system.xe_vertex_index_offset) & 0xFFFFFFu, "
        "xe_system.xe_vertex_index_min_max[0], "
        "xe_system.xe_vertex_index_min_max[1]);");
    Outdent();
    EmitLine("} else {");
    Indent();
    EmitLine(
        "uint xe_raw_patch_index = XeLoadPrimitiveIndex(xe_primitive_index, "
        "xe_shared_memory, xe_patch_id);");
    EmitLine(
        "xe_patch_index = XeNativeTessControlIndex(xe_raw_patch_index, "
        "xe_system);");
    Outdent();
    EmitLine("}");
  }
  if (is_quad) {
    EmitLine(
        "uint xe_micro_triangle_count = "
        "xe_tess_factor_u * xe_tess_factor_u * 2u;");
  } else {
    EmitLine(
        "uint xe_micro_triangle_count = "
        "xe_tess_factor_u * xe_tess_factor_u;");
  }
  EmitLine("if (xe_micro_triangle_count == 0u) {");
  Indent();
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine("xe_payload.xe_patch_index = xe_patch_index;");
  EmitLine("xe_payload.xe_control_point_indices = xe_control_indices;");
  EmitLine("xe_payload.xe_tess_factor = xe_tess_factor_u;");
  EmitLine(std::string("xe_payload.xe_is_quad = ") + (is_quad ? "1u;" : "0u;"));
  EmitLine(
      "xe_mesh_grid.set_threadgroups_per_grid("
      "uint3(xe_micro_triangle_count, 1u, 1u));");
  Outdent();
  EmitLine("}");
  EmitLine("");
  EmitLine(
      "using XeTessellationMesh = "
      "mesh<XeTriangleMeshVertexOutput, void, 3, 1, topology::triangle>;");
  EmitLine("[[mesh, max_total_threads_per_threadgroup(1)]]");
  EmitLine("void main_tess_mesh(");
  Indent();
  EmitLine("uint xe_thread_index [[thread_index_in_threadgroup]],");
  EmitLine("uint3 xe_micro_triangle_id [[threadgroup_position_in_grid]],");
  EmitLine("XeTessellationMesh xe_mesh,");
  Emit(indent_string_ +
       "const object_data XeTessellationPayload& xe_payload [[payload]]");
  EmitDirectResourceArguments(true, true);
  Emit(") {\n");
  Outdent();
  Indent();
  EmitNativeDrawConstantAliases();
  EmitLine("if (xe_thread_index != 0u) {");
  Indent();
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine("uint xe_tess_factor = max(xe_payload.xe_tess_factor, 1u);");
  EmitLine("uint xe_micro_id = xe_micro_triangle_id.x;");
  EmitLine("float3 xe_domain_locations[3];");
  EmitLine("if (xe_payload.xe_is_quad != 0u) {");
  Indent();
  EmitLine("uint xe_quad_micro_count = xe_tess_factor * xe_tess_factor * 2u;");
  EmitLine("if (xe_micro_id >= xe_quad_micro_count) {");
  Indent();
  EmitLine("xe_mesh.set_primitive_count(0u);");
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine("uint xe_quad_cell = xe_micro_id >> 1u;");
  EmitLine("uint xe_quad_x = xe_quad_cell % xe_tess_factor;");
  EmitLine("uint xe_quad_y = xe_quad_cell / xe_tess_factor;");
  EmitLine("float xe_step = 1.0f / float(xe_tess_factor);");
  EmitLine(
      "float2 xe_p00 = float2(float(xe_quad_x), "
      "float(xe_quad_y)) * xe_step;");
  EmitLine(
      "float2 xe_p10 = float2(float(xe_quad_x + 1u), "
      "float(xe_quad_y)) * xe_step;");
  EmitLine(
      "float2 xe_p01 = float2(float(xe_quad_x), "
      "float(xe_quad_y + 1u)) * xe_step;");
  EmitLine(
      "float2 xe_p11 = float2(float(xe_quad_x + 1u), "
      "float(xe_quad_y + 1u)) * xe_step;");
  EmitLine("if ((xe_micro_id & 1u) == 0u) {");
  Indent();
  EmitLine("xe_domain_locations[0] = float3(xe_p00, 0.0f);");
  EmitLine("xe_domain_locations[1] = float3(xe_p10, 0.0f);");
  EmitLine("xe_domain_locations[2] = float3(xe_p01, 0.0f);");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine("xe_domain_locations[0] = float3(xe_p10, 0.0f);");
  EmitLine("xe_domain_locations[1] = float3(xe_p11, 0.0f);");
  EmitLine("xe_domain_locations[2] = float3(xe_p01, 0.0f);");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine("uint xe_tri_micro_count = xe_tess_factor * xe_tess_factor;");
  EmitLine("if (xe_micro_id >= xe_tri_micro_count) {");
  Indent();
  EmitLine("xe_mesh.set_primitive_count(0u);");
  EmitLine("return;");
  Outdent();
  EmitLine("}");
  EmitLine("uint xe_row = 0u;");
  EmitLine("uint xe_row_count = xe_tess_factor * 2u - 1u;");
  EmitLine("uint xe_local = xe_micro_id;");
  EmitLine(
      "while (xe_local >= xe_row_count && xe_row + 1u < "
      "xe_tess_factor) {");
  Indent();
  EmitLine("xe_local -= xe_row_count;");
  EmitLine("++xe_row;");
  EmitLine("xe_row_count -= 2u;");
  Outdent();
  EmitLine("}");
  EmitLine("uint xe_col = xe_local >> 1u;");
  EmitLine("bool xe_lower = (xe_local & 1u) != 0u;");
  EmitLine("float xe_step = 1.0f / float(xe_tess_factor);");
  EmitLine("float2 xe_p0 = float2(float(xe_col), float(xe_row)) * xe_step;");
  EmitLine(
      "float2 xe_p1 = float2(float(xe_col + 1u), float(xe_row)) * "
      "xe_step;");
  EmitLine(
      "float2 xe_p2 = float2(float(xe_col), float(xe_row + 1u)) * "
      "xe_step;");
  EmitLine(
      "float2 xe_p3 = float2(float(xe_col + 1u), "
      "float(xe_row + 1u)) * xe_step;");
  EmitLine("if (xe_lower) {");
  Indent();
  EmitLine("xe_p0 = xe_p1;");
  EmitLine("xe_p1 = xe_p3;");
  Outdent();
  EmitLine("}");
  EmitLine(
      "xe_domain_locations[0] = "
      "float3(1.0f - xe_p0.x - xe_p0.y, xe_p0.x, xe_p0.y);");
  EmitLine(
      "xe_domain_locations[1] = "
      "float3(1.0f - xe_p1.x - xe_p1.y, xe_p1.x, xe_p1.y);");
  EmitLine(
      "xe_domain_locations[2] = "
      "float3(1.0f - xe_p2.x - xe_p2.y, xe_p2.x, xe_p2.y);");
  Outdent();
  EmitLine("}");
  EmitLine("XeVertexInvocationResult xe_tess_guest[3];");
  EmitLine("for (uint xe_i = 0u; xe_i < 3u; ++xe_i) {");
  Indent();
  EmitLine("xe_tess_guest[xe_i] = XeRunGuestVertex(");
  Indent();
  Emit(indent_string_ + "0u,\n");
  Emit(indent_string_ + "0u,\n");
  Emit(indent_string_ + "xe_domain_locations[xe_i],\n");
  Emit(indent_string_ + "xe_payload.xe_control_point_indices,\n");
  Emit(indent_string_ + "xe_payload.xe_patch_index");
  EmitDirectResourceArgumentNames(true);
  Emit(");\n");
  Outdent();
  Outdent();
  EmitLine("}");
  if (VertexShaderCullDistanceCount()) {
    EmitLine("bool xe_tess_culled = false;");
    for (uint32_t i = 0; i < VertexShaderCullDistanceCount(); ++i) {
      EmitLine(
          "xe_tess_culled = xe_tess_culled || "
          "(xe_tess_guest[0].xe_cull_distance[" +
          std::to_string(i) +
          "] < 0.0f && "
          "xe_tess_guest[1].xe_cull_distance[" +
          std::to_string(i) +
          "] < 0.0f && "
          "xe_tess_guest[2].xe_cull_distance[" +
          std::to_string(i) + "] < 0.0f);");
    }
    EmitLine("if (xe_tess_culled) {");
    Indent();
    EmitLine("xe_mesh.set_primitive_count(0u);");
    EmitLine("return;");
    Outdent();
    EmitLine("}");
  }
  EmitLine(
      "bool xe_tess_positions_have_nan = "
      "any(XeIsNaN(xe_tess_guest[0].output.xe_position)) || "
      "any(XeIsNaN(xe_tess_guest[1].output.xe_position)) || "
      "any(XeIsNaN(xe_tess_guest[2].output.xe_position));");
  EmitLine("if (xe_tess_positions_have_nan) {");
  Indent();
  EmitLine("float4 xe_tess_nan = float4(0.0f / 0.0f);");
  EmitLine("xe_tess_guest[0].output.xe_position = xe_tess_nan;");
  EmitLine("xe_tess_guest[1].output.xe_position = xe_tess_nan;");
  EmitLine("xe_tess_guest[2].output.xe_position = xe_tess_nan;");
  Outdent();
  EmitLine("}");
  EmitLine("xe_mesh.set_primitive_count(1u);");
  EmitLine("for (uint xe_i = 0u; xe_i < 3u; ++xe_i) {");
  Indent();
  EmitLine(
      "xe_mesh.set_vertex(xe_i, XeToTriangleMeshVertexOutput("
      "XeFinalizeVertexOutput(xe_tess_guest[xe_i], xe_system)));");
  EmitLine("xe_mesh.set_index(xe_i, xe_i);");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitVertexEntryPointWrappers() {
  if (!is_vertex_shader()) {
    return;
  }
  EmitVertexFinalizerFunction();
  if (!IsPrimitiveListAsMesh()) {
    EmitLine("");
    EmitLine("vertex XeVertexOutput main_vs(");
    Indent();
    Emit(indent_string_ + "uint xe_vertex_id [[vertex_id]],\n");
    Emit(indent_string_ + "uint xe_instance_id [[instance_id]]");
    EmitDirectResourceArguments(true, true);
    Emit(") {\n");
    Outdent();
    Indent();
    EmitNativeDrawConstantAliases();
    EmitLine("return XeFinalizeVertexOutput(XeRunGuestVertex(");
    Indent();
    Emit(indent_string_ + "xe_vertex_id,\n");
    Emit(indent_string_ + "xe_instance_id");
    if (IsDomainShader()) {
      Emit(",\n" + indent_string_ + "float3(0.0f),\n");
      Emit(indent_string_ + "uint4(0u),\n");
      Emit(indent_string_ + "0u");
    }
    EmitDirectResourceArgumentNames(true);
    Emit("), xe_system);\n");
    Outdent();
    Outdent();
    EmitLine("}");
  }
  if (IsPointListAsMesh()) {
    EmitPointMeshEntryPoint();
  }
  if (IsRectangleListAsMesh()) {
    EmitRectangleMeshEntryPoint();
  }
  if (IsQuadListAsMesh()) {
    EmitQuadMeshEntryPoint();
  }
  if (IsDomainShader()) {
    EmitTessellationMeshEntryPoints();
  }
}

void MslShaderTranslator::EmitEntryPointEnd() {
  const bool rectangle_list_as_triangle_strip =
      IsRectangleListAsTriangleStrip();
  if (rectangle_list_as_triangle_strip) {
    EmitRectangleListGuestLoopEnd();
  } else if (uses_memexport_) {
    EmitMemExportFlush(
        "shader end",
        current_shader().memexport_eM_potentially_written_before_end()
            ? current_shader().memexport_eM_potentially_written_before_end()
            : 0xFFu);
  }
  if (is_pixel_shader()) {
    EmitPixelShaderEpilogue();
  } else {
    EmitRectangleListOutputSynthesis();
  }
  if (is_vertex_shader()) {
    if (VertexShaderCullDistanceCount()) {
      Modification modification = GetMslShaderModification();
      uint32_t cull_distance_next_component = 0;
      if (modification.vertex.user_clip_plane_cull &&
          modification.vertex.user_clip_plane_count) {
        EmitLine(
            "// Compute native helper cull distances before NDC transform");
        EmitLine("float4 xe_cull_clip_position = output.xe_position;");
        EmitLine("if ((xe_system.xe_flags & 8u) == 0u) {");
        Indent();
        EmitLine("xe_cull_clip_position.w = 1.0f / xe_cull_clip_position.w;");
        Outdent();
        EmitLine("}");
        EmitLine("if ((xe_system.xe_flags & 2u) != 0u) {");
        Indent();
        EmitLine("xe_cull_clip_position.xy *= xe_cull_clip_position.w;");
        Outdent();
        EmitLine("}");
        EmitLine("if ((xe_system.xe_flags & 4u) != 0u) {");
        Indent();
        EmitLine("xe_cull_clip_position.z *= xe_cull_clip_position.w;");
        Outdent();
        EmitLine("}");
        for (uint32_t i = 0; i < modification.vertex.user_clip_plane_count;
             ++i) {
          EmitLine("xe_cull_distance[" +
                   std::to_string(cull_distance_next_component++) +
                   "] = dot(xe_cull_clip_position, "
                   "float4(xe_system.xe_user_clip_planes[" +
                   std::to_string(i) + "][0], xe_system.xe_user_clip_planes[" +
                   std::to_string(i) + "][1], xe_system.xe_user_clip_planes[" +
                   std::to_string(i) + "][2], xe_system.xe_user_clip_planes[" +
                   std::to_string(i) + "][3]));");
        }
      }
      if (modification.vertex.vertex_kill_and) {
        const bool shader_writes_vertex_kill =
            (current_shader().writes_point_size_edge_flag_kill_vertex() &
             0b100) != 0;
        if (shader_writes_vertex_kill) {
          EmitLine(
              "uint xe_vertex_kill_bits = "
              "asuint(xe_point_size_edge_flag_kill_vertex.z) & 0x7FFFFFFFu;");
          EmitLine("xe_cull_distance[" +
                   std::to_string(cull_distance_next_component++) +
                   "] = xe_vertex_kill_bits != 0u ? -1.0f : 0.0f;");
        } else {
          EmitLine("xe_cull_distance[" +
                   std::to_string(cull_distance_next_component++) +
                   "] = 0.0f;");
        }
      }
    }
    EmitLine("XeVertexInvocationResult invocation;");
    EmitLine("invocation.output = output;");
    for (uint32_t i = 0; i < VertexShaderCullDistanceCount(); ++i) {
      EmitLine("invocation.xe_cull_distance[" + std::to_string(i) +
               "] = xe_cull_distance[" + std::to_string(i) + "];");
    }
    EmitLine(
        "invocation.xe_point_size_edge_flag_kill_vertex = "
        "xe_point_size_edge_flag_kill_vertex;");
    EmitLine("invocation.xe_point_sprite_vertex = xe_point_sprite_vertex;");
    EmitLine("return invocation;");
  } else {
    EmitLine("return output;");
  }
  Outdent();
  EmitLine("}");
  if (is_vertex_shader()) {
    EmitVertexEntryPointWrappers();
  }
}

void MslShaderTranslator::StartTranslation() {
  EmitLine("// Generated native MSL shader - Xenia Xbox 360 Emulator");
  EmitLine("#include <metal_stdlib>");
  EmitLine("using namespace metal;");
  EmitLine("");
  CollectStaticResourceBindings();
  EmitSystemConstants();
  EmitInputOutputDeclarations();
  EmitHelperFunctions();
  EmitNativeResourceHeapDeclarations();
  uses_staged_vector_result_ = CurrentShaderUsesStagedVectorResult();
  uses_staged_scalar_result_ = CurrentShaderUsesStagedScalarResult();
  EmitEntryPointBegin();
}

std::vector<uint8_t> MslShaderTranslator::CompleteTranslation() {
  CloseExecConditionals();
  if (has_main_switch_) {
    EmitLine("break;");
    Outdent();
    EmitLine("default:");
    Indent();
    EmitLine("break;");
    Outdent();
    Outdent();
    EmitLine("}");
    EmitLine("break;");
    Outdent();
    EmitLine("}");
  }
  EmitEntryPointEnd();
  msl_source_ = msl_stream_.str();

  if (!cvars::dump_shaders.empty()) {
    uint64_t hash = current_shader().ucode_data_hash();
    std::filesystem::path base =
        std::filesystem::path(cvars::dump_shaders) / "native_msl_shaders";
    std::string filename =
        "shader_" + fmt::format("{:016X}", hash) + "_" +
        fmt::format("{:016X}", current_translation().modification()) +
        (is_vertex_shader() ? ".native.vert.metal" : ".native.frag.metal");
    std::filesystem::path path = base / filename;
    xe::filesystem::CreateParentFolder(path);
    FILE* f = xe::filesystem::OpenFile(path, "wb");
    if (f) {
      fwrite(msl_source_.data(), 1, msl_source_.size(), f);
      fclose(f);
    }
  }
  set_host_disassembly(current_translation(), msl_source_);
  return std::vector<uint8_t>(msl_source_.begin(), msl_source_.end());
}

std::string MslShaderTranslator::RegisterToMsl(
    uint32_t storage_index, InstructionStorageAddressingMode mode) const {
  switch (mode) {
    case InstructionStorageAddressingMode::kAbsolute:
      if (UseScalarGprLocals()) {
        return "xe_gpr_" + std::to_string(storage_index);
      }
      return "xe_gprs[" + std::to_string(storage_index) + "]";
    case InstructionStorageAddressingMode::kAddressRegisterRelative:
      return "xe_gprs[" +
             RelativeIndexExpression(storage_index, "xe_a0",
                                     std::max(register_count(), 1u)) +
             "]";
    case InstructionStorageAddressingMode::kLoopRelative:
      return "xe_gprs[" +
             RelativeIndexExpression(storage_index, "xe_aL.x",
                                     std::max(register_count(), 1u)) +
             "]";
  }
  return "xe_gprs[" + std::to_string(storage_index) + "]";
}

std::string MslShaderTranslator::RelativeIndexExpression(
    uint32_t storage_index, const char* index_expr,
    uint32_t element_count) const {
  uint32_t last_element = element_count ? element_count - 1u : 0u;
  return "uint(clamp(int(" + std::to_string(storage_index) + ") + " +
         index_expr + ", 0, " + std::to_string(last_element) + "))";
}

std::string MslShaderTranslator::OperandToMsl(const InstructionOperand& operand,
                                              uint32_t needed_components) {
  std::string result;
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      result =
          RegisterToMsl(operand.storage_index, operand.storage_addressing_mode);
      break;
    case InstructionStorageSource::kConstantFloat: {
      const Shader::ConstantRegisterMap& constant_map =
          current_shader().constant_register_map();
      if (operand.storage_addressing_mode ==
          InstructionStorageAddressingMode::kAbsolute) {
        uint32_t packed_index =
            constant_map.GetPackedFloatConstantIndex(operand.storage_index);
        result = packed_index == UINT32_MAX
                     ? "float4(0.0f)"
                     : "xe_float_constants_data[" +
                           std::to_string(packed_index) + "]";
      } else if (operand.storage_addressing_mode ==
                 InstructionStorageAddressingMode::kAddressRegisterRelative) {
        result = "xe_float_constants_data[" +
                 RelativeIndexExpression(operand.storage_index, "xe_a0", 256u) +
                 "]";
      } else {
        result =
            "xe_float_constants_data[" +
            RelativeIndexExpression(operand.storage_index, "xe_aL.x", 256u) +
            "]";
      }
      break;
    }
    default:
      result = "float4(0.0f)";
      break;
  }
  if (needed_components > 0 && needed_components <= 4) {
    bool can_apply_swizzle_directly = true;
    for (uint32_t i = 0; i < needed_components; ++i) {
      SwizzleSource component = operand.components[i];
      if (component == SwizzleSource::k0 || component == SwizzleSource::k1) {
        can_apply_swizzle_directly = false;
        break;
      }
    }
    if (can_apply_swizzle_directly) {
      result += "." + GetSwizzleString(operand.components, needed_components);
    } else {
      std::vector<std::string> components;
      components.reserve(needed_components);
      for (uint32_t i = 0; i < needed_components; ++i) {
        SwizzleSource component = operand.components[i];
        if (component == SwizzleSource::k0) {
          components.emplace_back("0.0f");
        } else if (component == SwizzleSource::k1) {
          components.emplace_back("1.0f");
        } else {
          components.emplace_back("(" + result + ")." +
                                  std::string(1, GetCharForSwizzle(component)));
        }
      }
      if (needed_components == 1) {
        result = components[0];
      } else {
        std::string constructor =
            "float" + std::to_string(needed_components) + "(";
        for (uint32_t i = 0; i < needed_components; ++i) {
          if (i) {
            constructor += ", ";
          }
          constructor += components[i];
        }
        constructor += ")";
        result = constructor;
      }
    }
  }
  if (operand.is_absolute_value) {
    result = "abs(" + result + ")";
  }
  if (operand.is_negated) {
    result = "-(" + result + ")";
  }
  return result;
}

std::string MslShaderTranslator::OperandToMslNoSwizzle(
    const InstructionOperand& operand) {
  std::string result;
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      result =
          RegisterToMsl(operand.storage_index, operand.storage_addressing_mode);
      break;
    case InstructionStorageSource::kConstantFloat: {
      const Shader::ConstantRegisterMap& constant_map =
          current_shader().constant_register_map();
      if (operand.storage_addressing_mode ==
          InstructionStorageAddressingMode::kAbsolute) {
        uint32_t packed_index =
            constant_map.GetPackedFloatConstantIndex(operand.storage_index);
        result = packed_index == UINT32_MAX
                     ? "float4(0.0f)"
                     : "xe_float_constants_data[" +
                           std::to_string(packed_index) + "]";
      } else if (operand.storage_addressing_mode ==
                 InstructionStorageAddressingMode::kAddressRegisterRelative) {
        result = "xe_float_constants_data[" +
                 RelativeIndexExpression(operand.storage_index, "xe_a0", 256u) +
                 "]";
      } else {
        result =
            "xe_float_constants_data[" +
            RelativeIndexExpression(operand.storage_index, "xe_aL.x", 256u) +
            "]";
      }
      break;
    }
    default:
      result = "float4(0.0f)";
      break;
  }
  return result;
}

std::string MslShaderTranslator::ResultToMsl(const InstructionResult& result) {
  std::string output;
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      output =
          RegisterToMsl(result.storage_index, result.storage_addressing_mode);
      break;
    case InstructionStorageTarget::kInterpolator: {
      Modification modification = GetMslShaderModification();
      uint32_t mask = is_vertex_shader() ? modification.vertex.interpolator_mask
                                         : modification.pixel.interpolator_mask;
      if (mask & (UINT32_C(1) << result.storage_index)) {
        output =
            "output.xe_interpolator_" + std::to_string(result.storage_index);
      }
      break;
    }
    case InstructionStorageTarget::kPosition:
      output = "output.xe_position";
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      output = "xe_point_size_edge_flag_kill_vertex";
      break;
    case InstructionStorageTarget::kExportAddress:
    case InstructionStorageTarget::kExportData:
      output = MemExportTargetToMsl(result);
      break;
    case InstructionStorageTarget::kColor:
      if (!is_pixel_shader() || result.storage_index >= 4 ||
          !current_shader().writes_color_target(result.storage_index)) {
        return "";
      }
      output = "output.xe_color_" + std::to_string(result.storage_index);
      break;
    case InstructionStorageTarget::kDepth:
      return PixelShaderWritesDepthOutput() ? "output.xe_depth" : "";
    default:
      return "";
  }
  if (output.empty()) {
    return "";
  }
  uint32_t write_mask = result.GetUsedWriteMask();
  if (write_mask != 0b1111) {
    output += GetWriteMaskString(write_mask);
  }
  return output;
}

std::string MslShaderTranslator::MemExportTargetToMsl(
    const InstructionResult& result) const {
  if (!uses_memexport_ || result.storage_index >= kNativeMslMemExportSlots) {
    return "";
  }
  switch (result.storage_target) {
    case InstructionStorageTarget::kExportAddress:
      return "xe_memexport_address";
    case InstructionStorageTarget::kExportData:
      return "xe_memexport_data[" + std::to_string(result.storage_index) + "]";
    default:
      return "";
  }
}

bool MslShaderTranslator::PixelShaderNeedsFloat24DepthOutput() const {
  if (!is_pixel_shader()) {
    return false;
  }
  Modification::DepthStencilMode mode =
      GetMslShaderModification().pixel.depth_stencil_mode;
  return mode == Modification::DepthStencilMode::kFloat24Truncating ||
         mode == Modification::DepthStencilMode::kFloat24Rounding;
}

bool MslShaderTranslator::PixelShaderWritesDepthOutput() const {
  return is_pixel_shader() && (current_shader().writes_depth() ||
                               PixelShaderNeedsFloat24DepthOutput());
}

bool MslShaderTranslator::IsPixelShaderSampleRate() const {
  return is_pixel_shader() && PixelShaderNeedsFloat24DepthOutput() &&
         !current_shader().writes_depth();
}

bool MslShaderTranslator::IsForceEarlyDepthStencilEnabled() const {
  return is_pixel_shader() &&
         GetMslShaderModification().pixel.depth_stencil_mode ==
             Modification::DepthStencilMode::kEarlyHint &&
         !edram_rov_used_ &&
         current_shader().implicit_early_z_write_allowed() &&
         !PixelShaderWritesDepthOutput();
}

bool MslShaderTranslator::PixelShaderNeedsCoverageOutput() const {
  return is_pixel_shader() && current_shader().writes_color_target(0) &&
         !IsForceEarlyDepthStencilEnabled();
}

uint32_t MslShaderTranslator::VertexShaderClipDistanceCount() const {
  if (!is_vertex_shader()) {
    return 0;
  }
  return GetMslShaderModification().GetVertexClipDistanceCount();
}

uint32_t MslShaderTranslator::VertexShaderCullDistanceCount() const {
  if (!is_vertex_shader()) {
    return 0;
  }
  return GetMslShaderModification().GetVertexCullDistanceCount();
}

bool MslShaderTranslator::VertexShaderEmitsPointSizeOutput() const {
  if (!is_vertex_shader()) {
    return false;
  }
  Modification modification = GetMslShaderModification();
  return modification.vertex.output_point_size &&
         modification.vertex.host_vertex_shader_type ==
             Shader::HostVertexShaderType::kVertex;
}

bool MslShaderTranslator::ResultNeedsSaturation(
    const InstructionResult& result) const {
  return result.is_clamped ||
         result.storage_target == InstructionStorageTarget::kDepth;
}

std::string MslShaderTranslator::SaturateExpressionIfNeeded(
    const InstructionResult& result, const std::string& expression) const {
  return ResultNeedsSaturation(result) ? "XeSaturateNoNaN(" + expression + ")"
                                       : expression;
}

std::string MslShaderTranslator::GetSwizzleString(
    const SwizzleSource* components, uint32_t component_count) {
  std::string swizzle;
  for (uint32_t i = 0; i < component_count; ++i) {
    swizzle += GetCharForSwizzle(components[i]);
  }
  return swizzle;
}

std::string MslShaderTranslator::GetWriteMaskString(uint32_t write_mask) {
  std::string mask = ".";
  if (write_mask & 0b0001) {
    mask += "x";
  }
  if (write_mask & 0b0010) {
    mask += "y";
  }
  if (write_mask & 0b0100) {
    mask += "z";
  }
  if (write_mask & 0b1000) {
    mask += "w";
  }
  return mask;
}

void MslShaderTranslator::EmitVectorResultAssignment(
    const InstructionResult& result, const std::string& source_expr) {
  uint32_t write_mask = result.GetUsedWriteMask();
  if (!write_mask) {
    return;
  }
  if (result.storage_target == InstructionStorageTarget::kDepth) {
    if (!PixelShaderWritesDepthOutput()) {
      return;
    }
    const char comp_chars[] = {'x', 'y', 'z', 'w'};
    std::string source = SaturateExpressionIfNeeded(result, source_expr);
    std::string value = "(" + source + ").x";
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(write_mask & (1u << i))) {
        continue;
      }
      SwizzleSource src = result.components[i];
      if (src == SwizzleSource::k0) {
        value = "0.0f";
      } else if (src == SwizzleSource::k1) {
        value = "1.0f";
      } else if (src >= SwizzleSource::kX && src <= SwizzleSource::kW) {
        value = "(" + source + ")." +
                std::string(
                    1, comp_chars[uint32_t(src) - uint32_t(SwizzleSource::kX)]);
      }
      break;
    }
    EmitLine("output.xe_depth = " + value + ";");
    return;
  }
  std::string dest_base;
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      dest_base =
          RegisterToMsl(result.storage_index, result.storage_addressing_mode);
      break;
    case InstructionStorageTarget::kInterpolator: {
      Modification modification = GetMslShaderModification();
      uint32_t mask = modification.vertex.interpolator_mask;
      if (mask & (UINT32_C(1) << result.storage_index)) {
        dest_base =
            "output.xe_interpolator_" + std::to_string(result.storage_index);
      }
      break;
    }
    case InstructionStorageTarget::kPosition:
      dest_base = "output.xe_position";
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      dest_base = "xe_point_size_edge_flag_kill_vertex";
      break;
    case InstructionStorageTarget::kExportAddress:
    case InstructionStorageTarget::kExportData:
      dest_base = MemExportTargetToMsl(result);
      break;
    case InstructionStorageTarget::kColor:
      if (is_pixel_shader() && result.storage_index < 4 &&
          current_shader().writes_color_target(result.storage_index)) {
        dest_base = "output.xe_color_" + std::to_string(result.storage_index);
      }
      break;
    case InstructionStorageTarget::kDepth:
      if (PixelShaderWritesDepthOutput()) {
        dest_base = "output.xe_depth";
      }
      break;
    default:
      break;
  }
  if (dest_base.empty()) {
    RejectUnsupportedResultTarget("vector", result);
    return;
  }
  const char comp_chars[] = {'x', 'y', 'z', 'w'};
  std::string source = SaturateExpressionIfNeeded(result, source_expr);
  bool direct = true;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(write_mask & (1u << i))) {
      continue;
    }
    if (result.components[i] !=
        SwizzleSource(uint32_t(SwizzleSource::kX) + i)) {
      direct = false;
      break;
    }
  }
  if (direct) {
    if (write_mask == 0b1111) {
      EmitLine(dest_base + " = " + source + ";");
    } else {
      std::string dst = ".";
      std::string args;
      uint32_t count = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(write_mask & (1u << i))) {
          continue;
        }
        dst += comp_chars[i];
        if (!args.empty()) {
          args += ", ";
        }
        args += "(" + source + ").";
        args += comp_chars[i];
        ++count;
      }
      if (count == 1) {
        EmitLine(dest_base + dst + " = " + args + ";");
      } else {
        EmitLine(dest_base + dst + " = float" + std::to_string(count) + "(" +
                 args + ");");
      }
    }
  } else {
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(write_mask & (1u << i))) {
        continue;
      }
      SwizzleSource src = result.components[i];
      if (src >= SwizzleSource::kX && src <= SwizzleSource::kW) {
        uint32_t src_idx = uint32_t(src) - uint32_t(SwizzleSource::kX);
        EmitLine(dest_base + "." + comp_chars[i] + " = (" + source + ")." +
                 comp_chars[src_idx] + ";");
      }
    }
  }
  EmitMemExportMarkDirty(result);
  uint32_t point_size_write_mask = 0;
  if ((write_mask & 0b0001) && result.components[0] >= SwizzleSource::kX &&
      result.components[0] <= SwizzleSource::kW) {
    point_size_write_mask = 0b0001;
  }
  EmitPointSizeClampIfNeeded(result, point_size_write_mask);
}

void MslShaderTranslator::EmitScalarResultAssignment(
    const InstructionResult& result, const std::string& scalar_expr) {
  std::string dest = ResultToMsl(result);
  if (dest.empty()) {
    RejectUnsupportedResultTarget("scalar", result);
    return;
  }
  uint32_t write_mask = result.GetUsedWriteMask();
  if (!write_mask) {
    return;
  }
  uint32_t component_count = 0;
  if (write_mask & 0b0001) {
    ++component_count;
  }
  if (write_mask & 0b0010) {
    ++component_count;
  }
  if (write_mask & 0b0100) {
    ++component_count;
  }
  if (write_mask & 0b1000) {
    ++component_count;
  }
  std::string source = SaturateExpressionIfNeeded(result, scalar_expr);
  if (result.storage_target == InstructionStorageTarget::kDepth) {
    EmitLine("output.xe_depth = " + source + ";");
    return;
  }
  if (component_count == 1) {
    EmitLine(dest + " = " + source + ";");
  } else {
    EmitLine(dest + " = float" + std::to_string(component_count) + "(" +
             source + ");");
  }
  EmitMemExportMarkDirty(result);
  EmitPointSizeClampIfNeeded(result, write_mask & 1u);
}

void MslShaderTranslator::EmitPointSizeClampIfNeeded(
    const InstructionResult& result, uint32_t write_mask) {
  if (!is_vertex_shader() || !write_mask ||
      result.storage_target !=
          InstructionStorageTarget::kPointSizeEdgeFlagKillVertex) {
    return;
  }
  if (VertexShaderEmitsPointSizeOutput()) {
    EmitLine(
        "output.xe_point_size = XeClampPointSize("
        "xe_point_size_edge_flag_kill_vertex.x, "
        "xe_system.xe_point_vertex_diameter_min, "
        "xe_system.xe_point_vertex_diameter_max);");
  }
  EmitLine(
      "output.xe_point_parameters.xy = "
      "xe_point_size_edge_flag_kill_vertex.xy;");
}

void MslShaderTranslator::EmitPixelShaderParamGen() {
  if (!is_pixel_shader()) {
    return;
  }
  Modification modification = GetMslShaderModification();
  if (!modification.pixel.param_gen_enable ||
      modification.pixel.param_gen_interpolator >= register_count()) {
    return;
  }
  std::string dest = RegisterToMsl(modification.pixel.param_gen_interpolator,
                                   InstructionStorageAddressingMode::kAbsolute);
  EmitLine("// Generate PsParamGen pseudo-interpolator");
  EmitLine("{");
  Indent();
  EmitLine("float2 xe_param_xy = floor(input.xe_position.xy);");
  if (draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1) {
    EmitLine("xe_param_xy *= float2(" +
             MslFloatLiteral(1.0f / float(draw_resolution_scale_x_)) + ", " +
             MslFloatLiteral(1.0f / float(draw_resolution_scale_y_)) + ");");
  }
  EmitLine("float4 xe_param_gen = float4(abs(xe_param_xy), 0.0f, 0.0f);");
  if (modification.pixel.param_gen_point) {
    EmitLine("xe_param_gen.y = XeSetFloatSignBit(abs(xe_param_gen.y));");
    EmitLine("xe_param_gen.zw = XeSaturate(input.xe_point_parameters.xy);");
  } else {
    EmitLine(
        "if ((xe_system.xe_flags & 16u) != 0u && !input.xe_is_front_face) "
        "xe_param_gen.x = XeSetFloatSignBit(xe_param_gen.x);");
    EmitLine(
        "if ((xe_system.xe_flags & 32u) != 0u) xe_param_gen.z = "
        "asfloat(0x80000000u);");
  }
  EmitLine(dest + " = xe_param_gen;");
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitPixelShaderAlphaTest() {
  if (!is_pixel_shader() || !current_shader().writes_color_target(0) ||
      IsForceEarlyDepthStencilEnabled()) {
    return;
  }
  EmitLine("// Alpha test");
  EmitLine("{");
  Indent();
  EmitLine("uint xe_alpha_test_function = (xe_system.xe_flags >> 7u) & 7u;");
  EmitLine("if (xe_alpha_test_function != 7u) {");
  Indent();
  EmitLine("float xe_alpha_test_alpha = output.xe_color_0.a;");
  EmitLine("bool xe_alpha_test_pass = false;");
  EmitLine(
      "if (xe_alpha_test_function == 5u) xe_alpha_test_pass = "
      "xe_alpha_test_alpha != xe_system.xe_alpha_test_reference;");
  EmitLine("else {");
  Indent();
  EmitLine(
      "xe_alpha_test_pass = xe_alpha_test_pass || (((xe_alpha_test_function & "
      "1u) != 0u) && (xe_alpha_test_alpha < "
      "xe_system.xe_alpha_test_reference));");
  EmitLine(
      "xe_alpha_test_pass = xe_alpha_test_pass || (((xe_alpha_test_function & "
      "2u) != 0u) && (xe_alpha_test_alpha == "
      "xe_system.xe_alpha_test_reference));");
  EmitLine(
      "xe_alpha_test_pass = xe_alpha_test_pass || (((xe_alpha_test_function & "
      "4u) != 0u) && (xe_system.xe_alpha_test_reference < "
      "xe_alpha_test_alpha));");
  Outdent();
  EmitLine("}");
  EmitLine("if (!xe_alpha_test_pass) {");
  Indent();
  EmitDiscardFragment();
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitDiscardFragment() {
  if (uses_memexport_ && alu_kill_memexport_flush_mask_) {
    EmitMemExportFlush("before kill/discard", alu_kill_memexport_flush_mask_);
  }
  if (uses_memexport_) {
    EmitLine("xe_killed = true;");
    EmitLine("xe_memexport_allowed = false;");
  }
  EmitLine("discard_fragment();");
}

void MslShaderTranslator::EmitPixelShaderAlphaToCoverage() {
  if (!PixelShaderNeedsCoverageOutput()) {
    return;
  }
  EmitLine("// Alpha-to-coverage / sample mask");
  EmitLine("{");
  Indent();
  EmitLine("uint xe_a2c_static_mask = xe_system.xe_alpha_to_mask;");
  EmitLine("output.xe_coverage = 0xFFFFFFFFu;");
  EmitLine("if ((xe_a2c_static_mask & 0x100u) != 0u) {");
  Indent();
  EmitLine("output.xe_coverage = 0u;");
  EmitLine("float xe_a2c_alpha = XeSaturateNoNaN(output.xe_color_0.a);");
  EmitLine(
      "uint xe_a2c_offset_index = (uint(floor(input.xe_position.y)) & 1u) | "
      "((uint(floor(input.xe_position.x)) & 1u) << 1u);");
  EmitLine(
      "float xe_a2c_threshold_offset = float((xe_a2c_static_mask >> "
      "(xe_a2c_offset_index * 2u)) & 3u);");
  EmitLine(
      "uint xe_a2c_sample_count = 1u << min(xe_system.xe_sample_count_log2[0] "
      "+ xe_system.xe_sample_count_log2[1], 2u);");
  EmitLine("if (xe_a2c_sample_count >= 4u) {");
  Indent();
  EmitLine(
      "if (xe_a2c_alpha >= 0.75f - xe_a2c_threshold_offset * (1.0f / 16.0f)) "
      "output.xe_coverage |= 1u << 0u;");
  EmitLine(
      "if (xe_a2c_alpha >= 0.25f - xe_a2c_threshold_offset * (1.0f / 16.0f)) "
      "output.xe_coverage |= 1u << 1u;");
  EmitLine(
      "if (xe_a2c_alpha >= 0.5f - xe_a2c_threshold_offset * (1.0f / 16.0f)) "
      "output.xe_coverage |= 1u << 2u;");
  EmitLine(
      "if (xe_a2c_alpha >= 1.0f - xe_a2c_threshold_offset * (1.0f / 16.0f)) "
      "output.xe_coverage |= 1u << 3u;");
  Outdent();
  EmitLine("} else if (xe_a2c_sample_count >= 2u) {");
  Indent();
  EmitLine(
      "if (xe_a2c_alpha >= 0.5f - xe_a2c_threshold_offset * (1.0f / 8.0f)) "
      "output.xe_coverage |= 1u << 1u;");
  EmitLine(
      "if (xe_a2c_alpha >= 1.0f - xe_a2c_threshold_offset * (1.0f / 8.0f)) "
      "output.xe_coverage |= 1u << 0u;");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine(
      "if (xe_a2c_alpha >= 1.0f - xe_a2c_threshold_offset * (1.0f / 4.0f)) "
      "output.xe_coverage |= 1u;");
  Outdent();
  EmitLine("}");
  EmitLine("if (output.xe_coverage == 0u) {");
  Indent();
  EmitDiscardFragment();
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::EmitVertexShaderEpilogue() {
  if (!is_vertex_shader()) {
    return;
  }

  EmitLine("// Convert Xenos clip-space output to Metal clip space");
  EmitLine("if ((xe_system.xe_flags & 8u) == 0u) {");
  Indent();
  EmitLine("output.xe_position.w = 1.0f / output.xe_position.w;");
  Outdent();
  EmitLine("}");
  EmitLine("if ((xe_system.xe_flags & 2u) != 0u) {");
  Indent();
  EmitLine("output.xe_position.xy *= output.xe_position.w;");
  Outdent();
  EmitLine("}");
  EmitLine("if ((xe_system.xe_flags & 4u) != 0u) {");
  Indent();
  EmitLine("output.xe_position.z *= output.xe_position.w;");
  Outdent();
  EmitLine("}");

  Modification modification = GetMslShaderModification();
  if (modification.vertex.user_clip_plane_count) {
    if (modification.vertex.user_clip_plane_cull) {
      if (!IsPrimitiveListAsMesh()) {
        EmitNativeMslUnsupported(
            "native MSL cannot emit true Xenos user cull distances");
      } else {
        EmitLine(
            "// User cull distances are carried privately by the native mesh "
            "helper");
      }
    } else {
      EmitLine("// Emit user clip distances before NDC transform");
      EmitLine("float4 xe_guest_clip_position = output.xe_position;");
      for (uint32_t i = 0; i < modification.vertex.user_clip_plane_count; ++i) {
        EmitLine("output.xe_clip_distance[" + std::to_string(i) +
                 "] = dot(xe_guest_clip_position, "
                 "float4(xe_system.xe_user_clip_planes[" +
                 std::to_string(i) + "][0], xe_system.xe_user_clip_planes[" +
                 std::to_string(i) + "][1], xe_system.xe_user_clip_planes[" +
                 std::to_string(i) + "][2], xe_system.xe_user_clip_planes[" +
                 std::to_string(i) + "][3]));");
      }
    }
  }

  EmitLine(
      "output.xe_position.xyz *= float3(xe_system.xe_ndc_scale[0], "
      "xe_system.xe_ndc_scale[1], xe_system.xe_ndc_scale[2]);");
  EmitLine(
      "output.xe_position.xyz += float3(xe_system.xe_ndc_offset[0], "
      "xe_system.xe_ndc_offset[1], xe_system.xe_ndc_offset[2]) * "
      "output.xe_position.w;");

  if (modification.vertex.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    EmitLine("// Expand point-list vertices to a host triangle strip");
    EmitLine("{");
    Indent();
    EmitLine(
        "float2 xe_point_positive = float2("
        "(xe_point_sprite_vertex & 2u) != 0u ? 1.0f : 0.0f, "
        "(xe_point_sprite_vertex & 1u) != 0u ? 1.0f : 0.0f);");
    EmitLine(
        "float2 xe_point_diameter = float2("
        "xe_system.xe_point_constant_diameter[0], "
        "xe_system.xe_point_constant_diameter[1]);");
    if (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b001) {
      EmitLine("if (xe_point_size_edge_flag_kill_vertex.x >= 0.0f) {");
      Indent();
      EmitLine(
          "xe_point_diameter = "
          "float2(xe_point_size_edge_flag_kill_vertex.x);");
      Outdent();
      EmitLine("}");
    }
    EmitLine(
        "float2 xe_point_radius = xe_point_diameter * "
        "float2(xe_system.xe_point_screen_diameter_to_ndc_radius[0], "
        "xe_system.xe_point_screen_diameter_to_ndc_radius[1]) * "
        "output.xe_position.w;");
    EmitLine(
        "float2 xe_point_offset = select(-xe_point_radius, xe_point_radius, "
        "xe_point_positive != float2(0.0f));");
    EmitLine("output.xe_position.xy += xe_point_offset;");
    EmitLine("output.xe_point_parameters.xy = xe_point_positive;");
    Outdent();
    EmitLine("}");
  }

  bool shader_writes_vertex_kill =
      (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b100) != 0;
  if (modification.vertex.vertex_kill_and) {
    if (!IsPrimitiveListAsMesh()) {
      EmitNativeMslUnsupported(
          "native MSL cannot emit true Xenos vertex-kill cull distance");
    } else {
      EmitLine(
          "// Vertex-kill AND is carried as a private cull distance by the "
          "native mesh helper");
    }
  } else if (shader_writes_vertex_kill) {
    EmitLine(
        "uint xe_vertex_kill_bits = "
        "asuint(xe_point_size_edge_flag_kill_vertex.z) & 0x7FFFFFFFu;");
    EmitLine("if (xe_vertex_kill_bits != 0u) {");
    Indent();
    EmitLine("output.xe_position.w = asfloat(0x7FC00000u);");
    Outdent();
    EmitLine("}");
  }

  if (modification.vertex.output_point_size &&
      (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b001)) {
    EmitLine(
        "output.xe_point_parameters.x = "
        "xe_point_size_edge_flag_kill_vertex.x;");
  }
  if (VertexShaderEmitsPointSizeOutput()) {
    EmitLine(
        "output.xe_point_size = XeClampPointSize(output.xe_point_size, "
        "xe_system.xe_point_vertex_diameter_min, "
        "xe_system.xe_point_vertex_diameter_max);");
  }
  EmitLine(
      "output.xe_position = XeVertexPositionInfToNaN(output.xe_position);");
}

void MslShaderTranslator::EmitPixelShaderEpilogue() {
  if (!is_pixel_shader()) {
    return;
  }

  EmitPixelShaderAlphaTest();
  EmitPixelShaderAlphaToCoverage();

  uint32_t color_targets_written = current_shader().writes_color_targets();
  if (color_targets_written) {
    EmitLine("// Apply color exponent bias");
    if (color_targets_written & (1u << 0)) {
      EmitLine("output.xe_color_0 *= xe_system.xe_color_exp_bias[0];");
    }
    if (color_targets_written & (1u << 1)) {
      EmitLine("output.xe_color_1 *= xe_system.xe_color_exp_bias[1];");
    }
    if (color_targets_written & (1u << 2)) {
      EmitLine("output.xe_color_2 *= xe_system.xe_color_exp_bias[2];");
    }
    if (color_targets_written & (1u << 3)) {
      EmitLine("output.xe_color_3 *= xe_system.xe_color_exp_bias[3];");
    }
    EmitLine("// Apply per-render-target PWL gamma conversion");
    if (color_targets_written & (1u << 0)) {
      EmitLine(
          "if ((xe_system.xe_flags & (1u << 10u)) != 0u) "
          "output.xe_color_0.rgb = "
          "XeLinearToPWLGamma3(output.xe_color_0.rgb);");
    }
    if (color_targets_written & (1u << 1)) {
      EmitLine(
          "if ((xe_system.xe_flags & (1u << 11u)) != 0u) "
          "output.xe_color_1.rgb = "
          "XeLinearToPWLGamma3(output.xe_color_1.rgb);");
    }
    if (color_targets_written & (1u << 2)) {
      EmitLine(
          "if ((xe_system.xe_flags & (1u << 12u)) != 0u) "
          "output.xe_color_2.rgb = "
          "XeLinearToPWLGamma3(output.xe_color_2.rgb);");
    }
    if (color_targets_written & (1u << 3)) {
      EmitLine(
          "if ((xe_system.xe_flags & (1u << 13u)) != 0u) "
          "output.xe_color_3.rgb = "
          "XeLinearToPWLGamma3(output.xe_color_3.rgb);");
    }
  }

  Modification modification = GetMslShaderModification();
  if (PixelShaderNeedsFloat24DepthOutput()) {
    if (current_shader().writes_depth()) {
      EmitLine("float xe_depth_guest = output.xe_depth;");
    } else {
      if (IsPixelShaderSampleRate()) {
        EmitLine("(void)xe_sample_id;");
      }
      EmitLine(
          "float xe_depth_guest = "
          "XeSaturateNoNaN(input.xe_position.z * 2.0f);");
    }
    if (modification.pixel.depth_stencil_mode ==
        Modification::DepthStencilMode::kFloat24Truncating) {
      EmitLine(
          "output.xe_depth = "
          "XeDepthFloat24TruncateToHost(xe_depth_guest);");
    } else {
      EmitLine(
          "output.xe_depth = "
          "XeDepthFloat24RoundToHost(xe_depth_guest);");
    }
  } else if (current_shader().writes_depth()) {
    EmitLine("if ((xe_system.xe_flags & 64u) != 0u) {");
    Indent();
    EmitLine("output.xe_depth *= 0.5f;");
    Outdent();
    EmitLine("}");
  }
}

void MslShaderTranslator::StoreConstantComponents(
    const InstructionResult& result) {
  uint32_t used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }
  std::string dest = ResultToMsl(result);
  if (dest.empty()) {
    RejectUnsupportedResultTarget("constant", result);
    return;
  }
  uint32_t constant_mask = 0;
  uint32_t constant_1_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(used_write_mask & (1u << i))) {
      continue;
    }
    SwizzleSource component = result.components[i];
    if (component == SwizzleSource::k0) {
      constant_mask |= 1u << i;
    } else if (component == SwizzleSource::k1) {
      constant_mask |= 1u << i;
      constant_1_mask |= 1u << i;
    }
  }
  if (!constant_mask) {
    return;
  }
  uint32_t component_count = 0;
  if (constant_mask & 0b0001) {
    ++component_count;
  }
  if (constant_mask & 0b0010) {
    ++component_count;
  }
  if (constant_mask & 0b0100) {
    ++component_count;
  }
  if (constant_mask & 0b1000) {
    ++component_count;
  }
  std::string value_expr;
  if (component_count == 1) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (constant_mask & (1u << i)) {
        value_expr = (constant_1_mask & (1u << i)) ? "1.0f" : "0.0f";
        break;
      }
    }
  } else {
    value_expr = "float" + std::to_string(component_count) + "(";
    bool first = true;
    for (uint32_t i = 0; i < 4; ++i) {
      if (constant_mask & (1u << i)) {
        if (!first) {
          value_expr += ", ";
        }
        value_expr += (constant_1_mask & (1u << i)) ? "1.0f" : "0.0f";
        first = false;
      }
    }
    value_expr += ")";
  }
  std::string write_mask_str = ".";
  if (constant_mask & 0b0001) {
    write_mask_str += "x";
  }
  if (constant_mask & 0b0010) {
    write_mask_str += "y";
  }
  if (constant_mask & 0b0100) {
    write_mask_str += "z";
  }
  if (constant_mask & 0b1000) {
    write_mask_str += "w";
  }
  std::string dest_base = ResultToMsl(result);
  size_t dot_pos = dest_base.rfind('.');
  if (dot_pos != std::string::npos) {
    std::string potential_mask = dest_base.substr(dot_pos + 1);
    bool is_mask = !potential_mask.empty();
    for (char c : potential_mask) {
      if (c != 'x' && c != 'y' && c != 'z' && c != 'w') {
        is_mask = false;
        break;
      }
    }
    if (is_mask) {
      dest_base = dest_base.substr(0, dot_pos);
    }
  }
  EmitLine(dest_base + write_mask_str + " = " + value_expr + ";");
  EmitMemExportMarkDirty(result);
  EmitPointSizeClampIfNeeded(result, constant_mask);
}

void MslShaderTranslator::ProcessLabel(uint32_t cf_index) {
  if (cf_index == 0) {
    return;
  }
  CloseExecConditionals();
  EmitControlFlowCase(cf_index);
}

void MslShaderTranslator::ProcessExecInstructionBegin(
    const ParsedExecInstruction& instr) {
  switch (instr.type) {
    case ParsedExecInstruction::Type::kConditional:
      cf_exec_bool_constant_ = instr.bool_constant_index;
      cf_exec_bool_constant_condition_ = instr.condition;
      EmitLine("if (XeGetBoolConstant(xe_bool_loop_constants_data, " +
               std::to_string(instr.bool_constant_index) + "u) " +
               (instr.condition ? "==" : "!=") + " true) {");
      Indent();
      break;
    case ParsedExecInstruction::Type::kPredicated:
      cf_exec_predicated_ = true;
      cf_exec_predicate_condition_ = instr.condition;
      EmitLine("if (xe_p0 " + std::string(instr.condition ? "==" : "!=") +
               " true) {");
      Indent();
      break;
    default:
      break;
  }
}

void MslShaderTranslator::ProcessExecInstructionEnd(
    const ParsedExecInstruction& instr) {
  if (instr.is_end) {
    if (has_main_switch_) {
      EmitLine("xe_pc = 0x7FFFFFFFu;");
      EmitLine("continue;");
    }
  }
  if (cf_exec_predicated_ || cf_exec_bool_constant_ != UINT32_MAX) {
    Outdent();
    EmitLine("}");
  }
  cf_exec_predicated_ = false;
  cf_exec_bool_constant_ = UINT32_MAX;
}

void MslShaderTranslator::ProcessLoopStartInstruction(
    const ParsedLoopStartInstruction& instr) {
  CloseExecConditionals();
  if (!has_main_switch_) {
    EmitNativeMslUnsupported(
        "loop start requires the native control-flow switch path");
    return;
  }
  EmitLine("{");
  Indent();
  EmitLine(
      "uint xe_loop_const = XeGetLoopConstant(xe_bool_loop_constants_data, " +
      std::to_string(instr.loop_constant_index) + "u);");
  EmitLine("uint xe_loop_count_val = xe_loop_const & 0xFFu;");
  EmitLine("if (xe_loop_count_val == 0u) { xe_pc = " +
           std::to_string(instr.loop_skip_address) + "u; continue; }");
  EmitLine("xe_loop_count = uint4(xe_loop_count_val, xe_loop_count.xyz);");
  if (instr.is_repeat) {
    EmitLine("xe_aL = int4(xe_aL.x, xe_aL.xyz);");
  } else {
    EmitLine("xe_aL = int4(int((xe_loop_const >> 8u) & 0xFFu), xe_aL.xyz);");
  }
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::ProcessLoopEndInstruction(
    const ParsedLoopEndInstruction& instr) {
  CloseExecConditionals();
  if (!has_main_switch_) {
    EmitNativeMslUnsupported(
        "loop end requires the native control-flow switch path");
    return;
  }
  EmitLine("xe_loop_count.x = xe_loop_count.x - 1u;");
  if (instr.is_predicated_break) {
    EmitLine("if (xe_loop_count.x == 0u || xe_p0 " +
             std::string(instr.predicate_condition ? "==" : "!=") + " true) {");
  } else {
    EmitLine("if (xe_loop_count.x == 0u) {");
  }
  Indent();
  EmitLine("xe_loop_count = uint4(xe_loop_count.yzw, 0u);");
  EmitLine("xe_aL = int4(xe_aL.yzw, 0);");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine(
      "uint xe_loop_const = XeGetLoopConstant(xe_bool_loop_constants_data, " +
      std::to_string(instr.loop_constant_index) + "u);");
  EmitLine("int xe_loop_step = int((xe_loop_const >> 16u) & 0xFFu);");
  EmitLine("if (xe_loop_step > 127) xe_loop_step -= 256;");
  EmitLine("xe_aL.x = xe_aL.x + xe_loop_step;");
  EmitLine("xe_pc = " + std::to_string(instr.loop_body_address) + "u;");
  EmitLine("continue;");
  Outdent();
  EmitLine("}");
}

std::string MslShaderTranslator::GetCallConditionExpression(
    const ParsedCallInstruction& instr) {
  switch (instr.type) {
    case ParsedCallInstruction::Type::kUnconditional:
      return "true";
    case ParsedCallInstruction::Type::kConditional:
      return "XeGetBoolConstant(xe_bool_loop_constants_data, " +
             std::to_string(instr.bool_constant_index) + "u) " +
             (instr.condition ? "==" : "!=") + " true";
    case ParsedCallInstruction::Type::kPredicated:
      return "xe_p0 " + std::string(instr.condition ? "==" : "!=") + " true";
  }
  return "false";
}

void MslShaderTranslator::ProcessCallInstruction(
    const ParsedCallInstruction& instr) {
  CloseExecConditionals();
  if (!has_main_switch_) {
    EmitNativeMslUnsupported(
        "call/return requires the native control-flow switch path");
    return;
  }
  const uint32_t return_address = instr.dword_index + 1u;
  EmitLine("if (" + GetCallConditionExpression(instr) + ") {");
  Indent();
  EmitLine("if (xe_call_stack_depth >= " +
           std::to_string(kNativeMslCallStackDepth) + "u) {");
  Indent();
  EmitLine("xe_pc = 0x7FFFFFFFu;");
  EmitLine("continue;");
  Outdent();
  EmitLine("}");
  EmitLine("xe_call_stack[xe_call_stack_depth++] = " +
           std::to_string(return_address) + "u;");
  EmitLine("xe_pc = " + std::to_string(instr.target_address) + "u;");
  EmitLine("continue;");
  Outdent();
  EmitLine("}");
}

void MslShaderTranslator::ProcessReturnInstruction(
    const ParsedReturnInstruction& instr) {
  (void)instr;
  CloseExecConditionals();
  if (!has_main_switch_) {
    EmitNativeMslUnsupported(
        "return requires the native control-flow switch path");
    return;
  }
  EmitLine("if (xe_call_stack_depth == 0u) {");
  Indent();
  EmitLine("xe_pc = 0x7FFFFFFFu;");
  EmitLine("continue;");
  Outdent();
  EmitLine("}");
  EmitLine("xe_pc = xe_call_stack[--xe_call_stack_depth];");
  EmitLine("continue;");
}

void MslShaderTranslator::ProcessJumpInstruction(
    const ParsedJumpInstruction& instr) {
  if (!has_main_switch_) {
    EmitNativeMslUnsupported(
        "jump requires the native control-flow switch path");
    return;
  }
  std::string target = std::to_string(instr.target_address) + "u";
  switch (instr.type) {
    case ParsedJumpInstruction::Type::kUnconditional:
      EmitLine("xe_pc = " + target + ";");
      EmitLine("continue;");
      break;
    case ParsedJumpInstruction::Type::kConditional:
      EmitLine("if (XeGetBoolConstant(xe_bool_loop_constants_data, " +
               std::to_string(instr.bool_constant_index) + "u) " +
               (instr.condition ? "==" : "!=") + " true) {");
      Indent();
      EmitLine("xe_pc = " + target + ";");
      EmitLine("continue;");
      Outdent();
      EmitLine("}");
      break;
    case ParsedJumpInstruction::Type::kPredicated:
      EmitLine("if (xe_p0 " + std::string(instr.condition ? "==" : "!=") +
               " true) {");
      Indent();
      EmitLine("xe_pc = " + target + ";");
      EmitLine("continue;");
      Outdent();
      EmitLine("}");
      break;
  }
}

void MslShaderTranslator::ProcessAllocInstruction(
    const ParsedAllocInstruction& instr, uint8_t export_eM) {
  const bool start_memexport =
      instr.type == ucode::AllocType::kMemory && uses_memexport_;
  if (export_eM) {
    EmitMemExportFlush("alloc/export", export_eM);
    EmitLine("xe_memexport_data_valid_mask = 0u;");
    for (uint32_t i = 0; i < kNativeMslMemExportSlots; ++i) {
      if (export_eM & (1u << i)) {
        EmitLine("xe_memexport_data[" + std::to_string(i) +
                 "] = float4(0.0f);");
      }
    }
  }
  if (start_memexport) {
    EmitLine("xe_memexport_address = float4(0.0f);");
  }
  EmitLine("// Alloc: " + std::to_string(instr.count) + " exports");
}

void MslShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  bool predication_open = false;
  EmitFetchPredicationBegin(instr.is_predicated, instr.predicate_condition,
                            predication_open);

  uint32_t used_result_components = instr.result.GetUsedResultComponents();
  uint32_t fetch_constant_index = instr.operands[1].storage_index;
  EmitLine("{");
  Indent();

  uint32_t fetch_uint4_index = fetch_constant_index >> 1;
  bool fetch_use_zw = (fetch_constant_index & 1u) != 0;
  const char* fetch_comp0 = fetch_use_zw ? "z" : "x";
  const char* fetch_comp1 = fetch_use_zw ? "w" : "y";
  if (!instr.is_mini_fetch) {
    EmitLine("uint xe_vf_word0 = xe_fetch_constants_data[" +
             std::to_string(fetch_uint4_index) + "u]." + fetch_comp0 + ";");
  }
  if (!instr.is_mini_fetch) {
    EmitLine("uint xe_vf_base_addr = xe_vf_word0 & 0xFFFFFFFCu;");
  }

  if (!instr.is_mini_fetch) {
    std::string index_operand = OperandToMsl(instr.operands[0], 1);
    if (instr.attributes.is_index_rounded) {
      EmitLine("int xe_vf_index = int(floor(" + index_operand + " + 0.5f));");
    } else if (cvars::ac6_ground_fix) {
      EmitLine("int xe_vf_index = int(floor(" + index_operand +
               " + 0.00025f));");
    } else {
      EmitLine("int xe_vf_index = int(floor(" + index_operand + ")); ");
    }
    if (instr.attributes.stride) {
      EmitLine("xe_vfetch_address = xe_vf_base_addr + uint(xe_vf_index) * " +
               std::to_string(instr.attributes.stride * sizeof(uint32_t)) +
               "u;");
    } else {
      EmitLine("xe_vfetch_address = xe_vf_base_addr;");
    }
  }

  xenos::VertexFormat format = instr.attributes.data_format;
  uint32_t needed_words = 0;
  bool known_format = true;
  switch (format) {
    case xenos::VertexFormat::k_8_8_8_8:
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_16_16:
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32:
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      needed_words =
          xenos::GetVertexFormatNeededWords(format, used_result_components);
      break;
    default:
      known_format = false;
      break;
  }

  if (known_format && !needed_words) {
    EmitVectorResultAssignment(instr.result, "float4(0.0f)");
    StoreConstantComponents(instr.result);
    Outdent();
    EmitLine("}");
    EmitFetchPredicationEnd(predication_open);
    return;
  }
  if (known_format) {
    EmitLine("uint xe_vf_addr = xe_vfetch_address" +
             MslByteOffsetTerm(instr.attributes.offset) +
             (instr.is_mini_fetch
                  ? "; // vfetch_mini reuses the previous full-fetch address"
                  : ";"));
    EmitLine("uint xe_vf_word1 = xe_fetch_constants_data[" +
             std::to_string(fetch_uint4_index) + "u]." + fetch_comp1 + ";");
    EmitLine("uint xe_vf_endian = xe_vf_word1 & 0x3u;");
  }

  auto make_signed_int = [](const std::string& value_expr,
                            uint32_t bits) -> std::string {
    if (bits == 32u) {
      return "as_type<int>(" + value_expr + ")";
    }
    return "XeSignExtend(" + value_expr + ", " + std::to_string(bits) + "u)";
  };
  auto make_integer_component = [&](const std::string& value_expr,
                                    uint32_t bits) -> std::string {
    if (instr.attributes.is_signed) {
      return "float(" + make_signed_int(value_expr, bits) + ")";
    }
    return "float(" + value_expr + ")";
  };
  auto make_normalized_component = [&](const std::string& value_expr,
                                       uint32_t bits) -> std::string {
    if (instr.attributes.is_signed) {
      const std::string signed_value = make_signed_int(value_expr, bits);
      if (instr.attributes.signed_rf_mode ==
          xenos::SignedRepeatingFractionMode::kNoZero) {
        return "XeNormalizeSignedNoZero(" + signed_value + ", " +
               std::to_string(bits) + "u)";
      }
      return "XeNormalizeSignedZeroClampMinusOne(" + signed_value + ", " +
             std::to_string(bits) + "u)";
    }
    return "XeNormalizeUnsigned(" + value_expr + ", " + std::to_string(bits) +
           "u)";
  };
  auto make_fixed_component = [&](const std::string& value_expr,
                                  uint32_t bits) -> std::string {
    return instr.attributes.is_integer
               ? make_integer_component(value_expr, bits)
               : make_normalized_component(value_expr, bits);
  };
  auto make_packed_component = [&](const std::string& packed_expr,
                                   uint32_t shift, uint32_t mask,
                                   uint32_t bits) -> std::string {
    std::string value_expr;
    if (shift) {
      value_expr = "((" + packed_expr + " >> " + std::to_string(shift) +
                   "u) & 0x" + fmt::format("{:X}", mask) + "u)";
    } else {
      value_expr =
          "(" + packed_expr + " & 0x" + fmt::format("{:X}", mask) + "u)";
    }
    return make_fixed_component(value_expr, bits);
  };
  auto result_component_or_zero =
      [&](uint32_t i, const std::string& value_expr) -> std::string {
    return (used_result_components & (UINT32_C(1) << i)) ? value_expr : "0.0f";
  };
  auto make_packed_result_component =
      [&](uint32_t i, const std::string& packed_expr, uint32_t shift,
          uint32_t mask, uint32_t bits) -> std::string {
    if (!(used_result_components & (UINT32_C(1) << i))) {
      return "0.0f";
    }
    return make_packed_component(packed_expr, shift, mask, bits);
  };
  auto emit_32bit_word_loads = [&](const std::string& raw_name,
                                   uint32_t component_count) {
    const uint32_t component_mask =
        component_count >= 4u ? 0xFu : ((UINT32_C(1) << component_count) - 1u);
    const uint32_t needed_mask = needed_words & component_mask;
    EmitLine("uint4 " + raw_name + " = uint4(0u);");
    for (uint32_t i = 0; i < component_count; ++i) {
      if (!(needed_mask & (UINT32_C(1) << i))) {
        continue;
      }
      const std::string component(1, "xyzw"[i]);
      const std::string address =
          i ? ("xe_vf_addr + " + std::to_string(i * sizeof(uint32_t)) + "u")
            : "xe_vf_addr";
      EmitLine(raw_name + "." + component +
               " = XeSharedMemoryLoad(xe_shared_memory, " + address + ");");
      EmitLine(raw_name + "." + component + " = XeEndianSwap(" + raw_name +
               "." + component + ", xe_vf_endian);");
    }
  };

  std::string xe_vf_value_expr;
  switch (format) {
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      emit_32bit_word_loads("xe_vf_raw", 4u);
      xe_vf_value_expr =
          "float4(" + result_component_or_zero(0, "asfloat(xe_vf_raw).x") +
          ", " + result_component_or_zero(1, "asfloat(xe_vf_raw).y") + ", " +
          result_component_or_zero(2, "asfloat(xe_vf_raw).z") + ", " +
          result_component_or_zero(3, "asfloat(xe_vf_raw).w") + ")";
      break;
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      emit_32bit_word_loads("xe_vf_raw", 3u);
      xe_vf_value_expr =
          "float4(" + result_component_or_zero(0, "asfloat(xe_vf_raw).x") +
          ", " + result_component_or_zero(1, "asfloat(xe_vf_raw).y") + ", " +
          result_component_or_zero(2, "asfloat(xe_vf_raw).z") + ", 0.0f)";
      break;
    case xenos::VertexFormat::k_32_32_FLOAT:
      emit_32bit_word_loads("xe_vf_raw", 2u);
      xe_vf_value_expr =
          "float4(" + result_component_or_zero(0, "asfloat(xe_vf_raw).x") +
          ", " + result_component_or_zero(1, "asfloat(xe_vf_raw).y") +
          ", 0.0f, 0.0f)";
      break;
    case xenos::VertexFormat::k_32_FLOAT:
      emit_32bit_word_loads("xe_vf_raw", 1u);
      xe_vf_value_expr = "float4(" +
                         result_component_or_zero(0, "asfloat(xe_vf_raw).x") +
                         ", 0.0f, 0.0f, 0.0f)";
      break;
    case xenos::VertexFormat::k_8_8_8_8:
      EmitLine(
          "uint xe_vf_raw = XeSharedMemoryLoad(xe_shared_memory, xe_vf_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      xe_vf_value_expr =
          "float4(" + make_packed_result_component(0, "xe_vf_raw", 0, 0xFF, 8) +
          ", " + make_packed_result_component(1, "xe_vf_raw", 8, 0xFF, 8) +
          ", " + make_packed_result_component(2, "xe_vf_raw", 16, 0xFF, 8) +
          ", " + make_packed_result_component(3, "xe_vf_raw", 24, 0xFF, 8) +
          ")";
      break;
    case xenos::VertexFormat::k_2_10_10_10:
      EmitLine(
          "uint xe_vf_raw = XeSharedMemoryLoad(xe_shared_memory, xe_vf_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      xe_vf_value_expr =
          "float4(" +
          make_packed_result_component(0, "xe_vf_raw", 0, 0x3FF, 10) + ", " +
          make_packed_result_component(1, "xe_vf_raw", 10, 0x3FF, 10) + ", " +
          make_packed_result_component(2, "xe_vf_raw", 20, 0x3FF, 10) + ", " +
          make_packed_result_component(3, "xe_vf_raw", 30, 0x3, 2) + ")";
      break;
    case xenos::VertexFormat::k_10_11_11:
      EmitLine(
          "uint xe_vf_raw = XeSharedMemoryLoad(xe_shared_memory, xe_vf_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      xe_vf_value_expr =
          "float4(" +
          make_packed_result_component(0, "xe_vf_raw", 0, 0x7FF, 11) + ", " +
          make_packed_result_component(1, "xe_vf_raw", 11, 0x7FF, 11) + ", " +
          make_packed_result_component(2, "xe_vf_raw", 22, 0x3FF, 10) +
          ", 0.0f)";
      break;
    case xenos::VertexFormat::k_11_11_10:
      EmitLine(
          "uint xe_vf_raw = XeSharedMemoryLoad(xe_shared_memory, xe_vf_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      xe_vf_value_expr =
          "float4(" +
          make_packed_result_component(0, "xe_vf_raw", 0, 0x3FF, 10) + ", " +
          make_packed_result_component(1, "xe_vf_raw", 10, 0x7FF, 11) + ", " +
          make_packed_result_component(2, "xe_vf_raw", 21, 0x7FF, 11) +
          ", 0.0f)";
      break;
    case xenos::VertexFormat::k_16_16:
      EmitLine(
          "uint xe_vf_raw = XeSharedMemoryLoad(xe_shared_memory, xe_vf_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      xe_vf_value_expr =
          "float4(" +
          make_packed_result_component(0, "xe_vf_raw", 0, 0xFFFF, 16) + ", " +
          make_packed_result_component(1, "xe_vf_raw", 16, 0xFFFF, 16) +
          ", 0.0f, 0.0f)";
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      EmitLine(
          "uint2 xe_vf_raw = XeSharedMemoryLoad2(xe_shared_memory, "
          "xe_vf_addr);");
      EmitLine("xe_vf_raw.x = XeEndianSwap(xe_vf_raw.x, xe_vf_endian);");
      EmitLine("xe_vf_raw.y = XeEndianSwap(xe_vf_raw.y, xe_vf_endian);");
      xe_vf_value_expr =
          "float4(" +
          make_packed_result_component(0, "xe_vf_raw.x", 0, 0xFFFF, 16) + ", " +
          make_packed_result_component(1, "xe_vf_raw.x", 16, 0xFFFF, 16) +
          ", " + make_packed_result_component(2, "xe_vf_raw.y", 0, 0xFFFF, 16) +
          ", " +
          make_packed_result_component(3, "xe_vf_raw.y", 16, 0xFFFF, 16) + ")";
      break;
    case xenos::VertexFormat::k_16_16_FLOAT:
      EmitLine(
          "uint xe_vf_raw = XeSharedMemoryLoad(xe_shared_memory, xe_vf_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      if (used_result_components & 0b0011u) {
        EmitLine("float2 xe_vf_xy = XeUnpackFloat16x2(xe_vf_raw);");
      }
      xe_vf_value_expr = "float4(" + result_component_or_zero(0, "xe_vf_xy.x") +
                         ", " + result_component_or_zero(1, "xe_vf_xy.y") +
                         ", 0.0f, 0.0f)";
      break;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
      EmitLine(
          "uint2 xe_vf_raw = XeSharedMemoryLoad2(xe_shared_memory, "
          "xe_vf_addr);");
      EmitLine("xe_vf_raw.x = XeEndianSwap(xe_vf_raw.x, xe_vf_endian);");
      EmitLine("xe_vf_raw.y = XeEndianSwap(xe_vf_raw.y, xe_vf_endian);");
      if (used_result_components & 0b0011u) {
        EmitLine("float2 xe_vf_xy = XeUnpackFloat16x2(xe_vf_raw.x);");
      }
      if (used_result_components & 0b1100u) {
        EmitLine("float2 xe_vf_zw = XeUnpackFloat16x2(xe_vf_raw.y);");
      }
      xe_vf_value_expr = "float4(" + result_component_or_zero(0, "xe_vf_xy.x") +
                         ", " + result_component_or_zero(1, "xe_vf_xy.y") +
                         ", " + result_component_or_zero(2, "xe_vf_zw.x") +
                         ", " + result_component_or_zero(3, "xe_vf_zw.y") + ")";
      break;
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32: {
      uint32_t component_count;
      if (format == xenos::VertexFormat::k_32) {
        component_count = 1u;
      } else if (format == xenos::VertexFormat::k_32_32) {
        component_count = 2u;
      } else {
        component_count = 4u;
      }
      emit_32bit_word_loads("xe_vf_raw", component_count);
      auto fixed_32_component_or_zero = [&](uint32_t i) -> std::string {
        if (i >= component_count ||
            !(used_result_components & (UINT32_C(1) << i))) {
          return "0.0f";
        }
        const std::string component(1, "xyzw"[i]);
        return make_fixed_component("xe_vf_raw." + component, 32);
      };
      xe_vf_value_expr = "float4(" + fixed_32_component_or_zero(0) + ", " +
                         fixed_32_component_or_zero(1) + ", " +
                         fixed_32_component_or_zero(2) + ", " +
                         fixed_32_component_or_zero(3) + ")";
      break;
    }
    default:
      EmitNativeMslUnsupported(
          fmt::format("unsupported vertex fetch format {}",
                      uint32_t(instr.attributes.data_format)));
      xe_vf_value_expr = "float4(0.0f)";
      break;
  }
  if (instr.attributes.exp_adjust) {
    xe_vf_value_expr =
        "(" + xe_vf_value_expr + ") * " +
        MslFloatLiteral(std::ldexp(1.0f, instr.attributes.exp_adjust));
  }
  EmitLine("float4 xe_vf_value = " + xe_vf_value_expr + ";");
  EmitVectorResultAssignment(instr.result, "xe_vf_value");
  StoreConstantComponents(instr.result);
  Outdent();
  EmitLine("}");
  EmitFetchPredicationEnd(predication_open);
}

void MslShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  using FetchOpcode = ucode::FetchOpcode;

  bool predication_open = false;
  EmitFetchPredicationBegin(instr.is_predicated, instr.predicate_condition,
                            predication_open);

  switch (instr.opcode) {
    case FetchOpcode::kSetTextureLod:
      EmitLine("xe_texture_lod = " + OperandToMsl(instr.operands[0], 1) + ";");
      EmitFetchPredicationEnd(predication_open);
      return;
    case FetchOpcode::kSetTextureGradientsHorz:
      EmitLine("xe_texture_grad_h = " + OperandToMsl(instr.operands[0], 3) +
               ";");
      EmitFetchPredicationEnd(predication_open);
      return;
    case FetchOpcode::kSetTextureGradientsVert:
      EmitLine("xe_texture_grad_v = " + OperandToMsl(instr.operands[0], 3) +
               ";");
      EmitFetchPredicationEnd(predication_open);
      return;
    case FetchOpcode::kTextureFetch:
    case FetchOpcode::kGetTextureBorderColorFrac:
    case FetchOpcode::kGetTextureComputedLod:
    case FetchOpcode::kGetTextureGradients:
    case FetchOpcode::kGetTextureWeights:
      break;
    default:
      EmitNativeMslUnsupported(
          fmt::format("texture fetch opcode {} is unimplemented",
                      instr.opcode_name ? instr.opcode_name : "<unknown>"));
      if (instr.has_result()) {
        EmitVectorResultAssignment(instr.result,
                                   "float4(0.0f, 0.0f, 0.0f, 1.0f)");
        StoreConstantComponents(instr.result);
      }
      EmitFetchPredicationEnd(predication_open);
      return;
  }

  uint32_t used_result_components = instr.result.GetUsedResultComponents();
  if (!used_result_components) {
    EmitFetchPredicationEnd(predication_open);
    return;
  }

  uint32_t fetch_constant_index = instr.operands[1].storage_index;
  const bool texture_fetch = instr.opcode == FetchOpcode::kTextureFetch;
  const bool get_texture_weights =
      instr.opcode == FetchOpcode::kGetTextureWeights;
  const bool needs_texture_sampler =
      texture_fetch || instr.opcode == FetchOpcode::kGetTextureComputedLod;
  const bool needs_texture_runtime_info =
      needs_texture_sampler &&
      instr.dimension == xenos::FetchOpDimension::k3DOrStacked;
  uint32_t used_result_nonzero_components = instr.GetNonZeroResultComponents();
  if (get_texture_weights) {
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
        used_result_nonzero_components &= 0b0001;
        break;
      case xenos::FetchOpDimension::k2D:
      case xenos::FetchOpDimension::kCube:
        used_result_nonzero_components &= 0b0011;
        break;
      case xenos::FetchOpDimension::k3DOrStacked: {
        used_result_nonzero_components &= 0b0111;
        break;
      }
    }
  }
  if (!used_result_nonzero_components) {
    EmitVectorResultAssignment(instr.result, "float4(0.0f)");
    StoreConstantComponents(instr.result);
    EmitFetchPredicationEnd(predication_open);
    return;
  }
  if (instr.opcode == FetchOpcode::kGetTextureBorderColorFrac) {
    EmitLine(
        "// getBCF is unimplemented in the mature paths too; return zero.");
    EmitVectorResultAssignment(instr.result, "float4(0.0f)");
    StoreConstantComponents(instr.result);
    EmitFetchPredicationEnd(predication_open);
    return;
  }
  if (instr.opcode == FetchOpcode::kGetTextureGradients) {
    EmitLine("{");
    Indent();
    EmitLine("float4 xe_tf_coords = (" + OperandToMsl(instr.operands[0], 4) +
             ");");
    EmitLine("float4 xe_tf_result = float4(0.0f);");
    if (is_pixel_shader()) {
      EmitLine("float2 xe_tf_grad_x = dfdx(xe_tf_coords.xy);");
      EmitLine("float2 xe_tf_grad_y = dfdy(xe_tf_coords.xy);");
      EmitLine(
          "xe_tf_result = float4(xe_tf_grad_x.x, xe_tf_grad_y.x, "
          "xe_tf_grad_x.y, xe_tf_grad_y.y);");
    }
    EmitVectorResultAssignment(instr.result, "xe_tf_result");
    StoreConstantComponents(instr.result);
    Outdent();
    EmitLine("}");
    EmitFetchPredicationEnd(predication_open);
    return;
  }
  const uint8_t texture_sign_component_mask =
      texture_fetch
          ? uint8_t(used_result_components & used_result_nonzero_components)
          : uint8_t(0);
  uint8_t specialized_texture_signs = 0;
  const bool texture_signs_specialized =
      texture_fetch && GetNativeTextureSignSpecialization(
                           fetch_constant_index, texture_sign_component_mask,
                           specialized_texture_signs);
  const bool texture_needs_unsigned_sample =
      !texture_fetch || !texture_signs_specialized ||
      NativeMslTextureSignsNeedUnsigned(specialized_texture_signs,
                                        texture_sign_component_mask);
  const bool texture_needs_signed_sample =
      texture_fetch &&
      (!texture_signs_specialized ||
       NativeMslTextureSignsNeedSigned(specialized_texture_signs,
                                       texture_sign_component_mask));
  const bool use_sample_grad =
      instr.attributes.use_computed_lod &&
      (is_pixel_shader() || instr.attributes.use_register_gradients);
  const bool use_sample_level = !use_sample_grad;
  const xenos::TextureFilter sampler_mip_filter =
      instr.opcode == FetchOpcode::kGetTextureComputedLod
          ? xenos::TextureFilter::kLinear
          : instr.attributes.mip_filter;
  const xenos::AnisoFilter sampler_aniso_filter =
      use_sample_level ? xenos::AnisoFilter::kDisabled
                       : instr.attributes.aniso_filter;

  uint32_t texture_slot = UINT32_MAX;
  uint32_t texture_slot_signed = UINT32_MAX;
  uint32_t sampler_slot = UINT32_MAX;
  std::string sampler_expr;
  if (needs_texture_sampler) {
    if (texture_needs_unsigned_sample) {
      texture_slot =
          FindOrAddTextureBinding(fetch_constant_index, instr.dimension, false);
    }
    if (texture_needs_signed_sample) {
      texture_slot_signed =
          FindOrAddTextureBinding(fetch_constant_index, instr.dimension, true);
    }
    sampler_slot = FindOrAddSamplerBinding(
        fetch_constant_index, instr.attributes.mag_filter,
        instr.attributes.min_filter, sampler_mip_filter, sampler_aniso_filter);
    if ((texture_slot != UINT32_MAX &&
         texture_slot >= kNativeMaxTextureBindings) ||
        (texture_slot_signed != UINT32_MAX &&
         texture_slot_signed >= kNativeMaxTextureBindings) ||
        sampler_slot >= kNativeMaxSamplerBindings) {
      EmitNativeMslUnsupported(fmt::format(
          "native resource slot limit exceeded for texture fetch constant {}",
          fetch_constant_index));
      EmitVectorResultAssignment(instr.result,
                                 "float4(0.0f, 0.0f, 0.0f, 1.0f)");
      StoreConstantComponents(instr.result);
      EmitFetchPredicationEnd(predication_open);
      return;
    }
    sampler_expr = SamplerArgumentName(sampler_slot);
  }
  const std::string coords = "(" + OperandToMsl(instr.operands[0], 4) + ")";
  constexpr float kTextureRoundingOffset = 1.5f / 1024.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  float offset_z = 0.0f;
  if (instr.opcode != FetchOpcode::kGetTextureComputedLod) {
    offset_x = instr.attributes.offset_x + kTextureRoundingOffset;
    offset_y = instr.attributes.offset_y + kTextureRoundingOffset;
    offset_z = instr.attributes.offset_z + kTextureRoundingOffset;
    if (instr.dimension == xenos::FetchOpDimension::kCube) {
      offset_z = get_texture_weights ? 0.0f : instr.attributes.offset_z;
    }
    if (get_texture_weights) {
      offset_x -= 0.5f;
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k2D:
        case xenos::FetchOpDimension::kCube:
          offset_y -= 0.5f;
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          offset_y -= 0.5f;
          offset_z -= 0.5f;
          break;
        default:
          break;
      }
    }
  }
  const std::string offset_x_literal = MslFloatLiteral(offset_x);
  const std::string offset_y_literal = MslFloatLiteral(offset_y);
  const std::string offset_z_literal = MslFloatLiteral(offset_z);
  const std::string fetch_constant_literal =
      std::to_string(fetch_constant_index) + "u";
  const std::string fetch_word4_expr =
      TextureFetchWordName(fetch_constant_index, 4u);
  const std::string texture_dimension_expr =
      TextureFetchDerivedName(fetch_constant_index, "dimension");
  const std::string texture_is_3d_expr =
      "(" + texture_dimension_expr + " == 2u)";
  const std::string texture_size_1d_expr =
      TextureFetchDerivedName(fetch_constant_index, "width_1d");
  const std::string texture_size_2d_expr =
      TextureFetchDerivedName(fetch_constant_index, "size_2d");
  const std::string texture_size_3d_or_stacked_expr =
      "(xe_tf_is_3d ? " +
      TextureFetchDerivedName(fetch_constant_index, "size_3d") + " : " +
      TextureFetchDerivedName(fetch_constant_index, "size_stacked") + ")";
  const std::string texture_lod_bias_expr =
      TextureFetchDerivedName(fetch_constant_index, "lod_bias");
  const std::string texture_exp_adjust_expr =
      TextureFetchDerivedName(fetch_constant_index, "exp_adjust");
  const bool dimension_1d = instr.dimension == xenos::FetchOpDimension::k1D;
  const bool dimension_2d = instr.dimension == xenos::FetchOpDimension::k2D;
  const bool dimension_3d_or_stacked =
      instr.dimension == xenos::FetchOpDimension::k3DOrStacked;
  const bool dimension_cube = instr.dimension == xenos::FetchOpDimension::kCube;
  const bool needs_uv = needs_texture_sampler && (dimension_1d || dimension_2d);
  const bool needs_uvw = needs_texture_sampler && dimension_3d_or_stacked;
  const bool needs_cube_coord = needs_texture_sampler && dimension_cube;
  const bool needs_dir = needs_cube_coord;
  const bool needs_width =
      dimension_1d &&
      (needs_texture_sampler || !instr.attributes.unnormalized_coordinates);
  const bool needs_1d_row_remap = needs_uv && dimension_1d;
  const bool needs_size2 =
      (dimension_2d || dimension_cube) &&
      (needs_texture_sampler || !instr.attributes.unnormalized_coordinates);
  const bool needs_size3 =
      dimension_3d_or_stacked &&
      (needs_texture_sampler || !instr.attributes.unnormalized_coordinates);
  const bool needs_offset_z =
      dimension_3d_or_stacked || (dimension_cube && !get_texture_weights);
  const bool needs_is_3d = needs_size3;
  const bool needs_weight_coord = get_texture_weights || needs_1d_row_remap;

  EmitLine("{");
  Indent();
  EmitLine("float4 xe_tf_coords = " + coords + ";");
  EmitLine("float2 xe_tf_offset_xy = float2(" + offset_x_literal + ", " +
           offset_y_literal + ");");
  if (!get_texture_weights && cvars::draw_resolution_scaled_texture_offsets &&
      (draw_resolution_scale_x_ > 1u || draw_resolution_scale_y_ > 1u) &&
      (offset_x != 0.0f || offset_y != 0.0f)) {
    EmitLine("if ((xe_system.xe_textures_resolution_scaled & (1u << " +
             fetch_constant_literal + ")) != 0u) {");
    Indent();
    EmitLine("xe_tf_offset_xy *= float2(" +
             MslFloatLiteral(1.0f / float(draw_resolution_scale_x_)) + ", " +
             MslFloatLiteral(1.0f / float(draw_resolution_scale_y_)) + ");");
    Outdent();
    EmitLine("}");
  }
  if (needs_offset_z) {
    EmitLine("float xe_tf_offset_z = " + offset_z_literal + ";");
  }
  if (texture_fetch && !texture_signs_specialized) {
    EmitLine("uint xe_tf_signs_word = xe_system.xe_texture_swizzled_signs[" +
             std::to_string(fetch_constant_index >> 2) + "u];");
    EmitLine("uint xe_tf_signs = (xe_tf_signs_word >> " +
             std::to_string((fetch_constant_index & 3) * 8) + "u) & 0xFFu;");
    std::string needs_signed_expr;
    std::string needs_unsigned_expr;
    auto emit_texture_sign = [&](uint32_t component_bit,
                                 const char* component_name,
                                 const char* sign_expr) {
      if (!(used_result_components & component_bit)) {
        return;
      }
      EmitLine(std::string("uint xe_tf_sign_") + component_name + " = " +
               sign_expr + ";");
      if (!needs_signed_expr.empty()) {
        needs_signed_expr += " || ";
        needs_unsigned_expr += " || ";
      }
      needs_signed_expr +=
          std::string("xe_tf_sign_") + component_name + " == 1u";
      needs_unsigned_expr +=
          std::string("xe_tf_sign_") + component_name + " != 1u";
    };
    emit_texture_sign(0b0001, "x", "xe_tf_signs & 0x3u");
    emit_texture_sign(0b0010, "y", "(xe_tf_signs >> 2u) & 0x3u");
    emit_texture_sign(0b0100, "z", "(xe_tf_signs >> 4u) & 0x3u");
    emit_texture_sign(0b1000, "w", "(xe_tf_signs >> 6u) & 0x3u");
    EmitLine("bool xe_tf_needs_signed = " + needs_signed_expr + ";");
    EmitLine("bool xe_tf_needs_unsigned = " + needs_unsigned_expr + ";");
    if (needs_texture_runtime_info) {
      EmitLine("uint4 xe_tf_info = " +
               TextureRuntimeInfoExpression(texture_slot) + ";");
      EmitLine("if (!xe_tf_needs_unsigned && xe_tf_needs_signed) {");
      Indent();
      EmitLine("xe_tf_info = " +
               TextureRuntimeInfoExpression(texture_slot_signed) + ";");
      Outdent();
      EmitLine("}");
    }
  } else if (needs_texture_runtime_info) {
    uint32_t texture_runtime_info_slot = texture_slot;
    if (texture_fetch && texture_signs_specialized &&
        !texture_needs_unsigned_sample && texture_needs_signed_sample) {
      texture_runtime_info_slot = texture_slot_signed;
    }
    EmitLine("uint4 xe_tf_info = " +
             TextureRuntimeInfoExpression(texture_runtime_info_slot) + ";");
  }
  if (needs_width) {
    EmitLine("float xe_tf_width = " + texture_size_1d_expr + ";");
  }
  if (needs_is_3d) {
    if (!needs_texture_sampler) {
      EmitLine("bool xe_tf_is_3d = " + texture_is_3d_expr + ";");
    } else if (needs_texture_runtime_info) {
      EmitLine("bool xe_tf_is_3d = xe_tf_info.x == " +
               std::to_string(kNativeTextureRuntimeType3D) + "u;");
    } else {
      EmitLine("bool xe_tf_is_3d = false;");
    }
  }
  if (needs_size2) {
    EmitLine("float2 xe_tf_size2 = " + texture_size_2d_expr + ";");
  }
  if (needs_size3) {
    EmitLine("float3 xe_tf_size3 = " + texture_size_3d_or_stacked_expr + ";");
  }
  if (needs_uv) {
    EmitLine("float2 xe_tf_uv;");
  }
  if (needs_uvw) {
    EmitLine("float3 xe_tf_uvw;");
  }
  if (needs_dir) {
    EmitLine("float3 xe_tf_dir;");
  }
  if (needs_cube_coord) {
    EmitLine("float3 xe_tf_cube_coord;");
  }
  if (needs_weight_coord) {
    EmitLine("float3 xe_tf_weight_coord;");
  }

  switch (instr.dimension) {
    case xenos::FetchOpDimension::k1D:
      if (instr.attributes.unnormalized_coordinates) {
        if (needs_weight_coord) {
          EmitLine(
              "xe_tf_weight_coord.x = xe_tf_coords.x + xe_tf_offset_xy.x;");
        }
        if (needs_uv) {
          EmitLine(
              "xe_tf_uv = float2((xe_tf_coords.x + xe_tf_offset_xy.x) / "
              "xe_tf_width, 0.0f);");
        }
      } else {
        if (needs_weight_coord) {
          EmitLine(
              "xe_tf_weight_coord.x = xe_tf_coords.x * xe_tf_width + "
              "xe_tf_offset_xy.x;");
        }
        if (needs_uv) {
          EmitLine(
              "xe_tf_uv = float2(xe_tf_coords.x + (xe_tf_offset_xy.x / "
              "xe_tf_width), 0.0f);");
        }
      }
      if (needs_1d_row_remap) {
        EmitLine("if (" + texture_dimension_expr +
                 " == 0u && uint(xe_tf_width - 1.0f) >= 8192u) {");
        Indent();
        EmitLine("float xe_tf_row_width = 8192.0f;");
        EmitLine(
            "float xe_tf_row = floor(xe_tf_weight_coord.x / "
            "xe_tf_row_width);");
        EmitLine("float xe_tf_rows = ceil(xe_tf_width / xe_tf_row_width);");
        EmitLine("xe_tf_uv.x = frac(xe_tf_weight_coord.x / xe_tf_row_width);");
        EmitLine("xe_tf_uv.y = xe_tf_row / xe_tf_rows;");
        Outdent();
        EmitLine("}");
      }
      break;
    case xenos::FetchOpDimension::k2D:
      if (instr.attributes.unnormalized_coordinates) {
        if (needs_weight_coord) {
          EmitLine(
              "xe_tf_weight_coord.xy = xe_tf_coords.xy + xe_tf_offset_xy;");
        }
        if (needs_uv) {
          EmitLine(
              "xe_tf_uv = (xe_tf_coords.xy + xe_tf_offset_xy) / "
              "xe_tf_size2;");
        }
      } else {
        if (needs_weight_coord) {
          EmitLine(
              "xe_tf_weight_coord.xy = xe_tf_coords.xy * xe_tf_size2 + "
              "xe_tf_offset_xy;");
        }
        if (needs_uv) {
          EmitLine(
              "xe_tf_uv = xe_tf_coords.xy + xe_tf_offset_xy / xe_tf_size2;");
        }
      }
      break;
    case xenos::FetchOpDimension::k3DOrStacked:
      if (instr.attributes.unnormalized_coordinates) {
        if (needs_weight_coord) {
          EmitLine(
              "xe_tf_weight_coord = xe_tf_coords.xyz + "
              "float3(xe_tf_offset_xy, xe_tf_offset_z);");
        }
        if (needs_uvw) {
          EmitLine(
              "xe_tf_uvw = (xe_tf_coords.xyz + "
              "float3(xe_tf_offset_xy, xe_tf_offset_z)) / xe_tf_size3;");
          EmitLine(
              "if (!xe_tf_is_3d) xe_tf_uvw.z = xe_tf_coords.z + "
              "xe_tf_offset_z;");
        }
      } else {
        if (needs_weight_coord) {
          EmitLine(
              "xe_tf_weight_coord = xe_tf_coords.xyz * xe_tf_size3 + "
              "float3(xe_tf_offset_xy, xe_tf_offset_z);");
        }
        if (needs_uvw) {
          EmitLine(
              "xe_tf_uvw.xy = xe_tf_coords.xy + xe_tf_offset_xy / "
              "xe_tf_size3.xy;");
          EmitLine(
              "xe_tf_uvw.z = xe_tf_is_3d ? (xe_tf_coords.z + "
              "(xe_tf_offset_z / xe_tf_size3.z)) : "
              "(xe_tf_coords.z * xe_tf_size3.z + xe_tf_offset_z);");
        }
      }
      break;
    case xenos::FetchOpDimension::kCube:
      if (instr.attributes.unnormalized_coordinates) {
        if (needs_weight_coord) {
          EmitLine(
              "xe_tf_weight_coord.xy = xe_tf_coords.xy + xe_tf_offset_xy;");
        }
        if (needs_cube_coord) {
          EmitLine(
              "xe_tf_cube_coord = float3("
              "(xe_tf_coords.xy + xe_tf_offset_xy) / xe_tf_size2, "
              "xe_tf_coords.z + xe_tf_offset_z);");
        }
      } else {
        if (needs_weight_coord) {
          EmitLine(
              "xe_tf_weight_coord.xy = xe_tf_coords.xy * xe_tf_size2 + "
              "xe_tf_offset_xy;");
        }
        if (needs_cube_coord) {
          EmitLine(
              "xe_tf_cube_coord = float3(xe_tf_coords.xy + xe_tf_offset_xy / "
              "xe_tf_size2, xe_tf_coords.z + xe_tf_offset_z);");
        }
      }
      if (needs_offset_z) {
        if (needs_weight_coord) {
          EmitLine("xe_tf_weight_coord.z = xe_tf_coords.z + xe_tf_offset_z;");
        }
      }
      if (needs_dir) {
        EmitLine("xe_tf_dir = XeTextureCubeDirection(xe_tf_cube_coord);");
      }
      break;
    default:
      EmitNativeMslUnsupported(
          fmt::format("texture fetch dimension {} is unimplemented",
                      uint32_t(instr.dimension)));
      break;
  }

  if (instr.opcode == FetchOpcode::kGetTextureComputedLod &&
      (!is_pixel_shader() || !instr.attributes.use_computed_lod ||
       instr.attributes.use_register_lod ||
       instr.attributes.use_register_gradients)) {
    EmitNativeMslUnsupported(
        "getCompTexLOD with explicit LOD/gradients or outside pixel shader is "
        "unsupported in native MSL");
    EmitVectorResultAssignment(instr.result, "float4(0.0f)");
    StoreConstantComponents(instr.result);
    Outdent();
    EmitLine("}");
    EmitFetchPredicationEnd(predication_open);
    return;
  }

  const bool texture_result_needs_zero_init =
      !(dimension_1d || dimension_2d || dimension_3d_or_stacked ||
        dimension_cube);
  EmitLine(std::string("float4 xe_tf_result") +
           (texture_result_needs_zero_init ? " = float4(0.0f);" : ";"));
  if (instr.opcode == FetchOpcode::kGetTextureWeights) {
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
        EmitLine(
            "xe_tf_result = float4(fract(xe_tf_weight_coord.x), "
            "0.0f, 0.0f, 0.0f);");
        break;
      case xenos::FetchOpDimension::k2D:
      case xenos::FetchOpDimension::kCube:
        EmitLine(
            "xe_tf_result = float4(fract(xe_tf_weight_coord.xy), "
            "0.0f, 0.0f);");
        break;
      case xenos::FetchOpDimension::k3DOrStacked: {
        EmitLine("xe_tf_result = float4(fract(xe_tf_weight_coord), 0.0f);");
        break;
      }
      default:
        break;
    }
  } else if (instr.opcode == FetchOpcode::kGetTextureComputedLod) {
    EmitLine("float xe_tf_computed_lod = 0.0f;");
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        EmitLine(
            "xe_tf_computed_lod = " + TextureArgumentName2DArray(texture_slot) +
            ".calculate_unclamped_lod(" + sampler_expr + ", xe_tf_uv);");
        break;
      case xenos::FetchOpDimension::k3DOrStacked: {
        EmitLine("if (xe_tf_is_3d) {");
        Indent();
        EmitLine("xe_tf_computed_lod = " + TextureArgumentName3D(texture_slot) +
                 ".calculate_unclamped_lod(" + sampler_expr + ", xe_tf_uvw);");
        Outdent();
        EmitLine("} else {");
        Indent();
        EmitLine(
            "xe_tf_computed_lod = " + TextureArgumentName2DArray(texture_slot) +
            ".calculate_unclamped_lod(" + sampler_expr + ", xe_tf_uvw.xy);");
        Outdent();
        EmitLine("}");
        break;
      }
      case xenos::FetchOpDimension::kCube:
        EmitLine(
            "xe_tf_computed_lod = " + TextureArgumentNameCube(texture_slot) +
            ".calculate_unclamped_lod(" + sampler_expr + ", xe_tf_dir);");
        break;
      default:
        break;
    }
    EmitLine("xe_tf_result = float4(xe_tf_computed_lod);");
  } else {
    std::string texture_lod_expr = texture_lod_bias_expr;
    if (instr.attributes.lod_bias != 0.0f) {
      texture_lod_expr += " + " + MslFloatLiteral(instr.attributes.lod_bias);
    }
    EmitLine("float xe_tf_lod = " + texture_lod_expr + ";");
    if (instr.attributes.use_register_lod) {
      EmitLine("xe_tf_lod += xe_texture_lod;");
    }
    if (use_sample_grad) {
      if (instr.attributes.use_register_gradients) {
        EmitLine("float3 xe_tf_grad_h = xe_texture_grad_h;");
        EmitLine("float3 xe_tf_grad_v = xe_texture_grad_v;");
        if (instr.attributes.unnormalized_coordinates &&
            instr.dimension != xenos::FetchOpDimension::k1D &&
            instr.dimension != xenos::FetchOpDimension::kCube) {
          if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
            EmitLine("xe_tf_grad_h.xy /= xe_tf_size3.xy;");
            EmitLine("xe_tf_grad_v.xy /= xe_tf_size3.xy;");
            EmitLine("if (xe_tf_is_3d) {");
            Indent();
            EmitLine("xe_tf_grad_h.z /= xe_tf_size3.z;");
            EmitLine("xe_tf_grad_v.z /= xe_tf_size3.z;");
            Outdent();
            EmitLine("}");
          } else {
            EmitLine("xe_tf_grad_h.xy /= xe_tf_size2.xy;");
            EmitLine("xe_tf_grad_v.xy /= xe_tf_size2.xy;");
          }
        }
      } else {
        switch (instr.dimension) {
          case xenos::FetchOpDimension::k1D:
          case xenos::FetchOpDimension::k2D:
            EmitLine("float3 xe_tf_grad_h = float3(dfdx(xe_tf_uv), 0.0f);");
            EmitLine("float3 xe_tf_grad_v = float3(dfdy(xe_tf_uv), 0.0f);");
            break;
          case xenos::FetchOpDimension::k3DOrStacked:
            EmitLine("float3 xe_tf_grad_h = dfdx(xe_tf_uvw);");
            EmitLine("float3 xe_tf_grad_v = dfdy(xe_tf_uvw);");
            break;
          case xenos::FetchOpDimension::kCube:
            EmitLine("float3 xe_tf_grad_h = dfdx(xe_tf_dir);");
            EmitLine("float3 xe_tf_grad_v = dfdy(xe_tf_dir);");
            break;
          default:
            EmitLine("float3 xe_tf_grad_h = float3(0.0f);");
            EmitLine("float3 xe_tf_grad_v = float3(0.0f);");
            break;
        }
      }
      EmitLine("float xe_tf_lod_gradient_scale = exp2(xe_tf_lod);");
      EmitLine("xe_tf_grad_h *= xe_tf_lod_gradient_scale;");
      EmitLine("xe_tf_grad_v *= xe_tf_lod_gradient_scale;");
    }

    const bool sample_results_need_zero_init =
        !texture_fetch || !texture_signs_specialized ||
        !(dimension_1d || dimension_2d || dimension_3d_or_stacked ||
          dimension_cube);
    auto emit_sample_result_declaration = [&](const char* name) {
      EmitLine(std::string("float4 ") + name +
               (sample_results_need_zero_init ? " = float4(0.0f);" : ";"));
    };
    if (texture_needs_unsigned_sample) {
      emit_sample_result_declaration("xe_tf_result_unsigned");
    }
    if (texture_needs_signed_sample) {
      emit_sample_result_declaration("xe_tf_result_signed");
    }

    auto emit_sample =
        [&](const std::string& result_name, const std::string& texture_name,
            const std::string& sample_coord, const std::string& array_index,
            const std::string& grad_h, const std::string& grad_v) {
          std::string base = result_name + " = " + texture_name + ".sample(" +
                             sampler_expr + ", " + sample_coord;
          if (!array_index.empty()) {
            base += ", " + array_index;
          }
          if (use_sample_grad) {
            base += ", gradient";
            if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked &&
                array_index.empty()) {
              base += "3d(" + grad_h + ", " + grad_v + ")";
            } else if (instr.dimension == xenos::FetchOpDimension::kCube) {
              base += "cube(" + grad_h + ", " + grad_v + ")";
            } else {
              base += "2d(" + grad_h + ", " + grad_v + ")";
            }
          } else if (use_sample_level) {
            base += ", level(xe_tf_lod)";
          } else {
            base += ", bias(xe_tf_lod)";
          }
          base += ");";
          EmitLine(base);
        };
    auto emit_conditional_sample =
        [&](const std::string& condition, const std::string& result_name,
            const std::string& texture_name, const std::string& sample_coord,
            const std::string& array_index, const std::string& grad_h,
            const std::string& grad_v) {
          if (condition == "false") {
            return;
          }
          if (condition == "true") {
            emit_sample(result_name, texture_name, sample_coord, array_index,
                        grad_h, grad_v);
            return;
          }
          EmitLine("if (" + condition + ") {");
          Indent();
          emit_sample(result_name, texture_name, sample_coord, array_index,
                      grad_h, grad_v);
          Outdent();
          EmitLine("}");
        };
    auto condition_and = [](const std::string& a, const std::string& b) {
      if (a == "false" || b == "false") {
        return std::string("false");
      }
      if (a == "true") {
        return b;
      }
      if (b == "true") {
        return a;
      }
      return "(" + a + " && " + b + ")";
    };
    const std::string unsigned_sample_condition =
        (!texture_fetch || texture_signs_specialized)
            ? (texture_needs_unsigned_sample ? "true" : "false")
            : "xe_tf_needs_unsigned";
    const std::string signed_sample_condition =
        (!texture_fetch || texture_signs_specialized)
            ? (texture_needs_signed_sample ? "true" : "false")
            : "xe_tf_needs_signed";

    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        if (texture_needs_unsigned_sample) {
          emit_conditional_sample(
              unsigned_sample_condition, "xe_tf_result_unsigned",
              TextureArgumentName2DArray(texture_slot), "xe_tf_uv", "0u",
              "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
        }
        if (texture_needs_signed_sample) {
          emit_conditional_sample(
              signed_sample_condition, "xe_tf_result_signed",
              TextureArgumentName2DArray(texture_slot_signed), "xe_tf_uv", "0u",
              "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
        }
        break;
      case xenos::FetchOpDimension::k3DOrStacked: {
        EmitLine("if (xe_tf_is_3d) {");
        Indent();
        if (texture_needs_unsigned_sample) {
          emit_conditional_sample(
              unsigned_sample_condition, "xe_tf_result_unsigned",
              TextureArgumentName3D(texture_slot), "xe_tf_uvw", "",
              "xe_tf_grad_h", "xe_tf_grad_v");
        }
        if (texture_needs_signed_sample) {
          emit_conditional_sample(
              signed_sample_condition, "xe_tf_result_signed",
              TextureArgumentName3D(texture_slot_signed), "xe_tf_uvw", "",
              "xe_tf_grad_h", "xe_tf_grad_v");
        }
        Outdent();
        EmitLine("} else {");
        Indent();
        EmitLine("float xe_tf_stacked_coord = xe_tf_uvw.z;");
        EmitLine("bool xe_tf_stacked_linear = false;");
        const bool vol_mag_filter_is_fetch_const =
            instr.attributes.vol_mag_filter ==
            xenos::TextureFilter::kUseFetchConst;
        const bool vol_min_filter_is_fetch_const =
            instr.attributes.vol_min_filter ==
            xenos::TextureFilter::kUseFetchConst;
        const bool vol_mag_filter_is_linear =
            instr.attributes.vol_mag_filter == xenos::TextureFilter::kLinear;
        const bool vol_min_filter_is_linear =
            instr.attributes.vol_min_filter == xenos::TextureFilter::kLinear;
        const bool needs_runtime_volume_filter =
            use_sample_grad &&
            (vol_mag_filter_is_fetch_const || vol_min_filter_is_fetch_const ||
             vol_mag_filter_is_linear != vol_min_filter_is_linear);
        if (needs_runtime_volume_filter) {
          EmitLine(
              "float xe_tf_layer_gradient = "
              "max(abs(xe_tf_grad_h.z), abs(xe_tf_grad_v.z));");
          if (!instr.attributes.unnormalized_coordinates) {
            EmitLine("xe_tf_layer_gradient *= xe_tf_size3.z;");
          }
          EmitLine("bool xe_tf_layer_minifying = xe_tf_layer_gradient > 1.0f;");
          if (vol_mag_filter_is_fetch_const || vol_min_filter_is_fetch_const) {
            EmitLine("if (xe_tf_layer_minifying) {");
            Indent();
            if (vol_min_filter_is_fetch_const) {
              EmitLine("xe_tf_stacked_linear = ((" + fetch_word4_expr +
                       " >> 1u) & 1u) != 0u;");
            } else {
              EmitLine(std::string("xe_tf_stacked_linear = ") +
                       (vol_min_filter_is_linear ? "true;" : "false;"));
            }
            Outdent();
            EmitLine("} else {");
            Indent();
            if (vol_mag_filter_is_fetch_const) {
              EmitLine("xe_tf_stacked_linear = (" + fetch_word4_expr +
                       " & 1u) != 0u;");
            } else {
              EmitLine(std::string("xe_tf_stacked_linear = ") +
                       (vol_mag_filter_is_linear ? "true;" : "false;"));
            }
            Outdent();
            EmitLine("}");
          } else if (vol_mag_filter_is_linear != vol_min_filter_is_linear) {
            EmitLine(std::string("xe_tf_stacked_linear = ") +
                     (vol_min_filter_is_linear ? "xe_tf_layer_minifying;"
                                               : "!xe_tf_layer_minifying;"));
          } else {
            EmitLine(std::string("xe_tf_stacked_linear = ") +
                     (vol_mag_filter_is_linear ? "true;" : "false;"));
          }
        } else if (vol_mag_filter_is_fetch_const) {
          EmitLine("xe_tf_stacked_linear = (" + fetch_word4_expr +
                   " & 1u) != 0u;");
        } else {
          EmitLine(std::string("xe_tf_stacked_linear = ") +
                   (vol_mag_filter_is_linear ? "true;" : "false;"));
        }
        EmitLine("float xe_tf_layer_lerp = 0.0f;");
        EmitLine("if (xe_tf_stacked_linear) {");
        Indent();
        EmitLine("xe_tf_stacked_coord -= 0.5f;");
        EmitLine("xe_tf_layer_lerp = fract(xe_tf_stacked_coord);");
        Outdent();
        EmitLine("}");
        EmitLine(
            "uint xe_tf_layer0 = uint(clamp(floor(xe_tf_stacked_coord), "
            "0.0f, max(xe_tf_size3.z - 1.0f, 0.0f)));");
        EmitLine(
            "uint xe_tf_layer1 = uint(clamp(floor(xe_tf_stacked_coord) + "
            "1.0f, 0.0f, max(xe_tf_size3.z - 1.0f, 0.0f)));");
        if (texture_needs_unsigned_sample) {
          emit_conditional_sample(
              unsigned_sample_condition, "xe_tf_result_unsigned",
              TextureArgumentName2DArray(texture_slot), "xe_tf_uvw.xy",
              "xe_tf_layer0", "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          EmitLine(
              "if (" +
              condition_and(unsigned_sample_condition, "xe_tf_stacked_linear") +
              ") {");
          Indent();
          EmitLine("float4 xe_tf_result_unsigned_layer1 = float4(0.0f);");
          emit_sample("xe_tf_result_unsigned_layer1",
                      TextureArgumentName2DArray(texture_slot), "xe_tf_uvw.xy",
                      "xe_tf_layer1", "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          EmitLine(
              "xe_tf_result_unsigned = mix(xe_tf_result_unsigned, "
              "xe_tf_result_unsigned_layer1, xe_tf_layer_lerp);");
          Outdent();
          EmitLine("}");
        }
        if (texture_needs_signed_sample) {
          emit_conditional_sample(
              signed_sample_condition, "xe_tf_result_signed",
              TextureArgumentName2DArray(texture_slot_signed), "xe_tf_uvw.xy",
              "xe_tf_layer0", "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          EmitLine(
              "if (" +
              condition_and(signed_sample_condition, "xe_tf_stacked_linear") +
              ") {");
          Indent();
          EmitLine("float4 xe_tf_result_signed_layer1 = float4(0.0f);");
          emit_sample("xe_tf_result_signed_layer1",
                      TextureArgumentName2DArray(texture_slot_signed),
                      "xe_tf_uvw.xy", "xe_tf_layer1", "xe_tf_grad_h.xy",
                      "xe_tf_grad_v.xy");
          EmitLine(
              "xe_tf_result_signed = mix(xe_tf_result_signed, "
              "xe_tf_result_signed_layer1, xe_tf_layer_lerp);");
          Outdent();
          EmitLine("}");
        }
        Outdent();
        EmitLine("}");
        break;
      }
      case xenos::FetchOpDimension::kCube:
        if (texture_needs_unsigned_sample) {
          emit_conditional_sample(
              unsigned_sample_condition, "xe_tf_result_unsigned",
              TextureArgumentNameCube(texture_slot), "xe_tf_dir", "",
              "xe_tf_grad_h", "xe_tf_grad_v");
        }
        if (texture_needs_signed_sample) {
          emit_conditional_sample(
              signed_sample_condition, "xe_tf_result_signed",
              TextureArgumentNameCube(texture_slot_signed), "xe_tf_dir", "",
              "xe_tf_grad_h", "xe_tf_grad_v");
        }
        break;
      default:
        break;
    }

    if (texture_fetch) {
      bool emitted_texture_exp_adjust_local = false;
      const std::string texture_exp_adjust_local = "xe_tf_exp_adjust";
      auto emit_texture_exp_adjust_local = [&]() {
        if (emitted_texture_exp_adjust_local) {
          return;
        }
        EmitLine("float " + texture_exp_adjust_local + " = " +
                 texture_exp_adjust_expr + ";");
        emitted_texture_exp_adjust_local = true;
      };
      bool emitted_specialized_vector_result = false;
      if (texture_signs_specialized) {
        auto texture_sign = [&](uint32_t component_index) {
          return uint32_t(
              (specialized_texture_signs >> (component_index * 2u)) & 0x3u);
        };
        const uint32_t sign_x = texture_sign(0);
        const uint32_t sign_y = texture_sign(1);
        const uint32_t sign_z = texture_sign(2);
        const uint32_t sign_w = texture_sign(3);
        if (used_result_components == 0b1111u && sign_x == sign_y &&
            sign_x == sign_z && sign_x == sign_w) {
          EmitLine("xe_tf_result = (" +
                   NativeMslApplyTextureSignExpr(
                       "xe_tf_result_unsigned", "xe_tf_result_signed", sign_x) +
                   ") * " + texture_exp_adjust_expr + ";");
          emitted_specialized_vector_result = true;
        } else if ((used_result_components == 0b0111u ||
                    used_result_components == 0b1111u) &&
                   sign_x == sign_y && sign_x == sign_z &&
                   sign_w == uint32_t(xenos::TextureSign::kUnsigned)) {
          const std::string rgb_expr = NativeMslApplyTextureSignExpr(
              "xe_tf_result_unsigned.xyz", "xe_tf_result_signed.xyz", sign_x);
          if (used_result_components == 0b1111u) {
            EmitLine("xe_tf_result = float4(" + rgb_expr +
                     ", xe_tf_result_unsigned.w) * " + texture_exp_adjust_expr +
                     ";");
          } else {
            EmitLine("xe_tf_result = float4((" + rgb_expr + ") * " +
                     texture_exp_adjust_expr + ", 0.0f);");
          }
          emitted_specialized_vector_result = true;
        } else if ((used_result_components & 0b0011u) == 0b0011u &&
                   sign_x == sign_y) {
          const std::string xy_exp_adjust_expr =
              (used_result_components & 0b1100u) ? texture_exp_adjust_local
                                                 : texture_exp_adjust_expr;
          if (xy_exp_adjust_expr == texture_exp_adjust_local) {
            emit_texture_exp_adjust_local();
          }
          const std::string xy_expr = NativeMslApplyTextureSignExpr(
              "xe_tf_result_unsigned.xy", "xe_tf_result_signed.xy", sign_x);
          auto signed_component_expr = [&](uint32_t component_index,
                                           const char* component_name,
                                           uint32_t sign) {
            const uint32_t component_bit = 1u << component_index;
            if (!(used_result_components & component_bit)) {
              return std::string("0.0f");
            }
            const std::string unsigned_value =
                texture_needs_unsigned_sample
                    ? (std::string("xe_tf_result_unsigned.") + component_name)
                    : "0.0f";
            const std::string signed_value =
                texture_needs_signed_sample
                    ? (std::string("xe_tf_result_signed.") + component_name)
                    : "0.0f";
            return "(" +
                   NativeMslApplyTextureSignExpr(unsigned_value, signed_value,
                                                 sign) +
                   " * " + xy_exp_adjust_expr + ")";
          };
          EmitLine("xe_tf_result = float4((" + xy_expr + ") * " +
                   xy_exp_adjust_expr + ", " +
                   signed_component_expr(2, "z", sign_z) + ", " +
                   signed_component_expr(3, "w", sign_w) + ");");
          emitted_specialized_vector_result = true;
        }
      }
      if (!emitted_specialized_vector_result) {
        emit_texture_exp_adjust_local();
        EmitLine("xe_tf_result = float4(");
        Indent();
      }
      auto emit_texture_result_component = [&](uint32_t component_index,
                                               uint32_t component_bit,
                                               const char* component_name,
                                               bool last) {
        std::string line;
        if (used_result_components & component_bit) {
          if (texture_signs_specialized) {
            const uint32_t sign =
                (specialized_texture_signs >> (component_index * 2)) & 0x3u;
            const std::string unsigned_value =
                texture_needs_unsigned_sample
                    ? (std::string("xe_tf_result_unsigned.") + component_name)
                    : "0.0f";
            const std::string signed_value =
                texture_needs_signed_sample
                    ? (std::string("xe_tf_result_signed.") + component_name)
                    : "0.0f";
            line = "(" +
                   NativeMslApplyTextureSignExpr(unsigned_value, signed_value,
                                                 sign) +
                   " * xe_tf_exp_adjust)";
          } else {
            line = "(XeApplyTextureSign(xe_tf_result_unsigned.";
            line += component_name;
            line += ", xe_tf_result_signed.";
            line += component_name;
            line += ", xe_tf_sign_";
            line += component_name;
            line += ") * xe_tf_exp_adjust)";
          }
        } else {
          line = "0.0f";
        }
        line += last ? ");" : ",";
        EmitLine(line);
      };
      if (!emitted_specialized_vector_result) {
        emit_texture_result_component(0, 0b0001, "x", false);
        emit_texture_result_component(1, 0b0010, "y", false);
        emit_texture_result_component(2, 0b0100, "z", false);
        emit_texture_result_component(3, 0b1000, "w", true);
        Outdent();
      }
    } else {
      EmitLine("xe_tf_result = xe_tf_result_unsigned;");
    }
  }

  EmitVectorResultAssignment(instr.result, "xe_tf_result");
  StoreConstantComponents(instr.result);
  Outdent();
  EmitLine("}");
  EmitFetchPredicationEnd(predication_open);
}

void MslShaderTranslator::ProcessAluInstruction(
    const ParsedAluInstruction& instr,
    uint8_t memexport_eM_potentially_written_before) {
  uint8_t previous_kill_memexport_flush_mask = alu_kill_memexport_flush_mask_;
  alu_kill_memexport_flush_mask_ = (IsVectorKillOpcode(instr.vector_opcode) ||
                                    IsScalarKillOpcode(instr.scalar_opcode))
                                       ? memexport_eM_potentially_written_before
                                       : 0;
  bool needs_predicate_close = false;
  if (instr.is_predicated) {
    EmitLine("if (xe_p0 " +
             std::string(instr.predicate_condition ? "==" : "!=") + " true) {");
    Indent();
    needs_predicate_close = true;
  }
  std::string vector_result_expr;
  bool has_vector_result =
      ProcessVectorAluInstruction(instr, &vector_result_expr);
  std::string scalar_result_expr;
  bool has_scalar_result =
      ProcessScalarAluInstruction(instr, &scalar_result_expr);
  if (has_vector_result) {
    EmitVectorResultAssignment(instr.vector_and_constant_result,
                               vector_result_expr);
    StoreConstantComponents(instr.vector_and_constant_result);
  }
  if (has_scalar_result) {
    EmitScalarResultAssignment(instr.scalar_result, scalar_result_expr);
  }
  StoreConstantComponents(instr.scalar_result);
  if (needs_predicate_close) {
    Outdent();
    EmitLine("}");
  }
  alu_kill_memexport_flush_mask_ = previous_kill_memexport_flush_mask;
}

bool MslShaderTranslator::ProcessVectorAluInstruction(
    const ParsedAluInstruction& instr, std::string* staged_result_expr) {
  uint32_t used_result_components =
      instr.vector_and_constant_result.GetUsedResultComponents();
  if (!used_result_components &&
      !ucode::GetAluVectorOpcodeInfo(instr.vector_opcode).changed_state) {
    return false;
  }
  std::string op0, op1, op2;
  if (instr.vector_operand_count > 0) {
    op0 = OperandToMsl(instr.vector_operands[0], 4);
  }
  if (instr.vector_operand_count > 1) {
    op1 = OperandToMsl(instr.vector_operands[1], 4);
  }
  if (instr.vector_operand_count > 2) {
    op2 = OperandToMsl(instr.vector_operands[2], 4);
  }
  std::string result;
  using AluVectorOpcode = ucode::AluVectorOpcode;
  switch (instr.vector_opcode) {
    case ucode::AluVectorOpcode::kAdd:
      result = "(" + op0 + " + " + op1 + ")";
      break;
    case ucode::AluVectorOpcode::kMul:
      result = MslMulSM3Expr(op0, op1, 4);
      break;
    case ucode::AluVectorOpcode::kMad:
      if (staged_result_expr) {
        *staged_result_expr = "xe_pv";
        EmitLine("xe_pv = " + MslMulSM3Expr(op0, op1, 4) + ";");
        EmitLine("xe_pv = xe_pv + " + op2 + ";");
        return true;
      }
      result = "(" + MslMulSM3Expr(op0, op1, 4) + " + " + op2 + ")";
      break;
    case ucode::AluVectorOpcode::kMax:
      result = op0 == op1
                   ? op0
                   : SelectExpr("(" + op0 + " >= " + op1 + ")", op0, op1);
      break;
    case ucode::AluVectorOpcode::kMin:
      result = op0 == op1 ? op0
                          : SelectExpr("(" + op0 + " < " + op1 + ")", op0, op1);
      break;
    case ucode::AluVectorOpcode::kSeq:
      result = SelectExpr("(" + op0 + " == " + op1 + ")", "float4(1.0f)",
                          "float4(0.0f)");
      break;
    case ucode::AluVectorOpcode::kSgt:
      result = SelectExpr("(" + op0 + " > " + op1 + ")", "float4(1.0f)",
                          "float4(0.0f)");
      break;
    case ucode::AluVectorOpcode::kSge:
      result = SelectExpr("(" + op0 + " >= " + op1 + ")", "float4(1.0f)",
                          "float4(0.0f)");
      break;
    case ucode::AluVectorOpcode::kSne:
      result = SelectExpr("(" + op0 + " != " + op1 + ")", "float4(1.0f)",
                          "float4(0.0f)");
      break;
    case ucode::AluVectorOpcode::kFrc:
      result = "frac(" + op0 + ")";
      break;
    case ucode::AluVectorOpcode::kTrunc:
      result = "trunc(" + op0 + ")";
      break;
    case ucode::AluVectorOpcode::kFloor:
      result = "floor(" + op0 + ")";
      break;
    case ucode::AluVectorOpcode::kCndEq:
      result = SelectExpr("(" + op0 + " == float4(0.0f))", op1, op2);
      break;
    case ucode::AluVectorOpcode::kCndGe:
      result = SelectExpr("(" + op0 + " >= float4(0.0f))", op1, op2);
      break;
    case ucode::AluVectorOpcode::kCndGt:
      result = SelectExpr("(" + op0 + " > float4(0.0f))", op1, op2);
      break;
    case ucode::AluVectorOpcode::kDp4:
      result = "float4(" + MslMulSM3Expr(op0 + ".x", op1 + ".x", 1) + " + " +
               MslMulSM3Expr(op0 + ".y", op1 + ".y", 1) + " + " +
               MslMulSM3Expr(op0 + ".z", op1 + ".z", 1) + " + " +
               MslMulSM3Expr(op0 + ".w", op1 + ".w", 1) + ")";
      break;
    case ucode::AluVectorOpcode::kDp3:
      result = "float4(" + MslMulSM3Expr(op0 + ".x", op1 + ".x", 1) + " + " +
               MslMulSM3Expr(op0 + ".y", op1 + ".y", 1) + " + " +
               MslMulSM3Expr(op0 + ".z", op1 + ".z", 1) + ")";
      break;
    case ucode::AluVectorOpcode::kDp2Add:
      result = "float4(" + MslMulSM3Expr(op0 + ".x", op1 + ".x", 1) + " + " +
               MslMulSM3Expr(op0 + ".y", op1 + ".y", 1) + " + " +
               OperandToMsl(instr.vector_operands[2], 1) + ")";
      break;
    case ucode::AluVectorOpcode::kCube: {
      if (staged_result_expr) {
        *staged_result_expr = "xe_pv";
      }
      EmitLine("{");
      Indent();
      EmitLine("float4 xe_cube_o = " + op0 + ";");
      EmitLine("float xe_cube_x = xe_cube_o.z;");
      EmitLine("float xe_cube_y = xe_cube_o.w;");
      EmitLine("float xe_cube_z = xe_cube_o.x;");
      EmitLine("float4 xe_cube_result;");
      EmitLine(
          "if (abs(xe_cube_z) >= abs(xe_cube_x) && abs(xe_cube_z) >= "
          "abs(xe_cube_y)) {");
      Indent();
      EmitLine(
          "xe_cube_result = float4(-xe_cube_y, xe_cube_z < 0.0f ? -xe_cube_x : "
          "xe_cube_x, 2.0f * xe_cube_z, xe_cube_z < 0.0f ? 5.0f : 4.0f);");
      Outdent();
      EmitLine("} else if (abs(xe_cube_y) >= abs(xe_cube_x)) {");
      Indent();
      EmitLine(
          "xe_cube_result = float4(xe_cube_y < 0.0f ? -xe_cube_z : xe_cube_z, "
          "xe_cube_x, 2.0f * xe_cube_y, xe_cube_y < 0.0f ? 3.0f : 2.0f);");
      Outdent();
      EmitLine("} else {");
      Indent();
      EmitLine(
          "xe_cube_result = float4(-xe_cube_y, xe_cube_x < 0.0f ? xe_cube_z : "
          "-xe_cube_z, 2.0f * xe_cube_x, xe_cube_x < 0.0f ? 1.0f : 0.0f);");
      Outdent();
      EmitLine("}");
      if (staged_result_expr) {
        EmitLine(*staged_result_expr + " = xe_cube_result;");
      } else {
        EmitVectorResultAssignment(instr.vector_and_constant_result,
                                   "xe_cube_result");
        StoreConstantComponents(instr.vector_and_constant_result);
      }
      Outdent();
      EmitLine("}");
      return true;
    }
    case ucode::AluVectorOpcode::kMax4:
      result = "float4(max(max(" + op0 + ".x, " + op0 + ".y), max(" + op0 +
               ".z, " + op0 + ".w)))";
      break;
    case ucode::AluVectorOpcode::kSetpEqPush:
      EmitLine("xe_p0 = ((" + op0 + ".w == 0.0f) && (" + op1 + ".w == 0.0f));");
      result = "float4(((" + op0 + ".x == 0.0f) && (" + op1 +
               ".x == 0.0f)) ? 0.0f : (" + op0 + ".x + 1.0f))";
      break;
    case ucode::AluVectorOpcode::kSetpNePush:
      EmitLine("xe_p0 = ((" + op0 + ".w == 0.0f) && (" + op1 + ".w != 0.0f));");
      result = "float4(((" + op0 + ".x == 0.0f) && (" + op1 +
               ".x != 0.0f)) ? 0.0f : (" + op0 + ".x + 1.0f))";
      break;
    case ucode::AluVectorOpcode::kSetpGtPush:
      EmitLine("xe_p0 = ((" + op0 + ".w == 0.0f) && (" + op1 + ".w > 0.0f));");
      result = "float4(((" + op0 + ".x == 0.0f) && (" + op1 +
               ".x > 0.0f)) ? 0.0f : (" + op0 + ".x + 1.0f))";
      break;
    case ucode::AluVectorOpcode::kSetpGePush:
      EmitLine("xe_p0 = ((" + op0 + ".w == 0.0f) && (" + op1 + ".w >= 0.0f));");
      result = "float4(((" + op0 + ".x == 0.0f) && (" + op1 +
               ".x >= 0.0f)) ? 0.0f : (" + op0 + ".x + 1.0f))";
      break;
    case ucode::AluVectorOpcode::kKillEq:
      EmitLine("if (any(" + op0 + " == " + op1 + ")) {");
      Indent();
      EmitDiscardFragment();
      Outdent();
      EmitLine("}");
      result =
          "(any(" + op0 + " == " + op1 + ") ? float4(1.0f) : float4(0.0f))";
      break;
    case ucode::AluVectorOpcode::kKillGt:
      EmitLine("if (any(" + op0 + " > " + op1 + ")) {");
      Indent();
      EmitDiscardFragment();
      Outdent();
      EmitLine("}");
      result = "(any(" + op0 + " > " + op1 + ") ? float4(1.0f) : float4(0.0f))";
      break;
    case ucode::AluVectorOpcode::kKillGe:
      EmitLine("if (any(" + op0 + " >= " + op1 + ")) {");
      Indent();
      EmitDiscardFragment();
      Outdent();
      EmitLine("}");
      result =
          "(any(" + op0 + " >= " + op1 + ") ? float4(1.0f) : float4(0.0f))";
      break;
    case ucode::AluVectorOpcode::kKillNe:
      EmitLine("if (any(" + op0 + " != " + op1 + ")) {");
      Indent();
      EmitDiscardFragment();
      Outdent();
      EmitLine("}");
      result =
          "(any(" + op0 + " != " + op1 + ") ? float4(1.0f) : float4(0.0f))";
      break;
    case ucode::AluVectorOpcode::kDst:
      result = "float4(1.0f, " + MslMulSM3Expr(op0 + ".y", op1 + ".y", 1) +
               ", " + op0 + ".z, " + op1 + ".w)";
      break;
    case ucode::AluVectorOpcode::kMaxA:
      EmitLine("xe_a0 = XeAddressIndexRound(" + op0 + ".w);");
      result = op0 == op1
                   ? op0
                   : SelectExpr("(" + op0 + " >= " + op1 + ")", op0, op1);
      break;
    default:
      EmitNativeMslUnsupported(fmt::format(
          "unhandled vector ALU opcode {}",
          instr.vector_opcode_name ? instr.vector_opcode_name : "<unknown>"));
      result = "float4(0.0f)";
      break;
  }
  if (!result.empty()) {
    if (staged_result_expr) {
      *staged_result_expr = "xe_pv";
      EmitLine("xe_pv = " + result + ";");
    } else {
      EmitVectorResultAssignment(instr.vector_and_constant_result, result);
      StoreConstantComponents(instr.vector_and_constant_result);
    }
    return true;
  }
  return false;
}

bool MslShaderTranslator::ProcessScalarAluInstruction(
    const ParsedAluInstruction& instr, std::string* staged_result_expr) {
  using AluScalarOpcode = ucode::AluScalarOpcode;
  if (instr.scalar_opcode == ucode::AluScalarOpcode::kRetainPrev) {
    return false;
  }
  std::string op0_a, op0_b, op1;
  if (instr.scalar_operand_count > 0) {
    const auto& operand0 = instr.scalar_operands[0];
    SwizzleSource comp_a = operand0.components[0];
    SwizzleSource comp_b = operand0.components[1];
    std::string base = OperandToMslNoSwizzle(operand0);
    std::string base_mod = base;
    if (operand0.is_absolute_value) {
      base_mod = "abs(" + base + ")";
    }
    if (operand0.is_negated) {
      base_mod = "-(" + base_mod + ")";
    }
    op0_a = base_mod + "." + GetCharForSwizzle(comp_a);
    op0_b = base_mod + "." + GetCharForSwizzle(comp_b);
  }
  if (instr.scalar_operand_count > 1) {
    op1 = OperandToMsl(instr.scalar_operands[1], 1);
  }
  std::string result;
  switch (instr.scalar_opcode) {
    case ucode::AluScalarOpcode::kAdds:
      result = "(" + op0_a + " + " + op0_b + ")";
      break;
    case ucode::AluScalarOpcode::kAddsPrev:
      result = "(" + op0_a + " + xe_ps.x)";
      break;
    case ucode::AluScalarOpcode::kMuls:
      result = MslMulSM3Expr(op0_a, op0_b, 1);
      break;
    case ucode::AluScalarOpcode::kMulsPrev:
      result = MslMulSM3Expr(op0_a, "xe_ps.x", 1);
      break;
    case ucode::AluScalarOpcode::kMulsPrev2:
      result =
          "((xe_ps.y == -3.402823466e+38f || !XeIsFinite(xe_ps.y) || "
          "!XeIsFinite(" +
          op0_b + ") || " + op0_b + " <= 0.0f) ? -3.402823466e+38f : " +
          MslMulSM3Expr(op0_a, "xe_ps.y", 1) + ")";
      break;
    case ucode::AluScalarOpcode::kSubs:
      result = "(" + op0_a + " - " + op0_b + ")";
      break;
    case ucode::AluScalarOpcode::kSubsPrev:
      result = "(" + op0_a + " - xe_ps.x)";
      break;
    case ucode::AluScalarOpcode::kMaxs:
      result = op0_a == op0_b ? op0_a
                              : "(" + op0_a + " >= " + op0_b + " ? " + op0_a +
                                    " : " + op0_b + ")";
      break;
    case ucode::AluScalarOpcode::kMins:
      result = op0_a == op0_b ? op0_a
                              : "(" + op0_a + " < " + op0_b + " ? " + op0_a +
                                    " : " + op0_b + ")";
      break;
    case ucode::AluScalarOpcode::kSeqs:
      result = "(" + op0_a + " == 0.0f ? 1.0f : 0.0f)";
      break;
    case ucode::AluScalarOpcode::kSgts:
      result = "(" + op0_a + " > 0.0f ? 1.0f : 0.0f)";
      break;
    case ucode::AluScalarOpcode::kSges:
      result = "(" + op0_a + " >= 0.0f ? 1.0f : 0.0f)";
      break;
    case ucode::AluScalarOpcode::kSnes:
      result = "(" + op0_a + " != 0.0f ? 1.0f : 0.0f)";
      break;
    case ucode::AluScalarOpcode::kFrcs:
      result = "frac(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kTruncs:
      result = "trunc(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kFloors:
      result = "floor(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kExp:
      result = "exp2(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kLogc:
      result = "XeLogc(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kLog:
      result = "log2(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kRcpc:
      result = "XeRcpc(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kRcpf:
      result = "XeRcpf(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kRcp:
      result = "(1.0f / " + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kRsqc:
      result = "XeRsqc(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kRsqf:
      result = "XeRsqf(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kRsq:
      result = "rsqrt(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kMaxAs:
      EmitLine("xe_a0 = XeAddressIndexRound(" + op0_a + ");");
      result = op0_a == op0_b ? op0_a
                              : "(" + op0_a + " >= " + op0_b + " ? " + op0_a +
                                    " : " + op0_b + ")";
      break;
    case ucode::AluScalarOpcode::kMaxAsf:
      EmitLine("xe_a0 = XeAddressIndexFloor(" + op0_a + ");");
      result = op0_a == op0_b ? op0_a
                              : "(" + op0_a + " >= " + op0_b + " ? " + op0_a +
                                    " : " + op0_b + ")";
      break;
    case ucode::AluScalarOpcode::kSetpEq:
      EmitLine("xe_p0 = (" + op0_a + " == 0.0f);");
      result = "(xe_p0 ? 0.0f : 1.0f)";
      break;
    case ucode::AluScalarOpcode::kSetpNe:
      EmitLine("xe_p0 = (" + op0_a + " != 0.0f);");
      result = "(xe_p0 ? 0.0f : 1.0f)";
      break;
    case ucode::AluScalarOpcode::kSetpGt:
      EmitLine("xe_p0 = (" + op0_a + " > 0.0f);");
      result = "(xe_p0 ? 0.0f : 1.0f)";
      break;
    case ucode::AluScalarOpcode::kSetpGe:
      EmitLine("xe_p0 = (" + op0_a + " >= 0.0f);");
      result = "(xe_p0 ? 0.0f : 1.0f)";
      break;
    case ucode::AluScalarOpcode::kSetpInv:
      EmitLine("xe_p0 = (" + op0_a + " == 1.0f);");
      result =
          "(xe_p0 ? 0.0f : ((" + op0_a + " == 0.0f) ? 1.0f : " + op0_a + "))";
      break;
    case ucode::AluScalarOpcode::kSetpPop:
      EmitLine("xe_p0 = ((" + op0_a + " - 1.0f) <= 0.0f);");
      result = "(xe_p0 ? 0.0f : (" + op0_a + " - 1.0f))";
      break;
    case ucode::AluScalarOpcode::kSetpClr:
      EmitLine("xe_p0 = false;");
      result = "3.402823466e+38f";
      break;
    case ucode::AluScalarOpcode::kSetpRstr:
      EmitLine("xe_p0 = (" + op0_a + " == 0.0f);");
      result = "(xe_p0 ? 0.0f : " + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kKillsEq:
      EmitLine("if (" + op0_a + " == 0.0f) {");
      Indent();
      EmitDiscardFragment();
      Outdent();
      EmitLine("}");
      result = "((" + op0_a + " == 0.0f) ? 1.0f : 0.0f)";
      break;
    case ucode::AluScalarOpcode::kKillsGt:
      EmitLine("if (" + op0_a + " > 0.0f) {");
      Indent();
      EmitDiscardFragment();
      Outdent();
      EmitLine("}");
      result = "((" + op0_a + " > 0.0f) ? 1.0f : 0.0f)";
      break;
    case ucode::AluScalarOpcode::kKillsGe:
      EmitLine("if (" + op0_a + " >= 0.0f) {");
      Indent();
      EmitDiscardFragment();
      Outdent();
      EmitLine("}");
      result = "((" + op0_a + " >= 0.0f) ? 1.0f : 0.0f)";
      break;
    case ucode::AluScalarOpcode::kKillsNe:
      EmitLine("if (" + op0_a + " != 0.0f) {");
      Indent();
      EmitDiscardFragment();
      Outdent();
      EmitLine("}");
      result = "((" + op0_a + " != 0.0f) ? 1.0f : 0.0f)";
      break;
    case ucode::AluScalarOpcode::kKillsOne:
      EmitLine("if (" + op0_a + " == 1.0f) {");
      Indent();
      EmitDiscardFragment();
      Outdent();
      EmitLine("}");
      result = "((" + op0_a + " == 1.0f) ? 1.0f : 0.0f)";
      break;
    case ucode::AluScalarOpcode::kSqrt:
      result = "sqrt(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kSin:
      result = "sin(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kCos:
      result = "cos(" + op0_a + ")";
      break;
    case ucode::AluScalarOpcode::kMulsc0:
    case ucode::AluScalarOpcode::kMulsc1:
      result = MslMulSM3Expr(op0_a, op1, 1);
      break;
    case ucode::AluScalarOpcode::kAddsc0:
    case ucode::AluScalarOpcode::kAddsc1:
      result = "(" + op0_a + " + " + op1 + ")";
      break;
    case ucode::AluScalarOpcode::kSubsc0:
    case ucode::AluScalarOpcode::kSubsc1:
      result = "(" + op0_a + " - " + op1 + ")";
      break;
    default:
      EmitNativeMslUnsupported(fmt::format(
          "unhandled scalar ALU opcode {}",
          instr.scalar_opcode_name ? instr.scalar_opcode_name : "<unknown>"));
      result = "0.0f";
      break;
  }
  if (!result.empty()) {
    if (staged_result_expr) {
      *staged_result_expr = "xe_ps_result";
      EmitLine("xe_ps_result = " + result + ";");
      EmitLine("xe_ps = float4(" + *staged_result_expr + ");");
    } else {
      EmitLine("xe_ps = float4(" + result + ");");
      EmitScalarResultAssignment(instr.scalar_result, result);
    }
    return true;
  }
  return false;
}

void MslShaderTranslator::CloseExecConditionals() {
  if (cf_exec_predicated_ || cf_exec_bool_constant_ != UINT32_MAX) {
    Outdent();
    EmitLine("}");
  }
  cf_exec_predicated_ = false;
  cf_exec_bool_constant_ = UINT32_MAX;
}

}  // namespace gpu
}  // namespace xe
