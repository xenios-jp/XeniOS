/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_argument_binder.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_shared_memory.h"
#include "xenia/gpu/metal/metal_texture_cache.h"

namespace xe {
namespace gpu {
namespace metal {

MetalArgumentBinder::MetalArgumentBinder(
    MetalCommandProcessor& command_processor)
    : command_processor_(command_processor) {}

MetalArgumentBinder::~MetalArgumentBinder() = default;

void MetalArgumentBinder::ResetUploadCaches() {
  gather_cache_valid_ = false;
  for (auto& cache : constant_slices_) {
    cache.Reset();
  }
  for (auto& cache : stage_index_slices_) {
    cache.Reset();
  }
  descriptor_heap_slices_valid_ = false;
  cached_texture_heap_slice_ = {};
  cached_sampler_heap_slice_ = {};
  argument_buffer_slices_.Reset();
}

bool MetalArgumentBinder::Upload(const void* data, uint32_t size,
                                 Slice& slice_out) {
  uint32_t bytes = std::max(size, uint32_t(1));
  if (!command_processor_.AcquireSpirvArgumentBufferSlice(
          bytes, kSliceAlignment, &slice_out.buffer, &slice_out.offset)) {
    return false;
  }
  slice_out.gpu_address =
      uint64_t(slice_out.buffer->gpuAddress()) + uint64_t(slice_out.offset);
  uint8_t* mapping =
      static_cast<uint8_t*>(slice_out.buffer->contents()) + slice_out.offset;
  if (data && size) {
    std::memcpy(mapping, data, size);
  } else {
    std::memset(mapping, 0, bytes);
  }
  return true;
}

bool MetalArgumentBinder::GatherStage(const SpirvShader* shader,
                                      StageRange& range_out) {
  range_out.texture_start = uint32_t(texture_heap_entries_.size());
  range_out.sampler_start = uint32_t(sampler_heap_entries_.size());
  if (!shader) {
    return true;
  }

  MetalTextureCache* texture_cache = command_processor_.texture_cache();
  for (const SpirvShader::TextureBinding& binding :
       shader->GetTextureBindingsAfterTranslation()) {
    MTL::Texture* texture = texture_cache->GetTextureForBinding(
        binding.fetch_constant, binding.dimension, binding.is_signed != 0,
        /*cube_as_array=*/true);
    if (!texture) {
      switch (binding.dimension) {
        case xenos::FetchOpDimension::k3DOrStacked:
          texture = texture_cache->GetNullTexture3D();
          break;
        case xenos::FetchOpDimension::kCube:
          texture = texture_cache->GetNullTextureCube(/*as_array=*/true);
          break;
        default:
          texture = texture_cache->GetNullTexture2D();
          break;
      }
    }
    if (!texture) {
      XELOGE("MetalArgumentBinder: no texture for fetch constant {}",
             uint32_t(binding.fetch_constant));
      return false;
    }
    DescriptorEntry entry = {};
    entry.texture_view_id = texture->gpuResourceID()._impl;
    texture_heap_entries_.push_back(entry);
    resident_textures_.push_back(texture);
  }
  range_out.texture_count =
      uint32_t(texture_heap_entries_.size()) - range_out.texture_start;

  for (const SpirvShader::SamplerBinding& binding :
       shader->GetSamplerBindingsAfterTranslation()) {
    MTL::SamplerState* sampler = texture_cache->GetOrCreateSampler(
        texture_cache->GetSamplerParameters(binding));
    if (!sampler) {
      sampler = command_processor_.null_sampler();
    }
    if (!sampler) {
      XELOGE("MetalArgumentBinder: no sampler for fetch constant {}",
             uint32_t(binding.fetch_constant));
      return false;
    }
    DescriptorEntry entry = {};
    entry.gpu_va = sampler->gpuResourceID()._impl;
    sampler_heap_entries_.push_back(entry);
  }
  range_out.sampler_count =
      uint32_t(sampler_heap_entries_.size()) - range_out.sampler_start;
  return true;
}

bool MetalArgumentBinder::Bind(MTL::RenderCommandEncoder* encoder,
                               const SpirvShader* vertex_shader,
                               const SpirvShader* pixel_shader,
                               const Constants& constants, bool memexport_used,
                               uint32_t vertex_buffer_mask,
                               uint32_t pixel_buffer_mask) {
  SCOPE_profile_cpu_i("gpu", "MetalArgumentBinder::Bind");
  if (!encoder) {
    return false;
  }
  MetalTextureCache* texture_cache = command_processor_.texture_cache();
  MetalSharedMemory* shared_memory = command_processor_.shared_memory();
  MTL::Buffer* shared_memory_buffer =
      shared_memory ? shared_memory->GetBuffer() : nullptr;
  MTL::Buffer* runtime_data_buffer = command_processor_.null_buffer();
  if (!texture_cache || !shared_memory_buffer || !runtime_data_buffer) {
    return false;
  }

  const SpirvShader* stage_shaders[kStageCount] = {};
  stage_shaders[kStageVertex] = vertex_shader;
  stage_shaders[kStagePixel] = pixel_shader;
  StageRange stage_ranges[kStageCount];
  const GatherKey gather_key = {
      vertex_shader,
      pixel_shader,
      texture_cache->binding_state_generation(),
  };
  // The generation doesn't change for changes of the fetch constant words, so
  // the words the shaders use are compared here.
  const uint32_t* fetch_words = texture_cache->texture_fetch_constant_words();
  auto gathered_fetch_words_equal = [&]() {
    uint32_t fetch_mask = gather_cache_fetch_mask_;
    uint32_t fetch_index;
    while (xe::bit_scan_forward(fetch_mask, &fetch_index)) {
      fetch_mask = xe::clear_lowest_bit(fetch_mask);
      if (std::memcmp(fetch_words + fetch_index * 6,
                      gather_cache_fetch_words_.data() + fetch_index * 6,
                      6 * sizeof(uint32_t))) {
        return false;
      }
    }
    return true;
  };
  if (gather_cache_valid_ && gather_key == gather_cache_key_ &&
      gathered_fetch_words_equal()) {
    std::copy(gather_cache_ranges_.begin(), gather_cache_ranges_.end(),
              stage_ranges);
  } else {
    gather_cache_valid_ = false;
    texture_heap_entries_.clear();
    sampler_heap_entries_.clear();
    resident_textures_.clear();
    for (uint32_t stage = 0; stage < kStageCount; ++stage) {
      if (!GatherStage(stage_shaders[stage], stage_ranges[stage])) {
        return false;
      }
    }
    gather_cache_key_ = gather_key;
    std::copy(stage_ranges, stage_ranges + kStageCount,
              gather_cache_ranges_.begin());
    uint32_t fetch_mask = 0;
    for (const SpirvShader* shader : stage_shaders) {
      if (!shader) {
        continue;
      }
      for (const SpirvShader::TextureBinding& binding :
           shader->GetTextureBindingsAfterTranslation()) {
        fetch_mask |= UINT32_C(1) << binding.fetch_constant;
      }
      for (const SpirvShader::SamplerBinding& binding :
           shader->GetSamplerBindingsAfterTranslation()) {
        fetch_mask |= UINT32_C(1) << binding.fetch_constant;
      }
    }
    gather_cache_fetch_mask_ = fetch_mask;
    uint32_t fetch_index;
    while (xe::bit_scan_forward(fetch_mask, &fetch_index)) {
      fetch_mask = xe::clear_lowest_bit(fetch_mask);
      std::memcpy(gather_cache_fetch_words_.data() + fetch_index * 6,
                  fetch_words + fetch_index * 6, 6 * sizeof(uint32_t));
    }
    gather_cache_valid_ = true;
  }

  Slice texture_heap, sampler_heap;
  auto entries_equal = [](const std::vector<DescriptorEntry>& a,
                          const std::vector<DescriptorEntry>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(),
                                     a.size() * sizeof(DescriptorEntry)) == 0);
  };
  if (descriptor_heap_slices_valid_ &&
      entries_equal(texture_heap_entries_, cached_texture_heap_entries_) &&
      entries_equal(sampler_heap_entries_, cached_sampler_heap_entries_)) {
    texture_heap = cached_texture_heap_slice_;
    sampler_heap = cached_sampler_heap_slice_;
  } else {
    if (!Upload(
            texture_heap_entries_.data(),
            uint32_t(texture_heap_entries_.size() * sizeof(DescriptorEntry)),
            texture_heap) ||
        !Upload(
            sampler_heap_entries_.data(),
            uint32_t(sampler_heap_entries_.size() * sizeof(DescriptorEntry)),
            sampler_heap)) {
      XELOGE("MetalArgumentBinder: failed to allocate the descriptor heaps");
      return false;
    }
    cached_texture_heap_entries_ = texture_heap_entries_;
    cached_sampler_heap_entries_ = sampler_heap_entries_;
    cached_texture_heap_slice_ = texture_heap;
    cached_sampler_heap_slice_ = sampler_heap;
    descriptor_heap_slices_valid_ = true;
  }

  // The layout the Mesa bindless lowering reads: one two-uint32 entry per
  // SPIR-V binding, textures then samplers, holding the heap index in the first
  // uint32 for a texture and in the second for a sampler.
  Slice stage_index_buffers[kStageCount];
  for (uint32_t stage = 0; stage < kStageCount; ++stage) {
    const StageRange& range = stage_ranges[stage];
    size_t entry_count =
        std::max(size_t(range.texture_count + range.sampler_count), size_t(1));
    const uint32_t byte_count = uint32_t(entry_count * 2 * sizeof(uint32_t));
    const StageIndexKey key = {range.texture_start, range.texture_count,
                               range.sampler_start, range.sampler_count};
    auto upload_indices = [&](Slice& out) {
      stage_indices_.assign(entry_count * 2, 0);
      for (uint32_t i = 0; i < range.texture_count; ++i) {
        stage_indices_[size_t(i) * 2] = range.texture_start + i;
      }
      for (uint32_t j = 0; j < range.sampler_count; ++j) {
        stage_indices_[(size_t(range.texture_count) + j) * 2 + 1] =
            range.sampler_start + j;
      }
      return Upload(stage_indices_.data(), byte_count, out);
    };
    if (!stage_index_slices_[stage].GetOrUpload(key, upload_indices,
                                                stage_index_buffers[stage])) {
      XELOGE("MetalArgumentBinder: failed to allocate a texture index buffer");
      return false;
    }
  }

  const struct {
    ArgumentSlot slot;
    const ConstantBlock& block;
  } constant_bindings[kConstantCount] = {
      {ArgumentSlot::kSystemConstants, constants.system},
      {ArgumentSlot::kFloatConstantsVertex, constants.float_vertex},
      {ArgumentSlot::kFloatConstantsPixel, constants.float_pixel},
      {ArgumentSlot::kBoolLoopConstants, constants.bool_loop},
      {ArgumentSlot::kFetchConstants, constants.fetch},
  };
  Slice constant_slices[kConstantCount];
  for (size_t i = 0; i < kConstantCount; ++i) {
    const ConstantBlock& block = constant_bindings[i].block;
    auto upload = [&](Slice& out) {
      return Upload(block.data, block.size, out);
    };
    if (!constant_slices_[i].GetOrUpload({block.size, block.revision}, upload,
                                         constant_slices[i])) {
      XELOGE("MetalArgumentBinder: failed to allocate a constant buffer");
      return false;
    }
  }

  Slice argument_buffer;
  auto slice_address = [](const Slice& slice) -> uint64_t {
    return slice.gpu_address;
  };
  ArgumentBufferKey argument_key{};
  auto set_argument = [&](ArgumentSlot slot, uint64_t address) {
    argument_key[size_t(slot)] = address;
  };
  for (size_t i = 0; i < kConstantCount; ++i) {
    set_argument(constant_bindings[i].slot, slice_address(constant_slices[i]));
  }
  set_argument(ArgumentSlot::kRuntimeData,
               uint64_t(runtime_data_buffer->gpuAddress()));
  uint64_t shared_memory_address = uint64_t(shared_memory_buffer->gpuAddress());
  set_argument(ArgumentSlot::kSharedMemorySrv, shared_memory_address);
  set_argument(ArgumentSlot::kSharedMemoryUav, shared_memory_address);
  set_argument(ArgumentSlot::kTextureIndicesVertex,
               slice_address(stage_index_buffers[kStageVertex]));
  set_argument(ArgumentSlot::kTextureIndicesPixel,
               slice_address(stage_index_buffers[kStagePixel]));
  // The lowered shader indexes the heaps directly and never dereferences these
  // tables, but their slots still have to be valid for MSC fallback shaders.
  uint64_t texture_heap_address = slice_address(texture_heap);
  uint64_t sampler_heap_address = slice_address(sampler_heap);
  set_argument(ArgumentSlot::kTextureRangeVertex, texture_heap_address);
  set_argument(ArgumentSlot::kTextureRangePixel, texture_heap_address);
  set_argument(ArgumentSlot::kSamplerRangeVertex, sampler_heap_address);
  set_argument(ArgumentSlot::kSamplerRangePixel, sampler_heap_address);
  auto upload_argument_buffer = [&](Slice& out) {
    return Upload(argument_key.data(), uint32_t(sizeof(argument_key)), out);
  };
  if (!argument_buffer_slices_.GetOrUpload(argument_key, upload_argument_buffer,
                                           argument_buffer)) {
    XELOGE("MetalArgumentBinder: failed to allocate the argument buffer");
    return false;
  }
  using Stage = MetalCommandProcessor::RenderEncoderStage;
  auto bind = [&](Stage stage, const Slice& slice, uint32_t bind_point) {
    command_processor_.SetRenderEncoderBuffer(stage, slice.buffer, slice.offset,
                                              NS::UInteger(bind_point));
  };
  auto bind_all_stages = [&](const Slice& slice, uint32_t bind_point) {
    // Only to a stage whose function can read the bind point.
    if ((vertex_buffer_mask >> bind_point) & 1) {
      bind(Stage::kVertex, slice, bind_point);
    }
    if ((pixel_buffer_mask >> bind_point) & 1) {
      bind(Stage::kFragment, slice, bind_point);
    }
  };
  bind_all_stages(texture_heap, kDescriptorHeapBufferIndex);
  bind_all_stages(sampler_heap, kSamplerHeapBufferIndex);
  bind_all_stages(argument_buffer, kArgumentBufferIndex);
  // Binding covers only the three buffers above; everything the shaders reach
  // through a GPU address has to be made resident explicitly.
  auto use_slice = [&](const Slice& slice) {
    command_processor_.UseRenderEncoderResource(slice.buffer,
                                                MTL::ResourceUsageRead);
  };
  use_slice(texture_heap);
  use_slice(sampler_heap);
  use_slice(argument_buffer);
  for (const Slice& slice : constant_slices) {
    use_slice(slice);
  }
  for (const Slice& slice : stage_index_buffers) {
    use_slice(slice);
  }
  command_processor_.UseRenderEncoderResource(runtime_data_buffer,
                                              MTL::ResourceUsageRead);
  MTL::ResourceUsage shared_memory_usage = MTL::ResourceUsageRead;
  if (memexport_used) {
    shared_memory_usage |= MTL::ResourceUsageWrite;
  }
  command_processor_.UseRenderEncoderResource(shared_memory_buffer,
                                              shared_memory_usage);
  for (MTL::Texture* texture : resident_textures_) {
    command_processor_.UseRenderEncoderResource(texture,
                                                MTL::ResourceUsageRead);
  }
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
