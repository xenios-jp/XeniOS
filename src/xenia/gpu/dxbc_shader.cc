/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/dxbc_shader.h"

#include <cstring>

namespace xe {
namespace gpu {

DxbcShader::DxbcShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                       const uint32_t* ucode_dwords, size_t ucode_dword_count,
                       std::endian ucode_source_endian)
    : Shader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
             ucode_source_endian) {}

DxbcShader::TranslationMetadata DxbcShader::GetTranslationMetadata() const {
  TranslationMetadata metadata;
  metadata.texture_bindings = texture_bindings_;
  metadata.sampler_bindings = sampler_bindings_;
  metadata.used_texture_mask = used_texture_mask_;
  metadata.used_cbuffer_mask = used_cbuffer_mask_;
  metadata.fetch_constant_dword_mask = fetch_constant_dword_mask_;
  metadata.texture_sign_component_masks = texture_sign_component_masks_;
  metadata.uses_shared_memory = uses_shared_memory_;
  metadata.uses_primitive_index_constants = uses_primitive_index_constants_;
  return metadata;
}

void DxbcShader::SetTranslationMetadata(const TranslationMetadata& metadata) {
  texture_bindings_ = metadata.texture_bindings;
  sampler_bindings_ = metadata.sampler_bindings;
  used_texture_mask_ = metadata.used_texture_mask;
  used_cbuffer_mask_ = metadata.used_cbuffer_mask;
  fetch_constant_dword_mask_ = metadata.fetch_constant_dword_mask;
  texture_sign_component_masks_ = metadata.texture_sign_component_masks;
  uses_shared_memory_ = metadata.uses_shared_memory;
  uses_primitive_index_constants_ = metadata.uses_primitive_index_constants;
}

Shader::Translation* DxbcShader::CreateTranslationInstance(
    uint64_t modification) {
  return new DxbcTranslation(*this, modification);
}

}  // namespace gpu
}  // namespace xe
