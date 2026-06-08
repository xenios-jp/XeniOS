/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Native MSL resource binding helpers.
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_NATIVE_MSL_BINDINGS_H_
#define XENIA_GPU_METAL_NATIVE_MSL_BINDINGS_H_

#include <cstdint>
#include <vector>

#include "third_party/metal-cpp/Metal/Metal.hpp"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/msl_shader_translator.h"

namespace xe {
namespace gpu {
namespace metal {
namespace native_msl {

// CPU layout matching one uint4 in the MSL xe_texture_runtime_info buffer.
struct NativeMslTextureRuntimeInfo {
  uint32_t type = kNativeTextureRuntimeType2DArray;
  uint32_t base_texture_slot = 0;
  uint32_t fetch_constant = 0;
  uint32_t flags = 0;
};
static_assert(sizeof(NativeMslTextureRuntimeInfo) == sizeof(uint32_t) * 4);

// CPU layout matching XeNativeDrawConstants in generated MSL. Each field is a
// Metal GPU address of the corresponding constant-buffer payload.
struct NativeMslDrawConstantPointers {
  uint64_t system = 0;
  uint64_t float_constants_data = 0;
  uint64_t bool_loop_constants_data = 0;
  uint64_t fetch_constants_data = 0;
  uint64_t descriptor_indices = 0;
  uint64_t primitive_index = 0;
};
static_assert(sizeof(NativeMslDrawConstantPointers) == sizeof(uint64_t) * 6);

bool UsesTextureRuntimeInfo(const DxbcShader::TranslationMetadata& metadata);

struct NativeMslStageBindings {
  std::vector<NativeMslTextureRuntimeInfo> runtime_info;

  void Clear() { runtime_info.clear(); }
};

bool CaptureTextureRuntimeInfo(MetalTextureCache& texture_cache,
                               const DxbcShader::TranslationMetadata& metadata,
                               NativeMslStageBindings& bindings);

}  // namespace native_msl
}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_NATIVE_MSL_BINDINGS_H_
