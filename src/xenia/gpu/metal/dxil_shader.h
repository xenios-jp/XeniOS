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
#include <vector>

#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
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
                      const MetalShaderConverter& converter,
                      std::vector<uint8_t>* metallib_out = nullptr);

    // Loads a Metal library this translation was compiled to earlier (see
    // MetalShaderLibraryCache) instead of translating the guest shader.
    bool LoadMetallib(MTL::Device* device, const std::vector<uint8_t>& metallib,
                      const std::string& entry_point_name,
                      uint32_t used_buffer_mask);

    MTL::Library* metal_library() const { return metal_library_; }
    MTL::Function* metal_function() const { return metal_function_; }
    const std::string& entry_point_name() const { return entry_point_name_; }
    // Which of the Metal Shader Converter bind points (the descriptor and
    // sampler heaps, the top-level argument buffer, the draw arguments and
    // the draw uniforms) the compiled function can read, a bit a buffer
    // index. A function compiled by spirv2air never declares the last two and
    // declares a heap only for a shader with textures or samplers; one from
    // the converter may declare any. Other buffer indices aren't described.
    uint32_t used_buffer_mask() const { return used_buffer_mask_; }

   private:
    // A pixel shader that reads its render targets
    // (host_render_target_feedback), which only spirv2air can compile.
    bool ReadsRenderTargets() const {
      return shader().type() == xenos::ShaderType::kPixel &&
             SpirvShaderTranslator::Modification(modification())
                 .pixel.host_render_target_feedback;
    }
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
    uint32_t used_buffer_mask_ = ~uint32_t(0);
  };

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_DXIL_SHADER_H_
