/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shader.h"

#include <dispatch/dispatch.h>
#include <inttypes.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/dxbc_to_dxil_converter.h"
#include "xenia/gpu/metal/metal_shader_converter.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

constexpr uint32_t kAllTranslatedCbvMask =
    (uint32_t(1)
     << (uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices) +
         1)) -
    1;
constexpr uint32_t kDescriptorIndicesCbvSizeBytes = 4096;

struct NativeMslSourceLibraryCacheKey {
  MTL::Device* device = nullptr;
  uint64_t source_hash = 0;
  size_t source_size = 0;
  uint32_t compile_options = 0;

  bool operator==(const NativeMslSourceLibraryCacheKey& other) const {
    return device == other.device && source_hash == other.source_hash &&
           source_size == other.source_size &&
           compile_options == other.compile_options;
  }
};

struct NativeMslSourceLibraryCacheKeyHasher {
  size_t operator()(const NativeMslSourceLibraryCacheKey& key) const {
    uint64_t words[] = {
        reinterpret_cast<uintptr_t>(key.device),
        key.source_hash,
        uint64_t(key.source_size),
        key.compile_options,
    };
    return size_t(XXH3_64bits(words, sizeof(words)));
  }
};

struct NativeMslSourceLibraryCacheEntry {
  explicit NativeMslSourceLibraryCacheEntry(std::string source)
      : source(std::move(source)) {}

  ~NativeMslSourceLibraryCacheEntry() {
    if (library) {
      library->release();
    }
  }

  std::string source;
  MTL::Library* library = nullptr;
  bool compiling = true;
  std::string error_message;
  std::condition_variable ready_cond;
};

class NativeMslSourceLibraryCache {
 public:
  MTL::Library* GetOrCompile(MTL::Device* device, const std::string& source,
                             uint32_t compile_options_key,
                             MTL::CompileOptions* compile_options,
                             uint64_t* out_new_library_ms,
                             bool* out_new_library_created, bool* out_cache_hit,
                             std::string* out_error_message,
                             std::string* out_diagnostics_message) {
    if (out_new_library_ms) {
      *out_new_library_ms = 0;
    }
    if (out_new_library_created) {
      *out_new_library_created = false;
    }
    if (out_cache_hit) {
      *out_cache_hit = false;
    }
    if (out_error_message) {
      out_error_message->clear();
    }
    if (out_diagnostics_message) {
      out_diagnostics_message->clear();
    }

    NativeMslSourceLibraryCacheKey key = {};
    key.device = device;
    key.source_hash = XXH3_64bits(source.data(), source.size());
    key.source_size = source.size();
    key.compile_options = compile_options_key;

    std::shared_ptr<NativeMslSourceLibraryCacheEntry> entry;
    bool compile_this_entry = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      auto& bucket = entries_[key];
      for (const auto& candidate : bucket) {
        if (candidate->source != source) {
          continue;
        }
        entry = candidate;
        while (entry->compiling) {
          entry->ready_cond.wait(lock);
        }
        if (entry->library) {
          entry->library->retain();
          if (out_cache_hit) {
            *out_cache_hit = true;
          }
          return entry->library;
        }
        if (out_error_message) {
          *out_error_message = entry->error_message;
        }
        return nullptr;
      }

      entry = std::make_shared<NativeMslSourceLibraryCacheEntry>(source);
      bucket.push_back(entry);
      compile_this_entry = true;
    }

    assert_true(compile_this_entry);
    NS::Error* error = nullptr;
    NS::String* source_string =
        NS::String::string(source.c_str(), NS::UTF8StringEncoding);
    const auto library_start = std::chrono::steady_clock::now();
    MTL::Library* library =
        device->newLibrary(source_string, compile_options, &error);
    const auto library_end = std::chrono::steady_clock::now();
    if (out_new_library_ms) {
      *out_new_library_ms =
          uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                       library_end - library_start)
                       .count());
    }
    if (out_new_library_created) {
      *out_new_library_created = true;
    }

    std::string error_message;
    if (error) {
      error_message = error->localizedDescription()->utf8String();
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (library) {
        entry->library = library;
      } else {
        entry->error_message = error_message;
        auto found_bucket = entries_.find(key);
        if (found_bucket != entries_.end()) {
          auto& bucket = found_bucket->second;
          bucket.erase(std::remove(bucket.begin(), bucket.end(), entry),
                       bucket.end());
          if (bucket.empty()) {
            entries_.erase(found_bucket);
          }
        }
      }
      entry->compiling = false;
    }
    entry->ready_cond.notify_all();

    if (!library) {
      if (out_error_message) {
        *out_error_message = error_message;
      }
      return nullptr;
    }
    if (out_diagnostics_message && !error_message.empty()) {
      *out_diagnostics_message = error_message;
    }
    library->retain();
    return library;
  }

 private:
  std::mutex mutex_;
  std::unordered_map<
      NativeMslSourceLibraryCacheKey,
      std::vector<std::shared_ptr<NativeMslSourceLibraryCacheEntry>>,
      NativeMslSourceLibraryCacheKeyHasher>
      entries_;
};

NativeMslSourceLibraryCache& GetNativeMslSourceLibraryCache() {
  static NativeMslSourceLibraryCache cache;
  return cache;
}

uint32_t GetNativeMslCompileOptionsKey() {
  uint32_t key = 0;
  if (cvars::metal_native_msl_debug_validation) {
    key |= uint32_t(1) << 0;
  }
  if (cvars::metal_native_msl_fast_math) {
    key |= uint32_t(1) << 1;
  }
  if (cvars::metal_native_msl_debug_validation ||
      !cvars::metal_native_msl_fast_math) {
    key |= uint32_t(1) << 2;  // MTL::MathModeSafe.
    key |= uint32_t(1) << 3;  // MTL::MathFloatingPointFunctionsFast.
    key |= uint32_t(1) << 4;  // MTL::LibraryOptimizationLevelDefault.
  }
  if (cvars::metal_native_msl_debug_validation) {
    key |= uint32_t(1) << 5;  // logging.
    key |= uint32_t(1) << 6;  // preserve invariance.
  }
  return key;
}

void MarkFetchConstantDword(DxbcShader::FetchConstantDwordMask& mask,
                            uint32_t dword_index) {
  if (dword_index >= DxbcShader::kFetchConstantDwordCount) {
    assert_always();
    return;
  }
  mask[dword_index >> 5] |= uint32_t(1) << (dword_index & 31);
}

void MarkVertexFetchConstant(DxbcShader::FetchConstantDwordMask& mask,
                             uint32_t fetch_constant_index) {
  if (fetch_constant_index >= xenos::kVertexFetchConstantCount) {
    assert_always();
    return;
  }
  const uint32_t dword_index = fetch_constant_index * 2;
  MarkFetchConstantDword(mask, dword_index);
  MarkFetchConstantDword(mask, dword_index + 1);
}

void MarkTextureFetchConstant(DxbcShader::FetchConstantDwordMask& mask,
                              uint32_t fetch_constant_index) {
  if (fetch_constant_index >= xenos::kTextureFetchConstantCount) {
    assert_always();
    return;
  }
  const uint32_t dword_index = fetch_constant_index * 6;
  for (uint32_t i = 0; i < 6; ++i) {
    MarkFetchConstantDword(mask, dword_index + i);
  }
}

}  // namespace

MetalShader::MetalShader(xenos::ShaderType shader_type,
                         uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
                         size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : DxbcShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                 ucode_source_endian) {}

const MetalShader::DrawConstantMetadata&
MetalShader::GetDrawConstantMetadata() const {
  std::call_once(draw_constant_metadata_once_, [this]() {
    DrawConstantMetadata metadata = {};

    const Shader::ConstantRegisterMap& constant_map = constant_register_map();
    for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap);
         ++i) {
      uint32_t vfetch_bits_remaining = constant_map.vertex_fetch_bitmap[i];
      uint32_t bit_index;
      while (xe::bit_scan_forward(vfetch_bits_remaining, &bit_index)) {
        vfetch_bits_remaining = xe::clear_lowest_bit(vfetch_bits_remaining);
        MarkVertexFetchConstant(metadata.shader_fetch_constant_dword_mask,
                                i * 32 + bit_index);
      }
    }

    for (const Shader::VertexBinding& binding : vertex_bindings()) {
      MarkVertexFetchConstant(metadata.shader_fetch_constant_dword_mask,
                              binding.fetch_constant);
    }
    for (const Shader::TextureBinding& binding : texture_bindings()) {
      MarkTextureFetchConstant(metadata.shader_fetch_constant_dword_mask,
                               binding.fetch_constant);
    }

    constexpr uint32_t kMaxDescriptorIndexWords =
        kDescriptorIndicesCbvSizeBytes / sizeof(uint32_t);
    metadata.descriptor_indices_word_count = 1;
    for (const DxbcShader::TextureBinding& binding :
         GetTextureBindingsAfterTranslation()) {
      assert_true(binding.bindless_descriptor_index < kMaxDescriptorIndexWords);
      if (binding.bindless_descriptor_index < kMaxDescriptorIndexWords) {
        metadata.descriptor_indices_word_count =
            std::max(metadata.descriptor_indices_word_count,
                     binding.bindless_descriptor_index + 1);
      }
    }
    for (const DxbcShader::SamplerBinding& binding :
         GetSamplerBindingsAfterTranslation()) {
      assert_true(binding.bindless_descriptor_index < kMaxDescriptorIndexWords);
      if (binding.bindless_descriptor_index < kMaxDescriptorIndexWords) {
        metadata.descriptor_indices_word_count =
            std::max(metadata.descriptor_indices_word_count,
                     binding.bindless_descriptor_index + 1);
      }
    }

    const uint32_t used_cbuffer_mask = GetUsedCbufferMaskAfterTranslation();
    metadata.active_cbv_mask =
        used_cbuffer_mask ? (used_cbuffer_mask & kAllTranslatedCbvMask)
                          : kAllTranslatedCbvMask;

    draw_constant_metadata_ = metadata;
  });
  return draw_constant_metadata_;
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

bool MetalShader::MetalTranslation::ConfigureNativeMslVariant(
    uint64_t shader_modification, uint64_t texture_sign_key,
    const DxbcShader::TextureSignComponentMasks& texture_sign_component_masks,
    const DxbcShader::TextureSignComponentMasks& texture_sign_values) {
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  if (native_msl_variant_configured_) {
    return shader_modification_ == shader_modification &&
           native_msl_texture_sign_key_ == texture_sign_key &&
           native_msl_texture_sign_component_masks_ ==
               texture_sign_component_masks &&
           native_msl_texture_sign_values_ == texture_sign_values;
  }
  shader_modification_ = shader_modification;
  native_msl_texture_sign_key_ = texture_sign_key;
  native_msl_texture_sign_component_masks_ = texture_sign_component_masks;
  native_msl_texture_sign_values_ = texture_sign_values;
  native_msl_variant_configured_ = true;
  return true;
}

bool MetalShader::MetalTranslation::InstallMetal(
    MTL::Device* device, const MetalStageCompileResult& result,
    uint64_t* out_new_library_ms, bool* out_new_library_created) {
  if (out_new_library_ms) {
    *out_new_library_ms = 0;
  }
  if (out_new_library_created) {
    *out_new_library_created = false;
  }
  if (!device) {
    XELOGE("MetalShader: No Metal device provided");
    return false;
  }
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  if (metal_function_) {
    return true;
  }
  if (!result.success || result.metallib_data.empty()) {
    XELOGE("MetalShader: invalid stage compile result: {}",
           result.error_message);
    return false;
  }
  function_name_ = result.function_name;
  metallib_data_ = result.metallib_data;
  native_msl_source_.clear();
  native_msl_metadata_ = {};
  backend_ = Backend::kMetalShaderConverter;

  // Debug: Dump shader artifacts (DXBC, DXIL, MetalLib) to files when enabled.
  if (!cvars::dump_shaders.empty()) {
    std::filesystem::path base_dir = cvars::dump_shaders / "metal_shaders";

    char filename[128];
    const char* stage_str =
        (shader().type() == xenos::ShaderType::kVertex) ? "vert" : "frag";

    // Dump DXBC (translated binary from DXBC translator)
    const auto& dxbc_data = translated_binary();
    if (!dxbc_data.empty()) {
      snprintf(filename, sizeof(filename),
               "shader_%016" PRIX64 "_%016" PRIX64 ".msc.%s.dxbc",
               shader().ucode_data_hash(), modification(), stage_str);
      std::filesystem::path dxbc_path = base_dir / filename;
      xe::filesystem::CreateParentFolder(dxbc_path);
      FILE* f = xe::filesystem::OpenFile(dxbc_path, "wb");
      if (f) {
        fwrite(dxbc_data.data(), sizeof(dxbc_data[0]), dxbc_data.size(), f);
        fclose(f);
      }
    }

    // Dump DXIL
    if (!dxil_data_.empty()) {
      snprintf(filename, sizeof(filename),
               "shader_%016" PRIX64 "_%016" PRIX64 ".msc.%s.dxil",
               shader().ucode_data_hash(), modification(), stage_str);
      std::filesystem::path dxil_path = base_dir / filename;
      xe::filesystem::CreateParentFolder(dxil_path);
      FILE* f = xe::filesystem::OpenFile(dxil_path, "wb");
      if (f) {
        fwrite(dxil_data_.data(), sizeof(dxil_data_[0]), dxil_data_.size(), f);
        fclose(f);
      }
    }

    // Dump MetalLib
    if (!metallib_data_.empty()) {
      snprintf(filename, sizeof(filename),
               "shader_%016" PRIX64 "_%016" PRIX64 ".msc.%s.metallib",
               shader().ucode_data_hash(), modification(), stage_str);
      std::filesystem::path metallib_path = base_dir / filename;
      xe::filesystem::CreateParentFolder(metallib_path);
      FILE* f = xe::filesystem::OpenFile(metallib_path, "wb");
      if (f) {
        fwrite(metallib_data_.data(), sizeof(metallib_data_[0]),
               metallib_data_.size(), f);
        fclose(f);
      }
    }
  }

  NS::Error* error = nullptr;
  dispatch_data_t data =
      dispatch_data_create(metallib_data_.data(), metallib_data_.size(),
                           nullptr, DISPATCH_DATA_DESTRUCTOR_NONE);

  auto library_start = std::chrono::steady_clock::now();
  metal_library_ = device->newLibrary(data, &error);
  auto library_end = std::chrono::steady_clock::now();
  if (out_new_library_created) {
    *out_new_library_created = true;
  }
  if (out_new_library_ms) {
    *out_new_library_ms =
        uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                     library_end - library_start)
                     .count());
  }
  dispatch_release(data);

  if (!metal_library_) {
    if (error) {
      XELOGE("MetalShader: Failed to create Metal library: {}",
             error->localizedDescription()->utf8String());
    } else {
      XELOGE("MetalShader: Failed to create Metal library (unknown error)");
    }
    return false;
  }

  NS::String* function_name = NS::String::string(
      result.function_name.c_str(), NS::UTF8StringEncoding);

  metal_function_ = metal_library_->newFunction(function_name);

  if (!metal_function_) {
    XELOGE("MetalShader: Function '{}' not found in metallib",
           function_name_);
    return false;
  }

  return true;
}

bool MetalShader::MetalTranslation::InstallNativeMslSource(
    MTL::Device* device, const std::string& msl_source,
    const std::string& function_name,
    const DxbcShader::TranslationMetadata& native_metadata,
    uint64_t* out_new_library_ms, bool* out_new_library_created,
    bool* out_library_cache_hit) {
  if (out_new_library_ms) {
    *out_new_library_ms = 0;
  }
  if (out_new_library_created) {
    *out_new_library_created = false;
  }
  if (out_library_cache_hit) {
    *out_library_cache_hit = false;
  }
  if (!device) {
    XELOGE("MetalShader: No Metal device provided for native MSL");
    return false;
  }
  if (msl_source.empty() || function_name.empty()) {
    XELOGE("MetalShader: invalid native MSL install request");
    return false;
  }

  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  if (metal_function_) {
    return backend_ == Backend::kNativeMsl;
  }

  MTL::CompileOptions* compile_options = nullptr;
  if (cvars::metal_native_msl_debug_validation ||
      !cvars::metal_native_msl_fast_math) {
    compile_options = MTL::CompileOptions::alloc()->init();
    compile_options->setMathMode(MTL::MathModeSafe);
    compile_options->setMathFloatingPointFunctions(
        MTL::MathFloatingPointFunctionsFast);
    if (cvars::metal_native_msl_debug_validation) {
      compile_options->setEnableLogging(true);
      compile_options->setPreserveInvariance(true);
    }
    compile_options->setOptimizationLevel(MTL::LibraryOptimizationLevelDefault);
  }
  std::string error_message;
  std::string diagnostics_message;
  metal_library_ = GetNativeMslSourceLibraryCache().GetOrCompile(
      device, msl_source, GetNativeMslCompileOptionsKey(), compile_options,
      out_new_library_ms, out_new_library_created, out_library_cache_hit,
      &error_message, &diagnostics_message);
  if (compile_options) {
    compile_options->release();
  }
  if (!metal_library_) {
    if (!error_message.empty()) {
      XELOGE("MetalShader: native MSL library compile failed: {}",
             error_message);
    } else {
      XELOGE("MetalShader: native MSL library compile failed");
    }
    return false;
  }
  if (!diagnostics_message.empty()) {
    XELOGW("MetalShader: native MSL library compile diagnostics: {}",
           diagnostics_message);
  }

  function_name_ = function_name;
  NS::String* function_name_string =
      NS::String::string(function_name_.c_str(), NS::UTF8StringEncoding);
  metal_function_ = metal_library_->newFunction(function_name_string);
  if (!metal_function_) {
    XELOGE("MetalShader: native MSL function '{}' not found", function_name_);
    return false;
  }

  native_msl_source_ = msl_source;
  native_msl_metadata_ = native_metadata;
  metallib_data_.clear();
  backend_ = Backend::kNativeMsl;
  return true;
}

void MetalShader::MetalTranslation::SetDxilData(
    std::vector<uint8_t> dxil_data) {
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  dxil_data_ = std::move(dxil_data);
}

std::vector<uint8_t> MetalShader::MetalTranslation::GetDxilDataCopy() const {
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  return dxil_data_;
}

Shader::Translation* MetalShader::CreateTranslationInstance(
    uint64_t modification) {
  return new MetalTranslation(*this, modification);
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
