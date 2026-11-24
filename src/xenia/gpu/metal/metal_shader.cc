/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shader.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/gpu/spirv_shader_translator.h"

#include "spirv2air.h"

namespace xe {
namespace gpu {
namespace metal {

MetalShader::MetalShader(xenos::ShaderType shader_type,
                         uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
                         size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian) {}

Shader::Translation* MetalShader::CreateTranslationInstance(
    uint64_t modification) {
  return new MetalTranslation(*this, modification);
}

MetalShader::MetalTranslation::~MetalTranslation() {
  if (metal_function_) {
    metal_function_->release();
    metal_function_ = nullptr;
  }
  if (metal_library_) {
    metal_library_->release();
    metal_library_ = nullptr;
  }
}

bool MetalShader::MetalTranslation::CompileToAirWithSpirv2air(
    std::vector<uint8_t>& metallib_out,
    std::string& entry_point_name_out) const {
  const std::vector<uint8_t>& spirv = translated_binary();
  if (spirv.empty() || (spirv.size() % sizeof(uint32_t)) != 0) {
    XELOGE("MetalShader: shader {:016X} has no usable SPIR-V",
           shader().ucode_data_hash());
    return false;
  }
  spirv2air::Options options;
  options.halfPackingRounding = spirv2air::HalfPackingRounding::TowardZero;
  options.graphics.functionConstantSet =
      SpirvShaderTranslator::kDescriptorSetFetchState;
  // Input attachment N is color attachment N. The mask is part of the
  // modification, which keys the translation.
  if (ReadsRenderTargets()) {
    options.graphics.framebufferFetchMask =
        (UINT32_C(1) << xenos::kMaxColorRenderTargets) - 1;
  }
  spirv2air::Result result =
      spirv2air::translate(reinterpret_cast<const uint32_t*>(spirv.data()),
                           spirv.size() / sizeof(uint32_t), options);
  if (!result.ok()) {
    static std::atomic<uint64_t> spirv2air_reject_count{0};
    const uint64_t reject_index =
        spirv2air_reject_count.fetch_add(1, std::memory_order_relaxed) + 1;
    XELOGW(
        "MetalShader: spirv2air rejected {} shader {:016X} modification "
        "{:016X} (reject #{}: {})",
        IsTessellationDomain()
            ? "domain"
            : (shader().type() == xenos::ShaderType::kVertex ? "vertex"
                                                             : "pixel"),
        shader().ucode_data_hash(), modification(), reject_index, result.error);
    return false;
  }
  metallib_out = std::move(result.metallib);
  entry_point_name_out = "main";
  return true;
}

// The buffer bind points a function spirv2air compiles from |spirv| can
// declare: always the top-level argument buffer (2), the descriptor heap (0)
// only if the module has an image type, and the sampler heap (1) only if it
// has a sampler type. Every bit is set if the module can't be walked.
static uint32_t Spirv2airBufferMask(const std::vector<uint8_t>& spirv) {
  constexpr uint32_t kOpTypeImage = 25;
  constexpr uint32_t kOpTypeSampler = 26;
  const size_t word_count = spirv.size() / sizeof(uint32_t);
  constexpr uint32_t kDescriptorHeapBuffer = 0;
  constexpr uint32_t kSamplerHeapBuffer = 1;
  constexpr uint32_t kArgumentBuffer = 2;
  uint32_t mask = uint32_t(1) << kArgumentBuffer;
  for (size_t i = 5; i < word_count;) {
    uint32_t word;
    std::memcpy(&word, spirv.data() + i * sizeof(uint32_t), sizeof(word));
    const uint32_t length = word >> 16;
    if (!length || length > word_count - i) {
      return ~uint32_t(0);
    }
    const uint32_t opcode = word & 0xFFFF;
    if (opcode == kOpTypeImage) {
      mask |= uint32_t(1) << kDescriptorHeapBuffer;
    } else if (opcode == kOpTypeSampler) {
      mask |= uint32_t(1) << kSamplerHeapBuffer;
    }
    i += length;
  }
  return mask;
}

bool MetalShader::MetalTranslation::CompileWithFetchState(
    const uint32_t* states, uint32_t count, std::vector<uint8_t>& metallib_out,
    std::string& entry_point_name_out) const {
  const std::vector<uint8_t>& spirv = translated_binary();
  if (spirv.empty() || (spirv.size() % sizeof(uint32_t)) != 0) {
    return false;
  }
  spirv2air::Options options;
  options.halfPackingRounding = spirv2air::HalfPackingRounding::TowardZero;
  options.graphics.functionConstantSet =
      SpirvShaderTranslator::kDescriptorSetFetchState;
  // As CompileToAirWithSpirv2air: input attachment N is color attachment N.
  if (ReadsRenderTargets()) {
    options.graphics.framebufferFetchMask =
        (UINT32_C(1) << xenos::kMaxColorRenderTargets) - 1;
  }
  for (uint32_t i = 0; i < count; ++i) {
    options.graphics.functionConstantValues[i] = {
        states[4 * i], states[4 * i + 1], states[4 * i + 2], states[4 * i + 3]};
  }
  spirv2air::Result result =
      spirv2air::translate(reinterpret_cast<const uint32_t*>(spirv.data()),
                           spirv.size() / sizeof(uint32_t), options);
  if (!result.ok()) {
    return false;
  }
  metallib_out = std::move(result.metallib);
  entry_point_name_out = "main";
  return true;
}

bool MetalShader::MetalTranslation::LoadMetallib(
    MTL::Device* device, const std::vector<uint8_t>& metallib,
    const std::string& entry_point_name, uint32_t used_buffer_mask) {
  if (metal_function_) {
    return true;
  }
  if (!device || metallib.empty()) {
    return false;
  }
  NS::Error* error = nullptr;
  dispatch_data_t metallib_data =
      dispatch_data_create(metallib.data(), metallib.size(), nullptr,
                           DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* library = device->newLibrary(metallib_data, &error);
  dispatch_release(metallib_data);
  if (!library) {
    return false;
  }
  MTL::Function* function = library->newFunction(
      NS::String::string(entry_point_name.c_str(), NS::UTF8StringEncoding));
  if (!function) {
    library->release();
    return false;
  }
  used_buffer_mask_ = used_buffer_mask;
  metal_library_ = library;
  metal_function_ = function;
  entry_point_name_ = entry_point_name;
  return true;
}

void MetalShader::MetalTranslation::SetBakesOnly(uint32_t used_buffer_mask) {
  used_buffer_mask_ = used_buffer_mask;
  // What CompileWithFetchState names the function of each of its libraries.
  entry_point_name_ = "main";
  bakes_only_ = true;
}

uint32_t MetalShader::MetalTranslation::GetSpirv2airBufferMask() const {
  return Spirv2airBufferMask(translated_binary());
}

MTL::Library* MetalShader::MetalTranslation::NewGenericLibrary(
    MTL::Device* device, std::string& entry_point_name_out) const {
  std::vector<uint8_t> metallib;
  std::string entry_point_name;
  if (!device || !CompileToAirWithSpirv2air(metallib, entry_point_name)) {
    return nullptr;
  }
  NS::Error* error = nullptr;
  dispatch_data_t metallib_data =
      dispatch_data_create(metallib.data(), metallib.size(), nullptr,
                           DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* library = device->newLibrary(metallib_data, &error);
  dispatch_release(metallib_data);
  if (!library) {
    return nullptr;
  }
  entry_point_name_out = std::move(entry_point_name);
  return library;
}

bool MetalShader::MetalTranslation::CompileToAir(
    MTL::Device* device, std::vector<uint8_t>* metallib_out) {
  if (metal_function_) {
    return true;
  }
  if (!device) {
    return false;
  }

  const std::vector<uint8_t>& spirv = translated_binary();
  std::vector<uint8_t> metallib;
  std::string entry_point_name_str;
  if (!CompileToAirWithSpirv2air(metallib, entry_point_name_str)) {
    return false;
  }
  used_buffer_mask_ = Spirv2airBufferMask(spirv);

  const size_t metallib_size = metallib.size();
  NS::Error* error = nullptr;
  dispatch_data_t metallib_data =
      dispatch_data_create(metallib.data(), metallib.size(), nullptr,
                           DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  metal_library_ = device->newLibrary(metallib_data, &error);
  dispatch_release(metallib_data);
  if (!metal_library_) {
    XELOGE(
        "MetalShader: could not create the Metal library for shader {:016X}: "
        "{}",
        shader().ucode_data_hash(),
        error ? error->localizedDescription()->utf8String() : "unknown");
    return false;
  }

  NS::String* entry_point_name =
      NS::String::string(entry_point_name_str.c_str(), NS::UTF8StringEncoding);
  metal_function_ = metal_library_->newFunction(entry_point_name);
  if (!metal_function_) {
    XELOGE("MetalShader: shader {:016X} has no '{}' function in its library",
           shader().ucode_data_hash(), entry_point_name_str);
    metal_library_->release();
    metal_library_ = nullptr;
    return false;
  }
  entry_point_name_ = std::move(entry_point_name_str);
  if (metallib_out) {
    *metallib_out = std::move(metallib);
  }

  XELOGD(
      "MetalShader: compiled {} shader {:016X} with spirv2air ({}B SPIR-V, "
      "{}B AIR)",
      IsTessellationDomain()
          ? "domain"
          : (shader().type() == xenos::ShaderType::kVertex ? "vertex"
                                                           : "pixel"),
      shader().ucode_data_hash(), spirv.size(), metallib_size);
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
