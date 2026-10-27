/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_DXIL_SHADER_H_
#define XENIA_GPU_METAL_DXIL_SHADER_H_

#include <cstdint>
#include <string>

#include "xenia/gpu/spirv_shader.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalShaderConverter;
struct MetalShaderConversionResult;

// Metal guest shader using the converter-compatible resource interface.
// Compile SPIR-V directly with spirv2air, with Mesa/DXIL and Metal Shader
// Converter as a fallback for unsupported guest shaders, or with the converter
// alone (metal_shader_compiler).
// Either way, resources follow the MSC top-level argument buffer ABI that
// MetalShaderConverter's root signature describes.
class DxilShader : public SpirvShader {
 public:
  DxilShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
             const uint32_t* ucode_dwords, size_t ucode_dword_count,
             std::endian ucode_source_endian = std::endian::big);

  class DxilTranslation : public SpirvTranslation {
   public:
    DxilTranslation(DxilShader& shader, uint64_t modification)
        : SpirvTranslation(shader, modification) {}
    ~DxilTranslation();

    // Compiles the SPIR-V binary (already in translated_binary()) to a Metal
    // library with an available compiler, and looks up its
    // entry point.
    bool CompileToAir(MTL::Device* device,
                      const MetalShaderConverter& converter);

    MTL::Library* metal_library() const { return metal_library_; }
    MTL::Function* metal_function() const { return metal_function_; }
    const std::string& entry_point_name() const { return entry_point_name_; }

   private:
    // SPIR-V -> AIR directly (spirv2air). False if this build lacks spirv2air
    // or the shader uses something it doesn't support (logged).
    bool CompileToAirWithSpirv2air(
        MetalShaderConversionResult& conversion) const;
    // SPIR-V -> DXIL (spirv_to_dxil) -> AIR (Metal Shader Converter).
    bool CompileToAirWithConverter(
        const MetalShaderConverter& converter,
        MetalShaderConversionResult& conversion) const;

    MTL::Library* metal_library_ = nullptr;
    MTL::Function* metal_function_ = nullptr;
    std::string entry_point_name_;
  };

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_DXIL_SHADER_H_
