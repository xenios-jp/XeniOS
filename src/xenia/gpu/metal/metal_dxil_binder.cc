/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_dxil_binder.h"

#include <algorithm>
#include <cstring>

#include "metal_irconverter_runtime.h"

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_shader_converter.h"
#include "xenia/gpu/metal/metal_shared_memory.h"
#include "xenia/gpu/metal/metal_texture_cache.h"

namespace xe {
namespace gpu {
namespace metal {

MetalDxilBinder::MetalDxilBinder(MetalCommandProcessor& command_processor,
                                 const MetalShaderConverter& converter)
    : command_processor_(command_processor), converter_(converter) {}

MetalDxilBinder::~MetalDxilBinder() = default;

bool MetalDxilBinder::Upload(const void* data, uint32_t size,
                             Slice& slice_out) {
  uint32_t bytes = std::max(size, uint32_t(1));
  if (!command_processor_.AcquireSpirvArgumentBufferSlice(
          bytes, kSliceAlignment, &slice_out.buffer, &slice_out.offset)) {
    return false;
  }
  uint8_t* mapping =
      static_cast<uint8_t*>(slice_out.buffer->contents()) + slice_out.offset;
  if (data && size) {
    std::memcpy(mapping, data, size);
  } else {
    std::memset(mapping, 0, bytes);
  }
  return true;
}

bool MetalDxilBinder::GatherStage(const SpirvShader* shader,
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
      XELOGE("MetalDxilBinder: no texture for fetch constant {}",
             uint32_t(binding.fetch_constant));
      return false;
    }
    IRDescriptorTableEntry entry = {};
    IRDescriptorTableSetTexture(&entry, texture, 0.0f, 0);
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
      XELOGE("MetalDxilBinder: no sampler for fetch constant {}",
             uint32_t(binding.fetch_constant));
      return false;
    }
    IRDescriptorTableEntry entry = {};
    IRDescriptorTableSetSampler(&entry, sampler, 0.0f);
    sampler_heap_entries_.push_back(entry);
  }
  range_out.sampler_count =
      uint32_t(sampler_heap_entries_.size()) - range_out.sampler_start;
  return true;
}

bool MetalDxilBinder::Bind(MTL::RenderCommandEncoder* encoder,
                           const SpirvShader* vertex_shader,
                           const SpirvShader* pixel_shader,
                           const Constants& constants, bool memexport_used,
                           bool tessellated) {
  SCOPE_profile_cpu_i("gpu", "MetalDxilBinder::Bind");
  if (!encoder || !converter_.is_available()) {
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

  texture_heap_entries_.clear();
  sampler_heap_entries_.clear();
  resident_textures_.clear();

  const SpirvShader* stage_shaders[kStageCount] = {};
  stage_shaders[kStageVertex] = vertex_shader;
  stage_shaders[kStagePixel] = pixel_shader;
  StageRange stage_ranges[kStageCount];
  for (uint32_t stage = 0; stage < kStageCount; ++stage) {
    if (!GatherStage(stage_shaders[stage], stage_ranges[stage])) {
      return false;
    }
  }

  Slice texture_heap, sampler_heap;
  if (!Upload(texture_heap_entries_.data(),
              uint32_t(texture_heap_entries_.size() *
                       sizeof(IRDescriptorTableEntry)),
              texture_heap) ||
      !Upload(sampler_heap_entries_.data(),
              uint32_t(sampler_heap_entries_.size() *
                       sizeof(IRDescriptorTableEntry)),
              sampler_heap)) {
    XELOGE("MetalDxilBinder: failed to allocate the descriptor heaps");
    return false;
  }

  // The layout the Mesa bindless lowering reads: one two-uint32 entry per
  // SPIR-V binding, textures then samplers, holding the heap index in the first
  // uint32 for a texture and in the second for a sampler.
  Slice stage_index_buffers[kStageCount];
  for (uint32_t stage = 0; stage < kStageCount; ++stage) {
    const StageRange& range = stage_ranges[stage];
    size_t entry_count =
        std::max(size_t(range.texture_count + range.sampler_count), size_t(1));
    stage_indices_.assign(entry_count * 2, 0);
    for (uint32_t i = 0; i < range.texture_count; ++i) {
      stage_indices_[size_t(i) * 2] = range.texture_start + i;
    }
    for (uint32_t j = 0; j < range.sampler_count; ++j) {
      stage_indices_[(size_t(range.texture_count) + j) * 2 + 1] =
          range.sampler_start + j;
    }
    if (!Upload(stage_indices_.data(),
                uint32_t(stage_indices_.size() * sizeof(uint32_t)),
                stage_index_buffers[stage])) {
      XELOGE("MetalDxilBinder: failed to allocate a texture index buffer");
      return false;
    }
  }

  constexpr size_t kConstantCount = 5;
  const struct {
    MetalRootParameter parameter;
    const ConstantBlock& block;
  } constant_bindings[kConstantCount] = {
      {MetalRootParameter::kSystemConstants, constants.system},
      {MetalRootParameter::kFloatConstantsVertex, constants.float_vertex},
      {MetalRootParameter::kFloatConstantsPixel, constants.float_pixel},
      {MetalRootParameter::kBoolLoopConstants, constants.bool_loop},
      {MetalRootParameter::kFetchConstants, constants.fetch},
  };
  Slice constant_slices[kConstantCount];
  for (size_t i = 0; i < kConstantCount; ++i) {
    if (!Upload(constant_bindings[i].block.data,
                constant_bindings[i].block.size, constant_slices[i])) {
      XELOGE("MetalDxilBinder: failed to allocate a constant buffer");
      return false;
    }
  }

  Slice argument_buffer;
  uint32_t argument_buffer_size = converter_.argument_buffer_size();
  if (!Upload(nullptr, argument_buffer_size, argument_buffer)) {
    XELOGE("MetalDxilBinder: failed to allocate the argument buffer");
    return false;
  }
  uint8_t* argument_buffer_mapping =
      static_cast<uint8_t*>(argument_buffer.buffer->contents()) +
      argument_buffer.offset;
  auto slice_address = [](const Slice& slice) -> uint64_t {
    return uint64_t(slice.buffer->gpuAddress()) + uint64_t(slice.offset);
  };
  // Initialize resolved every offset and checked it against the layout the
  // shaders were compiled against, so each one is known to fit here.
  auto write_root_parameter = [&](MetalRootParameter parameter,
                                  uint64_t address) {
    uint32_t offset = converter_.root_parameter_offset(parameter);
    assert_true(offset != UINT32_MAX &&
                offset + sizeof(address) <= argument_buffer_size);
    std::memcpy(argument_buffer_mapping + offset, &address, sizeof(address));
  };
  for (size_t i = 0; i < kConstantCount; ++i) {
    write_root_parameter(constant_bindings[i].parameter,
                         slice_address(constant_slices[i]));
  }
  write_root_parameter(MetalRootParameter::kRuntimeData,
                       uint64_t(runtime_data_buffer->gpuAddress()));
  uint64_t shared_memory_address = uint64_t(shared_memory_buffer->gpuAddress());
  write_root_parameter(MetalRootParameter::kSharedMemorySrv,
                       shared_memory_address);
  write_root_parameter(MetalRootParameter::kSharedMemoryUav,
                       shared_memory_address);
  write_root_parameter(MetalRootParameter::kTextureIndicesVertex,
                       slice_address(stage_index_buffers[kStageVertex]));
  write_root_parameter(MetalRootParameter::kTextureIndicesPixel,
                       slice_address(stage_index_buffers[kStagePixel]));
  // The lowered shader indexes the heaps directly and never dereferences these
  // tables, but their slots still have to be valid.
  uint64_t texture_heap_address = slice_address(texture_heap);
  uint64_t sampler_heap_address = slice_address(sampler_heap);
  write_root_parameter(MetalRootParameter::kTextureRangeVertex,
                       texture_heap_address);
  write_root_parameter(MetalRootParameter::kTextureRangePixel,
                       texture_heap_address);
  write_root_parameter(MetalRootParameter::kSamplerRangeVertex,
                       sampler_heap_address);
  write_root_parameter(MetalRootParameter::kSamplerRangePixel,
                       sampler_heap_address);

  // Tessellation emulation runs the hull stage as an object shader and the
  // tessellator as a mesh shader, so the pre-rasterization bindings go to those
  // stages instead of the vertex stage.
  auto bind_all_stages = [&](const Slice& slice, uint64_t bind_point) {
    if (tessellated) {
      encoder->setObjectBuffer(slice.buffer, slice.offset,
                               NS::UInteger(bind_point));
      encoder->setMeshBuffer(slice.buffer, slice.offset,
                             NS::UInteger(bind_point));
    } else {
      encoder->setVertexBuffer(slice.buffer, slice.offset,
                               NS::UInteger(bind_point));
    }
    encoder->setFragmentBuffer(slice.buffer, slice.offset,
                               NS::UInteger(bind_point));
  };
  bind_all_stages(texture_heap, kIRDescriptorHeapBindPoint);
  bind_all_stages(sampler_heap, kIRSamplerHeapBindPoint);
  bind_all_stages(argument_buffer, kIRArgumentBufferBindPoint);
  if (tessellated) {
    // The hull and domain stages read the same root parameters from their own
    // bind point.
    encoder->setObjectBuffer(
        argument_buffer.buffer, argument_buffer.offset,
        NS::UInteger(kIRArgumentBufferHullDomainBindPoint));
    encoder->setMeshBuffer(argument_buffer.buffer, argument_buffer.offset,
                           NS::UInteger(kIRArgumentBufferHullDomainBindPoint));
  }

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
