/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Native MSL resource binding helpers.
 ******************************************************************************
 */

#include "xenia/gpu/metal/native_msl_bindings.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>

#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"

namespace xe {
namespace gpu {
namespace metal {
namespace native_msl {
namespace {

uint32_t TextureRuntimeTypeFor(MTL::Texture* texture,
                               xenos::FetchOpDimension dimension) {
  if (dimension == xenos::FetchOpDimension::kCube) {
    return kNativeTextureRuntimeTypeCube;
  }
  if (dimension == xenos::FetchOpDimension::k3DOrStacked && texture &&
      texture->textureType() == MTL::TextureType3D) {
    return kNativeTextureRuntimeType3D;
  }
  return kNativeTextureRuntimeType2DArray;
}

bool ValidateTextureTypeForNativeSignature(MTL::Texture* texture,
                                           xenos::FetchOpDimension dimension,
                                           uint32_t runtime_type) {
  if (!texture) {
    return false;
  }
  MTL::TextureType actual_type = texture->textureType();
  switch (dimension) {
    case xenos::FetchOpDimension::kCube:
      return actual_type == MTL::TextureTypeCube;
    case xenos::FetchOpDimension::k3DOrStacked:
      return runtime_type == kNativeTextureRuntimeType3D
                 ? actual_type == MTL::TextureType3D
                 : actual_type == MTL::TextureType2DArray;
    case xenos::FetchOpDimension::k1D:
    case xenos::FetchOpDimension::k2D:
    default:
      // MetalTextureCache::MetalTexture::GetViewType returns 2DArray for the
      // regular 1D/2D shader fetch dimensions so native MSL can use a single
      // argument type for both true 2D and stacked 2D views.
      return actual_type == MTL::TextureType2DArray;
  }
}

uint32_t NativeMslFallbackReasonId(std::string_view reason) {
  if (reason == "fetch constant has no valid texture binding") {
    return 1u;
  }
  if (reason == "shader texture dimension is incompatible with bound texture") {
    return 2u;
  }
  if (reason == "resolved texture object is null") {
    return 3u;
  }
  if (reason == "resolved texture is not a Metal texture") {
    return 4u;
  }
  if (reason == "failed to create Metal SRV view") {
    return 5u;
  }
  return 0u;
}

bool ShouldLogNativeMslFallbackOnce(uint32_t fetch_constant,
                                    xenos::FetchOpDimension dimension,
                                    bool is_signed, uint32_t native_slot,
                                    std::string_view reason) {
  static std::unordered_set<uint64_t> logged_fallbacks;
  uint64_t key = uint64_t(fetch_constant & 31u);
  key |= uint64_t(uint32_t(dimension) & 3u) << 5;
  key |= uint64_t(is_signed ? 1u : 0u) << 7;
  key |= uint64_t(native_slot & 0xFFu) << 8;
  key |= uint64_t(NativeMslFallbackReasonId(reason) & 0xFFu) << 16;
  return logged_fallbacks.insert(key).second;
}

}  // namespace

bool UsesTextureRuntimeInfo(const DxbcShader::TranslationMetadata& metadata) {
  for (const DxbcShader::TextureBinding& binding : metadata.texture_bindings) {
    if (binding.dimension == xenos::FetchOpDimension::k3DOrStacked) {
      return true;
    }
  }
  return false;
}

bool CaptureTextureRuntimeInfo(MetalTextureCache& texture_cache,
                               const DxbcShader::TranslationMetadata& metadata,
                               NativeMslStageBindings& bindings) {
  bindings.Clear();
  const bool needs_runtime_info = UsesTextureRuntimeInfo(metadata);
  if (needs_runtime_info) {
    bindings.runtime_info.resize(
        std::max<uint32_t>(uint32_t(metadata.texture_bindings.size()), 1));
  }

  for (uint32_t i = 0; i < metadata.texture_bindings.size(); ++i) {
    const DxbcShader::TextureBinding& binding = metadata.texture_bindings[i];
    MTL::Texture* texture = nullptr;
    bool is_fallback_texture = false;
    const char* fallback_reason = nullptr;
    uint32_t bindless_index = texture_cache.GetBindlessSRVIndexForBinding(
        binding.fetch_constant, binding.dimension, binding.is_signed, &texture,
        &is_fallback_texture, &fallback_reason);
    if (bindless_index == UINT32_MAX || !texture) {
      XELOGW("Native MSL failed to resolve texture fetch constant {}",
             binding.fetch_constant);
      return false;
    }
    if (is_fallback_texture && ::cvars::metal_native_msl_debug_validation &&
        fallback_reason &&
        std::string_view(fallback_reason)
                .find("not selected by texture signs") ==
            std::string_view::npos &&
        ShouldLogNativeMslFallbackOnce(
            binding.fetch_constant, binding.dimension, binding.is_signed,
            binding.bindless_descriptor_index, fallback_reason)) {
      XELOGW(
          "Native MSL texture fallback: fetch_constant={} dim={} signed={} "
          "slot={} reason={} fallback_type={} size={}x{} layers={}",
          binding.fetch_constant, uint32_t(binding.dimension),
          binding.is_signed ? 1 : 0, binding.bindless_descriptor_index,
          fallback_reason, uint32_t(texture->textureType()),
          uint32_t(texture->width()), uint32_t(texture->height()),
          uint32_t(std::max<NS::UInteger>(texture->arrayLength(),
                                          texture->depth())));
    }

    const uint32_t runtime_type =
        TextureRuntimeTypeFor(texture, binding.dimension);
    if (!ValidateTextureTypeForNativeSignature(texture, binding.dimension,
                                               runtime_type)) {
      XELOGW(
          "Native MSL texture fetch constant {} returned incompatible "
          "Metal texture type {} for dimension {}",
          binding.fetch_constant, uint32_t(texture->textureType()),
          uint32_t(binding.dimension));
      return false;
    }
    const uint32_t native_bindless_index =
        texture_cache.GetNativeMslSRVIndexForBindlessIndex(bindless_index);
    if (native_bindless_index == UINT32_MAX) {
      XELOGW(
          "Native MSL failed to map bindless texture slot {} for fetch "
          "constant {}",
          bindless_index, binding.fetch_constant);
      return false;
    }

    if (needs_runtime_info) {
      bindings.runtime_info[i].type = runtime_type;
      bindings.runtime_info[i].base_texture_slot = native_bindless_index;
      bindings.runtime_info[i].fetch_constant = binding.fetch_constant;
      bindings.runtime_info[i].flags = binding.is_signed ? 1u : 0u;
    }
  }

  for (const DxbcShader::SamplerBinding& binding : metadata.sampler_bindings) {
    const uint32_t bindless_index =
        texture_cache.GetBindlessSamplerIndexForBinding(binding);
    if (bindless_index == UINT32_MAX ||
        texture_cache.GetNativeMslSamplerIndexForBindlessIndex(
            bindless_index) == UINT32_MAX) {
      XELOGW("Native MSL failed to resolve sampler for fetch constant {}",
             binding.fetch_constant);
      return false;
    }
  }

  return true;
}

}  // namespace native_msl
}  // namespace metal
}  // namespace gpu
}  // namespace xe
