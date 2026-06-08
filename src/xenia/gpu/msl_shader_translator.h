/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Native Metal Shading Language production-path translator.
 *
 * This backend emits MSL source directly from the analyzed Xenos ucode stream.
 * It intentionally uses a native, direct Metal ABI and does not consume the
 * HLSL/DXIL/Metal Shader Converter ABI.
 ******************************************************************************
 */

#ifndef XENIA_GPU_MSL_SHADER_TRANSLATOR_H_
#define XENIA_GPU_MSL_SHADER_TRANSLATOR_H_

#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xenia/gpu/shader_abi.h"
#include "xenia/gpu/shader_translator.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/graphics_provider.h"

namespace xe {
namespace gpu {

// Direct native-MSL bind points. These are intentionally high for CBVs so the
// native path can coexist with the MSC path while command-processor integration
// is brought up incrementally.
inline constexpr uint32_t kNativeBufferSystemConstants = 20;
inline constexpr uint32_t kNativeBufferFloatConstants = 21;
inline constexpr uint32_t kNativeBufferBoolLoopConstants = 22;
inline constexpr uint32_t kNativeBufferFetchConstants = 23;
inline constexpr uint32_t kNativeBufferDescriptorIndices = 24;
inline constexpr uint32_t kNativeBufferSharedMemory = 25;
inline constexpr uint32_t kNativeBufferTextureRuntimeInfo = 26;
// Spare slot for future memexport debug capture.
inline constexpr uint32_t kNativeBufferMemExportDebug = 27;
inline constexpr uint32_t kNativeBufferPrimitiveIndexConstants = 28;
// Argument-buffer record of per-draw constant-buffer GPU addresses. This keeps
// volatile CBV offsets out of the render encoder buffer table.
inline constexpr uint32_t kNativeBufferDrawConstants = 29;
inline constexpr uint32_t kNativeBufferTexture2DArrayHeap = 0;
inline constexpr uint32_t kNativeBufferTexture3DHeap = 1;
inline constexpr uint32_t kNativeBufferTextureCubeHeap = 2;
inline constexpr uint32_t kNativeBufferSamplerHeap = 3;

// Native MSL texture/sampler heaps are Metal argument-buffer resource arrays.
// They use a compact native index space rather than the MSC descriptor heap's
// 1M-entry address space because the shader-visible argument-buffer layout
// directly affects compile and pipeline creation cost.
inline constexpr uint32_t kNativeMslTextureHeapSize = 65536;
inline constexpr uint32_t kNativeMslSamplerHeapSize = 2048;
inline constexpr uint32_t kNativeMaxTextureBindings = 1024;
inline constexpr uint32_t kNativeMaxSamplerBindings = 1024;
inline constexpr uint32_t kNativeMslCallStackDepth = 16;
inline constexpr uint32_t kNativeMslMemExportSlots =
    ucode::kMaxMemExportElementCount;

// Native texture runtime-info word layout, one uint4 per texture binding:
// x: 0 = 2D-array/1D/2D, 1 = 3D, 2 = cube.
// y: persistent bindless texture heap slot.
// z: fetch constant.
// w: reserved for signed/swizzle/debug flags.
inline constexpr uint32_t kNativeTextureRuntimeType2DArray = 0;
inline constexpr uint32_t kNativeTextureRuntimeType3D = 1;
inline constexpr uint32_t kNativeTextureRuntimeTypeCube = 2;

class MslShaderTranslator : public ShaderTranslator {
 public:
  using Modification = ShaderModification;

  MslShaderTranslator(ui::GraphicsProvider::GpuVendorID vendor_id,
                      bool bindless_resources_used, bool edram_rov_used,
                      uint32_t draw_resolution_scale_x = 1,
                      uint32_t draw_resolution_scale_y = 1);
  ~MslShaderTranslator() override;

  const std::string& GetMslSource() const { return msl_source_; }
  const ShaderTranslationMetadata& GetNativeMetadata() const {
    return native_metadata_;
  }
  std::string GetEntryPointName() const;

  uint64_t GetDefaultVertexShaderModification(
      uint32_t dynamic_addressable_register_count,
      Shader::HostVertexShaderType host_vertex_shader_type =
          Shader::HostVertexShaderType::kVertex) const override;
  uint64_t GetDefaultPixelShaderModification(
      uint32_t dynamic_addressable_register_count) const override;

 protected:
  void Reset() override;
  void StartTranslation() override;
  std::vector<uint8_t> CompleteTranslation() override;
  uint32_t GetModificationRegisterCount() const override;

  void PreProcessControlFlowInstructions(
      std::vector<ucode::ControlFlowInstruction> instrs) override;
  void ProcessControlFlowInstructionBegin(uint32_t cf_index) override;
  void ProcessLabel(uint32_t cf_index) override;
  void ProcessExecInstructionBegin(const ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const ParsedExecInstruction& instr) override;
  void ProcessLoopStartInstruction(
      const ParsedLoopStartInstruction& instr) override;
  void ProcessLoopEndInstruction(
      const ParsedLoopEndInstruction& instr) override;
  void ProcessCallInstruction(const ParsedCallInstruction& instr) override;
  void ProcessReturnInstruction(const ParsedReturnInstruction& instr) override;
  void ProcessJumpInstruction(const ParsedJumpInstruction& instr) override;
  void ProcessAllocInstruction(const ParsedAllocInstruction& instr,
                               uint8_t export_eM) override;
  void ProcessVertexFetchInstruction(
      const ParsedVertexFetchInstruction& instr) override;
  void ProcessTextureFetchInstruction(
      const ParsedTextureFetchInstruction& instr) override;
  void ProcessAluInstruction(
      const ParsedAluInstruction& instr,
      uint8_t memexport_eM_potentially_written_before) override;
  void PostTranslation() override;

 private:
  Modification GetMslShaderModification() const;

  void EmitLine(const std::string& line = std::string());
  void Emit(const std::string& text);
  void EmitNativeMslUnsupported(const std::string& feature);
  bool CollectStaticResourceBindings();
  bool CurrentShaderUsesMemExport() const;
  bool RejectUnsupportedResultTarget(const char* operation,
                                     const InstructionResult& result);
  void Indent();
  void Outdent();

  void EmitSystemConstants();
  void EmitInputOutputDeclarations();
  void EmitHelperFunctions();
  void EmitNativeResourceHeapDeclarations();
  void EmitDirectResourceArguments(bool first_argument_written,
                                   bool emit_attributes);
  void EmitDirectResourceArgumentNames(bool first_argument_written);
  void EmitNativeDrawConstantAliases();
  void EmitEntryPointBegin();
  void EmitEntryPointEnd();
  void EmitVertexFinalizerFunction();
  void EmitVertexEntryPointWrappers();
  void EmitPointMeshEntryPoint();
  void EmitRectangleMeshEntryPoint();
  void EmitQuadMeshEntryPoint();
  void EmitTessellationMeshEntryPoints();
  bool IsDomainShader() const;
  bool IsRectangleListAsTriangleStrip() const;
  bool IsPointListAsMesh() const;
  bool IsRectangleListAsMesh() const;
  bool IsQuadListAsMesh() const;
  bool IsPrimitiveListAsMesh() const;
  void EmitInvocationStateInitialization();
  void EmitVertexOutputInitialization();
  void EmitDomainRegisterInitialization();
  void EmitRectangleListGuestLoopEnd();
  void EmitRectangleListOutputSynthesis();

  std::string RegisterToMsl(uint32_t storage_index,
                            InstructionStorageAddressingMode mode) const;
  std::string RelativeIndexExpression(uint32_t storage_index,
                                      const char* index_expr,
                                      uint32_t element_count) const;
  std::string OperandToMsl(const InstructionOperand& operand,
                           uint32_t needed_components);
  std::string OperandToMslNoSwizzle(const InstructionOperand& operand);
  std::string ResultToMsl(const InstructionResult& result);

  bool PixelShaderNeedsFloat24DepthOutput() const;
  bool PixelShaderWritesDepthOutput() const;
  bool IsPixelShaderSampleRate() const;
  bool IsForceEarlyDepthStencilEnabled() const;
  bool PixelShaderNeedsCoverageOutput() const;
  uint32_t VertexShaderClipDistanceCount() const;
  uint32_t VertexShaderCullDistanceCount() const;
  bool VertexShaderEmitsPointSizeOutput() const;
  bool ResultNeedsSaturation(const InstructionResult& result) const;
  std::string SaturateExpressionIfNeeded(const InstructionResult& result,
                                         const std::string& expression) const;

  std::string GetSwizzleString(const SwizzleSource* components,
                               uint32_t component_count);
  std::string GetWriteMaskString(uint32_t write_mask);

  void EmitVectorResultAssignment(const InstructionResult& result,
                                  const std::string& source_expr);
  void EmitScalarResultAssignment(const InstructionResult& result,
                                  const std::string& scalar_expr);
  void EmitPointSizeClampIfNeeded(const InstructionResult& result,
                                  uint32_t write_mask);
  void EmitVertexShaderEpilogue();
  void EmitPixelShaderEpilogue();
  void EmitPixelShaderParamGen();
  void EmitPixelShaderAlphaTest();
  void EmitPixelShaderAlphaToCoverage();
  void EmitDiscardFragment();
  void StoreConstantComponents(const InstructionResult& result);
  void EmitFetchPredicationBegin(bool is_predicated, bool predicate_condition,
                                 bool& predication_open_out);
  void EmitFetchPredicationEnd(bool predication_open);
  bool GetNativeTextureSignSpecialization(uint32_t fetch_constant,
                                          uint8_t component_mask,
                                          uint8_t& sign_values_out) const;
  std::string TextureArgumentName(uint32_t texture_slot) const;
  std::string TextureArgumentName2DArray(uint32_t texture_slot) const;
  std::string TextureArgumentName3D(uint32_t texture_slot) const;
  std::string TextureArgumentNameCube(uint32_t texture_slot) const;
  std::string SamplerArgumentName(uint32_t sampler_slot) const;
  uint32_t GetTextureArgumentSlotCount(xenos::FetchOpDimension dimension) const;
  bool UsesNativeSystemConstants() const;
  bool UsesNativeFloatConstants() const;
  bool UsesNativeBoolLoopConstants() const;
  bool UsesNativeFetchConstants() const;
  bool UsesNativeSharedMemory() const;
  bool UsesNativePrimitiveIndexConstants() const;
  bool UsesTextureRuntimeInfo() const;
  bool UsesNativeDescriptorIndices() const;
  bool UsesNativeDrawConstants() const;
  bool UsesNativeTexture2DArrayHeap() const;
  bool UsesNativeTexture3DHeap() const;
  bool UsesNativeTextureCubeHeap() const;
  bool CurrentShaderUsesStagedVectorResult() const;
  bool CurrentShaderUsesStagedScalarResult() const;
  bool UseScalarGprLocals() const;
  std::string TextureRuntimeInfoExpression(uint32_t texture_slot) const;
  std::string TextureSizeExpression(uint32_t texture_slot,
                                    xenos::FetchOpDimension dimension) const;
  uint8_t GetTextureFetchWordMask(
      const ParsedTextureFetchInstruction& instr) const;
  uint32_t GetTextureFetchDerivedMask(
      const ParsedTextureFetchInstruction& instr) const;
  std::string TextureFetchWordName(uint32_t fetch_constant,
                                   uint32_t word) const;
  std::string TextureFetchDerivedName(uint32_t fetch_constant,
                                      const char* name) const;
  void EmitTextureFetchWordCache();
  void EmitTextureFetchDerivedConstantCache();
  void EmitMemExportFlush(const char* reason, uint32_t eM_mask = 0xFFu);
  void EmitMemExportMarkDirty(const InstructionResult& result);
  std::string MemExportTargetToMsl(const InstructionResult& result) const;

  struct NativeMslHelperUsage {
    bool uses_nan_helpers = false;
    bool uses_finite_helper = false;
    bool uses_saturate = false;
    bool uses_saturate_no_nan = false;
    bool uses_frac = false;
    bool uses_vertex_position_inf_to_nan = false;
    bool uses_set_float_sign_bit = false;
    bool uses_first_bit_low = false;
    bool uses_address_index_round = false;
    bool uses_address_index_floor = false;
    bool uses_point_size_clamp = false;
    bool uses_mul_sm3 = false;
    uint8_t saturate_widths = 0;
    uint8_t saturate_no_nan_widths = 0;
    uint8_t texture_pwl_gamma_widths = 0;
    bool uses_clamp_inf_to_max = false;
    bool uses_flush_inf_to_signed_zero = false;
    bool uses_logc = false;
    bool uses_rcpc = false;
    bool uses_rcpf = false;
    bool uses_rsqc = false;
    bool uses_rsqf = false;
    bool uses_endian_swap = false;
    bool uses_shared_memory_load = false;
    bool uses_primitive_index_load = false;
    bool uses_vertex_fetch = false;
    bool uses_texture_fetch = false;
    bool uses_cube_texture = false;
    bool uses_texture_weights = false;
    bool uses_sign_extend = false;
    bool uses_vertex_fetch_unpack = false;
    bool uses_bool_constant = false;
    bool uses_loop_constant = false;
    bool uses_depth_float24 = false;
    bool uses_texture_sign_helpers = false;
    bool uses_texture_pwl_gamma = false;
    bool uses_color_pwl_gamma = false;
    bool uses_memexport = false;
  };
  NativeMslHelperUsage GetNativeMslHelperUsage() const;

  bool ProcessVectorAluInstruction(const ParsedAluInstruction& instr,
                                   std::string* staged_result_expr = nullptr);
  bool ProcessScalarAluInstruction(const ParsedAluInstruction& instr,
                                   std::string* staged_result_expr = nullptr);
  void EmitControlFlowCase(uint32_t cf_index);
  std::string GetCallConditionExpression(const ParsedCallInstruction& instr);
  void CloseExecConditionals();

  struct TextureBinding {
    uint32_t bindful_srv_index = 0;
    // Reuses shared metadata's bindless_descriptor_index as the descriptor
    // index word that contains the persistent native argument-heap slot.
    uint32_t bindless_descriptor_index = 0;
    // One texture-runtime-info index per translator binding.
    uint32_t texture_runtime_info_index = 0;
    uint32_t fetch_constant = 0;
    xenos::FetchOpDimension dimension = xenos::FetchOpDimension::k2D;
    bool is_signed = false;
  };
  uint32_t FindOrAddTextureBinding(uint32_t fetch_constant,
                                   xenos::FetchOpDimension dimension,
                                   bool is_signed);

  struct SamplerBinding {
    uint32_t bindful_sampler_index = 0;
    uint32_t bindless_descriptor_index = 0;
    uint32_t fetch_constant = 0;
    xenos::TextureFilter mag_filter = xenos::TextureFilter::kPoint;
    xenos::TextureFilter min_filter = xenos::TextureFilter::kPoint;
    xenos::TextureFilter mip_filter = xenos::TextureFilter::kPoint;
    xenos::AnisoFilter aniso_filter = xenos::AnisoFilter::kDisabled;
  };
  uint32_t FindOrAddSamplerBinding(uint32_t fetch_constant,
                                   xenos::TextureFilter mag_filter,
                                   xenos::TextureFilter min_filter,
                                   xenos::TextureFilter mip_filter,
                                   xenos::AnisoFilter aniso_filter);
  uint32_t GetBindlessResourceCount() const {
    return uint32_t(texture_bindings_.size() + sampler_bindings_.size());
  }
  ShaderTranslationMetadata BuildNativeMetadata() const;

  std::vector<TextureBinding> texture_bindings_;
  std::vector<SamplerBinding> sampler_bindings_;
  ShaderTextureSignComponentMasks texture_sign_component_masks_ = {};
  ShaderTranslationMetadata native_metadata_ = {};
  bool uses_memexport_ = false;
  bool uses_staged_vector_result_ = false;
  bool uses_staged_scalar_result_ = false;
  uint8_t alu_kill_memexport_flush_mask_ = 0;

  ui::GraphicsProvider::GpuVendorID vendor_id_;
  bool bindless_resources_used_;
  bool edram_rov_used_;
  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;

  std::ostringstream msl_stream_;
  std::string msl_source_;
  uint32_t indent_level_ = 0;
  std::string indent_string_;

  bool cf_exec_predicated_ = false;
  bool cf_exec_predicate_condition_ = false;
  uint32_t cf_exec_bool_constant_ = UINT32_MAX;
  bool cf_exec_bool_constant_condition_ = false;
  bool has_main_switch_ = false;
  bool control_flow_has_call_return_ = false;
  std::set<uint32_t> synthetic_label_addresses_;
  std::set<uint32_t> emitted_cf_case_indices_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_MSL_SHADER_TRANSLATOR_H_
