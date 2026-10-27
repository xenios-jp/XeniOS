/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/dxil_shader.h"

#include <cstddef>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/gpu/metal/metal_shader_converter.h"
#include "xenia/gpu/spirv_to_dxil_compiler.h"

namespace xe {
namespace gpu {
namespace metal {

DxilShader::DxilShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                       const uint32_t* ucode_dwords, size_t ucode_dword_count,
                       std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian) {}

Shader::Translation* DxilShader::CreateTranslationInstance(
    uint64_t modification) {
  return new DxilTranslation(*this, modification);
}

DxilShader::DxilTranslation::~DxilTranslation() {
  if (metal_function_) {
    metal_function_->release();
    metal_function_ = nullptr;
  }
  if (metal_library_) {
    metal_library_->release();
    metal_library_ = nullptr;
  }
}

bool DxilShader::DxilTranslation::CompileToAir(
    MTL::Device* device, const MetalShaderConverter& converter) {
  if (metal_function_) {
    return true;
  }
  if (!device || !converter.is_available()) {
    return false;
  }

  const std::vector<uint8_t>& spirv = translated_binary();
  if (spirv.empty() || (spirv.size() % sizeof(uint32_t)) != 0) {
    XELOGE("DxilShader: shader {:016X} has no usable SPIR-V",
           shader().ucode_data_hash());
    return false;
  }

  bool is_vertex = shader().type() == xenos::ShaderType::kVertex;
  // Bindless matches the D3D12 guest path: every descriptor set resource
  // becomes a heap index the shader reads from its per-stage index buffer.
  std::vector<uint8_t> dxil = SpirvToDxilCompiler::Translate(
      reinterpret_cast<const uint32_t*>(spirv.data()),
      spirv.size() / sizeof(uint32_t),
      is_vertex ? SpirvToDxilCompiler::Stage::kVertex
                : SpirvToDxilCompiler::Stage::kPixel,
      /*lower_to_bindless=*/true, /*input_clip_size=*/0,
      /*preserve_msc_fmad32=*/SpirvToDxilCompiler::SupportsMscFmad32());
  if (dxil.empty()) {
    XELOGE("DxilShader: SPIR-V to DXIL failed for shader {:016X}",
           shader().ucode_data_hash());
    return false;
  }

  MetalShaderConversionResult conversion = converter.Convert(
      is_vertex ? MetalShaderStage::kVertex : MetalShaderStage::kFragment,
      dxil);
  if (!conversion.success) {
    XELOGE("DxilShader: DXIL to AIR failed for shader {:016X}: {}",
           shader().ucode_data_hash(), conversion.error_message);
    return false;
  }

  NS::Error* error = nullptr;
  dispatch_data_t metallib_data = dispatch_data_create(
      conversion.metallib.data(), conversion.metallib.size(), nullptr,
      DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  metal_library_ = device->newLibrary(metallib_data, &error);
  dispatch_release(metallib_data);
  if (!metal_library_) {
    XELOGE(
        "DxilShader: could not create the Metal library for shader {:016X}: {}",
        shader().ucode_data_hash(),
        error ? error->localizedDescription()->utf8String() : "unknown");
    return false;
  }

  NS::String* entry_point_name = NS::String::string(
      conversion.entry_point_name.c_str(), NS::UTF8StringEncoding);
  metal_function_ = metal_library_->newFunction(entry_point_name);
  if (!metal_function_) {
    XELOGE("DxilShader: shader {:016X} has no '{}' function in its library",
           shader().ucode_data_hash(), conversion.entry_point_name);
    metal_library_->release();
    metal_library_ = nullptr;
    return false;
  }
  entry_point_name_ = std::move(conversion.entry_point_name);

  XELOGI(
      "DxilShader: compiled {} shader {:016X} ({}B SPIR-V, {}B DXIL, {}B AIR)",
      is_vertex ? "vertex" : "pixel", shader().ucode_data_hash(), spirv.size(),
      dxil.size(), conversion.metallib.size());
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
