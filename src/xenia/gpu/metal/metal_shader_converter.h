/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHADER_CONVERTER_H_
#define XENIA_GPU_METAL_METAL_SHADER_CONVERTER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct IRVersionedInputLayoutDescriptor;

#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalStageCompileCache;

constexpr uint32_t kMetalRootSignatureAbiVersion = 3;

// Shader stage for Metal conversion
enum class MetalShaderStage {
  kVertex,
  kFragment,
  kGeometry,
  kCompute,
  kHull,
  kDomain
};

// Result of shader conversion
struct MetalShaderConversionResult {
  bool success = false;
  std::vector<uint8_t> metallib_data;
  std::string error_message;
  std::string function_name;  // Main function name in the metallib
  bool has_mesh_stage = false;
  bool has_geometry_stage = false;
};

struct MetalShaderReflectionInput {
  std::string name;
  uint8_t attribute_index = 0;
};

struct MetalShaderFunctionConstant {
  std::string name;
  uint32_t type = 0;
};

struct MetalShaderReflectionInfo {
  uint32_t vertex_output_size_in_bytes = 0;
  uint32_t vertex_input_count = 0;
  std::vector<MetalShaderReflectionInput> vertex_inputs;
  uint32_t gs_max_input_primitives_per_mesh_threadgroup = 0;
  std::vector<MetalShaderFunctionConstant> function_constants;
  bool has_hull_info = false;
  uint32_t hs_max_patches_per_object_threadgroup = 0;
  uint32_t hs_max_object_threads_per_patch = 0;
  uint32_t hs_patch_constants_size = 0;
  uint32_t hs_input_control_point_count = 0;
  uint32_t hs_output_control_point_count = 0;
  uint32_t hs_output_control_point_size = 0;
  uint32_t hs_tessellator_domain = 0;
  uint32_t hs_tessellator_partitioning = 0;
  uint32_t hs_tessellator_output_primitive = 0;
  bool hs_tessellation_type_half = false;
  float hs_max_tessellation_factor = 0.0f;
  bool has_domain_info = false;
  uint32_t ds_max_input_prims_per_mesh_threadgroup = 0;
  uint32_t ds_input_control_point_count = 0;
  uint32_t ds_input_control_point_size = 0;
  uint32_t ds_patch_constants_size = 0;
  uint32_t ds_tessellator_domain = 0;
  bool ds_tessellation_type_half = false;
};

enum MetalStageCompileOutputFlags : uint32_t {
  kMetalStageCompileOutputMetallib = 1u << 0,
  kMetalStageCompileOutputStageInMetallib = 1u << 1,
};

struct MetalStageCompileRequest {
  MetalShaderStage stage = MetalShaderStage::kVertex;
  std::shared_ptr<const std::vector<uint8_t>> dxil_data;
  uint32_t requested_outputs = kMetalStageCompileOutputMetallib;
  const IRVersionedInputLayoutDescriptor* input_layout = nullptr;
  // Optional builder used when the stage-in layout depends on reflection from
  // the same MSC compile. The callback is invoked before MSC objects are
  // destroyed and must return a layout valid until CompileStageUncached
  // returns.
  std::function<const IRVersionedInputLayoutDescriptor*(
      const MetalShaderReflectionInfo&)>
      stage_in_layout_builder;
  uint64_t stage_in_layout_key = 0;
  bool enable_geometry_emulation = false;
  int input_topology = 0;

  uint32_t compatibility_flags = 0;
  bool ignore_root_signature = true;
  bool ignore_debug_information = false;
  uint32_t validation_flags = UINT32_MAX;
  uint32_t stage_in_generation_mode = UINT32_MAX;
  uint32_t function_constant_resource_space = 0;
  bool has_minimum_target = false;
  uint32_t minimum_gpu_family = 0;
  uint32_t minimum_os = 0;
  std::string minimum_os_version;

  uint32_t root_signature_abi_version = kMetalRootSignatureAbiVersion;
  bool root_signature_bindless_resources_used = true;

  bool RequestsStageIn() const {
    return (requested_outputs & kMetalStageCompileOutputStageInMetallib) != 0;
  }
};

struct MetalStageCompileResult {
  bool success = false;
  std::vector<uint8_t> metallib_data;
  std::string error_message;
  std::string function_name;
  MetalShaderReflectionInfo reflection;
  bool has_reflection = false;
  std::vector<uint8_t> stage_in_metallib;
  bool has_mesh_stage = false;
  bool has_geometry_stage = false;
};

// Converts DXIL shaders to Metal IR using Apple's Metal Shader Converter
// Uses the correct Xbox 360 root signatures from xbox360_rootsig_helper.h
class MetalShaderConverter {
 public:
  MetalShaderConverter();
  ~MetalShaderConverter();

  // Initialize the converter (loads MSC library)
  bool Initialize();

  // Check if the converter is available
  bool IsAvailable() const { return is_available_; }

  // Convert DXIL to Metal IR
  // shader_type: Xenia shader type (vertex or pixel)
  // dxil_data: DXIL bytecode from dxbc2dxil
  // result: Output conversion result with metallib data
  bool Convert(xenos::ShaderType shader_type,
               const std::vector<uint8_t>& dxil_data,
               MetalShaderConversionResult& result);

  // Convert with explicit stage specification
  bool ConvertWithStage(MetalShaderStage stage,
                        const std::vector<uint8_t>& dxil_data,
                        MetalShaderConversionResult& result);

  bool ConvertWithStageEx(MetalShaderStage stage,
                          const std::vector<uint8_t>& dxil_data,
                          MetalShaderConversionResult& result,
                          MetalShaderReflectionInfo* reflection,
                          const IRVersionedInputLayoutDescriptor* input_layout,
                          std::vector<uint8_t>* stage_in_metallib,
                          bool enable_geometry_emulation, int input_topology);

  std::shared_ptr<const MetalStageCompileResult> CompileStage(
      const MetalStageCompileRequest& request);
  MetalStageCompileResult CompileStageUncached(
      const MetalStageCompileRequest& request);

  void SetMinimumTarget(uint32_t gpu_family, uint32_t os,
                        const std::string& version);

  MetalStageCompileCache* stage_compile_cache() {
    return stage_compile_cache_.get();
  }

 private:
  void PopulateDefaultRequestOptions(MetalStageCompileRequest& request) const;

  bool is_available_ = false;
  bool has_minimum_target_ = false;
  uint32_t minimum_gpu_family_ = 0;
  uint32_t minimum_os_ = 0;
  std::string minimum_os_version_;
  std::unique_ptr<MetalStageCompileCache> stage_compile_cache_;

  void* CreateXbox360RootSignature(bool bindless_resources_used);
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_SHADER_CONVERTER_H_
