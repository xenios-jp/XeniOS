/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHADER_H_
#define XENIA_GPU_METAL_METAL_SHADER_H_

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/gpu/dxbc_shader.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

class DxbcToDxilConverter;
class MetalShaderConverter;
struct MetalStageCompileResult;

class MetalShader : public DxbcShader {
 public:
  class MetalTranslation : public DxbcTranslation {
   public:
    enum class Backend : uint32_t {
      kMetalShaderConverter = 0,
      kNativeMsl = 1,
    };

    MetalTranslation(MetalShader& shader, uint64_t modification)
        : DxbcTranslation(shader, modification),
          shader_modification_(modification) {}
    ~MetalTranslation();

    // Install a stage-compile result as Metal library/function state.
    bool InstallMetal(MTL::Device* device,
                      const MetalStageCompileResult& result,
                      uint64_t* out_new_library_ms = nullptr,
                      bool* out_new_library_created = nullptr);
    bool InstallNativeMslSource(
        MTL::Device* device, const std::string& msl_source,
        const std::string& function_name,
        const DxbcShader::TranslationMetadata& native_metadata,
        uint64_t* out_new_library_ms = nullptr,
        bool* out_new_library_created = nullptr,
        bool* out_library_cache_hit = nullptr);

    void SetDxilData(std::vector<uint8_t> dxil_data);
    std::vector<uint8_t> GetDxilDataCopy() const;

    // Get the Metal library (contains compiled shader)
    MTL::Library* metal_library() const { return metal_library_; }

    // Get the Metal function (shader entry point)
    MTL::Function* metal_function() const { return metal_function_; }

    // Check if translation succeeded
    bool is_valid() const { return metal_function_ != nullptr; }
    Backend backend() const { return backend_; }
    bool is_native_msl() const { return backend_ == Backend::kNativeMsl; }
    uint64_t shader_modification() const { return shader_modification_; }
    uint64_t native_msl_texture_sign_key() const {
      return native_msl_texture_sign_key_;
    }
    const DxbcShader::TextureSignComponentMasks&
    native_msl_texture_sign_component_masks() const {
      return native_msl_texture_sign_component_masks_;
    }
    const DxbcShader::TextureSignComponentMasks&
    native_msl_texture_sign_values() const {
      return native_msl_texture_sign_values_;
    }
    bool ConfigureNativeMslVariant(
        uint64_t shader_modification, uint64_t texture_sign_key,
        const DxbcShader::TextureSignComponentMasks&
            texture_sign_component_masks,
        const DxbcShader::TextureSignComponentMasks& texture_sign_values);
    bool TryClaimNativeMslAttempt() {
      return !native_msl_attempted_.test_and_set(std::memory_order_acq_rel);
    }

    // Thread-safe translation claiming for async pipeline compilation.
    // Returns true if this thread successfully claimed translation rights.
    // Returns false if another thread already claimed it.
    bool TryClaimTranslation() {
      return !translation_claimed_.test_and_set(std::memory_order_acq_rel);
    }

    // Get intermediate data for debugging
    const std::vector<uint8_t>& dxil_data() const { return dxil_data_; }
    const std::vector<uint8_t>& metallib_data() const { return metallib_data_; }
    const std::string& native_msl_source() const { return native_msl_source_; }
    const DxbcShader::TranslationMetadata& native_msl_metadata() const {
      return native_msl_metadata_;
    }
    const std::string& function_name() const { return function_name_; }

   private:
    std::atomic_flag translation_claimed_ = ATOMIC_FLAG_INIT;
    std::atomic_flag native_msl_attempted_ = ATOMIC_FLAG_INIT;
    mutable std::mutex metal_translation_mutex_;
    MTL::Library* metal_library_ = nullptr;
    MTL::Function* metal_function_ = nullptr;
    Backend backend_ = Backend::kMetalShaderConverter;
    std::vector<uint8_t> dxil_data_;
    std::vector<uint8_t> metallib_data_;
    std::string native_msl_source_;
    DxbcShader::TranslationMetadata native_msl_metadata_ = {};
    std::string function_name_;
    uint64_t shader_modification_ = 0;
    uint64_t native_msl_texture_sign_key_ = 0;
    DxbcShader::TextureSignComponentMasks
        native_msl_texture_sign_component_masks_ = {};
    DxbcShader::TextureSignComponentMasks native_msl_texture_sign_values_ = {};
    bool native_msl_variant_configured_ = false;
  };

  MetalShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
              const uint32_t* ucode_dwords, size_t ucode_dword_count,
              std::endian ucode_source_endian = std::endian::big);

  struct DrawConstantMetadata {
    DxbcShader::FetchConstantDwordMask shader_fetch_constant_dword_mask = {};
    uint32_t descriptor_indices_word_count = 1;
    uint32_t active_cbv_mask = 0;
  };

  const DrawConstantMetadata& GetDrawConstantMetadata() const;

  // For owning subsystem like the pipeline cache, accessors for unique
  // identifiers (used instead of hashes to make sure collisions can't happen)
  // of binding layouts used by the shader, for invalidation if a shader with an
  // incompatible layout was bound.
  size_t GetTextureBindingLayoutUserUID() const {
    return texture_binding_layout_user_uid_;
  }
  size_t GetSamplerBindingLayoutUserUID() const {
    return sampler_binding_layout_user_uid_;
  }
  // Modifications of the same shader can be translated on different threads.
  // The "set" function must only be called if "enter" returned true - these are
  // set up only once.
  bool EnterBindingLayoutUserUIDSetup() {
    return !binding_layout_user_uids_set_up_.test_and_set();
  }
  void SetTextureBindingLayoutUserUID(size_t uid) {
    texture_binding_layout_user_uid_ = uid;
  }
  void SetSamplerBindingLayoutUserUID(size_t uid) {
    sampler_binding_layout_user_uid_ = uid;
  }

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;

 private:
  std::atomic_flag binding_layout_user_uids_set_up_ = ATOMIC_FLAG_INIT;
  size_t texture_binding_layout_user_uid_ = 0;
  size_t sampler_binding_layout_user_uid_ = 0;
  mutable std::once_flag draw_constant_metadata_once_;
  mutable DrawConstantMetadata draw_constant_metadata_ = {};
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_SHADER_H_
