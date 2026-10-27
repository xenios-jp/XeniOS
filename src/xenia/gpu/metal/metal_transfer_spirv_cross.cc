/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_transfer_spirv_cross.h"

#include <exception>

#include "spirv_msl.hpp"

namespace xe {
namespace gpu {
namespace metal {

namespace {
MTL::Function* CompileFunctionMsl(MTL::Device* device,
                                  const std::vector<uint32_t>& spirv,
                                  std::string* error_out,
                                  bool resolve_refresh) {
  const auto stage = resolve_refresh ? spv::ExecutionModelGLCompute
                                     : spv::ExecutionModelFragment;
  std::string msl_source;
  std::string entry_point_name;
  try {
    spirv_cross::CompilerMSL compiler(spirv.data(), spirv.size());
    spirv_cross::CompilerMSL::Options options = compiler.get_msl_options();
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    // Stencil reference output needs 2.1; the guest MSL path uses 2.4.
    options.msl_version =
        spirv_cross::CompilerMSL::Options::make_msl_version(2, 4);
    options.argument_buffers = false;
    compiler.set_msl_options(options);
    for (uint32_t set = 0; set < kTransferDescriptorSetCount; ++set) {
      for (uint32_t binding = 0; binding < kTransferBindingsPerSet; ++binding) {
        spirv_cross::MSLResourceBinding resource_binding = {};
        resource_binding.stage = stage;
        resource_binding.desc_set = set;
        resource_binding.binding = binding;
        // The host depth source is the only buffer, and it declares the lowest
        // descriptor set, so it is always set 0 binding 0.
        resource_binding.msl_buffer = kTransferMslHostDepthBufferIndex;
        resource_binding.msl_texture =
            resolve_refresh ? (set == 1 ? 0 : 1)
                            : TransferMslTextureIndex(set, binding);
        compiler.add_msl_resource_binding(resource_binding);
      }
    }
    {
      spirv_cross::MSLResourceBinding resource_binding = {};
      resource_binding.stage = stage;
      resource_binding.desc_set = spirv_cross::kPushConstDescSet;
      resource_binding.binding = spirv_cross::kPushConstBinding;
      resource_binding.msl_buffer = kTransferMslPushConstantBufferIndex;
      compiler.add_msl_resource_binding(resource_binding);
    }
    msl_source = compiler.compile();
    entry_point_name = compiler.get_cleansed_entry_point_name("main", stage);
  } catch (const std::exception& e) {
    *error_out = std::string("SPIRV-Cross: ") + e.what();
    return nullptr;
  }

  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
  NS::Error* error = nullptr;
  MTL::CompileOptions* compile_options = MTL::CompileOptions::alloc()->init();
  compile_options->setFastMathEnabled(true);
  compile_options->setLanguageVersion(MTL::LanguageVersion2_4);
  MTL::Library* library = device->newLibrary(
      NS::String::string(msl_source.c_str(), NS::UTF8StringEncoding),
      compile_options, &error);
  compile_options->release();
  MTL::Function* function = nullptr;
  if (!library) {
    *error_out = error && error->localizedDescription()
                     ? error->localizedDescription()->utf8String()
                     : "unknown Metal compiler error";
  } else {
    function = library->newFunction(
        NS::String::string(entry_point_name.c_str(), NS::UTF8StringEncoding));
    if (!function) {
      *error_out = "no function named " + entry_point_name;
    }
    library->release();
  }
  pool->release();
  return function;
}
}  // namespace

MTL::Function* CompileTransferFragmentFunctionMsl(
    MTL::Device* device, const std::vector<uint32_t>& spirv,
    std::string* error_out) {
  return CompileFunctionMsl(device, spirv, error_out, false);
}

MTL::Function* CompileResolveRefreshFunctionMsl(
    MTL::Device* device, const std::vector<uint32_t>& spirv,
    std::string* error_out) {
  return CompileFunctionMsl(device, spirv, error_out, true);
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
