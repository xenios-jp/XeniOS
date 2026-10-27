/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TRANSFER_SPIRV_CROSS_H_
#define XENIA_GPU_METAL_METAL_TRANSFER_SPIRV_CROSS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

// The render target ownership transfer fragment shaders, taken from the shared
// SPIR-V emitter through SPIRV-Cross rather than through spirv_to_dxil and the
// Metal Shader Converter, so their textures and push constants are bound
// directly instead of through an argument buffer and a descriptor heap.
//
// SPIRV-Cross and glslang both define namespace spv, incompatibly, so this
// cannot be compiled beside the emitter that uses glslang's SPIR-V builder.

// The transfer emitter numbers its descriptor sets densely and puts at most two
// textures in each.
constexpr uint32_t kTransferDescriptorSetCount = 2;
constexpr uint32_t kTransferBindingsPerSet = 2;

// Fragment binding slots of the MSL transfer shaders.
constexpr uint32_t kTransferMslPushConstantBufferIndex = 0;
constexpr uint32_t kTransferMslHostDepthBufferIndex = 1;
constexpr uint32_t kTransferMslTextureCount =
    kTransferDescriptorSetCount * kTransferBindingsPerSet;
constexpr uint32_t TransferMslTextureIndex(uint32_t set, uint32_t binding) {
  return set * kTransferBindingsPerSet + binding;
}

// Returns null and fills error_out on failure.
MTL::Function* CompileTransferFragmentFunctionMsl(
    MTL::Device* device, const std::vector<uint32_t>& spirv,
    std::string* error_out);

// Direct resolve with a sampled-image output: push constants at buffer 0,
// guest destination at buffer 1, source at texture 0, output at texture 1.
MTL::Function* CompileResolveRefreshFunctionMsl(
    MTL::Device* device, const std::vector<uint32_t>& spirv,
    std::string* error_out);

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_TRANSFER_SPIRV_CROSS_H_
