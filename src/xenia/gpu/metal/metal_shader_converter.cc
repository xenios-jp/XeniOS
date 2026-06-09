/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shader_converter.h"

#include "metal_irconverter.h"

#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_stage_compile_cache.h"

namespace xe {
namespace gpu {
namespace metal {

constexpr uint32_t kFunctionConstantRegisterSpace = 2147420894u;
// Metal Shader Converter 4.0 enables NaN/Inf optimization by default, in which
// Metal assumes float operands are neither NaN nor Inf (per Apple's MSC user
// manual). Xenia's DXBC translator deliberately depends on real IEEE NaN/Inf
// semantics: it writes NaN to SV_Position to cull primitives, flushes NaN to 0
// on saturate, uses ordered (NaN-false) comparisons for alpha test, and emits
// explicit ==/!= INFINITY clamps in rcp/rsq/log. Keep IRCompatibilityFlag-
// DisableNanInfOptimization set so the upgrade preserves pre-4.0 behavior;
// drop it only after auditing those paths for the GPU-side perf gain.
constexpr uint32_t kDefaultCompatibilityFlags =
    IRCompatibilityFlagForceTextureArray | IRCompatibilityFlagBoundsCheck |
    IRCompatibilityFlagVertexPositionInfToNan |
    IRCompatibilityFlagDisableNanInfOptimization;
constexpr uint32_t kDebugCompatibilityFlags =
    IRCompatibilityFlagBoundsCheck | IRCompatibilityFlagVertexPositionInfToNan |
    IRCompatibilityFlagTextureMinLODClamp | IRCompatibilityFlagSamplerLODBias |
    IRCompatibilityFlagPositionInvariance | IRCompatibilityFlagSampleNanToZero |
    IRCompatibilityFlagTexWriteRoundingRTZ |
    IRCompatibilityFlagSuppress2DComputeDerivativeErrors |
    IRCompatibilityFlagForceTextureArray |
    IRCompatibilityFlagDisableNanInfOptimization;
constexpr uint32_t kUnsetCompilerOption = UINT32_MAX;

MetalShaderConverter::MetalShaderConverter()
    : stage_compile_cache_(std::make_unique<MetalStageCompileCache>()) {}

MetalShaderConverter::~MetalShaderConverter() = default;

void MetalShaderConverter::SetMinimumTarget(uint32_t gpu_family, uint32_t os,
                                            const std::string& version) {
  has_minimum_target_ = true;
  minimum_gpu_family_ = gpu_family;
  minimum_os_ = os;
  minimum_os_version_ = version;
}

void MetalShaderConverter::PopulateDefaultRequestOptions(
    MetalStageCompileRequest& request) const {
  if (cvars::metal_msc_debug_validation) {
    request.compatibility_flags |= kDebugCompatibilityFlags;
    request.validation_flags = IRCompilerValidationFlagAll;
  } else if (!request.compatibility_flags) {
    request.compatibility_flags = kDefaultCompatibilityFlags;
  }
  if (!request.function_constant_resource_space) {
    request.function_constant_resource_space = kFunctionConstantRegisterSpace;
  }
  if (!request.has_minimum_target && has_minimum_target_) {
    request.has_minimum_target = true;
    request.minimum_gpu_family = minimum_gpu_family_;
    request.minimum_os = minimum_os_;
    request.minimum_os_version = minimum_os_version_;
  }
  if (!request.root_signature_abi_version) {
    request.root_signature_abi_version = kMetalRootSignatureAbiVersion;
  }
}

std::shared_ptr<const MetalStageCompileResult>
MetalShaderConverter::CompileStage(const MetalStageCompileRequest& request) {
  MetalStageCompileRequest normalized = request;
  PopulateDefaultRequestOptions(normalized);
  if (!normalized.dxil_data) {
    auto result = std::make_shared<MetalStageCompileResult>();
    result->success = false;
    result->error_message = "Missing DXIL data";
    return result;
  }
  return stage_compile_cache_->GetOrCompile(normalized, [this, normalized]() {
    return std::make_shared<MetalStageCompileResult>(
        CompileStageUncached(normalized));
  });
}

bool MetalShaderConverter::Initialize() {
  // Metal Shader Converter is a library that should be available
  // at /usr/local/lib/libmetalirconverter.dylib
  // The headers are at /usr/local/include/metal_irconverter/
  // or in third_party/metal-shader-converter/include/

  // Test if we can create basic MSC objects
  IRCompiler* test_compiler = IRCompilerCreate();
  if (!test_compiler) {
    XELOGE(
        "MetalShaderConverter: Failed to create IR compiler - MSC not "
        "available");
    is_available_ = false;
    return false;
  }
  IRCompilerDestroy(test_compiler);

  XELOGI("MetalShaderConverter: Initialized successfully");
  is_available_ = true;
  return true;
}

// Create Xbox 360 root signature matching xbox360_rootsig_helper.h.
void* MetalShaderConverter::CreateXbox360RootSignature(
    bool bindless_resources_used) {
  // Create root parameters for Xbox 360 shader resources. Descriptor tables
  // keep the heap-backed texture/UAV/sampler resources, while shader constants
  // use MSC root CBVs matching the D3D12 bindless root-signature shape.
  IRDescriptorRange1 ranges[32] = {};
  IRRootDescriptorTable1 tables[32] = {};
  IRRootParameter1 params[32] = {};
  int range_count = 0;
  int table_count = 0;
  int param_count = 0;

  auto append_descriptor_table =
      [&](IRDescriptorRangeType type, uint32_t descriptor_count,
          uint32_t base_shader_register, uint32_t register_space,
          IRShaderVisibility table_visibility) {
        IRDescriptorRange1& range = ranges[range_count++];
        range.RangeType = type;
        range.NumDescriptors = descriptor_count;
        range.BaseShaderRegister = base_shader_register;
        range.RegisterSpace = register_space;
        range.Flags = IRDescriptorRangeFlagNone;
        range.OffsetInDescriptorsFromTableStart = 0;

        IRRootDescriptorTable1& table = tables[table_count++];
        table.NumDescriptorRanges = 1;
        table.pDescriptorRanges = &range;

        IRRootParameter1& param = params[param_count++];
        param.ParameterType = IRRootParameterTypeDescriptorTable;
        param.DescriptorTable = table;
        param.ShaderVisibility = table_visibility;
      };

  auto append_root_cbv = [&](uint32_t shader_register,
                             IRShaderVisibility cbv_visibility) {
    IRRootParameter1& param = params[param_count++];
    param.ParameterType = IRRootParameterTypeCBV;
    param.Descriptor.ShaderRegister = shader_register;
    param.Descriptor.RegisterSpace = 0;
    param.Descriptor.Flags = IRRootDescriptorFlagNone;
    param.ShaderVisibility = cbv_visibility;
  };

  const uint32_t resource_descriptor_count =
      bindless_resources_used ? UINT32_MAX : 1025;
  const uint32_t sampler_descriptor_count =
      bindless_resources_used ? UINT32_MAX : 257;

  // SRVs in spaces 0-3.
  for (uint32_t space = 0; space < 4; ++space) {
    append_descriptor_table(IRDescriptorRangeTypeSRV, resource_descriptor_count,
                            0, space, IRShaderVisibilityAll);
  }

  // SRV in space 10 for hull shaders.
  append_descriptor_table(IRDescriptorRangeTypeSRV, resource_descriptor_count,
                          0, 10, IRShaderVisibilityAll);

  // UAVs in spaces 0-3.
  for (uint32_t space = 0; space < 4; ++space) {
    append_descriptor_table(IRDescriptorRangeTypeUAV, resource_descriptor_count,
                            0, space, IRShaderVisibilityAll);
  }

  // Samplers in space 0.
  append_descriptor_table(IRDescriptorRangeTypeSampler,
                          sampler_descriptor_count, 0, 0,
                          IRShaderVisibilityAll);

  // Xenia root ABI v2 uses five shader-constant CBVs in space 0:
  //   b0 = system constants
  //   b1 = float constants
  //   b2 = bool/loop constants
  //   b3 = fetch constants
  //   b4 = descriptor indices
  // It keeps b0/b2 common and duplicates the volatile stage-local CBVs
  // with disjoint graphics-stage visibility. This lets the runtime build one
  // graphics root argument block while the DXIL-visible register numbers stay
  // unchanged.
  append_root_cbv(0, IRShaderVisibilityAll);
  append_root_cbv(1, IRShaderVisibilityVertex);
  append_root_cbv(2, IRShaderVisibilityAll);
  append_root_cbv(3, IRShaderVisibilityVertex);
  append_root_cbv(4, IRShaderVisibilityVertex);
  constexpr IRShaderVisibility kGeneratedAndPixelVisibilities[] = {
      IRShaderVisibilityHull, IRShaderVisibilityDomain,
      IRShaderVisibilityPixel};
  for (IRShaderVisibility stage_visibility : kGeneratedAndPixelVisibilities) {
    append_root_cbv(1, stage_visibility);
    append_root_cbv(3, stage_visibility);
    append_root_cbv(4, stage_visibility);
  }

  // Function-constant CBV space for MSC. MSC documentation requires it to be
  // declared when function constants are enabled, but the converter skips it
  // when laying out the top-level argument buffer.
  append_descriptor_table(IRDescriptorRangeTypeCBV, 1, 0,
                          kFunctionConstantRegisterSpace,
                          IRShaderVisibilityAll);

  // Create root signature descriptor
  IRRootSignatureDescriptor1 desc = {};
  desc.NumParameters = param_count;
  desc.pParameters = params;
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags = bindless_resources_used
                   ? static_cast<IRRootSignatureFlags>(
                         IRRootSignatureFlagCBVSRVUAVHeapDirectlyIndexed |
                         IRRootSignatureFlagSamplerHeapDirectlyIndexed)
                   : IRRootSignatureFlagNone;

  IRVersionedRootSignatureDescriptor versionedDesc = {};
  versionedDesc.version = IRRootSignatureVersion_1_1;
  versionedDesc.desc_1_1 = desc;

  // Create the root signature
  IRError* error = nullptr;
  IRRootSignature* rootSig =
      IRRootSignatureCreateFromDescriptor(&versionedDesc, &error);

  if (error) {
    const char* errMsg = (const char*)IRErrorGetPayload(error);
    XELOGE("MetalShaderConverter: Failed to create root signature: {}",
           errMsg ? errMsg : "unknown error");
    IRErrorDestroy(error);
    return nullptr;
  }

  return rootSig;
}

bool MetalShaderConverter::Convert(xenos::ShaderType shader_type,
                                   const std::vector<uint8_t>& dxil_data,
                                   MetalShaderConversionResult& result) {
  MetalShaderStage stage;
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      stage = MetalShaderStage::kVertex;
      break;
    case xenos::ShaderType::kPixel:
      stage = MetalShaderStage::kFragment;
      break;
    default:
      result.success = false;
      result.error_message = "Unsupported shader type";
      return false;
  }

  return ConvertWithStage(stage, dxil_data, result);
}

bool MetalShaderConverter::ConvertWithStage(
    MetalShaderStage stage, const std::vector<uint8_t>& dxil_data,
    MetalShaderConversionResult& result) {
  return ConvertWithStageEx(stage, dxil_data, result, nullptr, nullptr, nullptr,
                            false, IRInputTopologyUndefined);
}

bool MetalShaderConverter::ConvertWithStageEx(
    MetalShaderStage stage, const std::vector<uint8_t>& dxil_data,
    MetalShaderConversionResult& result, MetalShaderReflectionInfo* reflection,
    const IRVersionedInputLayoutDescriptor* input_layout,
    std::vector<uint8_t>* stage_in_metallib, bool enable_geometry_emulation,
    int input_topology) {
  auto dxil = std::make_shared<std::vector<uint8_t>>(dxil_data);
  MetalStageCompileRequest request;
  request.stage = stage;
  request.dxil_data = std::move(dxil);
  request.enable_geometry_emulation = enable_geometry_emulation;
  request.input_topology = input_topology;
  request.input_layout = input_layout;
  request.requested_outputs = kMetalStageCompileOutputMetallib;
  if (stage_in_metallib && input_layout) {
    request.requested_outputs |= kMetalStageCompileOutputStageInMetallib;
  }

  std::shared_ptr<const MetalStageCompileResult> stage_result =
      CompileStage(request);
  result.success = stage_result && stage_result->success;
  result.metallib_data =
      stage_result ? stage_result->metallib_data : std::vector<uint8_t>();
  result.error_message =
      stage_result ? stage_result->error_message : "Stage compile failed";
  result.function_name = stage_result ? stage_result->function_name : "";
  result.has_mesh_stage = stage_result && stage_result->has_mesh_stage;
  result.has_geometry_stage = stage_result && stage_result->has_geometry_stage;
  if (reflection && stage_result && stage_result->has_reflection) {
    *reflection = stage_result->reflection;
  }
  if (stage_in_metallib) {
    stage_in_metallib->clear();
    if (stage_result) {
      *stage_in_metallib = stage_result->stage_in_metallib;
    }
  }
  return result.success;
}

MetalStageCompileResult MetalShaderConverter::CompileStageUncached(
    const MetalStageCompileRequest& request) {
  MetalStageCompileResult result;
  const MetalShaderStage stage = request.stage;
  const bool enable_geometry_emulation = request.enable_geometry_emulation;
  const int input_topology = request.input_topology;
  if (!is_available_) {
    result.success = false;
    result.error_message = "MetalShaderConverter not initialized";
    return result;
  }
  if (!request.dxil_data) {
    result.success = false;
    result.error_message = "Missing DXIL data";
    return result;
  }

  const auto& dxil_data = *request.dxil_data;

  if (dxil_data.empty()) {
    result.success = false;
    result.error_message = "Empty DXIL data";
    return result;
  }

  // Create DXIL object from input data
  IRObject* dxilObject = IRObjectCreateFromDXIL(
      dxil_data.data(), dxil_data.size(), IRBytecodeOwnershipNone);

  if (!dxilObject) {
    result.success = false;
    result.error_message = "Failed to create DXIL object";
    return result;
  }

  // Create compiler
  IRCompiler* compiler = IRCompilerCreate();
  if (!compiler) {
    IRObjectDestroy(dxilObject);
    result.success = false;
    result.error_message = "Failed to create IR compiler";
    return result;
  }

  // Set compatibility flag to force texture array types
  // This is required because:
  // 1. Xenia's DXBC translator generates code expecting texture2d_array
  // 2. MSC 3.0+ defaults to non-array texture types
  // 3. Our Metal textures are created as MTLTextureType2DArray
  IRCompilerSetCompatibilityFlags(
      compiler, static_cast<IRCompatibilityFlags>(request.compatibility_flags));
  if (request.validation_flags != kUnsetCompilerOption) {
    IRCompilerSetValidationFlags(
        compiler,
        static_cast<IRCompilerValidationFlags>(request.validation_flags));
  }
  if (request.stage_in_generation_mode != kUnsetCompilerOption) {
    IRCompilerSetStageInGenerationMode(compiler,
                                       static_cast<IRStageInCodeGenerationMode>(
                                           request.stage_in_generation_mode));
  }

  if (input_topology != IRInputTopologyUndefined) {
    IRCompilerSetInputTopology(compiler,
                               static_cast<IRInputTopology>(input_topology));
  }
  if (enable_geometry_emulation) {
    IRCompilerEnableGeometryAndTessellationEmulation(compiler, true);
  }
  // Ignore embedded root signatures in DXIL; we provide our own.
  IRCompilerIgnoreRootSignature(compiler, request.ignore_root_signature);
  if (request.ignore_debug_information) {
    IRCompilerIgnoreDebugInformation(compiler, true);
  }
  // Enable function-constant register space for MSC specialization.
  IRCompilerSetFunctionConstantResourceSpace(
      compiler, request.function_constant_resource_space);
  if (request.has_minimum_target) {
    IRCompilerSetMinimumGPUFamily(
        compiler, static_cast<IRGPUFamily>(request.minimum_gpu_family));
    IRCompilerSetMinimumDeploymentTarget(
        compiler, static_cast<IROperatingSystem>(request.minimum_os),
        request.minimum_os_version.c_str());
  }

  // Create and set Xbox 360 root signature
  IRRootSignature* rootSig =
      static_cast<IRRootSignature*>(CreateXbox360RootSignature(
          request.root_signature_bindless_resources_used));
  if (!rootSig) {
    IRCompilerDestroy(compiler);
    IRObjectDestroy(dxilObject);
    result.success = false;
    result.error_message = "Failed to create root signature";
    return result;
  }
  IRCompilerSetGlobalRootSignature(compiler, rootSig);

  // Compile DXIL to Metal
  IRError* error = nullptr;
  IRObject* metalObject =
      IRCompilerAllocCompileAndLink(compiler, nullptr, dxilObject, &error);

  if (error) {
    const char* errMsg = (const char*)IRErrorGetPayload(error);
    result.success = false;
    result.error_message = std::string("MSC compilation failed: ") +
                           (errMsg ? errMsg : "unknown error");
    XELOGE("MetalShaderConverter: {}", result.error_message);
    IRErrorDestroy(error);
    IRRootSignatureDestroy(rootSig);
    IRCompilerDestroy(compiler);
    IRObjectDestroy(dxilObject);
    return result;
  }

  if (!metalObject) {
    result.success = false;
    result.error_message = "MSC returned null object without error";
    IRRootSignatureDestroy(rootSig);
    IRCompilerDestroy(compiler);
    IRObjectDestroy(dxilObject);
    return result;
  }

  auto extract_metallib = [&](IRShaderStage ir_stage,
                              std::vector<uint8_t>& out_bytes,
                              size_t* out_size) -> bool {
    IRMetalLibBinary* metallib = IRMetalLibBinaryCreate();
    if (!metallib) {
      if (out_size) {
        *out_size = 0;
      }
      return false;
    }
    bool ok = IRObjectGetMetalLibBinary(metalObject, ir_stage, metallib);
    size_t metallib_size = IRMetalLibGetBytecodeSize(metallib);
    if (!ok || metallib_size == 0) {
      IRMetalLibBinaryDestroy(metallib);
      if (out_size) {
        *out_size = 0;
      }
      return false;
    }
    out_bytes.resize(metallib_size);
    IRMetalLibGetBytecode(metallib, out_bytes.data());
    IRMetalLibBinaryDestroy(metallib);
    if (out_size) {
      *out_size = metallib_size;
    }
    return true;
  };

  IRShaderStage ir_stage = IRShaderStageInvalid;
  switch (stage) {
    case MetalShaderStage::kVertex:
      ir_stage = IRShaderStageVertex;
      break;
    case MetalShaderStage::kFragment:
      ir_stage = IRShaderStageFragment;
      break;
    case MetalShaderStage::kCompute:
      ir_stage = IRShaderStageCompute;
      break;
    case MetalShaderStage::kHull:
      ir_stage = IRShaderStageHull;
      break;
    case MetalShaderStage::kDomain:
      ir_stage = IRShaderStageDomain;
      break;
    case MetalShaderStage::kGeometry:
      // We'll determine mesh/geometry below.
      break;
    default:
      ir_stage = IRShaderStageInvalid;
      break;
  }

  result.has_mesh_stage = false;
  result.has_geometry_stage = false;
  size_t stage_size = 0;
  if (stage == MetalShaderStage::kGeometry) {
    std::vector<uint8_t> mesh_bytes;
    std::vector<uint8_t> geom_bytes;
    result.has_mesh_stage =
        extract_metallib(IRShaderStageMesh, mesh_bytes, nullptr);
    result.has_geometry_stage =
        extract_metallib(IRShaderStageGeometry, geom_bytes, nullptr);
    if (result.has_mesh_stage) {
      result.metallib_data = std::move(mesh_bytes);
      ir_stage = IRShaderStageMesh;
    } else if (result.has_geometry_stage) {
      result.metallib_data = std::move(geom_bytes);
      ir_stage = IRShaderStageGeometry;
    }
  } else if (ir_stage != IRShaderStageInvalid) {
    extract_metallib(ir_stage, result.metallib_data, &stage_size);
  }

  if (result.metallib_data.empty()) {
    auto stage_name = [](MetalShaderStage value) -> const char* {
      switch (value) {
        case MetalShaderStage::kVertex:
          return "vertex";
        case MetalShaderStage::kFragment:
          return "fragment";
        case MetalShaderStage::kGeometry:
          return "geometry";
        case MetalShaderStage::kCompute:
          return "compute";
        case MetalShaderStage::kHull:
          return "hull";
        case MetalShaderStage::kDomain:
          return "domain";
        default:
          return "unknown";
      }
    };
    result.success = false;
    result.error_message = "Generated MetalLib has zero size";
    XELOGE(
        "MetalShaderConverter: empty metallib (stage={}, ir_stage={}, "
        "geom_emulation={}, input_topology={}, mesh_ok={}, geom_ok={}, "
        "stage_size={})",
        stage_name(stage), int(ir_stage), enable_geometry_emulation,
        input_topology, result.has_mesh_stage, result.has_geometry_stage,
        stage_size);
    IRObjectDestroy(metalObject);
    IRRootSignatureDestroy(rootSig);
    IRCompilerDestroy(compiler);
    IRObjectDestroy(dxilObject);
    return result;
  }

  MetalShaderReflectionInfo* reflection = &result.reflection;
  if (reflection) {
    reflection->vertex_inputs.clear();
    reflection->function_constants.clear();
    reflection->vertex_output_size_in_bytes = 0;
    reflection->vertex_input_count = 0;
    reflection->gs_max_input_primitives_per_mesh_threadgroup = 0;
    reflection->has_hull_info = false;
    reflection->hs_max_patches_per_object_threadgroup = 0;
    reflection->hs_max_object_threads_per_patch = 0;
    reflection->hs_patch_constants_size = 0;
    reflection->hs_input_control_point_count = 0;
    reflection->hs_output_control_point_count = 0;
    reflection->hs_output_control_point_size = 0;
    reflection->hs_tessellator_domain = 0;
    reflection->hs_tessellator_partitioning = 0;
    reflection->hs_tessellator_output_primitive = 0;
    reflection->hs_tessellation_type_half = false;
    reflection->hs_max_tessellation_factor = 0.0f;
    reflection->has_domain_info = false;
    reflection->ds_max_input_prims_per_mesh_threadgroup = 0;
    reflection->ds_input_control_point_count = 0;
    reflection->ds_input_control_point_size = 0;
    reflection->ds_patch_constants_size = 0;
    reflection->ds_tessellator_domain = 0;
    reflection->ds_tessellation_type_half = false;
  }

  IRShaderReflection* shader_reflection = IRShaderReflectionCreate();
  if (shader_reflection && ir_stage != IRShaderStageInvalid) {
    if (IRObjectGetReflection(metalObject, ir_stage, shader_reflection)) {
      result.has_reflection = true;
      const char* entry_name =
          IRShaderReflectionGetEntryPointFunctionName(shader_reflection);
      if (entry_name) {
        result.function_name = entry_name;
      }
      if (reflection) {
        if (ir_stage == IRShaderStageVertex) {
          IRVersionedVSInfo vs_info = {};
          vs_info.version = IRReflectionVersion_1_0;
          if (IRShaderReflectionCopyVertexInfo(
                  shader_reflection, IRReflectionVersion_1_0, &vs_info)) {
            reflection->vertex_output_size_in_bytes =
                vs_info.info_1_0.vertex_output_size_in_bytes;
            reflection->vertex_input_count =
                static_cast<uint32_t>(vs_info.info_1_0.num_vertex_inputs);
            reflection->vertex_inputs.reserve(
                vs_info.info_1_0.num_vertex_inputs);
            for (size_t i = 0; i < vs_info.info_1_0.num_vertex_inputs; ++i) {
              const auto& input = vs_info.info_1_0.vertex_inputs[i];
              MetalShaderReflectionInput out;
              out.name = input.name ? input.name : "";
              out.attribute_index = input.attributeIndex;
              reflection->vertex_inputs.push_back(std::move(out));
            }
            IRShaderReflectionReleaseVertexInfo(&vs_info);
          }
        } else if (ir_stage == IRShaderStageGeometry ||
                   ir_stage == IRShaderStageMesh) {
          IRVersionedGSInfo gs_info = {};
          gs_info.version = IRReflectionVersion_1_0;
          if (IRShaderReflectionCopyGeometryInfo(
                  shader_reflection, IRReflectionVersion_1_0, &gs_info)) {
            reflection->gs_max_input_primitives_per_mesh_threadgroup =
                gs_info.info_1_0.max_input_primitives_per_mesh_threadgroup;
            IRShaderReflectionReleaseGeometryInfo(&gs_info);
          }
        }

        if (IRShaderReflectionNeedsFunctionConstants(shader_reflection)) {
          size_t constant_count =
              IRShaderReflectionGetFunctionConstantCount(shader_reflection);
          if (constant_count) {
            std::vector<IRFunctionConstant> constants(constant_count);
            IRShaderReflectionCopyFunctionConstants(shader_reflection,
                                                    constants.data());
            reflection->function_constants.reserve(constant_count);
            for (const auto& constant : constants) {
              MetalShaderFunctionConstant out;
              out.name = constant.name ? constant.name : "";
              out.type = static_cast<uint32_t>(constant.type);
              reflection->function_constants.push_back(std::move(out));
            }
            IRShaderReflectionReleaseFunctionConstants(constants.data(),
                                                       constant_count);
          }
        }

        if (ir_stage == IRShaderStageHull) {
          IRVersionedHSInfo hs_info = {};
          hs_info.version = IRReflectionVersion_1_0;
          if (IRShaderReflectionCopyHullInfo(
                  shader_reflection, IRReflectionVersion_1_0, &hs_info)) {
            reflection->has_hull_info = true;
            reflection->hs_max_patches_per_object_threadgroup =
                hs_info.info_1_0.max_patches_per_object_threadgroup;
            reflection->hs_max_object_threads_per_patch =
                hs_info.info_1_0.max_object_threads_per_patch;
            reflection->hs_patch_constants_size =
                hs_info.info_1_0.patch_constants_size;
            reflection->hs_input_control_point_count =
                hs_info.info_1_0.input_control_point_count;
            reflection->hs_output_control_point_count =
                hs_info.info_1_0.output_control_point_count;
            reflection->hs_output_control_point_size =
                hs_info.info_1_0.output_control_point_size;
            reflection->hs_tessellator_domain =
                static_cast<uint32_t>(hs_info.info_1_0.tessellator_domain);
            reflection->hs_tessellator_partitioning = static_cast<uint32_t>(
                hs_info.info_1_0.tessellator_partitioning);
            reflection->hs_tessellator_output_primitive = static_cast<uint32_t>(
                hs_info.info_1_0.tessellator_output_primitive);
            reflection->hs_tessellation_type_half =
                hs_info.info_1_0.tessellation_type_half;
            reflection->hs_max_tessellation_factor =
                hs_info.info_1_0.max_tessellation_factor;
            IRShaderReflectionReleaseHullInfo(&hs_info);
          }
        } else if (ir_stage == IRShaderStageDomain) {
          IRVersionedDSInfo ds_info = {};
          ds_info.version = IRReflectionVersion_1_0;
          if (IRShaderReflectionCopyDomainInfo(
                  shader_reflection, IRReflectionVersion_1_0, &ds_info)) {
            reflection->has_domain_info = true;
            reflection->ds_max_input_prims_per_mesh_threadgroup =
                ds_info.info_1_0.max_input_prims_per_mesh_threadgroup;
            reflection->ds_input_control_point_count =
                ds_info.info_1_0.input_control_point_count;
            reflection->ds_input_control_point_size =
                ds_info.info_1_0.input_control_point_size;
            reflection->ds_patch_constants_size =
                ds_info.info_1_0.patch_constants_size;
            reflection->ds_tessellator_domain =
                static_cast<uint32_t>(ds_info.info_1_0.tessellator_domain);
            reflection->ds_tessellation_type_half =
                ds_info.info_1_0.tessellation_type_half;
            IRShaderReflectionReleaseDomainInfo(&ds_info);
          }
        }
      }
    }
  }

  if (result.function_name.empty()) {
    switch (stage) {
      case MetalShaderStage::kVertex:
        result.function_name = "vertexMain";
        break;
      case MetalShaderStage::kFragment:
        result.function_name = "fragmentMain";
        break;
      case MetalShaderStage::kCompute:
        result.function_name = "computeMain";
        break;
      case MetalShaderStage::kGeometry:
      default:
        result.function_name = "main";
        break;
    }
  }

  const IRVersionedInputLayoutDescriptor* input_layout = request.input_layout;
  if (!input_layout && request.RequestsStageIn() &&
      request.stage_in_layout_builder) {
    input_layout = request.stage_in_layout_builder(result.reflection);
  }
  if (stage == MetalShaderStage::kVertex && request.RequestsStageIn() &&
      input_layout && shader_reflection) {
    IRMetalLibBinary* stage_in_lib = IRMetalLibBinaryCreate();
    if (stage_in_lib) {
      if (IRMetalLibSynthesizeStageInFunction(compiler, shader_reflection,
                                              input_layout, stage_in_lib)) {
        size_t stage_in_size = IRMetalLibGetBytecodeSize(stage_in_lib);
        if (stage_in_size) {
          result.stage_in_metallib.resize(stage_in_size);
          IRMetalLibGetBytecode(stage_in_lib, result.stage_in_metallib.data());
        }
      }
      IRMetalLibBinaryDestroy(stage_in_lib);
    }
  }

  if (shader_reflection) {
    IRShaderReflectionDestroy(shader_reflection);
  }

  XELOGD(
      "MetalShaderConverter: Successfully converted {} bytes DXIL to {} bytes "
      "MetalLib",
      dxil_data.size(), result.metallib_data.size());

  // Cleanup
  IRObjectDestroy(metalObject);
  IRRootSignatureDestroy(rootSig);
  IRCompilerDestroy(compiler);
  IRObjectDestroy(dxilObject);

  result.success = true;
  return result;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
