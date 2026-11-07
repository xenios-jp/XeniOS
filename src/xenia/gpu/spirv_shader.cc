/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/spirv_shader.h"

#include <cstring>
#include <thread>

namespace xe {
namespace gpu {

SpirvShader::SpirvShader(xenos::ShaderType shader_type,
                         uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
                         size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : Shader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
             ucode_source_endian) {}

void SpirvShader::PublishBindings(
    const std::vector<TextureBinding>& texture_bindings,
    const std::vector<SamplerBinding>& sampler_bindings) {
  if (bindings_setup_entered_.test_and_set(std::memory_order_relaxed)) {
    // Wait for the caller still publishing them.
    while (!bindings_ready()) {
      std::this_thread::yield();
    }
    return;
  }
  texture_bindings_ = texture_bindings;
  used_texture_mask_ = 0;
  for (const TextureBinding& binding : texture_bindings_) {
    used_texture_mask_ |= UINT32_C(1) << binding.fetch_constant;
  }
  sampler_bindings_ = sampler_bindings;
  // Draw-thread readers (GetGuestMesaSpirvShader) only consult the bindings
  // once they are fully written: a PS may be translated on a creation thread.
  bindings_ready_.store(true, std::memory_order_release);
}

Shader::Translation* SpirvShader::CreateTranslationInstance(
    uint64_t modification) {
  return new SpirvTranslation(*this, modification);
}

}  // namespace gpu
}  // namespace xe
