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

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

// Metal guest shader compiled directly from SPIR-V to Apple AIR by spirv2air.
// Resources follow the bindless top-level argument buffer and descriptor heap
// layout in spirv2air/docs/bindings.md (bound by MetalDxilBinder).
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
    // library with spirv2air and looks up its entry point.
    bool CompileToAir(MTL::Device* device,
                      std::vector<uint8_t>* metallib_out = nullptr);

    // Loads a Metal library this translation was compiled to earlier (see
    // MetalShaderLibraryCache) instead of translating the guest shader.
    bool LoadMetallib(MTL::Device* device, const std::vector<uint8_t>& metallib,
                      const std::string& entry_point_name,
                      uint32_t used_buffer_mask);

    // The same shader with the fetch-state function constants |states| (|count|
    // uint4 values) compiled into it, as a metallib with one function that
    // has no function constants left. False when the SPIR-V isn't at hand (the
    // translation was made ready from the cache) or spirv2air can't translate
    // the shader.
    bool CompileWithFetchState(const uint32_t* states, uint32_t count,
                               std::vector<uint8_t>& metallib_out,
                               std::string& entry_point_name_out) const;

    // Makes the translation ready with no library of its own: all of its
    // functions are compiled by CompileWithFetchState, one library per set of
    // constant values, so a generic library would be compiled to go unused.
    void SetBakesOnly(uint32_t used_buffer_mask);
    bool bakes_only() const { return bakes_only_; }
    // Whether pipelines can be made of it: it has its function, or is one that
    // bakes only.
    bool is_ready() const { return metal_function_ != nullptr || bakes_only_; }
    // used_buffer_mask of a function spirv2air compiles from translated_binary().
    uint32_t GetSpirv2airBufferMask() const;
    // The library CompileToAir would have made for a translation that bakes
    // only, for constant values spirv2air doesn't compile in (none are known).
    // The caller owns the library; null if translated_binary() can't be
    // compiled.
    MTL::Library* NewGenericLibrary(MTL::Device* device,
                                    std::string& entry_point_name_out) const;

    // Both null for a translation that bakes only.
    MTL::Library* metal_library() const { return metal_library_; }
    MTL::Function* metal_function() const { return metal_function_; }
    const std::string& entry_point_name() const { return entry_point_name_; }
    // Which of the spirv2air bind points (descriptor heap 0, sampler heap 1,
    // top-level argument buffer 2) the compiled function can read, one bit per
    // buffer index. Other buffer indices aren't described.
    uint32_t used_buffer_mask() const {
      return used_buffer_mask_.load(std::memory_order_relaxed);
    }

   private:
    // A pixel shader that reads its render targets
    // (host_render_target_feedback).
    bool ReadsRenderTargets() const {
      return shader().type() == xenos::ShaderType::kPixel &&
             SpirvShaderTranslator::Modification(modification())
                 .pixel.host_render_target_feedback;
    }
    // A tessellation evaluation (domain) shader, compiled directly to a Metal
    // post-tessellation vertex function by spirv2air.
    bool IsTessellationDomain() const {
      return shader().type() == xenos::ShaderType::kVertex &&
             Shader::IsHostVertexShaderTypeDomain(
                 SpirvShaderTranslator::Modification(modification())
                     .vertex.host_vertex_shader_type);
    }
    // SPIR-V -> AIR directly via spirv2air.
    bool CompileToAirWithSpirv2air(std::vector<uint8_t>& metallib_out,
                                   std::string& entry_point_name_out) const;

    MTL::Library* metal_library_ = nullptr;
    MTL::Function* metal_function_ = nullptr;
    std::string entry_point_name_;
    bool bakes_only_ = false;
    // Atomic for NewGenericLibrary, which a pipeline thread calls while draws
    // read the mask.
    mutable std::atomic<uint32_t> used_buffer_mask_{~uint32_t(0)};
  };

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_DXIL_SHADER_H_
