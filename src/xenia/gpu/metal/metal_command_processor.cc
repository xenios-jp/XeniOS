/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_command_processor.h"

#include <dispatch/dispatch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <objc/message.h>
#include <objc/runtime.h>

#include "third_party/metal-cpp/Foundation/NSURL.hpp"
#include "third_party/metal-cpp/Metal/MTLEvent.hpp"
#include "third_party/metal-cpp/Metal/MTLResidencySet.hpp"

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/metal/metal_backend_telemetry.h"
#include "xenia/gpu/metal/metal_graphics_system.h"
#include "xenia/gpu/metal/native_msl_bindings.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/ui/metal/metal_presenter.h"

// Metal IR Converter Runtime - defines IRDescriptorTableEntry and bind points.
#ifndef IR_RUNTIME_METALCPP
#define IR_RUNTIME_METALCPP
#endif
#include "third_party/metal-shader-converter/include/metal_irconverter_runtime.h"

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

DECLARE_bool(async_shader_compilation);
DECLARE_bool(clear_memory_page_state);
DECLARE_bool(metal_native_msl_helper_msc);
DECLARE_bool(metal_native_msl_render);
DECLARE_bool(submit_on_primary_buffer_end);

DEFINE_bool(
    metal_float_constants_dirty_on_change, true,
    "Only invalidate Metal float constant CBVs when a register write changes a "
    "currently live float constant value. Disable to restore conservative "
    "dirty-on-write behavior.",
    "Metal");

namespace xe {
namespace gpu {
namespace metal {

namespace {
constexpr size_t kMaxPendingSharedMemoryWrites = 16;
constexpr size_t kMaxPendingSharedMemoryWriteCapacity = 64;
constexpr size_t kMaxSharedMemoryWaitSegmentsPerPending = 64;
constexpr size_t kMaxCurrentDrawVertexFetchRanges =
    xenos::kVertexFetchConstantCount;
constexpr uint32_t kNativeMslDrawConstantsChangeSystem = 1u << 0;
constexpr uint32_t kNativeMslDrawConstantsChangeFloat = 1u << 1;
constexpr uint32_t kNativeMslDrawConstantsChangeBoolLoop = 1u << 2;
constexpr uint32_t kNativeMslDrawConstantsChangeFetch = 1u << 3;
constexpr uint32_t kNativeMslDrawConstantsChangeDescriptorIndices = 1u << 4;
constexpr uint32_t kNativeMslDrawConstantsChangePrimitiveIndex = 1u << 5;
// Keep helper-stage MSC translations separate from native-MSL standard
// VS/PS translations. The bit is outside the semantic DxbcShaderTranslator
// modification fields and only changes the Translation cache key.
constexpr uint64_t kMetalHelperStageMscTranslationKeyBit = 1ull << 63;
const char* MetalTelemetryCbvSlotName(size_t slot) {
  switch (slot) {
    case 0:
      return "system";
    case 1:
      return "float";
    case 2:
      return "bool_loop";
    case 3:
      return "fetch";
    case 4:
      return "descriptor_indices";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryShaderStageName(size_t stage) {
  switch (stage) {
    case 0:
      return "vertex";
    case 1:
      return "pixel";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootScopeName(size_t stage) {
  switch (stage) {
    case 0:
      return "graphics";
    case 1:
      return "unused";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRenderResourceSetName(size_t set) {
  switch (set) {
    case 0:
      return "fixed";
    case 1:
      return "texture";
    case 2:
      return "root";
    default:
      return "invalid";
  }
}

MTL::RenderStages MetalAllGraphicsRenderStages() {
  return MTL::RenderStages(MTL::RenderStageVertex | MTL::RenderStageFragment |
                           MTL::RenderStageObject | MTL::RenderStageMesh);
}

template <typename Binding>
size_t MetalNativeBindingLayoutHash(const std::vector<Binding>& bindings,
                                    uint64_t empty_hash) {
  if (bindings.empty()) {
    return static_cast<size_t>(empty_hash);
  }
  uint64_t hash =
      XXH3_64bits(bindings.data(), bindings.size() * sizeof(Binding));
  hash ^= uint64_t(bindings.size()) * 0x9E3779B185EBCA87ull;
  hash ^= empty_hash;
  return static_cast<size_t>(hash ? hash : empty_hash);
}

uint32_t MetalNativeDescriptorIndicesWordCount(
    const DxbcShader::TranslationMetadata& metadata) {
  uint32_t word_count = 1;
  for (const DxbcShader::TextureBinding& binding : metadata.texture_bindings) {
    word_count =
        std::max(word_count, binding.bindless_descriptor_index + uint32_t(1));
  }
  for (const DxbcShader::SamplerBinding& binding : metadata.sampler_bindings) {
    word_count =
        std::max(word_count, binding.bindless_descriptor_index + uint32_t(1));
  }
  return word_count;
}

struct NativeMslTextureSignVariant {
  uint64_t key = 0;
  DxbcShader::TextureSignComponentMasks component_masks = {};
  DxbcShader::TextureSignComponentMasks sign_values = {};
};

uint8_t MaskNativeMslTextureSigns(uint8_t signs, uint8_t component_mask) {
  uint8_t masked = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (component_mask & (uint8_t(1) << i)) {
      masked |= signs & (uint8_t(0x3) << (i * 2));
    }
  }
  return masked;
}

uint64_t NativeMslTextureSignInputKey(const RegisterFile& regs,
                                      const Shader& shader) {
  uint64_t key = UINT64_C(0x4E4D534C53696E70);  // "NMSLSinp"
  bool has_sign_inputs = false;
  auto mix_key = [](uint64_t hash, uint64_t value) {
    hash ^= value + UINT64_C(0x9E3779B185EBCA87) + (hash << 6) + (hash >> 2);
    return hash;
  };
  for (const Shader::TextureBinding& shader_binding :
       shader.texture_bindings()) {
    const ParsedTextureFetchInstruction& fetch = shader_binding.fetch_instr;
    if (fetch.opcode != ucode::FetchOpcode::kTextureFetch ||
        shader_binding.fetch_constant >= xenos::kTextureFetchConstantCount) {
      continue;
    }
    uint8_t component_mask = uint8_t(fetch.result.GetUsedResultComponents() &
                                     fetch.GetNonZeroResultComponents());
    if (!component_mask) {
      continue;
    }
    const uint32_t fetch_constant = shader_binding.fetch_constant;
    const uint8_t signs =
        texture_util::SwizzleSigns(regs.GetTextureFetch(fetch_constant));
    const uint8_t masked_signs =
        MaskNativeMslTextureSigns(signs, component_mask);
    key = mix_key(key, fetch_constant);
    key = mix_key(key, component_mask);
    key = mix_key(key, masked_signs);
    has_sign_inputs = true;
  }
  if (!has_sign_inputs) {
    return 0;
  }
  return key ? key : 1;
}

NativeMslTextureSignVariant BuildNativeMslTextureSignVariant(
    const RegisterFile& regs, const Shader& shader) {
  NativeMslTextureSignVariant variant = {};
  bool has_sign_inputs = false;
  for (const Shader::TextureBinding& shader_binding :
       shader.texture_bindings()) {
    const ParsedTextureFetchInstruction& fetch = shader_binding.fetch_instr;
    if (fetch.opcode != ucode::FetchOpcode::kTextureFetch ||
        shader_binding.fetch_constant >= xenos::kTextureFetchConstantCount) {
      continue;
    }
    uint8_t component_mask = uint8_t(fetch.result.GetUsedResultComponents() &
                                     fetch.GetNonZeroResultComponents());
    if (!component_mask) {
      continue;
    }
    const uint32_t fetch_constant = shader_binding.fetch_constant;
    const uint8_t signs =
        texture_util::SwizzleSigns(regs.GetTextureFetch(fetch_constant));
    variant.component_masks[fetch_constant] |= component_mask;
    variant.sign_values[fetch_constant] |=
        MaskNativeMslTextureSigns(signs, component_mask);
    has_sign_inputs = true;
  }
  if (!has_sign_inputs) {
    return variant;
  }
  XXH3_state_t hash_state;
  XXH3_64bits_reset(&hash_state);
  XXH3_64bits_update(&hash_state, variant.component_masks.data(),
                     variant.component_masks.size());
  XXH3_64bits_update(&hash_state, variant.sign_values.data(),
                     variant.sign_values.size());
  variant.key = XXH3_64bits_digest(&hash_state);
  if (!variant.key) {
    variant.key = 1;
  }
  return variant;
}

uint64_t NativeMslTranslationCacheKey(uint64_t shader_modification,
                                      uint64_t texture_sign_key) {
  if (!texture_sign_key) {
    return shader_modification;
  }
  uint64_t key_data[] = {
      shader_modification,
      texture_sign_key,
      UINT64_C(0x4E4D534C53676E31),  // "NMSLSgn1"
  };
  uint64_t key = XXH3_64bits(key_data, sizeof(key_data));
  return key ? key : UINT64_C(0x4E4D534C53676E31);
}

uint32_t MetalRenderStageBits(MTL::RenderStages stages) {
  return uint32_t(NS::UInteger(stages));
}

uint32_t MetalResourceUsageBits(MTL::ResourceUsage usage) {
  return uint32_t(NS::UInteger(usage));
}

bool MetalObjectRespondsToSelector(const void* object,
                                   const char* selector_name) {
  if (!object || !selector_name) {
    return false;
  }
  SEL selector = sel_registerName(selector_name);
  SEL responds_to_selector = sel_registerName("respondsToSelector:");
  using RespondsToSelectorFn = BOOL (*)(id, SEL, SEL);
  return reinterpret_cast<RespondsToSelectorFn>(objc_msgSend)(
             reinterpret_cast<id>(const_cast<void*>(object)),
             responds_to_selector, selector) != 0;
}

bool MetalResidencySetCvarAllowsEnable(bool supported) {
  const std::string& mode = cvars::metal_residency_sets;
  if (mode == "0" || mode == "false" || mode == "off" || mode == "disabled") {
    return false;
  }
  if (mode == "1" || mode == "true" || mode == "on" || mode == "enabled") {
    return true;
  }
  return supported;
}

MTL::Texture* CreateNativeNullTexture(MTL::Device* device,
                                      MTL::TextureType texture_type,
                                      const char* label) {
  if (!device) {
    return nullptr;
  }
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setTextureType(texture_type);
  desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  desc->setWidth(1);
  desc->setHeight(1);
  if (texture_type == MTL::TextureType3D) {
    desc->setDepth(1);
  }
  desc->setStorageMode(MTL::StorageModeShared);
  desc->setUsage(MTL::TextureUsageShaderRead);

  MTL::Texture* texture = device->newTexture(desc);
  desc->release();
  if (texture && label) {
    texture->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
  }
  return texture;
}

const char* MetalTelemetryRootRebuildReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "frame_open";
    case 1:
      return "descriptor_indices_pointer_tuple";
    case 2:
      return "other_cbv_pointer_tuple";
    case 3:
      return "shared_memory_uav_mode";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootSlotsChangedName(size_t bin) {
  switch (bin) {
    case 0:
      return "0";
    case 1:
      return "1";
    case 2:
      return "2";
    case 3:
      return "3";
    case 4:
      return "4";
    case 5:
      return "5_plus";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootRebuildDetailName(size_t detail) {
  switch (detail) {
    case 0:
      return "same_buffer_offset_changed";
    case 1:
      return "different_buffer";
    case 2:
      return "descriptor_indices_only";
    case 3:
      return "other_cbv_only";
    case 4:
      return "mixed_descriptor_and_other";
    case 5:
      return "resource_identity_changed";
    case 6:
      return "resource_identity_same";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryDrawMaterializationSourceName(size_t source) {
  switch (source) {
    case 0:
      return "vertex_fetch";
    case 1:
      return "guest_index";
    case 2:
      return "memexport";
    case 3:
      return "texture_source";
    default:
      return "invalid";
  }
}

const char* MetalPreparedDrawFlushReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "manual";
    case 1:
      return "rt_update";
    case 2:
      return "rt_key_mismatch";
    case 3:
      return "queue_budget";
    case 4:
      return "queue_reject";
    case 5:
      return "prepare_wait";
    case 6:
      return "swap";
    case 7:
      return "copy";
    case 8:
      return "transfer_request";
    case 9:
      return "render_encoder_end";
    case 10:
      return "command_buffer_end";
    case 11:
      return "query";
    default:
      return "invalid";
  }
}

const char* MetalPreparedDrawQueueRejectReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "none";
    case 1:
      return "resident_no_active_queue";
    case 2:
      return "no_shared_memory_ranges";
    case 3:
      return "memexport";
    case 4:
      return "texture_upload";
    case 5:
      return "texture_request_load_data";
    case 6:
      return "pending_draw_pass_transfers";
    case 7:
      return "zpd_active";
    case 8:
      return "rt_key_mismatch";
    case 9:
      return "native_msl_direct_resources";
    case 10:
      return "queue_budget";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootArgSlotName(size_t slot) {
  switch (slot) {
    case 0:
      return "srv0";
    case 1:
      return "srv1";
    case 2:
      return "srv2";
    case 3:
      return "srv3";
    case 4:
      return "srv10";
    case 5:
      return "uav0";
    case 6:
      return "uav1";
    case 7:
      return "uav2";
    case 8:
      return "uav3";
    case 9:
      return "sampler0";
    case 10:
      return "cbv_system";
    case 11:
      return "cbv_float";
    case 12:
      return "cbv_bool_loop";
    case 13:
      return "cbv_fetch";
    case 14:
      return "cbv_descriptor_indices";
    case 15:
      return "cbv_hull_float";
    case 16:
      return "cbv_hull_fetch";
    case 17:
      return "cbv_hull_descriptor_indices";
    case 18:
      return "cbv_domain_float";
    case 19:
      return "cbv_domain_fetch";
    case 20:
      return "cbv_domain_descriptor_indices";
    case 21:
      return "cbv_pixel_float";
    case 22:
      return "cbv_pixel_fetch";
    case 23:
      return "cbv_pixel_descriptor_indices";
    default:
      return "unused";
  }
}

const char* MetalTelemetryRenderEncoderBufferStageName(size_t stage) {
  switch (stage) {
    case 0:
      return "vertex";
    case 1:
      return "fragment";
    case 2:
      return "object";
    case 3:
      return "mesh";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryNativeMslDrawConstantsRebuildReasonName(
    size_t stage_reason) {
  constexpr size_t kReasonCount = 10;
  const size_t stage = stage_reason / kReasonCount;
  const size_t reason = stage_reason % kReasonCount;
  const char* stage_name = MetalTelemetryRenderEncoderBufferStageName(stage);
  const char* reason_name = nullptr;
  switch (reason) {
    case 0:
      reason_name = "reuse";
      break;
    case 1:
      reason_name = "initial";
      break;
    case 2:
      reason_name = "frame_open";
      break;
    case 3:
      reason_name = "system";
      break;
    case 4:
      reason_name = "float";
      break;
    case 5:
      reason_name = "bool_loop";
      break;
    case 6:
      reason_name = "fetch";
      break;
    case 7:
      reason_name = "descriptor_indices";
      break;
    case 8:
      reason_name = "primitive_index";
      break;
    case 9:
      reason_name = "mixed";
      break;
    default:
      reason_name = "invalid";
      break;
  }
  static thread_local std::string formatted;
  formatted = fmt::format("{}.{}", stage_name, reason_name);
  return formatted.c_str();
}

const char* MetalTelemetryNativeMslDrawConstantsChangeMaskName(
    size_t stage_mask) {
  constexpr size_t kMaskCount = 64;
  const size_t stage = stage_mask / kMaskCount;
  const uint32_t mask = uint32_t(stage_mask % kMaskCount);
  const char* stage_name = MetalTelemetryRenderEncoderBufferStageName(stage);
  static thread_local std::string formatted;
  std::string mask_name;
  auto append_field = [&](const char* name) {
    if (!mask_name.empty()) {
      mask_name += "+";
    }
    mask_name += name;
  };
  if (mask & kNativeMslDrawConstantsChangeSystem) {
    append_field("system");
  }
  if (mask & kNativeMslDrawConstantsChangeFloat) {
    append_field("float");
  }
  if (mask & kNativeMslDrawConstantsChangeBoolLoop) {
    append_field("bool_loop");
  }
  if (mask & kNativeMslDrawConstantsChangeFetch) {
    append_field("fetch");
  }
  if (mask & kNativeMslDrawConstantsChangeDescriptorIndices) {
    append_field("descriptor_indices");
  }
  if (mask & kNativeMslDrawConstantsChangePrimitiveIndex) {
    append_field("primitive_index");
  }
  if (mask_name.empty()) {
    mask_name = "none";
  }
  formatted = fmt::format("{}.{}", stage_name, mask_name);
  return formatted.c_str();
}

const char* MetalTelemetryRenderEncoderBufferSlotName(size_t stage_slot) {
  constexpr size_t kSlotCount = 32;
  const size_t stage = stage_slot / kSlotCount;
  const size_t slot = stage_slot % kSlotCount;
  const char* stage_name = MetalTelemetryRenderEncoderBufferStageName(stage);
  const char* slot_name = nullptr;
  switch (slot) {
    case kNativeBufferTexture2DArrayHeap:
      slot_name = "texture_2d_array_heap";
      break;
    case kNativeBufferTexture3DHeap:
      slot_name = "texture_3d_heap";
      break;
    case kNativeBufferTextureCubeHeap:
      slot_name = "texture_cube_heap";
      break;
    case kNativeBufferSamplerHeap:
      slot_name = "sampler_heap";
      break;
    case kNativeBufferSystemConstants:
      slot_name = "system";
      break;
    case kNativeBufferFloatConstants:
      slot_name = "float";
      break;
    case kNativeBufferBoolLoopConstants:
      slot_name = "bool_loop";
      break;
    case kNativeBufferFetchConstants:
      slot_name = "fetch";
      break;
    case kNativeBufferDescriptorIndices:
      slot_name = "descriptor_indices";
      break;
    case kNativeBufferSharedMemory:
      slot_name = "shared_memory";
      break;
    case kNativeBufferTextureRuntimeInfo:
      slot_name = "texture_runtime_info";
      break;
    case kNativeBufferMemExportDebug:
      slot_name = "memexport_debug";
      break;
    case kNativeBufferPrimitiveIndexConstants:
      slot_name = "primitive_index";
      break;
    case kNativeBufferDrawConstants:
      slot_name = "draw_constants";
      break;
    default:
      break;
  }
  static thread_local std::string formatted;
  formatted = slot_name ? fmt::format("{}.{}", stage_name, slot_name)
                        : fmt::format("{}.slot{}", stage_name, slot);
  return formatted.c_str();
}

template <size_t Count>
uint64_t MetalTelemetrySumCounts(const std::array<uint64_t, Count>& values) {
  uint64_t sum = 0;
  for (uint64_t value : values) {
    sum += value;
  }
  return sum;
}

struct SharedMemoryRangeSegment {
  uint32_t start;
  uint32_t end;
};

template <typename Visitor>
void ForEachSharedMemoryRangeSegment(
    const MetalCommandProcessor::SharedMemoryRange& range, Visitor&& visitor) {
  if (!range.length) {
    return;
  }
  constexpr uint32_t kSharedMemoryMask = SharedMemory::kBufferSize - 1;
  uint32_t remaining = std::min(range.length, SharedMemory::kBufferSize);
  uint32_t start = range.start & kSharedMemoryMask;
  while (remaining) {
    uint32_t segment_length =
        std::min(remaining, SharedMemory::kBufferSize - start);
    if (segment_length) {
      visitor(SharedMemoryRangeSegment{start, start + segment_length});
    }
    remaining -= segment_length;
    start = 0;
  }
}

bool SharedMemoryRangeOverlapsSegment(
    const MetalCommandProcessor::SharedMemoryRange& range,
    uint32_t segment_start, uint32_t segment_end) {
  if (segment_start >= segment_end) {
    return false;
  }
  bool overlaps = false;
  ForEachSharedMemoryRangeSegment(
      range, [&](SharedMemoryRangeSegment range_segment) {
        overlaps = overlaps || (segment_start < range_segment.end &&
                                segment_end > range_segment.start);
      });
  return overlaps;
}

bool GetTextureSize(MTL::Texture* texture, uint32_t& width_out,
                    uint32_t& height_out) {
  if (!texture) {
    return false;
  }
  width_out = std::max(static_cast<uint32_t>(texture->width()), uint32_t(1));
  height_out = std::max(static_cast<uint32_t>(texture->height()), uint32_t(1));
  return true;
}

bool GetRenderPassDescriptorSize(MTL::RenderPassDescriptor* pass_descriptor,
                                 uint32_t& width_out, uint32_t& height_out) {
  if (!pass_descriptor) {
    return false;
  }

  const uint32_t constrained_width =
      static_cast<uint32_t>(pass_descriptor->renderTargetWidth());
  const uint32_t constrained_height =
      static_cast<uint32_t>(pass_descriptor->renderTargetHeight());
  if (constrained_width && constrained_height) {
    width_out = std::max(constrained_width, uint32_t(1));
    height_out = std::max(constrained_height, uint32_t(1));
    return true;
  }

  if (auto* color_attachments = pass_descriptor->colorAttachments()) {
    for (uint32_t i = 0; i < 8; ++i) {
      auto* attachment = color_attachments->object(i);
      if (attachment &&
          GetTextureSize(attachment->texture(), width_out, height_out)) {
        return true;
      }
    }
  }
  if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
    if (GetTextureSize(depth_attachment->texture(), width_out, height_out)) {
      return true;
    }
  }
  if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
    if (GetTextureSize(stencil_attachment->texture(), width_out, height_out)) {
      return true;
    }
  }
  return false;
}

void GetBoundRenderTargetSize(const MetalRenderTargetCache* render_target_cache,
                              uint32_t fallback_width, uint32_t fallback_height,
                              uint32_t& width_out, uint32_t& height_out) {
  width_out = std::max(fallback_width, uint32_t(1));
  height_out = std::max(fallback_height, uint32_t(1));
  if (!render_target_cache) {
    return;
  }
  MTL::Texture* pass_size_texture = render_target_cache->GetColorTarget(0);
  if (!pass_size_texture) {
    pass_size_texture = render_target_cache->GetDepthTarget();
  }
  if (!pass_size_texture) {
    pass_size_texture = render_target_cache->GetDummyColorTarget();
  }
  if (!pass_size_texture) {
    return;
  }
  GetTextureSize(pass_size_texture, width_out, height_out);
}

void GetActiveRenderTargetSize(
    MTL::RenderPassDescriptor* pass_descriptor,
    const MetalRenderTargetCache* render_target_cache, uint32_t fallback_width,
    uint32_t fallback_height, uint32_t& width_out, uint32_t& height_out) {
  width_out = std::max(fallback_width, uint32_t(1));
  height_out = std::max(fallback_height, uint32_t(1));
  if (GetRenderPassDescriptorSize(pass_descriptor, width_out, height_out)) {
    return;
  }
  GetBoundRenderTargetSize(render_target_cache, fallback_width, fallback_height,
                           width_out, height_out);
}

void ClampScissorToBounds(draw_util::Scissor& scissor, uint32_t width,
                          uint32_t height) {
  width = std::max(width, uint32_t(1));
  height = std::max(height, uint32_t(1));

  scissor.offset[0] = std::min(scissor.offset[0], width);
  scissor.offset[1] = std::min(scissor.offset[1], height);

  uint32_t max_scissor_width = width - scissor.offset[0];
  uint32_t max_scissor_height = height - scissor.offset[1];
  scissor.extent[0] = std::min(scissor.extent[0], max_scissor_width);
  scissor.extent[1] = std::min(scissor.extent[1], max_scissor_height);
}

PipelineAttachmentFormats ResolvePipelineAttachmentFormats(
    const MetalRenderTargetCache* render_target_cache,
    MTL::RenderPassDescriptor* pass_descriptor, bool pixel_shader_writes_depth,
    const char* pipeline_name) {
  PipelineAttachmentFormats result;
  result.sample_count = 1;
  for (uint32_t i = 0; i < 4; ++i) {
    result.color_formats[i] = MTL::PixelFormatInvalid;
  }
  result.depth_format = MTL::PixelFormatInvalid;
  result.stencil_format = MTL::PixelFormatInvalid;

  if (render_target_cache) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (MTL::Texture* rt = render_target_cache->GetColorTargetForDraw(i)) {
        result.color_formats[i] = rt->pixelFormat();
        if (rt->sampleCount() > 0) {
          result.sample_count = std::max<uint32_t>(
              result.sample_count, static_cast<uint32_t>(rt->sampleCount()));
        }
      }
    }
    if (result.color_formats[0] == MTL::PixelFormatInvalid) {
      if (MTL::Texture* dummy =
              render_target_cache->GetDummyColorTargetForDraw()) {
        result.color_formats[0] = dummy->pixelFormat();
        if (dummy->sampleCount() > 0) {
          result.sample_count = std::max<uint32_t>(
              result.sample_count, static_cast<uint32_t>(dummy->sampleCount()));
        }
      }
    }
    if (MTL::Texture* depth_tex =
            render_target_cache->GetDepthTargetForDraw()) {
      result.depth_format = depth_tex->pixelFormat();
      switch (result.depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          result.stencil_format = result.depth_format;
          break;
        default:
          result.stencil_format = MTL::PixelFormatInvalid;
          break;
      }
      if (depth_tex->sampleCount() > 0) {
        result.sample_count =
            std::max<uint32_t>(result.sample_count,
                               static_cast<uint32_t>(depth_tex->sampleCount()));
      }
    }
  }

  if (pass_descriptor) {
    // Rebuild strictly from the active encoder descriptor.
    result.sample_count = 1;
    for (uint32_t i = 0; i < 4; ++i) {
      result.color_formats[i] = MTL::PixelFormatInvalid;
    }
    result.depth_format = MTL::PixelFormatInvalid;
    result.stencil_format = MTL::PixelFormatInvalid;

    auto update_sample_count = [&](MTL::Texture* texture) {
      if (!texture) {
        return;
      }
      NS::UInteger sc = texture->sampleCount();
      if (sc > 0) {
        result.sample_count =
            std::max<uint32_t>(result.sample_count, static_cast<uint32_t>(sc));
      }
    };

    auto* color_attachments = pass_descriptor->colorAttachments();
    for (uint32_t i = 0; i < 4; ++i) {
      auto* attachment =
          color_attachments ? color_attachments->object(i) : nullptr;
      if (!attachment) {
        continue;
      }
      MTL::Texture* texture = attachment->texture();
      if (!texture) {
        continue;
      }
      result.color_formats[i] = texture->pixelFormat();
      update_sample_count(texture);
    }
    if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
      MTL::Texture* texture = depth_attachment->texture();
      if (texture) {
        result.depth_format = texture->pixelFormat();
        update_sample_count(texture);
      }
    }
    if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
      MTL::Texture* texture = stencil_attachment->texture();
      if (texture) {
        result.stencil_format = texture->pixelFormat();
        update_sample_count(texture);
      }
    }
    // Propagate combined depth-stencil formats.
    if (result.depth_format != MTL::PixelFormatInvalid &&
        result.stencil_format == MTL::PixelFormatInvalid) {
      switch (result.depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          result.stencil_format = result.depth_format;
          break;
        default:
          break;
      }
    } else if (result.stencil_format != MTL::PixelFormatInvalid &&
               result.depth_format == MTL::PixelFormatInvalid) {
      switch (result.stencil_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          result.depth_format = result.stencil_format;
          break;
        default:
          break;
      }
    }
  }

  // Metal pipeline attachment formats must match the active framebuffer
  // exactly. If no depth attachment is bound, a depth-writing fragment output
  // must not fabricate a depth format in the pipeline descriptor.
  if (pixel_shader_writes_depth &&
      result.depth_format == MTL::PixelFormatInvalid) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      XELOGW(
          "{}: fragment writes depth without a bound depth attachment; "
          "using no depth attachment in the pipeline descriptor",
          pipeline_name);
    }
  }

  return result;
}

bool ShaderUsesVertexFetch(const Shader& shader) {
  if (!shader.vertex_bindings().empty()) {
    return true;
  }
  const Shader::ConstantRegisterMap& constant_map =
      shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    if (constant_map.vertex_fetch_bitmap[i] != 0) {
      return true;
    }
  }
  return false;
}

bool CollectMetalVertexFetchSharedMemoryRanges(const RegisterFile& regs,
                                               const Shader& vertex_shader,
                                               SharedMemory::Range* ranges,
                                               uint32_t max_ranges,
                                               uint32_t* range_count,
                                               bool strict_logging) {
  *range_count = 0;
  const Shader::ConstantRegisterMap& constant_map =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map.vertex_fetch_bitmap[i];
    uint32_t j;
    while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(vfetch_index);
      switch (vfetch.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (::cvars::gpu_allow_invalid_fetch_constants) {
            break;
          }
          if (strict_logging) {
            XELOGW(
                "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" "
                "type. Use --gpu_allow_invalid_fetch_constants to bypass.",
                vfetch_index, vfetch.dword_0, vfetch.dword_1);
          }
          return false;
        default:
          if (strict_logging) {
            XELOGW("Vertex fetch constant {} ({:08X} {:08X}) is invalid.",
                   vfetch_index, vfetch.dword_0, vfetch.dword_1);
          }
          return false;
      }
      uint32_t buffer_offset = vfetch.address << 2;
      uint32_t buffer_length = vfetch.size << 2;
      if (buffer_offset > SharedMemory::kBufferSize ||
          SharedMemory::kBufferSize - buffer_offset < buffer_length) {
        if (strict_logging) {
          XELOGW(
              "Vertex fetch constant {} out of range (offset=0x{:08X} "
              "size={})",
              vfetch_index, buffer_offset, buffer_length);
        }
        return false;
      }
      if (*range_count >= max_ranges) {
        if (strict_logging) {
          XELOGW("Too many vertex fetch shared-memory ranges");
        }
        return false;
      }
      ranges[(*range_count)++] =
          SharedMemory::Range{buffer_offset, buffer_length};
    }
  }
  return true;
}

MTL::CompareFunction ToMetalCompareFunction(xenos::CompareFunction compare) {
  static const MTL::CompareFunction kCompareMap[8] = {
      MTL::CompareFunctionNever,         // 0
      MTL::CompareFunctionLess,          // 1
      MTL::CompareFunctionEqual,         // 2
      MTL::CompareFunctionLessEqual,     // 3
      MTL::CompareFunctionGreater,       // 4
      MTL::CompareFunctionNotEqual,      // 5
      MTL::CompareFunctionGreaterEqual,  // 6
      MTL::CompareFunctionAlways,        // 7
  };
  return kCompareMap[uint32_t(compare) & 0x7];
}

MTL::StencilOperation ToMetalStencilOperation(xenos::StencilOp op) {
  static const MTL::StencilOperation kStencilOpMap[8] = {
      MTL::StencilOperationKeep,            // 0
      MTL::StencilOperationZero,            // 1
      MTL::StencilOperationReplace,         // 2
      MTL::StencilOperationIncrementClamp,  // 3
      MTL::StencilOperationDecrementClamp,  // 4
      MTL::StencilOperationInvert,          // 5
      MTL::StencilOperationIncrementWrap,   // 6
      MTL::StencilOperationDecrementWrap,   // 7
  };
  return kStencilOpMap[uint32_t(op) & 0x7];
}

bool FetchConstantDwordMaskEmpty(
    const DxbcShader::FetchConstantDwordMask& mask) {
  for (uint32_t word : mask) {
    if (word) {
      return false;
    }
  }
  return true;
}

void MarkAllFetchConstantDwords(DxbcShader::FetchConstantDwordMask& mask) {
  mask.fill(UINT32_MAX);
}

void MarkFetchConstantDword(DxbcShader::FetchConstantDwordMask& mask,
                            uint32_t dword_index) {
  if (dword_index >= DxbcShader::kFetchConstantDwordCount) {
    assert_always();
    return;
  }
  mask[dword_index >> 5] |= uint32_t(1) << (dword_index & 31);
}

void MergeFetchConstantDwordMask(
    DxbcShader::FetchConstantDwordMask& dest,
    const DxbcShader::FetchConstantDwordMask& src) {
  for (size_t i = 0; i < dest.size(); ++i) {
    dest[i] |= src[i];
  }
}

}  // namespace

MetalCommandProcessor::MetalCommandProcessor(
    MetalGraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {
  pending_shared_memory_writes_.reserve(kMaxPendingSharedMemoryWriteCapacity);
}

MetalCommandProcessor::~MetalCommandProcessor() {
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kShutdown);
  // End any active render encoder before releasing
  // Note: Only call endEncoding if the encoder is still active
  // (not already ended by a committed command buffer)
  if (current_render_encoder_) {
    // The encoder may already be ended if the command buffer was committed
    // In that case, just release it
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
    current_render_encoder_has_zpd_visibility_ = false;
    ResetRenderEncoderBufferBindings();
  }
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  WaitForPendingCompletionHandlers();

  // Release pipeline cache (owns shaders, pipelines, shader translation).
  pipeline_cache_.reset();

  for (auto& pair : depth_stencil_state_cache_) {
    if (pair.second) {
      pair.second->release();
    }
  }
  depth_stencil_state_cache_.clear();

  // Release IR Converter runtime buffers and resources
  if (null_buffer_) {
    null_buffer_->release();
    null_buffer_ = nullptr;
  }
  if (null_texture_) {
    null_texture_->release();
    null_texture_ = nullptr;
  }
  if (native_null_texture_3d_) {
    native_null_texture_3d_->release();
    native_null_texture_3d_ = nullptr;
  }
  if (native_null_texture_cube_) {
    native_null_texture_cube_->release();
    native_null_texture_cube_ = nullptr;
  }
  if (null_sampler_) {
    null_sampler_->release();
    null_sampler_ = nullptr;
  }
  graphics_root_argument_state_ = {};
  current_bindless_cbv_gpu_addresses_ = {};
  current_bindless_cbv_buffers_ = {};
  current_bindless_cbv_offsets_ = {};
  current_bindless_active_cbv_masks_.fill(0);
  current_bindless_fixed_resource_set_ = {};
  current_bindless_texture_resource_set_ = {};
  current_bindless_root_resource_set_ = {};
  current_bindless_fixed_resource_source_serial_ = 0;
  current_bindless_texture_resource_input_serial_ = 0;
  current_bindless_texture_resource_source_serial_ = 0;
  current_bindless_root_resource_source_serial_ = 0;
  render_encoder_bindless_fixed_resources_serial_ = 0;
  render_encoder_bindless_texture_resources_serial_ = 0;
  render_encoder_bindless_root_resources_serial_ = 0;
  render_encoder_bindless_stage_root_bind_serials_.fill(0);
  render_encoder_bindless_table_bind_mesh_path_ = false;
  render_encoder_bindless_table_bind_tessellation_ = false;
}

void MetalCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                     uint32_t length) {
  if (shared_memory_) {
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
  if (primitive_processor_) {
    primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
  }
}

void MetalCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Restore the guest EDRAM snapshot captured in the trace into the Metal
  // render-target cache so that subsequent host render targets created from
  // EDRAM (via LoadTiledData) see the same initial contents as other
  // backends like D3D12.
  if (!snapshot) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called with null "
        "snapshot");
    return;
  }
  if (!render_target_cache_) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called before render "
        "target "
        "cache initialization");
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

void MetalCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  // TODO(wmarti): Add cache_clear_requested_ flag like D3D12 for deferred
  // clearing of pipeline caches, texture caches, etc.
}

void MetalCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void MetalCommandProcessor::ClearReadbackBuffers() {
  // TODO(wmarti): Implement readback buffer clearing when memexport readback
  // is added. See D3D12's readback_buffers_ and memexport_readback_buffers_.
}

ui::metal::MetalProvider& MetalCommandProcessor::GetMetalProvider() const {
  return *static_cast<ui::metal::MetalProvider*>(graphics_system_->provider());
}

uint64_t MetalCommandProcessor::GetCurrentSubmission() const {
  return submission_current_ ? submission_current_ : 1;
}

uint64_t MetalCommandProcessor::GetCompletedSubmission() const {
  return completed_command_buffers_.load(std::memory_order_relaxed);
}

void MetalCommandProcessor::ForceIssueSwap() {
  // Force a swap to push any pending render target to presenter
  // This is used by trace dumps to capture output when there's no explicit swap
  if (saw_swap_) {
    return;
  }
  IssueSwap(0, 1280, 720);
}

void MetalCommandProcessor::SetSwapDestSwap(uint32_t dest_base, bool swap) {
  if (!dest_base) {
    return;
  }
  if (swap_dest_swaps_by_base_.size() > 256) {
    swap_dest_swaps_by_base_.clear();
  }
  swap_dest_swaps_by_base_[dest_base] = swap;
}

bool MetalCommandProcessor::ConsumeSwapDestSwap(uint32_t dest_base,
                                                bool* swap_out) {
  if (!swap_out || !dest_base) {
    return false;
  }
  auto it = swap_dest_swaps_by_base_.find(dest_base);
  if (it == swap_dest_swaps_by_base_.end()) {
    return false;
  }
  *swap_out = it->second;
  swap_dest_swaps_by_base_.erase(it);
  return true;
}

void MetalCommandProcessor::InitializeResidencySet() {
  residency_set_supported_ = false;
  residency_set_enabled_ = false;
  residency_set_attached_ = false;
  residency_set_resources_.clear();
  residency_set_heaps_.clear();
  if (!device_ || !command_queue_) {
    return;
  }

  residency_set_supported_ =
      MetalObjectRespondsToSelector(device_,
                                    "newResidencySetWithDescriptor:error:") &&
      MetalObjectRespondsToSelector(command_queue_, "addResidencySet:") &&
      MetalObjectRespondsToSelector(command_queue_, "removeResidencySet:");
  if (!MetalResidencySetCvarAllowsEnable(residency_set_supported_)) {
    return;
  }
  if (!residency_set_supported_) {
    XELOGW(
        "Metal residency sets requested, but this Metal runtime does not "
        "expose MTLResidencySet");
    return;
  }

  MTL::ResidencySetDescriptor* descriptor =
      MTL::ResidencySetDescriptor::alloc()->init();
  descriptor->setLabel(
      NS::String::string("XeniaResidencySet", NS::UTF8StringEncoding));
  descriptor->setInitialCapacity(512);
  NS::Error* error = nullptr;
  residency_set_ = device_->newResidencySet(descriptor, &error);
  descriptor->release();
  if (!residency_set_) {
    XELOGW("Metal residency set creation failed: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown");
    return;
  }

  command_queue_->addResidencySet(residency_set_);
  residency_set_enabled_ = true;
  residency_set_attached_ = true;
}

void MetalCommandProcessor::ShutdownResidencySet() {
  if (residency_set_attached_ && command_queue_ && residency_set_) {
    command_queue_->removeResidencySet(residency_set_);
  }
  residency_set_attached_ = false;
  residency_set_enabled_ = false;
  residency_set_resources_.clear();
  residency_set_heaps_.clear();
  if (residency_set_) {
    residency_set_->removeAllAllocations();
    residency_set_->commit();
    residency_set_->release();
    residency_set_ = nullptr;
  }
}

bool MetalCommandProcessor::AddResidencySetResource(MTL::Resource* resource) {
  if (!residency_set_enabled_ || !residency_set_ || !resource) {
    return false;
  }
  auto [it, inserted] = residency_set_resources_.insert(resource);
  if (!inserted) {
    ++backend_telemetry_.residency_set_allocation_duplicates;
    return true;
  }
  residency_set_->addAllocation(resource);
  residency_set_->commit();
  ++backend_telemetry_.residency_set_allocations_added;
  ++backend_telemetry_.residency_set_commits;
  return true;
}

bool MetalCommandProcessor::IsResidencySetResourceCovered(
    MTL::Resource* resource) const {
  if (!residency_set_enabled_ || !resource) {
    return false;
  }
  // TODO (xenios-jp): Revisit heap-backed texture / render-target
  // coverage only after the backend owns the full hazard model for those
  // allocations. MTLResidencySet proves that an allocation is resident, but it
  // doesn't replace the encoder usage declarations the current texture and
  // render-target paths rely on for hazard visibility. A safe removal of
  // per-encoder useResource / useHeap for heap-backed allocations needs:
  //  * read/write tracking for texture-cache and render-target-cache resources,
  //    including color, depth, stencil, MSAA, resolve, and transfer views;
  //  * parent-resource / texture-view / heap alias handling so subresource use
  //    is attributed to the actual allocation that may conflict;
  //  * explicit ordering between render, compute, and blit encoders with
  //    fences, events, or conservative encoder boundaries before conflicting
  //    uses;
  //  * validation coverage for unpooled textures, memoryless attachments, and
  //    direct device->newTexture fallback paths.
  //
  // Until that exists, only resources explicitly added to
  // residency_set_resources_ are considered safe to suppress here. Heap-backed
  // textures and render targets must still go through the encoder usage path.
  return residency_set_resources_.find(resource) !=
         residency_set_resources_.end();
}

bool MetalCommandProcessor::AddResidencySetHeap(MTL::Heap* heap) {
  if (!residency_set_enabled_ || !residency_set_ || !heap) {
    return false;
  }
  auto [it, inserted] = residency_set_heaps_.insert(heap);
  if (!inserted) {
    ++backend_telemetry_.residency_set_allocation_duplicates;
    return true;
  }
  residency_set_->addAllocation(heap);
  residency_set_->commit();
  ++backend_telemetry_.residency_set_allocations_added;
  ++backend_telemetry_.residency_set_commits;
  return true;
}

bool MetalCommandProcessor::IsResidencySetHeapCovered(MTL::Heap* heap) const {
  return residency_set_enabled_ && heap &&
         residency_set_heaps_.find(heap) != residency_set_heaps_.end();
}

void MetalCommandProcessor::RegisterInitialResidencySetResources() {
  AddResidencySetResource(shared_memory_ ? shared_memory_->GetBuffer()
                                         : nullptr);
  AddResidencySetResource(
      render_target_cache_ ? render_target_cache_->GetEdramBuffer() : nullptr);
  AddResidencySetResource(tessellator_tables_buffer_);
  AddResidencySetResource(null_buffer_);
  AddResidencySetResource(null_texture_);
  AddResidencySetResource(native_null_texture_3d_);
  AddResidencySetResource(native_null_texture_cube_);
  AddResidencySetResource(view_bindless_heap_);
  AddResidencySetResource(sampler_bindless_heap_);
  AddResidencySetResource(native_msl_texture_2d_array_heap_);
  AddResidencySetResource(native_msl_texture_3d_heap_);
  AddResidencySetResource(native_msl_texture_cube_heap_);
  AddResidencySetResource(native_msl_sampler_heap_);
  AddResidencySetResource(system_view_tables_);
}

bool MetalCommandProcessor::CreateNativeMslTextureArgumentHeap(
    MTL::TextureType texture_type, const char* label,
    MTL::ArgumentEncoder*& encoder_out, MTL::Buffer*& buffer_out) {
  encoder_out = nullptr;
  buffer_out = nullptr;
  MTL::ArgumentDescriptor* descriptor =
      MTL::ArgumentDescriptor::argumentDescriptor();
  if (!descriptor) {
    XELOGE("Failed to create native MSL texture argument descriptor");
    return false;
  }
  descriptor->setDataType(MTL::DataTypeTexture);
  descriptor->setTextureType(texture_type);
  descriptor->setArrayLength(kNativeMslTextureHeapSize);
  descriptor->setAccess(MTL::BindingAccessReadOnly);
  descriptor->setIndex(0);
  NS::Array* arguments = NS::Array::array(descriptor);
  encoder_out = device_->newArgumentEncoder(arguments);
  if (!encoder_out) {
    XELOGE("Failed to create native MSL texture argument encoder {}", label);
    return false;
  }
  encoder_out->setLabel(NS::String::string(label, NS::UTF8StringEncoding));

  const NS::UInteger encoded_length = encoder_out->encodedLength();
  buffer_out = device_->newBuffer(
      encoded_length,
      MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
  if (!buffer_out) {
    XELOGE("Failed to create native MSL texture argument heap {} ({} bytes)",
           label, uint64_t(encoded_length));
    encoder_out->release();
    encoder_out = nullptr;
    return false;
  }
  buffer_out->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
  std::memset(buffer_out->contents(), 0, buffer_out->length());
  encoder_out->setArgumentBuffer(buffer_out, 0);
  return true;
}

bool MetalCommandProcessor::CreateNativeMslSamplerArgumentHeap(
    MTL::ArgumentEncoder*& encoder_out, MTL::Buffer*& buffer_out) {
  encoder_out = nullptr;
  buffer_out = nullptr;
  MTL::ArgumentDescriptor* descriptor =
      MTL::ArgumentDescriptor::argumentDescriptor();
  if (!descriptor) {
    XELOGE("Failed to create native MSL sampler argument descriptor");
    return false;
  }
  descriptor->setDataType(MTL::DataTypeSampler);
  descriptor->setArrayLength(kNativeMslSamplerHeapSize);
  descriptor->setAccess(MTL::BindingAccessReadOnly);
  descriptor->setIndex(0);
  NS::Array* arguments = NS::Array::array(descriptor);
  encoder_out = device_->newArgumentEncoder(arguments);
  if (!encoder_out) {
    XELOGE("Failed to create native MSL sampler argument encoder");
    return false;
  }
  encoder_out->setLabel(
      NS::String::string("XeniaNativeMslSamplerHeap", NS::UTF8StringEncoding));

  const NS::UInteger encoded_length = encoder_out->encodedLength();
  buffer_out = device_->newBuffer(
      encoded_length,
      MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
  if (!buffer_out) {
    XELOGE("Failed to create native MSL sampler argument heap ({} bytes)",
           uint64_t(encoded_length));
    encoder_out->release();
    encoder_out = nullptr;
    return false;
  }
  buffer_out->setLabel(
      NS::String::string("XeniaNativeMslSamplerHeap", NS::UTF8StringEncoding));
  std::memset(buffer_out->contents(), 0, buffer_out->length());
  encoder_out->setArgumentBuffer(buffer_out, 0);
  return true;
}

bool MetalCommandProcessor::InitializeNativeMslArgumentHeaps() {
  native_msl_argument_heaps_ready_ = false;
  native_msl_view_indices_.clear();
  native_msl_view_index_free_.clear();
  native_msl_view_index_next_ = 0;
  native_msl_view_index_exhausted_logged_ = false;
  if (!device_) {
    return false;
  }
  if (device_->argumentBuffersSupport() != MTL::ArgumentBuffersTier2) {
    XELOGE("Native MSL resource heaps require Metal argument buffers tier 2");
    return false;
  }
  if (!CreateNativeMslTextureArgumentHeap(
          MTL::TextureType2DArray, "XeniaNativeMslTexture2DArrayHeap",
          native_msl_texture_2d_array_heap_encoder_,
          native_msl_texture_2d_array_heap_) ||
      !CreateNativeMslTextureArgumentHeap(
          MTL::TextureType3D, "XeniaNativeMslTexture3DHeap",
          native_msl_texture_3d_heap_encoder_, native_msl_texture_3d_heap_) ||
      !CreateNativeMslTextureArgumentHeap(MTL::TextureTypeCube,
                                          "XeniaNativeMslTextureCubeHeap",
                                          native_msl_texture_cube_heap_encoder_,
                                          native_msl_texture_cube_heap_) ||
      !CreateNativeMslSamplerArgumentHeap(native_msl_sampler_heap_encoder_,
                                          native_msl_sampler_heap_)) {
    ShutdownNativeMslArgumentHeaps();
    return false;
  }
  native_msl_argument_heaps_ready_ = true;
  native_msl_view_indices_.assign(kViewBindlessHeapSize, UINT32_MAX);
  return true;
}

void MetalCommandProcessor::ShutdownNativeMslArgumentHeaps() {
  native_msl_argument_heaps_ready_ = false;
  native_msl_view_indices_.clear();
  native_msl_view_index_free_.clear();
  native_msl_view_index_next_ = 0;
  native_msl_view_index_exhausted_logged_ = false;
  auto release_encoder = [](MTL::ArgumentEncoder*& encoder) {
    if (encoder) {
      encoder->release();
      encoder = nullptr;
    }
  };
  auto release_buffer = [](MTL::Buffer*& buffer) {
    if (buffer) {
      buffer->release();
      buffer = nullptr;
    }
  };
  release_encoder(native_msl_texture_2d_array_heap_encoder_);
  release_encoder(native_msl_texture_3d_heap_encoder_);
  release_encoder(native_msl_texture_cube_heap_encoder_);
  release_encoder(native_msl_sampler_heap_encoder_);
  release_buffer(native_msl_texture_2d_array_heap_);
  release_buffer(native_msl_texture_3d_heap_);
  release_buffer(native_msl_texture_cube_heap_);
  release_buffer(native_msl_sampler_heap_);
}

bool MetalCommandProcessor::SetupContext() {
  saw_swap_ = false;
  last_swap_ptr_ = 0;
  last_swap_width_ = 0;
  last_swap_height_ = 0;
  swap_dest_swaps_by_base_.clear();
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  const ui::metal::MetalProvider& provider = GetMetalProvider();
  device_ = provider.GetDevice();
  command_queue_ = provider.GetCommandQueue();

  if (!device_ || !command_queue_) {
    XELOGE("MetalCommandProcessor: No Metal device or command queue available");
    return false;
  }

  InitializeResidencySet();

  wait_shared_event_ = device_->newSharedEvent();
  if (wait_shared_event_) {
    wait_shared_event_->setLabel(
        NS::String::string("XeniaWaitEvent", NS::UTF8StringEncoding));
    wait_shared_event_value_ = 0;
  } else {
    XELOGW(
        "MetalCommandProcessor: SharedEvent unavailable; falling back to "
        "waitUntilCompleted");
  }

  shared_memory_fence_ = device_->newFence();
  if (!shared_memory_fence_) {
    XELOGE(
        "MetalCommandProcessor: MTLFence unavailable; cannot safely order "
        "shared-memory GPU write hazards");
    return false;
  }
  shared_memory_fence_->setLabel(
      NS::String::string("XeniaSharedMemoryFence", NS::UTF8StringEncoding));

  bool supports_apple7 = device_->supportsFamily(MTL::GPUFamilyApple7);
  bool supports_mac2 = device_->supportsFamily(MTL::GPUFamilyMac2);
  mesh_shader_supported_ = supports_apple7 || supports_mac2;

  // Initialize shared memory
  shared_memory_ = std::make_unique<MetalSharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  // Initialize primitive processor (index/primitive conversion like D3D12).
  primitive_processor_ = std::make_unique<MetalPrimitiveProcessor>(
      *this, *register_file_, *memory_, trace_writer_, *shared_memory_);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize Metal primitive processor");
    return false;
  }

  // Create persistent bindless descriptor heaps BEFORE the texture/sampler
  // caches, because their Initialize() allocates persistent heap slots for
  // null textures and samplers.
  view_bindless_heap_ = device_->newBuffer(
      kViewBindlessHeapSize * sizeof(IRDescriptorTableEntry),
      MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
  if (!view_bindless_heap_) {
    XELOGE("Failed to create view bindless heap");
    return false;
  }
  view_bindless_heap_->setLabel(
      NS::String::string("XeniaViewBindlessHeap", NS::UTF8StringEncoding));
  memset(view_bindless_heap_->contents(), 0, view_bindless_heap_->length());

  sampler_bindless_heap_ = device_->newBuffer(
      kSamplerBindlessHeapSize * sizeof(IRDescriptorTableEntry),
      MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
  if (!sampler_bindless_heap_) {
    XELOGE("Failed to create sampler bindless heap");
    return false;
  }
  sampler_bindless_heap_->setLabel(
      NS::String::string("XeniaSamplerBindlessHeap", NS::UTF8StringEncoding));
  memset(sampler_bindless_heap_->contents(), 0,
         sampler_bindless_heap_->length());

  // The bindless heaps are dedicated to dynamically indexed textures and
  // samplers only. System descriptors such as shared memory / EDRAM use small
  // explicit-layout top-level tables populated below once all resources exist.
  view_bindless_heap_next_ = 0;
  view_bindless_heap_exhausted_logged_ = false;
  sampler_bindless_heap_next_ = 0;
  sampler_bindless_heap_exhausted_logged_ = false;

  if (cvars::metal_native_msl_render && !InitializeNativeMslArgumentHeaps()) {
    return false;
  }

  texture_cache_ = std::make_unique<MetalTextureCache>(this, *register_file_,
                                                       *shared_memory_, 1, 1);
  if (!texture_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal texture cache");
    return false;
  }

  // Initialize render target cache
  render_target_cache_ = std::make_unique<MetalRenderTargetCache>(
      *register_file_, *memory_, &trace_writer_, 1, 1, *this);
  if (!render_target_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal render target cache");
    return false;
  }

  // Create and initialize pipeline cache (shader translation + pipeline
  // management).
  pipeline_cache_ =
      std::make_unique<MetalPipelineCache>(device_, *register_file_);
  {
    bool edram_rov_used = false;
    bool gamma_render_target_as_unorm8 =
        !(edram_rov_used ||
          render_target_cache_->gamma_render_target_as_unorm16());
    if (!pipeline_cache_->InitializeShaderTranslation(
            gamma_render_target_as_unorm8,
            render_target_cache_->msaa_2x_supported(),
            render_target_cache_->draw_resolution_scale_x(),
            render_target_cache_->draw_resolution_scale_y())) {
      XELOGE("Failed to initialize shader translation");
      return false;
    }
  }
  if (mesh_shader_supported_) {
    uint64_t tess_tables_size = IRRuntimeTessellatorTablesSize();
    tessellator_tables_buffer_ =
        device_->newBuffer(tess_tables_size, MTL::ResourceStorageModeShared);
    if (!tessellator_tables_buffer_) {
      XELOGE("Failed to allocate tessellator tables buffer ({} bytes)",
             tess_tables_size);
      return false;
    }
    tessellator_tables_buffer_->setLabel(
        NS::String::string("XeniaTessellatorTables", NS::UTF8StringEncoding));
    IRRuntimeLoadTessellatorTables(tessellator_tables_buffer_);
  }

  // Create the upload buffer pool for per-draw constant buffer allocations.
  constant_buffer_pool_ = std::make_unique<MetalUploadBufferPool>(device_);
  constant_buffer_pool_->SetPageCreatedCallback(
      [this](MTL::Buffer* buffer) { AddResidencySetResource(buffer); });

  // Initialize the ZPD occlusion query pool and resources.
  zpd_visibility_pool_ = std::make_unique<MetalZPDVisibilityPool>();
  EnsureZPDQueryResources();

  render_encoder_resource_usage_table_.reserve(256);
  render_encoder_heap_usage_.reserve(32);

  // Create a null buffer for unused descriptor entries
  // This prevents shader validation errors when accessing unpopulated
  // descriptors
  null_buffer_ =
      device_->newBuffer(kNullBufferSize, MTL::ResourceStorageModeShared);
  if (!null_buffer_) {
    XELOGE("Failed to create null buffer");
    return false;
  }
  null_buffer_->setLabel(
      NS::String::string("NullBuffer", NS::UTF8StringEncoding));
  std::memset(null_buffer_->contents(), 0, kNullBufferSize);

  // Create a 1x1x1 placeholder 2D array texture for unbound texture slots
  // Xbox 360 textures are typically 2D arrays (for texture atlases, cubemaps)
  // Using 2DArray prevents "Invalid texture type" validation errors
  MTL::TextureDescriptor* null_tex_desc =
      MTL::TextureDescriptor::alloc()->init();
  null_tex_desc->setTextureType(MTL::TextureType2DArray);
  null_tex_desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  null_tex_desc->setWidth(1);
  null_tex_desc->setHeight(1);
  null_tex_desc->setArrayLength(1);  // Single slice in the array
  null_tex_desc->setStorageMode(MTL::StorageModeShared);
  null_tex_desc->setUsage(MTL::TextureUsageShaderRead);

  null_texture_ = device_->newTexture(null_tex_desc);
  null_tex_desc->release();

  if (!null_texture_) {
    XELOGE("Failed to create null texture");
    return false;
  }
  null_texture_->setLabel(
      NS::String::string("NullTexture2DArray", NS::UTF8StringEncoding));

  // Fill the 1x1x1 texture with opaque white (helps debug if sampled)
  uint32_t white_pixel = 0xFFFFFFFF;
  MTL::Region region =
      MTL::Region(0, 0, 0, 1, 1, 1);  // x,y,z origin, w,h,d size
  null_texture_->replaceRegion(region, 0, 0, &white_pixel, 4, 0);  // slice 0

  native_null_texture_3d_ = CreateNativeNullTexture(device_, MTL::TextureType3D,
                                                    "NativeMslNullTexture3D");
  if (!native_null_texture_3d_) {
    XELOGE("Failed to create native MSL null 3D texture");
    return false;
  }
  native_null_texture_cube_ = CreateNativeNullTexture(
      device_, MTL::TextureTypeCube, "NativeMslNullTextureCube");
  if (!native_null_texture_cube_) {
    XELOGE("Failed to create native MSL null cube texture");
    return false;
  }

  // Create a default sampler for unbound sampler slots
  // Must set supportsArgumentBuffers=YES for use in argument buffers
  MTL::SamplerDescriptor* null_smp_desc =
      MTL::SamplerDescriptor::alloc()->init();
  null_smp_desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMipFilter(MTL::SamplerMipFilterLinear);
  null_smp_desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setSupportArgumentBuffers(true);

  null_sampler_ = device_->newSamplerState(null_smp_desc);
  null_smp_desc->release();

  if (!null_sampler_) {
    XELOGE("Failed to create null sampler");
    return false;
  }

  system_view_tables_ = device_->newBuffer(
      kSystemViewTableEntryCount * sizeof(IRDescriptorTableEntry),
      MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
  if (!system_view_tables_) {
    XELOGE("Failed to create system descriptor tables");
    return false;
  }
  system_view_tables_->setLabel(
      NS::String::string("XeniaSystemViewTables", NS::UTF8StringEncoding));
  std::memset(system_view_tables_->contents(), 0,
              system_view_tables_->length());
  {
    auto* system_entries = reinterpret_cast<IRDescriptorTableEntry*>(
        system_view_tables_->contents());
    MTL::Buffer* shared_mem_buffer =
        shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
    const uint64_t shared_memory_metadata =
        shared_mem_buffer ? shared_mem_buffer->length() : kNullBufferSize;
    IRDescriptorTableSetBuffer(&system_entries[kSystemViewTableSRVSharedMemory],
                               shared_mem_buffer
                                   ? shared_mem_buffer->gpuAddress()
                                   : null_buffer_->gpuAddress(),
                               shared_memory_metadata);
    IRDescriptorTableSetBuffer(&system_entries[kSystemViewTableSRVNull],
                               null_buffer_->gpuAddress(), kNullBufferSize);
    IRDescriptorTableSetBuffer(&system_entries[kSystemViewTableUAVNullStart],
                               null_buffer_->gpuAddress(), kNullBufferSize);
    if (!render_target_cache_ ||
        !render_target_cache_->WriteEdramUintPow2BindlessDescriptor(
            &system_entries[kSystemViewTableUAVNullStart + 1], 2)) {
      XELOGE("Failed to encode typed EDRAM UAV system descriptor");
      return false;
    }
    IRDescriptorTableSetBuffer(
        &system_entries[kSystemViewTableUAVSharedMemoryStart],
        shared_mem_buffer ? shared_mem_buffer->gpuAddress()
                          : null_buffer_->gpuAddress(),
        shared_memory_metadata);
    if (!render_target_cache_ ||
        !render_target_cache_->WriteEdramUintPow2BindlessDescriptor(
            &system_entries[kSystemViewTableUAVSharedMemoryStart + 1], 2)) {
      XELOGE("Failed to encode typed EDRAM UAV system descriptor");
      return false;
    }
  }
  RegisterInitialResidencySetResources();
  return true;
}

void MetalCommandProcessor::FlushCommandBufferAndWait(uint64_t timeout_ns,
                                                      const char* context) {
  // Phase 1: commit the active command buffer and wait for it.
  if (current_command_buffer_) {
    EndSharedMemoryUploadBlitEncoder(
        SharedMemoryUploadEncoderEndReason::kCommandBufferEnd);
    if (texture_cache_ &&
        !texture_cache_
             ->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
      XELOGE("Metal: failed to flush texture upload encoder before wait");
    }
    uint64_t wait_value = 0;
    if (wait_shared_event_) {
      wait_value = ++wait_shared_event_value_;
      current_command_buffer_->encodeSignalEvent(wait_shared_event_,
                                                 wait_value);
    }
    current_command_buffer_->commit();
    if (wait_shared_event_) {
      bool signaled =
          wait_shared_event_->waitUntilSignaledValue(wait_value, timeout_ns);
      if (!signaled) {
        XELOGE("{}: GPU timeout (possible GPU hang)", context);
      }
    } else {
      current_command_buffer_->waitUntilCompleted();
    }
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    submission_has_draws_ = false;
  }
  DrainCommandBufferAutoreleasePool();

  // Phase 2: submit a dummy command buffer to ensure ALL previously committed
  // GPU work completes before the caller tears down resources.
  if (command_queue_) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::CommandBuffer* sync_cmd = command_queue_->commandBuffer();
    if (sync_cmd) {
      uint64_t wait_value = 0;
      if (wait_shared_event_) {
        wait_value = ++wait_shared_event_value_;
        sync_cmd->encodeSignalEvent(wait_shared_event_, wait_value);
      }
      sync_cmd->commit();
      if (wait_shared_event_) {
        bool signaled =
            wait_shared_event_->waitUntilSignaledValue(wait_value, timeout_ns);
        if (!signaled) {
          XELOGE("{}: GPU sync timeout (possible GPU hang)", context);
        }
      } else {
        sync_cmd->waitUntilCompleted();
      }
    }
    pool->release();
  }
}

void MetalCommandProcessor::PrepareForWait() {
  // Flush pending Metal command buffers before entering wait state so that
  // the worker thread's autorelease pool can drain cleanly.
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kPrepareForWait)) {
    XELOGE("Metal PrepareForWait: failed to flush prepared draw queue");
  }
  EndRenderEncoder(RenderEncoderEndReason::kPrepareForWait);
  FlushCommandBufferAndWait(/*timeout_ns=*/5000000000ULL, "PrepareForWait");
  CommandProcessor::PrepareForWait();
}

void MetalCommandProcessor::PollCompletedSubmission() {
  // The base class calls PollCompletedSubmission during strict mode waits
  // and at submission boundaries. Just drain any ready query resolves.
  PumpQueryResolves();
}

void MetalCommandProcessor::WaitForPendingCompletionHandlers() {
  constexpr auto kMaxWait = std::chrono::seconds(5);
  const auto wait_start = std::chrono::steady_clock::now();
  while (pending_completion_handlers_.load(std::memory_order_acquire) != 0) {
    if (std::chrono::steady_clock::now() - wait_start >= kMaxWait) {
      XELOGW(
          "MetalCommandProcessor: timed out waiting for {} completion "
          "handler(s) during shutdown",
          pending_completion_handlers_.load(std::memory_order_relaxed));
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void MetalCommandProcessor::ShutdownContext() {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kManual)) {
    XELOGE("Metal ShutdownContext: failed to flush prepared draw queue");
  }
  MaybeDumpBackendTelemetry("shutdown", true);

  // End the render encoder directly (not via EndRenderEncoder — we release
  // the encoder object below after the command buffer completes).
  if (current_render_encoder_) {
    UpdateSharedMemoryFenceForActiveRenderEncoder();
    current_render_encoder_->endEncoding();
  }
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kShutdown);

  FlushCommandBufferAndWait(std::numeric_limits<uint64_t>::max(),
                            "ShutdownContext");
  WaitForPendingCompletionHandlers();

  // Now safe to release encoder and command buffer
  if (current_render_encoder_) {
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
    current_render_encoder_has_zpd_visibility_ = false;
    ResetRenderEncoderBufferBindings();
  }
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  DrainCommandBufferAutoreleasePool();
  ShutdownResidencySet();

  constant_buffer_pool_.reset();

  // Shut down texture cache first so texture/sampler destructors can release
  // their bindless heap slots while the heap buffers are still alive.
  if (texture_cache_) {
    texture_cache_->Shutdown();
    texture_cache_.reset();
  }

  ShutdownNativeMslArgumentHeaps();

  // Release persistent bindless heaps after texture/sampler caches are
  // torn down (destructors write to these heaps during release).
  if (view_bindless_heap_) {
    view_bindless_heap_->release();
    view_bindless_heap_ = nullptr;
  }
  if (sampler_bindless_heap_) {
    sampler_bindless_heap_->release();
    sampler_bindless_heap_ = nullptr;
  }
  if (system_view_tables_) {
    system_view_tables_->release();
    system_view_tables_ = nullptr;
  }
  view_bindless_heap_free_.clear();
  sampler_bindless_heap_free_.clear();
  retired_view_bindless_indices_.clear();
  retired_sampler_bindless_indices_.clear();
  for (RetiredMetalBuffer& retired_buffer : retired_memexport_index_buffers_) {
    if (retired_buffer.buffer) {
      retired_buffer.buffer->release();
    }
  }
  retired_memexport_index_buffers_.clear();
  view_bindless_heap_next_ = 0;
  sampler_bindless_heap_next_ = 0;
  view_bindless_heap_exhausted_logged_ = false;
  sampler_bindless_heap_exhausted_logged_ = false;

  if (primitive_processor_) {
    primitive_processor_->Shutdown();
    primitive_processor_.reset();
  }
  if (tessellator_tables_buffer_) {
    tessellator_tables_buffer_->release();
    tessellator_tables_buffer_ = nullptr;
  }
  frame_open_ = false;

  pipeline_cache_.reset();

  // Shut down ZPD query resources.
  ShutdownZPDQueryResources();
  zpd_visibility_pool_.reset();

  shared_memory_.reset();
  if (shared_memory_fence_) {
    shared_memory_fence_->release();
    shared_memory_fence_ = nullptr;
  }
  if (wait_shared_event_) {
    wait_shared_event_->release();
    wait_shared_event_ = nullptr;
  }

  CommandProcessor::ShutdownContext();
}

uint32_t MetalCommandProcessor::AllocateViewBindlessIndex() {
  if (!view_bindless_heap_free_.empty()) {
    uint32_t idx = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    view_bindless_heap_exhausted_logged_ = false;
    return idx;
  }
  if (view_bindless_heap_next_ >= kViewBindlessHeapSize) {
    // Reclaim retired indices from completed submissions first — this is cheap
    // and may free slots without needing to evict any textures.
    ProcessCompletedSubmissions();
    if (!view_bindless_heap_free_.empty()) {
      uint32_t idx = view_bindless_heap_free_.back();
      view_bindless_heap_free_.pop_back();
      view_bindless_heap_exhausted_logged_ = false;
      return idx;
    }
    // Still full — try evicting least-recently-used textures.
    if (texture_cache_ && texture_cache_->TrimViewBindlessPressure()) {
      if (!view_bindless_heap_free_.empty()) {
        uint32_t idx = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
        view_bindless_heap_exhausted_logged_ = false;
        return idx;
      }
    }
    if (!view_bindless_heap_exhausted_logged_) {
      uint64_t completed =
          completed_command_buffers_.load(std::memory_order_relaxed);
      uint64_t texture_count =
          texture_cache_ ? texture_cache_->GetTotalTextureCount() : 0;
      XELOGE(
          "View bindless heap exhausted ({} allocated, {} free, {} retired, "
          "submissions: current={} completed={}, textures={})",
          view_bindless_heap_next_, view_bindless_heap_free_.size(),
          retired_view_bindless_indices_.size(), submission_current_, completed,
          texture_count);
      view_bindless_heap_exhausted_logged_ = true;
    }
    return UINT32_MAX;
  }
  return view_bindless_heap_next_++;
}

void MetalCommandProcessor::ReleaseViewBindlessIndex(uint32_t index) {
  if (index >= kViewBindlessHeapSize || !view_bindless_heap_) {
    return;
  }
  // Match D3D12's persistent texture-descriptor lifetime more closely: by the
  // time a Metal texture reaches destruction through the cache, its last GPU
  // use has already completed, so the bindless view slot can be recycled
  // immediately rather than waiting for the current submission to end.
  FreeViewBindlessIndexNow(index);
}

void MetalCommandProcessor::RetireViewBindlessIndex(uint32_t index) {
  if (index >= kViewBindlessHeapSize || !view_bindless_heap_) {
    return;
  }
  uint64_t retirement_submission = GetBindlessDescriptorRetirementSubmission();
  if (!retirement_submission) {
    FreeViewBindlessIndexNow(index);
  } else {
    retired_view_bindless_indices_.push_back({index, retirement_submission});
  }
}

uint32_t MetalCommandProcessor::GetViewBindlessHeapAvailableCount() const {
  return uint32_t(kViewBindlessHeapSize - view_bindless_heap_next_) +
         uint32_t(view_bindless_heap_free_.size());
}

uint32_t MetalCommandProcessor::AllocateSamplerBindlessIndex() {
  if (!sampler_bindless_heap_free_.empty()) {
    uint32_t idx = sampler_bindless_heap_free_.back();
    sampler_bindless_heap_free_.pop_back();
    sampler_bindless_heap_exhausted_logged_ = false;
    return idx;
  }
  if (sampler_bindless_heap_next_ >= kSamplerBindlessHeapSize) {
    if (!sampler_bindless_heap_exhausted_logged_) {
      XELOGE("Sampler bindless heap exhausted");
      sampler_bindless_heap_exhausted_logged_ = true;
    }
    return UINT32_MAX;
  }
  return sampler_bindless_heap_next_++;
}

void MetalCommandProcessor::ReleaseSamplerBindlessIndex(uint32_t index) {
  if (index >= kSamplerBindlessHeapSize || !sampler_bindless_heap_) {
    return;
  }
  uint64_t retirement_submission = GetBindlessDescriptorRetirementSubmission();
  if (!retirement_submission) {
    FreeSamplerBindlessIndexNow(index);
  } else {
    retired_sampler_bindless_indices_.push_back({index, retirement_submission});
  }
}

uint64_t MetalCommandProcessor::GetBindlessDescriptorRetirementSubmission()
    const {
  if (!submission_current_) {
    return 0;
  }
  if (current_command_buffer_ ||
      completed_command_buffers_.load(std::memory_order_relaxed) <
          submission_current_) {
    return submission_current_;
  }
  return 0;
}

void MetalCommandProcessor::FreeViewBindlessIndexNow(uint32_t index) {
  if (index >= kViewBindlessHeapSize || !view_bindless_heap_) {
    return;
  }
  if (IRDescriptorTableEntry* entry = GetViewBindlessHeapEntry(index)) {
    std::memset(entry, 0, sizeof(IRDescriptorTableEntry));
  }
  ClearNativeMslViewBindlessTexture(index);
  view_bindless_heap_free_.push_back(index);
  view_bindless_heap_exhausted_logged_ = false;
}

void MetalCommandProcessor::FreeSamplerBindlessIndexNow(uint32_t index) {
  if (index >= kSamplerBindlessHeapSize || !sampler_bindless_heap_) {
    return;
  }
  if (IRDescriptorTableEntry* entry = GetSamplerBindlessHeapEntry(index)) {
    std::memset(entry, 0, sizeof(IRDescriptorTableEntry));
  }
  ClearNativeMslSamplerBindlessState(index);
  sampler_bindless_heap_free_.push_back(index);
  sampler_bindless_heap_exhausted_logged_ = false;
}

IRDescriptorTableEntry* MetalCommandProcessor::GetViewBindlessHeapEntry(
    uint32_t index) {
  if (!view_bindless_heap_) {
    XELOGE("GetViewBindlessHeapEntry: heap is null!");
    return nullptr;
  }
  if (index >= kViewBindlessHeapSize) {
    XELOGE("GetViewBindlessHeapEntry: index {} >= heap size {}", index,
           kViewBindlessHeapSize);
    return nullptr;
  }
  return reinterpret_cast<IRDescriptorTableEntry*>(
             view_bindless_heap_->contents()) +
         index;
}

IRDescriptorTableEntry* MetalCommandProcessor::GetSamplerBindlessHeapEntry(
    uint32_t index) {
  if (!sampler_bindless_heap_) {
    XELOGE("GetSamplerBindlessHeapEntry: heap is null!");
    return nullptr;
  }
  if (index >= kSamplerBindlessHeapSize) {
    XELOGE("GetSamplerBindlessHeapEntry: index {} >= heap size {}", index,
           kSamplerBindlessHeapSize);
    return nullptr;
  }
  return reinterpret_cast<IRDescriptorTableEntry*>(
             sampler_bindless_heap_->contents()) +
         index;
}

void MetalCommandProcessor::SetNativeMslViewBindlessTexture(
    uint32_t index, MTL::Texture* texture) {
  if (!native_msl_argument_heaps_ready_ || index >= kViewBindlessHeapSize ||
      !texture) {
    return;
  }
  const uint32_t native_index = GetOrAllocateNativeMslViewBindlessIndex(index);
  if (native_index == UINT32_MAX) {
    return;
  }
  MTL::TextureType texture_type = texture->textureType();
  auto set_heap_texture = [&](MTL::ArgumentEncoder* encoder,
                              MTL::Texture* heap_texture) {
    if (encoder) {
      encoder->setTexture(heap_texture, native_index);
    }
  };
  set_heap_texture(native_msl_texture_2d_array_heap_encoder_,
                   texture_type == MTL::TextureType2DArray ? texture : nullptr);
  set_heap_texture(native_msl_texture_3d_heap_encoder_,
                   texture_type == MTL::TextureType3D ? texture : nullptr);
  set_heap_texture(native_msl_texture_cube_heap_encoder_,
                   texture_type == MTL::TextureTypeCube ? texture : nullptr);
}

void MetalCommandProcessor::ClearNativeMslViewBindlessTexture(uint32_t index) {
  if (index >= kViewBindlessHeapSize) {
    return;
  }
  const uint32_t native_index = GetNativeMslViewBindlessIndex(index);
  if (native_index == UINT32_MAX) {
    return;
  }
  if (native_msl_argument_heaps_ready_) {
    if (native_msl_texture_2d_array_heap_encoder_) {
      native_msl_texture_2d_array_heap_encoder_->setTexture(nullptr,
                                                            native_index);
    }
    if (native_msl_texture_3d_heap_encoder_) {
      native_msl_texture_3d_heap_encoder_->setTexture(nullptr, native_index);
    }
    if (native_msl_texture_cube_heap_encoder_) {
      native_msl_texture_cube_heap_encoder_->setTexture(nullptr, native_index);
    }
  }
  FreeNativeMslViewBindlessIndex(index);
}

uint32_t MetalCommandProcessor::GetNativeMslViewBindlessIndex(
    uint32_t index) const {
  if (index >= native_msl_view_indices_.size()) {
    return UINT32_MAX;
  }
  return native_msl_view_indices_[index];
}

uint32_t MetalCommandProcessor::GetNativeMslSamplerBindlessIndex(
    uint32_t index) const {
  return index < kNativeMslSamplerHeapSize ? index : UINT32_MAX;
}

void MetalCommandProcessor::SetNativeMslSamplerBindlessState(
    uint32_t index, MTL::SamplerState* sampler) {
  if (!native_msl_argument_heaps_ready_ || index >= kSamplerBindlessHeapSize ||
      !native_msl_sampler_heap_encoder_ || !sampler) {
    return;
  }
  native_msl_sampler_heap_encoder_->setSamplerState(sampler, index);
}

void MetalCommandProcessor::ClearNativeMslSamplerBindlessState(uint32_t index) {
  if (!native_msl_argument_heaps_ready_ || index >= kSamplerBindlessHeapSize ||
      !native_msl_sampler_heap_encoder_) {
    return;
  }
  native_msl_sampler_heap_encoder_->setSamplerState(nullptr, index);
}

uint32_t MetalCommandProcessor::GetOrAllocateNativeMslViewBindlessIndex(
    uint32_t index) {
  if (!native_msl_argument_heaps_ready_ || index >= kViewBindlessHeapSize) {
    return UINT32_MAX;
  }
  if (native_msl_view_indices_.size() != kViewBindlessHeapSize) {
    native_msl_view_indices_.assign(kViewBindlessHeapSize, UINT32_MAX);
  }
  uint32_t native_index = native_msl_view_indices_[index];
  if (native_index != UINT32_MAX) {
    return native_index;
  }
  if (!native_msl_view_index_free_.empty()) {
    native_index = native_msl_view_index_free_.back();
    native_msl_view_index_free_.pop_back();
  } else {
    if (native_msl_view_index_next_ >= kNativeMslTextureHeapSize) {
      if (!native_msl_view_index_exhausted_logged_) {
        XELOGE(
            "Native MSL texture argument heap exhausted ({} slots, global "
            "bindless index {})",
            kNativeMslTextureHeapSize, index);
        native_msl_view_index_exhausted_logged_ = true;
      }
      return UINT32_MAX;
    }
    native_index = native_msl_view_index_next_++;
  }
  native_msl_view_indices_[index] = native_index;
  native_msl_view_index_exhausted_logged_ = false;
  return native_index;
}

void MetalCommandProcessor::FreeNativeMslViewBindlessIndex(uint32_t index) {
  if (index >= native_msl_view_indices_.size()) {
    return;
  }
  const uint32_t native_index = native_msl_view_indices_[index];
  if (native_index == UINT32_MAX) {
    return;
  }
  native_msl_view_indices_[index] = UINT32_MAX;
  native_msl_view_index_free_.push_back(native_index);
  native_msl_view_index_exhausted_logged_ = false;
}

void MetalCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking,
                                            nullptr);
  if (pipeline_cache_) {
    pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
  }
  if (completion_callback) {
    completion_callback();
  }
}

void MetalCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  ProcessCompletedSubmissions();
  saw_swap_ = true;
  ++backend_telemetry_.swaps;
  last_swap_ptr_ = frontbuffer_ptr;
  last_swap_width_ = frontbuffer_width;
  last_swap_height_ = frontbuffer_height;

  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kSwap)) {
    XELOGE("Metal IssueSwap: failed to flush prepared draw queue");
  }
  TryTrimPreparedDrawRetainedStorage();

  // End any active render encoder
  EndRenderEncoder(RenderEncoderEndReason::kSwap);
  EndSharedMemoryUploadBlitEncoder(SharedMemoryUploadEncoderEndReason::kSwap);
  if (texture_cache_ &&
      !texture_cache_->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
    XELOGE("Metal: failed to flush texture upload encoder before swap");
  }

  // Submit and wait for command buffer
  if (current_command_buffer_) {
    current_command_buffer_->commit();
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    submission_has_draws_ = false;
  }
  DrainCommandBufferAutoreleasePool();

  CloseFrameLifetime();
  // Proactive descriptor-pressure trimming: at frame boundaries the GPU has
  // likely completed prior submissions, so retired descriptor indices can be
  // reclaimed and the texture cache can be trimmed before we start the next
  // frame under pressure.  The high-water mark (75% utilization) gives
  // headroom so that mid-frame allocation bursts don't immediately exhaust
  // the heap.
  if (texture_cache_) {
    constexpr uint32_t kDescriptorPressureThreshold =
        kViewBindlessHeapSize / 4;  // trim when < 25% free
    uint32_t available = GetViewBindlessHeapAvailableCount();
    if (available < kDescriptorPressureThreshold) {
      texture_cache_->TrimViewBindlessPressure(kDescriptorPressureThreshold);
    }
  }

  if (shared_memory_ && ::cvars::clear_memory_page_state) {
    shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
  }

  // Push the rendered frame to the presenter's guest output mailbox.
  // This is required for trace dumps to capture the output via the
  // MetalRenderTargetCache color target (like D3D12).
  auto* presenter =
      static_cast<ui::metal::MetalPresenter*>(graphics_system_->presenter());
  if (presenter && render_target_cache_) {
    uint32_t output_width = frontbuffer_width ? frontbuffer_width : 1280;
    uint32_t output_height = frontbuffer_height ? frontbuffer_height : 720;

    MTL::Texture* source_texture = nullptr;
    bool use_pwl_gamma_ramp = false;
    if (texture_cache_) {
      uint32_t swap_width = 0;
      uint32_t swap_height = 0;
      xenos::TextureFormat swap_format = xenos::TextureFormat::k_8_8_8_8;
      source_texture = texture_cache_->RequestSwapTexture(
          swap_width, swap_height, swap_format);
      if (source_texture) {
        output_width = swap_width;
        output_height = swap_height;
        use_pwl_gamma_ramp =
            swap_format == xenos::TextureFormat::k_2_10_10_10 ||
            swap_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        if (presenter) {
          if (!gamma_ramp_256_entry_table_up_to_date_ ||
              !gamma_ramp_pwl_up_to_date_) {
            constexpr size_t kGammaRampTableBytes =
                sizeof(reg::DC_LUT_30_COLOR) * 256;
            constexpr size_t kGammaRampPwlBytes =
                sizeof(reg::DC_LUT_PWL_DATA) * 128 * 3;
            if (presenter->UpdateGammaRamp(
                    gamma_ramp_256_entry_table(), kGammaRampTableBytes,
                    gamma_ramp_pwl_rgb(), kGammaRampPwlBytes)) {
              gamma_ramp_256_entry_table_up_to_date_ = true;
              gamma_ramp_pwl_up_to_date_ = true;
            } else {
              XELOGW("Metal IssueSwap: gamma ramp upload failed");
            }
          }
        }
      }
    }

    bool swap_dest_swap = false;
    const bool has_swap_dest_swap =
        ConsumeSwapDestSwap(frontbuffer_ptr, &swap_dest_swap);
    bool force_swap_rb = has_swap_dest_swap && swap_dest_swap;

    if (!source_texture) {
      static bool missing_swap_logged = false;
      if (!missing_swap_logged) {
        missing_swap_logged = true;
        XELOGW(
            "MetalCommandProcessor::IssueSwap: swap texture unavailable; "
            "presenting inactive (black) output");
      }
      presenter->RefreshGuestOutput(
          0, 0, 0, 0, [](ui::Presenter::GuestOutputRefreshContext&) -> bool {
            return false;
          });
      MaybeDumpBackendTelemetry("swap");
      return;
    }

    if (source_texture) {
      ui::metal::MetalPresenter* metal_presenter = presenter;
      uint32_t source_width = output_width;
      uint32_t source_height = output_height;
      bool force_swap_rb_copy = force_swap_rb;
      bool use_pwl_gamma_ramp_copy = use_pwl_gamma_ramp;
      auto aspect = graphics_system_->GetScaledAspectRatio();
      presenter->RefreshGuestOutput(
          output_width, output_height, aspect.first, aspect.second,
          [source_texture, metal_presenter, source_width, source_height,
           force_swap_rb_copy, use_pwl_gamma_ramp_copy](
              ui::Presenter::GuestOutputRefreshContext& context) -> bool {
            auto& metal_context =
                static_cast<ui::metal::MetalGuestOutputRefreshContext&>(
                    context);
            context.SetIs8bpc(!use_pwl_gamma_ramp_copy);
            uint64_t submission_id = 0;
            bool copy_success = metal_presenter->CopyTextureToGuestOutput(
                source_texture, metal_context.resource_uav_capable(),
                source_width, source_height, force_swap_rb_copy,
                use_pwl_gamma_ramp_copy, &submission_id);
            if (copy_success && submission_id) {
              metal_context.SetSubmissionId(submission_id);
            }
            return copy_success;
          });
    }
  }
  MaybeDumpBackendTelemetry("swap");
}

void MetalCommandProcessor::OnPrimaryBufferEnd() {
  if (!current_command_buffer_) {
    return;
  }

  // Pump any completed resolves now since the guest is likely about to poll.
  PumpQueryResolves();
  PumpPendingRetire();

  if (!cvars::submit_on_primary_buffer_end) {
    return;
  }

  if (!CanEndSubmissionImmediately()) {
    return;
  }
  EndCommandBuffer();
}

bool MetalCommandProcessor::CanEndSubmissionImmediately() {
  if (!current_command_buffer_) {
    return false;
  }
  if (pipeline_cache_ && pipeline_cache_->IsCreatingPipelines()) {
    return false;
  }
  return true;
}

// ============================================================================
// ZPD (occlusion query) backend overrides.
// ============================================================================

void MetalCommandProcessor::EnsureZPDQueryResources() {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_visibility_pool_) {
    return;
  }

  zpd_visibility_pool_->EnsureInitialized(device_, kZPDQueryPoolCapacity);
}

void MetalCommandProcessor::ShutdownZPDQueryResources() {
  if (!zpd_visibility_pool_) {
    return;
  }
  zpd_resolves_in_flight_.clear();
  zpd_active_query_.Reset();
  zpd_visibility_pool_->Shutdown();
}

bool MetalCommandProcessor::IsZPDQueryPoolReady() const {
  return zpd_visibility_pool_ && zpd_visibility_pool_->is_initialized();
}

bool MetalCommandProcessor::CanOpenZPDQuery() const {
  // Metal visibility queries can only be enabled on a render encoder whose
  // descriptor had visibilityResultBuffer set before the encoder was created.
  return current_command_buffer_ != nullptr &&
         current_render_encoder_ != nullptr &&
         current_render_encoder_has_zpd_visibility_;
}

CommandProcessor::QueryOpenResult MetalCommandProcessor::OpenZPDQuery(
    ReportHandle report_handle, bool can_close_submission) {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kQuery)) {
    return QueryOpenResult::kFailed;
  }
  if (!zpd_visibility_pool_ || !zpd_visibility_pool_->is_initialized()) {
    return QueryOpenResult::kFailed;
  }
  if (!CanOpenZPDQuery()) {
    return QueryOpenResult::kDeferred;
  }

  bool is_pool_exhausted = !zpd_visibility_pool_->has_free_indices();

  if (is_pool_exhausted) {
    PumpQueryResolves();
    is_pool_exhausted = !zpd_visibility_pool_->has_free_indices();
  }

  bool waited_for_submission = false;

  if (is_pool_exhausted) {
    if (GetZPDMode() == ZPDMode::kFast) {
      return QueryOpenResult::kPoolExhausted;
    }

    uint64_t wait_for = 0;
    if (!zpd_resolves_in_flight_.empty()) {
      wait_for = zpd_resolves_in_flight_.front().submission;
    }

    uint64_t completed_submission = GetCompletedSubmission();
    if (wait_for > completed_submission) {
      if (wait_for >= GetCurrentSubmission()) {
        if (can_close_submission) {
          EndRenderEncoder(RenderEncoderEndReason::kUnknown);
          EndCommandBuffer();
        }
        return QueryOpenResult::kDeferred;
      }

      if (cvars::occlusion_query_log) {
        XELOGI("ZPD: Stall awaiting submission={} completed_before={}",
               wait_for, completed_submission);
      }

      // Wait for the oldest pending resolve's submission to complete.
      {
        std::unique_lock<std::mutex> lock(completion_mutex_);
        completion_cond_.wait(lock, [&]() {
          return completed_command_buffers_.load(std::memory_order_acquire) >=
                 wait_for;
        });
      }
      waited_for_submission = true;
      PumpQueryResolves();
      is_pool_exhausted = !zpd_visibility_pool_->has_free_indices();
    }
  }

  if (is_pool_exhausted) {
    return waited_for_submission ? QueryOpenResult::kPoolExhausted
                                 : QueryOpenResult::kDeferred;
  }

  MetalZPDActiveQuery active_query;
  if (!zpd_visibility_pool_->Acquire(
          active_query.index, active_query.generation, active_query.offset)) {
    return QueryOpenResult::kFailed;
  }

  current_render_encoder_->setVisibilityResultMode(
      MTL::VisibilityResultModeCounting, active_query.offset);
  zpd_active_query_ = active_query;
  return QueryOpenResult::kOpened;
}

bool MetalCommandProcessor::CloseZPDQuery(ReportHandle report_handle,
                                          uint64_t& out_submission) {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kQuery)) {
    return false;
  }
  if (!current_render_encoder_ || !current_render_encoder_has_zpd_visibility_ ||
      !zpd_active_query_.is_open()) {
    return false;
  }

  // Disable visibility counting.
  current_render_encoder_->setVisibilityResultMode(
      MTL::VisibilityResultModeDisabled, 0);

  MetalZPDResolve resolve;
  resolve.submission = GetCurrentSubmission();
  resolve.index = zpd_active_query_.index;
  resolve.generation = zpd_active_query_.generation;
  resolve.report_handle = report_handle;
  zpd_resolves_in_flight_.push_back(resolve);

  out_submission = resolve.submission;

  zpd_active_query_.Reset();
  return true;
}

bool MetalCommandProcessor::DiscardZPDQuery() {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kQuery)) {
    return false;
  }
  if (!zpd_visibility_pool_ || !zpd_active_query_.is_open()) {
    return false;
  }

  if (current_render_encoder_ && current_render_encoder_has_zpd_visibility_) {
    current_render_encoder_->setVisibilityResultMode(
        MTL::VisibilityResultModeDisabled, 0);
  }

  // The offset was used in this render pass and Metal only allows an offset to
  // be selected once per pass. Retire the slot after the submission completes
  // instead of making it immediately available for reuse.
  MetalZPDResolve resolve;
  resolve.submission = GetCurrentSubmission();
  resolve.index = zpd_active_query_.index;
  resolve.generation = zpd_active_query_.generation;
  resolve.report_handle = kInvalidReportHandle;
  zpd_resolves_in_flight_.push_back(resolve);

  zpd_active_query_.Reset();
  return true;
}

void MetalCommandProcessor::PumpQueryResolves() {
  if (!zpd_visibility_pool_) {
    return;
  }

  uint64_t completed = GetCompletedSubmission();
  if (completed == 0) {
    return;
  }

  while (!zpd_resolves_in_flight_.empty()) {
    if (zpd_resolves_in_flight_.front().submission > completed) {
      break;
    }
    MetalZPDResolve resolve = zpd_resolves_in_flight_.front();
    zpd_resolves_in_flight_.pop_front();

    if (zpd_visibility_pool_->IsGenerationCurrent(resolve.index,
                                                  resolve.generation)) {
      uint64_t raw_samples = zpd_visibility_pool_->Read(resolve.index);
      zpd_visibility_pool_->Release(resolve.index, resolve.generation);
      if (resolve.report_handle != kInvalidReportHandle) {
        OnZPDQueryResolved(resolve.report_handle, raw_samples);
      }
    } else {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD/Metal: Dropping stale query index={} generation={} "
            "handle={}",
            resolve.index, resolve.generation, resolve.report_handle);
      }
    }
  }
}

bool MetalCommandProcessor::AwaitQueryResolve(ReportHandle report_handle,
                                              uint64_t wait_for_submission) {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kQuery)) {
    return false;
  }
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  PumpQueryResolves();

  auto it = logical_zpd_reports_.find(report_handle);
  if (it == logical_zpd_reports_.end()) {
    return true;
  }
  if (it->second.pending_segments == 0 && it->second.ended) {
    return true;
  }
  if (wait_for_submission == 0) {
    return false;
  }

  // Ensure the submission is flushed.
  if (wait_for_submission >= GetCurrentSubmission()) {
    if (!current_command_buffer_) {
      return false;
    }
    // If async pipeline creation is in progress, we can't end the submission
    // cleanly. Return false and let PumpPendingRetire handle the stall limit —
    // it will abandon the report and write a cached delta if needed.
    if (!CanEndSubmissionImmediately()) {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD/Metal: AwaitQueryResolve cannot end submission "
            "(pipelines creating), deferring");
      }
      return false;
    }
    EndRenderEncoder(RenderEncoderEndReason::kUnknown);
    EndCommandBuffer();
  }

  if (wait_for_submission > GetCompletedSubmission()) {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    completion_cond_.wait(lock, [&]() {
      return completed_command_buffers_.load(std::memory_order_acquire) >=
             wait_for_submission;
    });
  }

  PumpQueryResolves();

  it = logical_zpd_reports_.find(report_handle);
  return it == logical_zpd_reports_.end() ||
         (it->second.pending_segments == 0 && it->second.ended);
}

void MetalCommandProcessor::MarkSharedMemoryComputeWritePending(
    uint32_t address, uint32_t length, MTL::ComputeCommandEncoder* encoder) {
  if (!length) {
    return;
  }
  if (shared_memory_fence_ && encoder) {
    encoder->updateFence(shared_memory_fence_);
    MarkSharedMemoryWritePending(address, length, MTL::RenderStages(0), false,
                                 true);
    return;
  }
  XELOGE("MetalCommandProcessor: shared-memory compute write cannot be fenced");
  MarkSharedMemoryWritePending(address, length, MTL::RenderStages(0), false,
                               false);
}

void MetalCommandProcessor::MarkSharedMemoryRenderWritePending(
    uint32_t address, uint32_t length, MTL::RenderStages stages) {
  if (!length) {
    return;
  }
  if (current_render_encoder_ && NS::UInteger(stages)) {
    active_render_encoder_shared_memory_write_stages_ = MTL::RenderStages(
        NS::UInteger(active_render_encoder_shared_memory_write_stages_) |
        NS::UInteger(stages));
    MarkSharedMemoryWritePending(address, length, stages, true, false);
    return;
  }
  XELOGE("MetalCommandProcessor: shared-memory render write cannot be fenced");
  MarkSharedMemoryWritePending(address, length, MTL::RenderStages(0), false,
                               false);
}

bool MetalCommandProcessor::PrepareSharedMemoryComputeReadDependency(
    const SharedMemoryRange* ranges, uint32_t range_count,
    bool consumer_can_join_current_submission,
    SharedMemoryReadDependency* dependency_out) {
  if (dependency_out) {
    *dependency_out = {};
  }
  if (!ranges || !range_count ||
      !PendingSharedMemoryWritesOverlapRanges(ranges, range_count)) {
    return true;
  }

  if (current_render_encoder_) {
    EndRenderEncoder(RenderEncoderEndReason::kSharedMemoryReadDependency);
  }

  bool needs_fence_wait = false;
  bool needs_submission_boundary = false;
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (!PendingSharedMemoryWriteOverlapsRanges(pending, ranges, range_count)) {
      continue;
    }
    if (pending.fence_updated && shared_memory_fence_) {
      needs_fence_wait = true;
      continue;
    }
    needs_submission_boundary = true;
    break;
  }

  // Ending the render encoder above may make transfer work eligible to join the
  // producer command buffer. Only skip a submission boundary when the consumer
  // can actually encode into that ordered submission; standalone consumers
  // still carry the fence wait after the producer is committed.
  const bool consumer_in_current_submission =
      consumer_can_join_current_submission &&
      CanJoinActiveSubmissionForTransfer();
  if (needs_fence_wait && dependency_out) {
    dependency_out->needs_fence_wait = true;
  }

  if (current_command_buffer_ &&
      (needs_submission_boundary ||
       (needs_fence_wait && !consumer_in_current_submission))) {
    EndCommandBuffer();
  }
  return true;
}

bool MetalCommandProcessor::EncodeSharedMemoryComputeReadDependency(
    MTL::ComputeCommandEncoder* encoder,
    const SharedMemoryReadDependency& dependency,
    const SharedMemoryRange* ranges, uint32_t range_count) {
  if (!dependency.needs_fence_wait) {
    return true;
  }
  if (!encoder || !shared_memory_fence_) {
    XELOGE(
        "MetalCommandProcessor: compute read cannot wait for shared-memory GPU "
        "writes without a fence");
    return false;
  }

  encoder->waitForFence(shared_memory_fence_);
  RetireFenceWaitedSharedMemoryWrites(ranges, range_count);
  return true;
}

void MetalCommandProcessor::MarkSharedMemoryWritePending(
    uint32_t address, uint32_t length, MTL::RenderStages producer_stages,
    bool active_render_encoder, bool fence_updated) {
  if (!length) {
    return;
  }

  constexpr uint32_t kSharedMemoryMask = SharedMemory::kBufferSize - 1;
  uint32_t remaining = std::min(length, SharedMemory::kBufferSize);
  uint32_t start = address & kSharedMemoryMask;
  while (remaining) {
    uint32_t segment_length =
        std::min(remaining, SharedMemory::kBufferSize - start);
    if (segment_length) {
      PendingSharedMemoryWrite pending = {};
      pending.start = start;
      pending.end = start + segment_length;
      pending.submission_id = GetCurrentSubmission();
      pending.producer_stages = producer_stages;
      pending.active_render_encoder = active_render_encoder;
      pending.fence_updated = fence_updated;
      pending_shared_memory_writes_.push_back(pending);
    }
    remaining -= segment_length;
    start = 0;
  }

  if (pending_shared_memory_writes_.size() <= kMaxPendingSharedMemoryWrites) {
    return;
  }

  PendingSharedMemoryWrite collapsed = {};
  collapsed.start = 0;
  collapsed.end = SharedMemory::kBufferSize;
  collapsed.submission_id = GetCurrentSubmission();
  collapsed.fence_updated = true;
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    collapsed.submission_id =
        std::max(collapsed.submission_id, pending.submission_id);
    collapsed.producer_stages =
        MTL::RenderStages(NS::UInteger(collapsed.producer_stages) |
                          NS::UInteger(pending.producer_stages));
    collapsed.active_render_encoder =
        collapsed.active_render_encoder || pending.active_render_encoder;
    collapsed.fence_updated = collapsed.fence_updated && pending.fence_updated;
  }
  if (collapsed.active_render_encoder) {
    collapsed.fence_updated = false;
  }
  pending_shared_memory_writes_.assign(1, collapsed);
}

bool MetalCommandProcessor::PendingSharedMemoryWritesOverlapRange(
    uint32_t start, uint32_t length) const {
  SharedMemoryRange range = {start, length};
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (PendingSharedMemoryWriteOverlapsRanges(pending, &range, 1)) {
      return true;
    }
  }
  return false;
}

bool MetalCommandProcessor::PendingSharedMemoryWriteOverlapsRanges(
    const PendingSharedMemoryWrite& pending, const SharedMemoryRange* ranges,
    uint32_t range_count) const {
  if (!ranges || !range_count || pending.end <= pending.start) {
    return false;
  }
  for (uint32_t i = 0; i < range_count; ++i) {
    if (SharedMemoryRangeOverlapsSegment(ranges[i], pending.start,
                                         pending.end)) {
      return true;
    }
  }
  return false;
}

bool MetalCommandProcessor::PendingSharedMemoryWritesOverlapRanges(
    const SharedMemoryRange* ranges, uint32_t range_count) const {
  if (!ranges || !range_count || pending_shared_memory_writes_.empty()) {
    return false;
  }
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (PendingSharedMemoryWriteOverlapsRanges(pending, ranges, range_count)) {
      return true;
    }
  }
  return false;
}

void MetalCommandProcessor::UpdateSharedMemoryFenceForActiveRenderEncoder() {
  if (!current_render_encoder_ ||
      !NS::UInteger(active_render_encoder_shared_memory_write_stages_)) {
    return;
  }

  if (shared_memory_fence_) {
    current_render_encoder_->updateFence(
        shared_memory_fence_,
        active_render_encoder_shared_memory_write_stages_);
  }

  for (PendingSharedMemoryWrite& pending : pending_shared_memory_writes_) {
    if (!pending.active_render_encoder) {
      continue;
    }
    pending.active_render_encoder = false;
    pending.fence_updated = shared_memory_fence_ != nullptr;
  }
  active_render_encoder_shared_memory_write_stages_ = MTL::RenderStages(0);
}

void MetalCommandProcessor::PruneCompletedSharedMemoryWrites(
    uint64_t completed_submission) {
  pending_shared_memory_writes_.erase(
      std::remove_if(
          pending_shared_memory_writes_.begin(),
          pending_shared_memory_writes_.end(),
          [completed_submission](const PendingSharedMemoryWrite& pending) {
            return !pending.active_render_encoder &&
                   pending.submission_id <= completed_submission;
          }),
      pending_shared_memory_writes_.end());
}

void MetalCommandProcessor::RetireFenceWaitedSharedMemoryWrites(
    const SharedMemoryRange* ranges, uint32_t range_count) {
  if (!ranges || !range_count) {
    return;
  }
  std::array<PendingSharedMemoryWrite, kMaxPendingSharedMemoryWriteCapacity>
      retained_entries;
  size_t retained_count = 0;
  bool retained_overflow = false;
  auto retain_entry = [&](const PendingSharedMemoryWrite& pending) {
    if (retained_count >= retained_entries.size()) {
      retained_overflow = true;
      return;
    }
    retained_entries[retained_count++] = pending;
  };

  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (pending.active_render_encoder || !pending.fence_updated ||
        !PendingSharedMemoryWriteOverlapsRanges(pending, ranges, range_count)) {
      retain_entry(pending);
      continue;
    }

    std::array<SharedMemoryRangeSegment, kMaxSharedMemoryWaitSegmentsPerPending>
        remaining_segments;
    std::array<SharedMemoryRangeSegment, kMaxSharedMemoryWaitSegmentsPerPending>
        next_segments;
    size_t remaining_count = 1;
    remaining_segments[0] = {pending.start, pending.end};
    bool segment_overflow = false;

    for (uint32_t i = 0; i < range_count && remaining_count; ++i) {
      ForEachSharedMemoryRangeSegment(
          ranges[i], [&](SharedMemoryRangeSegment waited_segment) {
            if (segment_overflow || !remaining_count) {
              return;
            }
            size_t next_count = 0;
            auto append_next = [&](SharedMemoryRangeSegment segment) {
              if (next_count >= next_segments.size()) {
                segment_overflow = true;
                return;
              }
              next_segments[next_count++] = segment;
            };

            for (size_t segment_index = 0; segment_index < remaining_count;
                 ++segment_index) {
              const SharedMemoryRangeSegment segment =
                  remaining_segments[segment_index];
              if (waited_segment.start >= segment.end ||
                  waited_segment.end <= segment.start) {
                append_next(segment);
                continue;
              }
              if (segment.start < waited_segment.start) {
                append_next({segment.start, waited_segment.start});
              }
              if (waited_segment.end < segment.end) {
                append_next({waited_segment.end, segment.end});
              }
            }

            if (!segment_overflow) {
              for (size_t segment_index = 0; segment_index < next_count;
                   ++segment_index) {
                remaining_segments[segment_index] =
                    next_segments[segment_index];
              }
              remaining_count = next_count;
            }
          });
      if (segment_overflow) {
        break;
      }
    }

    if (segment_overflow) {
      // Preserve correctness if the range set fragments too much for the fixed
      // stack workspace. This may cause one extra future wait, but avoids heap
      // churn and never drops an unwaited byte range.
      retain_entry(pending);
      continue;
    }

    for (size_t segment_index = 0; segment_index < remaining_count;
         ++segment_index) {
      const SharedMemoryRangeSegment segment =
          remaining_segments[segment_index];
      if (segment.start >= segment.end) {
        continue;
      }
      PendingSharedMemoryWrite split_pending = pending;
      split_pending.start = segment.start;
      split_pending.end = segment.end;
      retain_entry(split_pending);
    }
  }

  if (retained_overflow) {
    return;
  }

  pending_shared_memory_writes_.assign(
      retained_entries.begin(), retained_entries.begin() + retained_count);
  if (pending_shared_memory_writes_.size() <= kMaxPendingSharedMemoryWrites) {
    return;
  }

  PendingSharedMemoryWrite collapsed = {};
  collapsed.start = 0;
  collapsed.end = SharedMemory::kBufferSize;
  collapsed.submission_id = GetCurrentSubmission();
  collapsed.fence_updated = true;
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    collapsed.submission_id =
        std::max(collapsed.submission_id, pending.submission_id);
    collapsed.producer_stages =
        MTL::RenderStages(NS::UInteger(collapsed.producer_stages) |
                          NS::UInteger(pending.producer_stages));
    collapsed.active_render_encoder =
        collapsed.active_render_encoder || pending.active_render_encoder;
    collapsed.fence_updated = collapsed.fence_updated && pending.fence_updated;
  }
  if (collapsed.active_render_encoder) {
    collapsed.fence_updated = false;
  }
  pending_shared_memory_writes_.assign(1, collapsed);
}

bool MetalCommandProcessor::EncodeSharedMemoryRenderReadDependencies(
    const SharedMemoryRange* ranges, uint32_t range_count,
    MTL::RenderStages consumer_stages) {
  if (!current_render_encoder_ || !ranges || !range_count ||
      !NS::UInteger(consumer_stages) || pending_shared_memory_writes_.empty()) {
    return true;
  }

  MTL::RenderStages active_producer_stages = MTL::RenderStages(0);
  bool needs_fence_wait = false;
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (!PendingSharedMemoryWriteOverlapsRanges(pending, ranges, range_count)) {
      continue;
    }
    if (pending.active_render_encoder) {
      active_producer_stages =
          MTL::RenderStages(NS::UInteger(active_producer_stages) |
                            NS::UInteger(pending.producer_stages));
    } else if (pending.fence_updated && shared_memory_fence_) {
      needs_fence_wait = true;
    } else {
      XELOGE(
          "MetalCommandProcessor: render read cannot wait for shared-memory "
          "GPU "
          "writes without a fence");
      return false;
    }
  }

  if (NS::UInteger(active_producer_stages)) {
    current_render_encoder_->memoryBarrier(
        MTL::BarrierScopeBuffers, active_producer_stages, consumer_stages);
  }
  if (needs_fence_wait && shared_memory_fence_) {
    current_render_encoder_->waitForFence(shared_memory_fence_,
                                          consumer_stages);
    RetireFenceWaitedSharedMemoryWrites(ranges, range_count);
  }
  return true;
}

bool MetalCommandProcessor::EncodeSharedMemoryBlitReadDependency(
    MTL::BlitCommandEncoder* encoder, uint32_t start, uint32_t length) {
  if (!encoder || !length ||
      !PendingSharedMemoryWritesOverlapRange(start, length)) {
    return true;
  }
  if (!shared_memory_fence_) {
    XELOGE(
        "MetalCommandProcessor: blit read cannot wait for shared-memory GPU "
        "writes without a fence");
    return false;
  }
  encoder->waitForFence(shared_memory_fence_);
  SharedMemoryRange range = {start, length};
  RetireFenceWaitedSharedMemoryWrites(&range, 1);
  return true;
}

Shader* MetalCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                          const uint32_t* host_address,
                                          uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool MetalCommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  ++backend_telemetry_.draw_calls;
  const RegisterFile& regs = *register_file_;
  uint32_t normalized_color_mask = 0;

  // Check for copy mode
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }

  if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0) {
    // Doesn't actually draw.
    return true;
  }

  // Vertex shader analysis.
  Shader* vertex_shader = active_vertex_shader();
  if (!vertex_shader) {
    XELOGW("IssueDraw: No vertex shader");
    return false;
  }
  if (!vertex_shader->is_ucode_analyzed()) {
    vertex_shader->AnalyzeUcode(pipeline_cache_->ucode_disasm_buffer());
  }
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = active_pixel_shader();
      if (pixel_shader) {
        if (!pixel_shader->is_ucode_analyzed()) {
          pixel_shader->AnalyzeUcode(pipeline_cache_->ucode_disasm_buffer());
        }
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    if (!memexport_used_vertex) {
      return true;
    }
  }
  bool memexport_used_pixel =
      pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  // Primitive/index processing (like D3D12/Vulkan).
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_) {
    XELOGE("IssueDraw: primitive processor is not initialized");
    return false;
  }
  if (!primitive_processor_->Process(primitive_processing_result)) {
    XELOGE("IssueDraw: primitive processing failed");
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kMemExportCompute) {
    primitive_processing_result.host_vertex_shader_type =
        Shader::HostVertexShaderType::kVertex;
  }

  struct PendingDrawPassTransferGuard {
    MetalRenderTargetCache* cache = nullptr;
    bool Flush() {
      if (!cache || !cache->HasPendingDrawPassTransfers()) {
        return true;
      }
      return cache->FlushPendingDrawPassTransfers();
    }
    ~PendingDrawPassTransferGuard() { (void)Flush(); }
  } pending_draw_pass_transfer_guard;

  auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  bool use_tessellation_emulation = false;
  bool use_native_msl_tessellation = false;
  if (primitive_processing_result.IsTessellated()) {
    if (!mesh_shader_supported_) {
      static bool tess_mesh_logged = false;
      if (!tess_mesh_logged) {
        tess_mesh_logged = true;
        XELOGW(
            "Metal: Tessellation emulation requested but mesh shaders are not "
            "supported on this device");
      }
      return true;
    }
    if (!pixel_shader) {
      static bool tess_no_ps_logged = false;
      if (!tess_no_ps_logged) {
        tess_no_ps_logged = true;
        XELOGW(
            "Metal: Tessellation emulation requested without a pixel shader; "
            "using depth-only PS fallback");
      }
    }
    use_tessellation_emulation = true;
    use_native_msl_tessellation =
        cvars::metal_native_msl_render && !cvars::metal_native_msl_helper_msc;
  }

  // Configure render targets via MetalRenderTargetCache, similar to D3D12.
  // Update() may internally call PerformTransfersAndResolveClears for EDRAM
  // ownership transfers -- this is the draw-path transfer entry point and
  // no transfer operations bypass the render target cache.
  PreparedDrawRenderTargetKey render_target_key = {};
  if (render_target_cache_) {
    uint32_t ps_writes_color_targets =
        pixel_shader ? pixel_shader->writes_color_targets() : 0;
    normalized_color_mask = pixel_shader ? draw_util::GetNormalizedColorMask(
                                               regs, ps_writes_color_targets)
                                         : 0;
    render_target_key = BuildPreparedDrawRenderTargetKey(
        regs, is_rasterization_done, normalized_depth_control,
        normalized_color_mask);
    if (!prepared_draw_queue_.empty()) {
      if (!prepared_draw_queue_render_target_key_valid_) {
        if (!FlushPreparedDrawQueue(
                PreparedDrawFlushReason::kRenderTargetUpdate)) {
          return false;
        }
      } else if (render_target_key != prepared_draw_queue_render_target_key_) {
        if (!FlushPreparedDrawQueue(
                PreparedDrawFlushReason::kRenderTargetKeyMismatch)) {
          return false;
        }
      }
    }
    if (!render_target_cache_->Update(is_rasterization_done,
                                      normalized_depth_control,
                                      normalized_color_mask, *vertex_shader)) {
      XELOGE(
          "MetalCommandProcessor::IssueDraw - RenderTargetCache::Update "
          "failed");
      return false;
    }
    pending_draw_pass_transfer_guard.cache = render_target_cache_.get();
    if (current_render_encoder_ &&
        render_target_cache_->IsRenderPassDescriptorDirty() &&
        !render_target_cache_->IsRenderPassDescriptorCompatible(
            current_render_pass_descriptor_, 1)) {
      EndRenderEncoder(
          RenderEncoderEndReason::kRenderTargetUpdateDescriptorDirty);
    }
  }

  // Cast to the Metal shader subclass for translation and resource metadata.
  auto* metal_vertex_shader = static_cast<MetalShader*>(vertex_shader);
  auto* metal_pixel_shader = static_cast<MetalShader*>(pixel_shader);

  MTL::RenderPipelineState* pipeline = nullptr;
  // Select per-draw shader modifications (mirrors D3D12 PipelineCache).
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask = 0;
  if (pixel_shader) {
    interpolator_mask = vertex_shader->writes_interpolators() &
                        pixel_shader->GetInterpolatorInputMask(
                            regs.Get<reg::SQ_PROGRAM_CNTL>(),
                            regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos);
  }

  Shader::HostVertexShaderType host_vertex_shader_type_for_translation =
      primitive_processing_result.host_vertex_shader_type;
  bool host_point_list_as_triangle_strip =
      host_vertex_shader_type_for_translation ==
      Shader::HostVertexShaderType::kPointListAsTriangleStrip;
  bool host_rectangle_list_as_triangle_strip =
      host_vertex_shader_type_for_translation ==
      Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
  const bool native_primitive_mesh_supported =
      ::cvars::metal_native_msl_render && mesh_shader_supported_ &&
      !primitive_processing_result.IsTessellated();
  const bool raw_point_list_for_native_msl_mesh =
      native_primitive_mesh_supported &&
      primitive_processing_result.host_primitive_type ==
          xenos::PrimitiveType::kPointList &&
      host_vertex_shader_type_for_translation ==
          Shader::HostVertexShaderType::kVertex;
  const bool raw_rectangle_list_for_native_msl_mesh =
      native_primitive_mesh_supported &&
      primitive_processing_result.host_primitive_type ==
          xenos::PrimitiveType::kRectangleList &&
      host_vertex_shader_type_for_translation ==
          Shader::HostVertexShaderType::kVertex;
  const bool raw_quad_list_for_native_msl_mesh =
      native_primitive_mesh_supported &&
      primitive_processing_result.host_primitive_type ==
          xenos::PrimitiveType::kQuadList &&
      host_vertex_shader_type_for_translation ==
          Shader::HostVertexShaderType::kVertex;

  bool use_native_msl_primitive_mesh = false;
  MetalPipelineCache::NativeMeshPipelineType native_msl_primitive_mesh_type =
      MetalPipelineCache::NativeMeshPipelineType::kRectangleList;
  if (raw_point_list_for_native_msl_mesh ||
      (native_primitive_mesh_supported && host_point_list_as_triangle_strip)) {
    use_native_msl_primitive_mesh = true;
    native_msl_primitive_mesh_type =
        MetalPipelineCache::NativeMeshPipelineType::kPointList;
    host_vertex_shader_type_for_translation =
        Shader::HostVertexShaderType::kPointListAsMesh;
    host_point_list_as_triangle_strip = false;
  } else if (raw_rectangle_list_for_native_msl_mesh ||
             (native_primitive_mesh_supported &&
              host_rectangle_list_as_triangle_strip)) {
    use_native_msl_primitive_mesh = true;
    native_msl_primitive_mesh_type =
        MetalPipelineCache::NativeMeshPipelineType::kRectangleList;
    host_vertex_shader_type_for_translation =
        Shader::HostVertexShaderType::kRectangleListAsMesh;
    host_rectangle_list_as_triangle_strip = false;
  } else if (raw_quad_list_for_native_msl_mesh) {
    use_native_msl_primitive_mesh = true;
    native_msl_primitive_mesh_type =
        MetalPipelineCache::NativeMeshPipelineType::kQuadList;
    host_vertex_shader_type_for_translation =
        Shader::HostVertexShaderType::kQuadListAsMesh;
  }

  if (host_point_list_as_triangle_strip ||
      host_rectangle_list_as_triangle_strip) {
    const bool keep_native_msl_vs_expansion =
        host_point_list_as_triangle_strip && ::cvars::metal_native_msl_render;
    if (keep_native_msl_vs_expansion) {
      // Native MSL can still expand point sprites in the vertex shader on
      // devices without mesh shader support.
    } else {
      if (!mesh_shader_supported_) {
        static bool host_vs_expansion_logged = false;
        if (!host_vs_expansion_logged) {
          host_vs_expansion_logged = true;
          XELOGW(
              "Metal: Host VS expansion requested without mesh shader support; "
              "skipping draw");
        }
        return pending_draw_pass_transfer_guard.Flush();
      }
      // Geometry emulation handles point/rectangle expansion; use the normal
      // vertex shader translation path to avoid unsupported host VS types.
      host_vertex_shader_type_for_translation =
          Shader::HostVertexShaderType::kVertex;
    }
  }
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, host_vertex_shader_type_for_translation,
          interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                         *pixel_shader, interpolator_mask, ps_param_gen_pos,
                         normalized_depth_control)
                   : DxbcShaderTranslator::Modification(0);

  PipelineGeometryShader geometry_shader_type = PipelineGeometryShader::kNone;
  if (!primitive_processing_result.IsTessellated()) {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        if (!use_native_msl_primitive_mesh) {
          geometry_shader_type = PipelineGeometryShader::kPointList;
        }
        break;
      case xenos::PrimitiveType::kRectangleList:
        if (!use_native_msl_primitive_mesh &&
            host_vertex_shader_type_for_translation !=
                Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
          geometry_shader_type = PipelineGeometryShader::kRectangleList;
        }
        break;
      case xenos::PrimitiveType::kQuadList:
        if (!use_native_msl_primitive_mesh) {
          geometry_shader_type = PipelineGeometryShader::kQuadList;
        }
        break;
      default:
        break;
    }
  }
  if (geometry_shader_type != PipelineGeometryShader::kNone &&
      ::cvars::metal_native_msl_render &&
      !::cvars::metal_native_msl_helper_msc) {
    static bool native_helper_disabled_geometry_logged = false;
    if (!native_helper_disabled_geometry_logged) {
      native_helper_disabled_geometry_logged = true;
      XELOGE(
          "Metal: native-only geometry draw reached a generated helper class; "
          "skipping draw instead of using the disabled helper island");
    }
    return pending_draw_pass_transfer_guard.Flush();
  }

  GeometryShaderKey geometry_shader_key;
  bool use_geometry_emulation = false;
  if (geometry_shader_type != PipelineGeometryShader::kNone) {
    bool can_build_geometry_shader =
        pixel_shader || !vertex_shader_modification.vertex.interpolator_mask;
    if (!can_build_geometry_shader) {
      static bool geom_interp_mismatch_logged = false;
      if (!geom_interp_mismatch_logged) {
        geom_interp_mismatch_logged = true;
        XELOGW(
            "Metal: geometry emulation skipped because pixel shader is null "
            "but vertex interpolators are present");
      }
    } else {
      use_geometry_emulation =
          GetGeometryShaderKey(geometry_shader_type, vertex_shader_modification,
                               pixel_shader_modification, geometry_shader_key);
    }
  }
  if (use_geometry_emulation && !mesh_shader_supported_) {
    static bool mesh_support_logged = false;
    if (!mesh_support_logged) {
      mesh_support_logged = true;
      XELOGW(
          "Metal: geometry emulation requested but mesh shaders are not "
          "supported on this device");
    }
    use_geometry_emulation = false;
  }
  if (use_geometry_emulation && !pixel_shader) {
    static bool geom_no_ps_logged = false;
    if (!geom_no_ps_logged) {
      geom_no_ps_logged = true;
      XELOGW(
          "Metal: geometry emulation requested without a pixel shader; using "
          "depth-only PS fallback");
    }
  }

  bool use_helper_stage_pipeline =
      (use_tessellation_emulation && !use_native_msl_tessellation) ||
      use_geometry_emulation;
  if (use_helper_stage_pipeline) {
    vertex_shader_modification =
        pipeline_cache_->GetCurrentVertexShaderModification(
            *vertex_shader, host_vertex_shader_type_for_translation,
            interpolator_mask, /*native_guest_allowed=*/false);
    pixel_shader_modification =
        pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                           *pixel_shader, interpolator_mask, ps_param_gen_pos,
                           normalized_depth_control,
                           /*native_guest_allowed=*/false)
                     : DxbcShaderTranslator::Modification(0);
    if (use_geometry_emulation) {
      use_geometry_emulation =
          GetGeometryShaderKey(geometry_shader_type, vertex_shader_modification,
                               pixel_shader_modification, geometry_shader_key);
      use_helper_stage_pipeline =
          use_tessellation_emulation || use_geometry_emulation;
    }
    if (!use_helper_stage_pipeline) {
      vertex_shader_modification =
          pipeline_cache_->GetCurrentVertexShaderModification(
              *vertex_shader, host_vertex_shader_type_for_translation,
              interpolator_mask);
      pixel_shader_modification =
          pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                             *pixel_shader, interpolator_mask, ps_param_gen_pos,
                             normalized_depth_control)
                       : DxbcShaderTranslator::Modification(0);
    }
  }
  DxbcShaderTranslator::Modification vertex_translation_modification =
      vertex_shader_modification;
  DxbcShaderTranslator::Modification pixel_translation_modification =
      pixel_shader_modification;
  if (use_helper_stage_pipeline) {
    vertex_translation_modification.value |=
        kMetalHelperStageMscTranslationKeyBit;
    if (pixel_shader) {
      pixel_translation_modification.value |=
          kMetalHelperStageMscTranslationKeyBit;
    }
  }
  const bool use_native_msl_guest_translation =
      ::cvars::metal_native_msl_render && !use_helper_stage_pipeline;
  NativeMslTextureSignVariant vertex_texture_sign_variant = {};
  NativeMslTextureSignVariant pixel_texture_sign_variant = {};
  auto get_native_msl_texture_sign_variant = [&](size_t stage,
                                                 const Shader& shader) {
    NativeMslTextureSignVariantCache& cache =
        native_msl_texture_sign_variant_cache_[stage];
    const uint64_t input_key = NativeMslTextureSignInputKey(regs, shader);
    if (cache.shader == &shader && cache.input_key == input_key) {
      NativeMslTextureSignVariant variant = {};
      variant.key = cache.variant_key;
      variant.component_masks = cache.component_masks;
      variant.sign_values = cache.sign_values;
      return variant;
    }
    NativeMslTextureSignVariant variant =
        BuildNativeMslTextureSignVariant(regs, shader);
    cache.shader = &shader;
    cache.input_key = input_key;
    cache.variant_key = variant.key;
    cache.component_masks = variant.component_masks;
    cache.sign_values = variant.sign_values;
    return variant;
  };
  if (use_native_msl_guest_translation) {
    vertex_texture_sign_variant =
        get_native_msl_texture_sign_variant(kStageVertex, *vertex_shader);
    if (pixel_shader) {
      pixel_texture_sign_variant =
          get_native_msl_texture_sign_variant(kStagePixel, *pixel_shader);
    }
  }
  const uint64_t vertex_translation_key =
      use_native_msl_guest_translation
          ? NativeMslTranslationCacheKey(vertex_translation_modification.value,
                                         vertex_texture_sign_variant.key)
          : vertex_translation_modification.value;
  const uint64_t pixel_translation_key =
      (pixel_shader && use_native_msl_guest_translation)
          ? NativeMslTranslationCacheKey(pixel_translation_modification.value,
                                         pixel_texture_sign_variant.key)
          : pixel_translation_modification.value;

  // Get or create shader translations for the selected modifications.
  auto vertex_translation = static_cast<MetalShader::MetalTranslation*>(
      vertex_shader->GetOrCreateTranslation(vertex_translation_key));

  MetalShader::MetalTranslation* pixel_translation = nullptr;
  if (pixel_shader) {
    pixel_translation = static_cast<MetalShader::MetalTranslation*>(
        pixel_shader->GetOrCreateTranslation(pixel_translation_key));
  }

  if (use_native_msl_guest_translation) {
    if (!vertex_translation->ConfigureNativeMslVariant(
            vertex_translation_modification.value,
            vertex_texture_sign_variant.key,
            vertex_texture_sign_variant.component_masks,
            vertex_texture_sign_variant.sign_values)) {
      XELOGE(
          "metal_native_msl: vertex shader {:016X} texture-sign variant key "
          "collision",
          vertex_shader->ucode_data_hash());
      return false;
    }
    if (pixel_translation && !pixel_translation->ConfigureNativeMslVariant(
                                 pixel_translation_modification.value,
                                 pixel_texture_sign_variant.key,
                                 pixel_texture_sign_variant.component_masks,
                                 pixel_texture_sign_variant.sign_values)) {
      XELOGE(
          "metal_native_msl: pixel shader {:016X} texture-sign variant key "
          "collision",
          pixel_shader->ucode_data_hash());
      return false;
    }
  }

  if (use_helper_stage_pipeline) {
    if (!pipeline_cache_->EnsureDxilTranslationReady(
            vertex_translation,
            use_tessellation_emulation ? "tessellation helper domain"
                                       : "geometry helper vertex",
            /*helper_stage_pipeline=*/true)) {
      return false;
    }
    if (!pipeline_cache_->EnsureMetalTranslationReady(
            pixel_translation, /*native_guest_allowed=*/false,
            use_tessellation_emulation ? "tessellation helper pixel"
                                       : "geometry helper pixel",
            /*helper_stage_pipeline=*/true)) {
      return false;
    }
  }
  if (use_native_msl_tessellation) {
    if (!pipeline_cache_->EnsureNativeMslTranslationReady(
            vertex_translation, "native tessellation domain")) {
      return false;
    }
    if (pixel_translation &&
        !pipeline_cache_->EnsureNativeMslTranslationReady(
            pixel_translation, "native tessellation pixel")) {
      return false;
    }
  }
  if (use_native_msl_primitive_mesh) {
    if (!pipeline_cache_->EnsureNativeMslTranslationReady(
            vertex_translation, "native primitive mesh vertex")) {
      return false;
    }
    if (pixel_translation &&
        !pipeline_cache_->EnsureNativeMslTranslationReady(
            pixel_translation, "native primitive mesh pixel")) {
      return false;
    }
  }

  bool use_fallback_ps =
      (use_tessellation_emulation || use_geometry_emulation ||
       use_native_msl_primitive_mesh) &&
      !pixel_translation;

  // Resolve attachment formats once for all pipeline paths.
  bool pixel_shader_writes_depth_for_fmts =
      pixel_translation && pixel_translation->shader().writes_depth();
  if (use_fallback_ps) {
    pixel_shader_writes_depth_for_fmts = true;  // depth-only PS fallback
  }
  bool fallback_depth_attachment_required = pixel_shader_writes_depth_for_fmts;
  if (use_fallback_ps && render_target_cache_ &&
      !render_target_cache_->GetDepthTargetForDraw() &&
      !memexport_used_vertex) {
    static bool fallback_without_depth_logged = false;
    if (!fallback_without_depth_logged) {
      fallback_without_depth_logged = true;
      XELOGW(
          "Metal: depth-only fallback PS requested without a depth attachment "
          "or memexport side effects; skipping no-output emulated draw");
    }
    return pending_draw_pass_transfer_guard.Flush();
  }
  if (render_target_cache_) {
    render_target_key = BuildPreparedDrawRenderTargetKey(
        regs, is_rasterization_done, normalized_depth_control,
        normalized_color_mask);
  }
  if (current_render_encoder_ && render_target_cache_ &&
      !render_target_cache_->IsRenderPassDescriptorCompatible(
          current_render_pass_descriptor_, 1,
          fallback_depth_attachment_required)) {
    EndRenderEncoder(RenderEncoderEndReason::kPipelineDescriptorIncompatible);
  }
  MTL::RenderPassDescriptor* pass_desc_for_fmts =
      current_render_pass_descriptor_;
  if (render_target_cache_) {
    if (MTL::RenderPassDescriptor* cache_desc =
            render_target_cache_->GetRenderPassDescriptor(
                1, fallback_depth_attachment_required)) {
      pass_desc_for_fmts = cache_desc;
    }
  }
  auto attachment_formats = ResolvePipelineAttachmentFormats(
      render_target_cache_.get(), pass_desc_for_fmts,
      pixel_shader_writes_depth_for_fmts, "Pipeline");

  // Derive the shared rendering key (color mask, blend, alpha-to-mask) once
  // for all pipeline paths instead of re-reading registers in each method.
  auto rendering_key =
      ResolvePipelineRenderingKey(regs, pixel_translation, use_fallback_ps);

  MetalPipelineCache::TessellationPipelineState* tessellation_pipeline_state =
      nullptr;
  MetalPipelineCache::GeometryPipelineState* geometry_pipeline_state = nullptr;
  MetalPipelineCache::NativeMeshPipelineState* native_mesh_pipeline_state =
      nullptr;
  if (use_native_msl_primitive_mesh) {
    native_mesh_pipeline_state =
        pipeline_cache_->GetOrCreateNativeMslPrimitiveMeshPipelineState(
            vertex_translation, pixel_translation,
            native_msl_primitive_mesh_type, attachment_formats, rendering_key);
    pipeline = native_mesh_pipeline_state ? native_mesh_pipeline_state->pipeline
                                          : nullptr;
    if (!pipeline) {
      XELOGW(
          "Metal: native MSL primitive mesh pipeline creation failed; "
          "skipping draw");
      return pending_draw_pass_transfer_guard.Flush();
    }
  } else if (use_native_msl_tessellation) {
    tessellation_pipeline_state =
        pipeline_cache_->GetOrCreateNativeMslTessellationPipelineState(
            vertex_translation, pixel_translation, primitive_processing_result,
            attachment_formats, rendering_key);
    pipeline = tessellation_pipeline_state
                   ? tessellation_pipeline_state->pipeline
                   : nullptr;
    if (!pipeline) {
      static bool native_tessellation_pipeline_failure_logged = false;
      if (!native_tessellation_pipeline_failure_logged) {
        native_tessellation_pipeline_failure_logged = true;
        XELOGW(
            "Metal: native MSL tessellation pipeline creation failed; "
            "skipping tessellation-emulated draws until native helper object/"
            "mesh generation is implemented");
      }
      return pending_draw_pass_transfer_guard.Flush();
    }
  } else if (use_tessellation_emulation) {
    tessellation_pipeline_state =
        pipeline_cache_->GetOrCreateTessellationPipelineState(
            vertex_translation, pixel_translation, primitive_processing_result,
            attachment_formats, rendering_key);
    pipeline = tessellation_pipeline_state
                   ? tessellation_pipeline_state->pipeline
                   : nullptr;
    if (!pipeline) {
      static bool tessellation_pipeline_failure_logged = false;
      if (!tessellation_pipeline_failure_logged) {
        tessellation_pipeline_failure_logged = true;
        XELOGW(
            "Metal: tessellation emulation pipeline creation failed; skipping "
            "tessellation-emulated draws instead of submitting a null "
            "pipeline");
      }
      return pending_draw_pass_transfer_guard.Flush();
    }
  } else if (use_geometry_emulation) {
    geometry_pipeline_state = pipeline_cache_->GetOrCreateGeometryPipelineState(
        vertex_translation, pixel_translation, geometry_shader_key,
        attachment_formats, rendering_key);
    if (!geometry_pipeline_state || !geometry_pipeline_state->pipeline) {
      static bool geometry_pipeline_failure_logged = false;
      if (!geometry_pipeline_failure_logged) {
        geometry_pipeline_failure_logged = true;
        XELOGW(
            "Metal: geometry emulation pipeline creation failed; skipping "
            "geometry-emulated draws instead of aborting the backend draw "
            "packet");
      }
      return pending_draw_pass_transfer_guard.Flush();
    }
    pipeline = geometry_pipeline_state->pipeline;
  } else {
    auto* pipeline_handle = pipeline_cache_->GetOrCreatePipelineState(
        vertex_translation, pixel_translation, attachment_formats,
        rendering_key);
    if (pipeline_handle) {
      pipeline = pipeline_handle->state.load(std::memory_order_acquire);
    }
    if (!pipeline) {
      if (cvars::async_shader_compilation && pipeline_handle) {
        // Pipeline is being compiled in the background -- skip this draw.
        return pending_draw_pass_transfer_guard.Flush();
      }
      XELOGE("Failed to create pipeline state");
      return false;
    }
  }

  const bool use_native_msl =
      (!use_tessellation_emulation || use_native_msl_tessellation) &&
      !use_geometry_emulation && vertex_translation &&
      vertex_translation->is_native_msl() &&
      (!pixel_translation || pixel_translation->is_native_msl());
  const DxbcShader::TranslationMetadata* vertex_translation_metadata =
      use_native_msl ? &vertex_translation->native_msl_metadata() : nullptr;
  const DxbcShader::TranslationMetadata* pixel_translation_metadata =
      use_native_msl && pixel_translation
          ? &pixel_translation->native_msl_metadata()
          : nullptr;

  if (!use_native_msl) {
    pipeline_cache_->SetupShaderBindingLayoutUserUIDs(*metal_vertex_shader);
  }
  if (!use_native_msl && metal_pixel_shader) {
    pipeline_cache_->SetupShaderBindingLayoutUserUIDs(*metal_pixel_shader);
  }

  uint32_t used_texture_mask =
      vertex_translation_metadata
          ? vertex_translation_metadata->used_texture_mask
          : metal_vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (pixel_translation_metadata) {
    used_texture_mask |= pixel_translation_metadata->used_texture_mask;
  } else if (metal_pixel_shader) {
    used_texture_mask |=
        metal_pixel_shader->GetUsedTextureMaskAfterTranslation();
  }
  uint32_t texture_request_work_mask = 0;
  if (texture_cache_ && used_texture_mask) {
    texture_request_work_mask =
        texture_cache_->GetUsedTextureRequestWorkMask(used_texture_mask);
  }
  const bool has_texture_request_work =
      texture_cache_ && used_texture_mask && texture_request_work_mask;
  const bool may_texture_request_load_data =
      has_texture_request_work &&
      texture_cache_->MayRequestTexturesLoadData(used_texture_mask);

  MTL::RenderPassDescriptor* draw_pass_descriptor =
      GetDrawRenderPassDescriptor(fallback_depth_attachment_required);

  PreparedDraw* prepared_draw = AcquirePreparedDraw();
  PreparedDraw& draw = *prepared_draw;
  auto fail_prepared_draw = [&]() {
    RecyclePreparedDraw(prepared_draw);
    return false;
  };
  MetalTextureCache::TextureMaterializationPlan& texture_materialization_plan =
      draw.texture_materialization_plan;
  texture_materialization_plan.Reset();
  bool textures_requested_for_draw = false;
  auto request_textures_for_draw = [&](uint64_t& telemetry_counter) -> bool {
    if (!EnsureCommandBuffer()) {
      return false;
    }
    if (texture_materialization_plan.NeedsTextureUpload()) {
      texture_cache_->RequestTexturesWithoutLoading(used_texture_mask);
    } else {
      texture_cache_->RequestTextures(used_texture_mask);
    }
    textures_requested_for_draw = true;
    ++telemetry_counter;
    return true;
  };
  std::array<VertexBindingRange, 32> vertex_ranges;
  uint32_t vertex_range_count = 0;
  const auto& vb_bindings = vertex_shader->vertex_bindings();
  bool uses_vertex_fetch = ShaderUsesVertexFetch(*vertex_shader);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;
  MTL::RenderStages memexport_write_stages = MTL::RenderStages(0);
  if (memexport_used_vertex) {
    if (use_native_msl_primitive_mesh) {
      memexport_write_stages = MTL::RenderStageMesh;
    } else {
      memexport_write_stages =
          (use_geometry_emulation || use_tessellation_emulation)
              ? MTL::RenderStages(MTL::RenderStageObject | MTL::RenderStageMesh)
              : MTL::RenderStageVertex;
    }
  }
  if (memexport_used_pixel && is_rasterization_done) {
    memexport_write_stages =
        MTL::RenderStages(NS::UInteger(memexport_write_stages) |
                          NS::UInteger(MTL::RenderStageFragment));
  }
  bool shared_memory_is_uav = memexport_used;
  MTL::ResourceUsage shared_memory_usage =
      shared_memory_is_uav ? (MTL::ResourceUsageRead | MTL::ResourceUsageWrite)
                           : MTL::ResourceUsageRead;
  const bool native_mesh_guest_dma_index_load =
      use_native_msl_primitive_mesh &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA;
  const bool native_tess_guest_dma_index_load =
      use_native_msl_tessellation &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA;
  const bool guest_dma_index_buffer_read =
      !use_native_msl_primitive_mesh && !native_tess_guest_dma_index_load &&
      !memexport_used &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA;
  const bool shader_primitive_index_load =
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA ||
      native_mesh_guest_dma_index_load || native_tess_guest_dma_index_load;
  std::array<SharedMemory::Range, kMaxCurrentDrawVertexFetchRanges>
      vertex_fetch_ranges;
  uint32_t vertex_fetch_range_count = 0;
  std::vector<SharedMemory::Range>& current_draw_shared_memory_ranges =
      current_draw_shared_memory_ranges_scratch_;
  current_draw_shared_memory_ranges.clear();
  current_draw_shared_memory_ranges.reserve(kMaxCurrentDrawVertexFetchRanges +
                                            memexport_ranges_.size() + 2);
  auto add_current_draw_shared_memory_range =
      [&](DrawMaterializationSource source, uint32_t start, uint32_t length) {
        if (!length) {
          return;
        }
        size_t source_index = static_cast<size_t>(source);
        if (source_index < kDrawMaterializationSourceCount) {
          ++backend_telemetry_.draw_materialization_source_ranges[source_index];
          backend_telemetry_.draw_materialization_source_bytes[source_index] +=
              length;
          if (shared_memory_ && !shared_memory_->IsRangeValid(start, length)) {
            ++backend_telemetry_
                  .draw_materialization_source_invalid_ranges[source_index];
            backend_telemetry_
                .draw_materialization_source_invalid_bytes[source_index] +=
                length;
          }
        }
        current_draw_shared_memory_ranges.push_back({start, length});
      };
  if (texture_cache_ && may_texture_request_load_data) {
    if (!texture_cache_->PrepareTextureMaterialization(
            regs, used_texture_mask, texture_materialization_plan)) {
      return fail_prepared_draw();
    }
    for (const SharedMemory::Range& range :
         texture_materialization_plan.source_ranges) {
      add_current_draw_shared_memory_range(
          DrawMaterializationSource::kTextureSource, range.start, range.length);
    }
  }

  // Sync shared memory before drawing - ensure GPU has latest data
  // This is particularly important for trace playback where memory is
  // written incrementally
  if (shared_memory_) {
    if (!CollectMetalVertexFetchSharedMemoryRanges(
            regs, *vertex_shader, vertex_fetch_ranges.data(),
            uint32_t(vertex_fetch_ranges.size()), &vertex_fetch_range_count,
            true)) {
      return fail_prepared_draw();
    }
    for (uint32_t i = 0; i < vertex_fetch_range_count; ++i) {
      add_current_draw_shared_memory_range(
          DrawMaterializationSource::kVertexFetch, vertex_fetch_ranges[i].start,
          vertex_fetch_ranges[i].length);
    }
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      uint32_t base_bytes = memexport_range.base_address_dwords << 2;
      add_current_draw_shared_memory_range(
          DrawMaterializationSource::kMemexport, base_bytes,
          memexport_range.size_bytes);
    }

    auto add_guest_index_range = [&](uint64_t index_base, uint32_t index_count,
                                     xenos::IndexFormat index_format,
                                     const char* label) -> bool {
      uint32_t index_stride = index_format == xenos::IndexFormat::kInt16
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t);
      uint64_t index_length = uint64_t(index_count) * index_stride;
      if (index_base > SharedMemory::kBufferSize ||
          SharedMemory::kBufferSize - index_base < index_length) {
        XELOGW(
            "{} index buffer range out of bounds (base=0x{:08X} size={} "
            "count={})",
            label, static_cast<uint32_t>(index_base), index_length,
            index_count);
        return false;
      }
      add_current_draw_shared_memory_range(
          DrawMaterializationSource::kGuestIndex,
          static_cast<uint32_t>(index_base),
          static_cast<uint32_t>(index_length));
      return true;
    };
    if (guest_dma_index_buffer_read &&
        !add_guest_index_range(
            primitive_processing_result.guest_index_base,
            primitive_processing_result.host_draw_vertex_count,
            primitive_processing_result.host_index_format, "guest DMA")) {
      return fail_prepared_draw();
    }
    if (shader_primitive_index_load &&
        !add_guest_index_range(
            primitive_processing_result.guest_index_base,
            primitive_processing_result.guest_draw_vertex_count,
            regs.Get<reg::VGT_DRAW_INITIATOR>().index_size,
            "shader primitive")) {
      return fail_prepared_draw();
    }

    for (const auto& binding : vb_bindings) {
      xenos::xe_gpu_vertex_fetch_t vfetch =
          regs.GetVertexFetch(binding.fetch_constant);
      uint32_t buffer_offset = vfetch.address << 2;
      uint32_t buffer_length = vfetch.size << 2;
      VertexBindingRange range;
      range.binding_index = static_cast<uint32_t>(binding.binding_index);
      range.offset = buffer_offset;
      range.length = buffer_length;
      range.stride = binding.stride_words * 4;
      assert_true(vertex_range_count < vertex_ranges.size());
      vertex_ranges[vertex_range_count++] = range;
    }
  }

  const bool current_draw_has_invalid_shared_memory =
      shared_memory_ && !current_draw_shared_memory_ranges.empty() &&
      AnySharedMemoryRangeInvalid(
          current_draw_shared_memory_ranges.data(),
          static_cast<uint32_t>(current_draw_shared_memory_ranges.size()));
  if (shared_memory_ && !current_draw_shared_memory_ranges.empty()) {
    ++backend_telemetry_.draw_materialization_per_draw_requests;
    if (current_draw_has_invalid_shared_memory) {
      ++backend_telemetry_.draw_materialization_per_draw_invalid_requests;
    } else {
      ++backend_telemetry_.draw_materialization_per_draw_resident_skips;
    }
  }

  if (has_texture_request_work && !textures_requested_for_draw) {
    const bool texture_request_started_with_active_encoder =
        current_render_encoder_ != nullptr;
    uint64_t& texture_request_counter =
        texture_request_started_with_active_encoder
            ? backend_telemetry_.texture_requests_after_encoder_begin
            : backend_telemetry_.texture_requests_before_encoder;
    if (!request_textures_for_draw(texture_request_counter)) {
      return fail_prepared_draw();
    }
  }

  UniformBufferInfo uniforms;
  DrawDynamicState draw_dynamic_state;
  const bool prepare_uniforms = constant_buffer_pool_ && shared_memory_;
  if (use_native_msl && !prepare_uniforms) {
    XELOGE(
        "Native MSL draw requested without constant-buffer pool or shared "
        "memory");
    return fail_prepared_draw();
  }
  if (prepare_uniforms) {
    if (!draw_pass_descriptor) {
      XELOGE("IssueDraw: no render pass descriptor for draw constants");
      return fail_prepared_draw();
    }
    if (!EnsureCommandBuffer()) {
      return fail_prepared_draw();
    }
    if (!PrepareDrawConstants(
            regs, vertex_shader, pixel_shader, metal_vertex_shader,
            metal_pixel_shader, vertex_translation_metadata,
            pixel_translation_metadata, shared_memory_is_uav,
            is_rasterization_done, primitive_processing_result,
            used_texture_mask, normalized_color_mask, draw_pass_descriptor,
            uniforms, draw_dynamic_state)) {
      return fail_prepared_draw();
    }
  }

  PreparedIndexBuffer prepared_guest_dma_index_buffer;
  if (memexport_used && !use_native_msl_primitive_mesh &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
    if (!PrepareGuestDMAIndexBufferForMemexport(
            primitive_processing_result, prepared_guest_dma_index_buffer)) {
      return fail_prepared_draw();
    }
  }

  std::array<SharedMemoryRange, 96> shared_memory_hazard_ranges = {};
  uint32_t shared_memory_hazard_range_count = 0;
  auto add_shared_memory_hazard_range = [&](uint32_t start, uint32_t length) {
    if (!length || shared_memory_hazard_range_count >=
                       shared_memory_hazard_ranges.size()) {
      return;
    }
    shared_memory_hazard_ranges[shared_memory_hazard_range_count++] =
        SharedMemoryRange{start, length};
  };
  for (uint32_t i = 0; i < vertex_range_count; ++i) {
    add_shared_memory_hazard_range(vertex_ranges[i].offset,
                                   vertex_ranges[i].length);
  }
  if (guest_dma_index_buffer_read || shader_primitive_index_load) {
    const xenos::IndexFormat index_format =
        shader_primitive_index_load
            ? regs.Get<reg::VGT_DRAW_INITIATOR>().index_size
            : primitive_processing_result.host_index_format;
    uint32_t index_stride = index_format == xenos::IndexFormat::kInt16
                                ? sizeof(uint16_t)
                                : sizeof(uint32_t);
    uint32_t index_count =
        shader_primitive_index_load
            ? primitive_processing_result.guest_draw_vertex_count
            : primitive_processing_result.host_draw_vertex_count;
    uint64_t index_length = uint64_t(index_count) * index_stride;
    if (index_length <= SharedMemory::kBufferSize) {
      add_shared_memory_hazard_range(
          static_cast<uint32_t>(primitive_processing_result.guest_index_base),
          static_cast<uint32_t>(index_length));
    }
  }
  if (NS::UInteger(memexport_write_stages)) {
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      add_shared_memory_hazard_range(memexport_range.base_address_dwords << 2,
                                     memexport_range.size_bytes);
    }
  }
  MTL::RenderStages shared_memory_consumer_stages = MTL::RenderStages(0);
  if (vertex_range_count || guest_dma_index_buffer_read ||
      shader_primitive_index_load) {
    if (use_native_msl_primitive_mesh) {
      shared_memory_consumer_stages = MTL::RenderStageMesh;
    } else {
      shared_memory_consumer_stages =
          (use_geometry_emulation || use_tessellation_emulation)
              ? MTL::RenderStages(MTL::RenderStageObject | MTL::RenderStageMesh)
              : MTL::RenderStageVertex;
    }
  }
  shared_memory_consumer_stages =
      MTL::RenderStages(NS::UInteger(shared_memory_consumer_stages) |
                        NS::UInteger(memexport_write_stages));

  draw.pipeline = pipeline;
  draw.tessellation_pipeline_state = tessellation_pipeline_state;
  draw.geometry_pipeline_state = geometry_pipeline_state;
  draw.native_mesh_pipeline_state = native_mesh_pipeline_state;
  draw.primitive_processing_result = primitive_processing_result;
  draw.uniforms = uniforms;
  draw.dynamic_state = draw_dynamic_state;
  draw.prepared_guest_dma_index_buffer = prepared_guest_dma_index_buffer;
  if (index_buffer_info) {
    draw.index_buffer_info = *index_buffer_info;
    draw.has_index_buffer_info = true;
  }
  assert_true(vb_bindings.size() <= UINT32_MAX);
  draw.vertex_bindings = {vb_bindings.data(),
                          static_cast<uint32_t>(vb_bindings.size())};
  draw.vertex_ranges = vertex_ranges;
  draw.vertex_range_count = vertex_range_count;
  draw.materialization_ranges =
      StorePreparedDrawMaterializationRanges(current_draw_shared_memory_ranges);
  draw.texture_source_range_count =
      static_cast<uint32_t>(texture_materialization_plan.source_ranges.size());
  draw.has_invalid_shared_memory = current_draw_has_invalid_shared_memory;
  draw.shared_memory_hazard_ranges = shared_memory_hazard_ranges;
  draw.shared_memory_hazard_range_count = shared_memory_hazard_range_count;
  draw.shared_memory_consumer_stages = shared_memory_consumer_stages;
  draw.memexport_ranges = StorePreparedDrawMemexportRanges(memexport_ranges_);
  draw.memexport_write_stages = memexport_write_stages;
  draw.shared_memory_usage = shared_memory_usage;
  draw.render_target_key = render_target_key;
  draw.texture_resource_set = current_bindless_texture_resource_set_;
  draw.texture_upload_needed =
      texture_materialization_plan.NeedsTextureUpload();
  draw.use_tessellation_emulation = use_tessellation_emulation;
  draw.use_geometry_emulation = use_geometry_emulation;
  draw.shared_memory_is_uav = shared_memory_is_uav;
  draw.memexport_used = memexport_used;
  draw.uses_vertex_fetch = uses_vertex_fetch;
  draw.prepare_uniforms = prepare_uniforms;
  draw.fallback_depth_attachment_required = fallback_depth_attachment_required;
  draw.may_texture_request_load_data = may_texture_request_load_data;
  draw.use_native_msl = use_native_msl;
  draw.use_native_msl_primitive_mesh = use_native_msl_primitive_mesh;
  draw.use_native_msl_tessellation = use_native_msl_tessellation;
  if (use_native_msl && vertex_translation_metadata) {
    draw.native_vertex_metadata = *vertex_translation_metadata;
  }
  if (use_native_msl && pixel_translation_metadata) {
    draw.native_pixel_metadata = *pixel_translation_metadata;
    draw.native_pixel_metadata_valid = true;
  }
  if (use_native_msl) {
    if (vertex_translation_metadata &&
        !native_msl::CaptureTextureRuntimeInfo(*texture_cache_,
                                               *vertex_translation_metadata,
                                               draw.native_vertex_bindings)) {
      return fail_prepared_draw();
    }
    if (pixel_translation_metadata &&
        !native_msl::CaptureTextureRuntimeInfo(*texture_cache_,
                                               *pixel_translation_metadata,
                                               draw.native_pixel_bindings)) {
      return fail_prepared_draw();
    }

    uint32_t primitive_index_flags = 0u;
    const bool shader_loads_primitive_indices =
        primitive_processing_result.index_buffer_type ==
            PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA ||
        native_mesh_guest_dma_index_load || native_tess_guest_dma_index_load;
    if (shader_loads_primitive_indices) {
      primitive_index_flags |= 1u;
      xenos::IndexFormat primitive_index_format =
          (native_mesh_guest_dma_index_load || native_tess_guest_dma_index_load)
              ? primitive_processing_result.host_index_format
              : regs.Get<reg::VGT_DRAW_INITIATOR>().index_size;
      if (primitive_index_format == xenos::IndexFormat::kInt32) {
        primitive_index_flags |= 2u;
      }
    }
    uint32_t primitive_metadata = 0u;
    if (use_native_msl_tessellation) {
      primitive_metadata =
          uint32_t(primitive_processing_result.tessellation_mode) & 3u;
    }
    draw.native_primitive_index_constants = {
        primitive_index_flags, primitive_processing_result.guest_index_base,
        primitive_processing_result.guest_draw_vertex_count,
        primitive_metadata};
  }
  draw.has_pending_draw_pass_transfers =
      render_target_cache_ &&
      render_target_cache_->HasPendingDrawPassTransfers();
  return SubmitPreparedDraw(prepared_draw);
}

bool MetalCommandProcessor::PrepareDrawConstants(
    const RegisterFile& regs, Shader* vertex_shader, Shader* pixel_shader,
    MetalShader* metal_vertex_shader, MetalShader* metal_pixel_shader,
    const DxbcShader::TranslationMetadata* vertex_translation_metadata,
    const DxbcShader::TranslationMetadata* pixel_translation_metadata,
    bool shared_memory_is_uav, bool is_rasterization_done,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    uint32_t used_texture_mask, uint32_t normalized_color_mask,
    MTL::RenderPassDescriptor* render_pass_descriptor,
    UniformBufferInfo& uniforms_out, DrawDynamicState& dynamic_state_out) {
  // Determine primitive type characteristics
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);

  // Get viewport info for NDC transform. Use the actual RT0 dimensions
  // when available so system constants match the current render target.
  uint32_t vp_width = 1;
  uint32_t vp_height = 1;
  GetActiveRenderTargetSize(render_pass_descriptor, render_target_cache_.get(),
                            1280, 720, vp_width, vp_height);
  draw_util::ViewportInfo viewport_info;
  auto depth_control = draw_util::GetNormalizedDepthControl(regs);
  auto dynamic_depth_control = depth_control;
  if (!is_rasterization_done) {
    dynamic_depth_control.value = 0;
  }
  constexpr uint32_t kViewportBoundsMax = 32767;
  bool host_render_targets_used = true;
  bool convert_z_to_float24 = host_render_targets_used &&
                              ::cvars::depth_float24_convert_in_pixel_shader;
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(
      draw_resolution_scale_x, draw_resolution_scale_y,
      texture_cache_ ? texture_cache_->draw_resolution_scale_x_divisor()
                     : divisors::MagicDiv(1),
      texture_cache_ ? texture_cache_->draw_resolution_scale_y_divisor()
                     : divisors::MagicDiv(1),
      true, kViewportBoundsMax, kViewportBoundsMax, false, depth_control,
      convert_z_to_float24, host_render_targets_used,
      pixel_shader && pixel_shader->writes_depth());
  gviargs.SetupRegisterValues(regs);
  draw_util::GetHostViewportInfo(&gviargs, viewport_info);

  // Apply per-draw viewport and scissor so the Metal viewport
  // matches the guest viewport computed by draw_util.
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  // draw_resolution_scale_x/y already computed above for viewport.
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  // Clamp scissor to actual render target bounds (Metal requires this).
  ClampScissorToBounds(scissor, vp_width, vp_height);

  MTL::Viewport mtl_viewport;
  mtl_viewport.originX = static_cast<double>(viewport_info.xy_offset[0]);
  mtl_viewport.originY = static_cast<double>(viewport_info.xy_offset[1]);
  mtl_viewport.width = static_cast<double>(viewport_info.xy_extent[0]);
  mtl_viewport.height = static_cast<double>(viewport_info.xy_extent[1]);
  mtl_viewport.znear = viewport_info.z_min;
  mtl_viewport.zfar = viewport_info.z_max;

  MTL::ScissorRect mtl_scissor;
  mtl_scissor.x = scissor.offset[0];
  mtl_scissor.y = scissor.offset[1];
  mtl_scissor.width = scissor.extent[0];
  mtl_scissor.height = scissor.extent[1];
  dynamic_state_out.viewport = mtl_viewport;
  dynamic_state_out.scissor = mtl_scissor;
  dynamic_state_out.viewport_info = viewport_info;
  dynamic_state_out.depth_control = dynamic_depth_control;
  dynamic_state_out.primitive_polygonal = primitive_polygonal;
  dynamic_state_out.rasterization_enabled = is_rasterization_done;

  // Update full system constants from GPU registers.
  // normalized_color_mask was already computed above for render target update.
  UpdateSystemConstantValues(
      shared_memory_is_uav, primitive_polygonal,
      primitive_processing_result.line_loop_closing_index,
      primitive_processing_result.host_shader_index_endian, viewport_info,
      used_texture_mask, depth_control, normalized_color_mask);

  float blend_constants[4] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  std::memcpy(dynamic_state_out.blend_constants, blend_constants,
              sizeof(blend_constants));

  dynamic_state_out.stencil_ref_mask_front = regs.Get<reg::RB_STENCILREFMASK>();
  dynamic_state_out.stencil_ref_mask_back =
      regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  dynamic_state_out.pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();

  MTL::CullMode cull_mode = MTL::CullModeNone;
  if (primitive_polygonal) {
    bool cull_front = dynamic_state_out.pa_su_sc_mode_cntl.cull_front;
    bool cull_back = dynamic_state_out.pa_su_sc_mode_cntl.cull_back;
    if (cull_front && !cull_back) {
      cull_mode = MTL::CullModeFront;
    } else if (cull_back && !cull_front) {
      cull_mode = MTL::CullModeBack;
    }
  }
  dynamic_state_out.cull_mode = cull_mode;
  dynamic_state_out.front_facing_winding =
      dynamic_state_out.pa_su_sc_mode_cntl.face ? MTL::WindingClockwise
                                                : MTL::WindingCounterClockwise;
  dynamic_state_out.triangle_fill_mode = MTL::TriangleFillModeFill;
  if (primitive_polygonal && dynamic_state_out.pa_su_sc_mode_cntl.poly_mode ==
                                 xenos::PolygonModeEnable::kDualMode) {
    xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
    if (!dynamic_state_out.pa_su_sc_mode_cntl.cull_front) {
      polygon_type =
          std::min(polygon_type,
                   dynamic_state_out.pa_su_sc_mode_cntl.polymode_front_ptype);
    }
    if (!dynamic_state_out.pa_su_sc_mode_cntl.cull_back) {
      polygon_type =
          std::min(polygon_type,
                   dynamic_state_out.pa_su_sc_mode_cntl.polymode_back_ptype);
    }
    if (polygon_type != xenos::PolygonType::kTriangles) {
      dynamic_state_out.triangle_fill_mode = MTL::TriangleFillModeLines;
    }
  }

  float polygon_offset_scale = 0.0f;
  float polygon_offset = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(
      regs, primitive_polygonal, polygon_offset_scale, polygon_offset);
  dynamic_state_out.depth_bias_constant =
      static_cast<float>(draw_util::GetD3D10IntegerPolygonOffset(
          regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset));
  uint32_t rasterizer_resolution_scale = 1;
  if (render_target_cache_) {
    rasterizer_resolution_scale =
        std::max(render_target_cache_->draw_resolution_scale_x(),
                 render_target_cache_->draw_resolution_scale_y());
  }
  dynamic_state_out.depth_bias_slope = polygon_offset_scale *
                                       xenos::kPolygonOffsetScaleSubpixelUnit *
                                       float(rasterizer_resolution_scale);
  dynamic_state_out.depth_clip_mode = pa_cl_clip_cntl.clip_disable
                                          ? MTL::DepthClipModeClamp
                                          : MTL::DepthClipModeClip;

  // CBV descriptor table layout:
  //   b0: System constants
  //   b1: Packed float constants
  //   b2: Bool/loop constants
  //   b3: Fetch constants
  //   b4: Descriptor indices
  constexpr size_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);

  const MetalShader::DrawConstantMetadata& vertex_draw_metadata =
      metal_vertex_shader->GetDrawConstantMetadata();
  const MetalShader::DrawConstantMetadata* pixel_draw_metadata =
      metal_pixel_shader ? &metal_pixel_shader->GetDrawConstantMetadata()
                         : nullptr;
  std::array<uint32_t, kStageCount> active_cbv_masks = {
      vertex_translation_metadata
          ? vertex_translation_metadata->used_cbuffer_mask
          : vertex_draw_metadata.active_cbv_mask,
      pixel_translation_metadata
          ? pixel_translation_metadata->used_cbuffer_mask
          : (pixel_draw_metadata ? pixel_draw_metadata->active_cbv_mask : 0)};
  auto cbv_active = [&](size_t stage, CbvSlot slot) {
    return (active_cbv_masks[stage] & (uint32_t(1) << slot)) != 0;
  };
  std::array<DxbcShader::FetchConstantDwordMask, kStageCount>
      fetch_constant_dword_masks = {
          vertex_translation_metadata
              ? vertex_translation_metadata->fetch_constant_dword_mask
          : metal_vertex_shader
              ? metal_vertex_shader->GetFetchConstantDwordMaskAfterTranslation()
              : DxbcShader::FetchConstantDwordMask(),
          pixel_translation_metadata
              ? pixel_translation_metadata->fetch_constant_dword_mask
          : metal_pixel_shader
              ? metal_pixel_shader->GetFetchConstantDwordMaskAfterTranslation()
              : DxbcShader::FetchConstantDwordMask()};
  if (!vertex_translation_metadata) {
    MergeFetchConstantDwordMask(
        fetch_constant_dword_masks[kStageVertex],
        vertex_draw_metadata.shader_fetch_constant_dword_mask);
  }
  if (!pixel_translation_metadata && pixel_draw_metadata) {
    MergeFetchConstantDwordMask(
        fetch_constant_dword_masks[kStagePixel],
        pixel_draw_metadata->shader_fetch_constant_dword_mask);
  }
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    if ((active_cbv_masks[stage] & (uint32_t(1) << kCbvSlotFetch)) &&
        FetchConstantDwordMaskEmpty(fetch_constant_dword_masks[stage])) {
      MarkAllFetchConstantDwords(fetch_constant_dword_masks[stage]);
    }
  }

  // ---------------------------------------------------------------
  // Allocate dirty constant buffers individually from the upload buffer pool.
  // Clean CBVs keep their previous upload allocation and are re-referenced from
  // the new per-draw descriptor table.
  // ---------------------------------------------------------------

  // Check if float constant layout changed (different shader bound).
  // Matches D3D12 d3d12_command_processor.cc:4910-4943.
  {
    const Shader::ConstantRegisterMap& float_map_vs =
        vertex_shader->constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_vertex_[i] !=
          float_map_vs.float_bitmap[i]) {
        current_float_constant_map_vertex_[i] = float_map_vs.float_bitmap[i];
        if (float_map_vs.float_count) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
    }
    if (pixel_shader) {
      const Shader::ConstantRegisterMap& float_map_ps =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        if (current_float_constant_map_pixel_[i] !=
            float_map_ps.float_bitmap[i]) {
          current_float_constant_map_pixel_[i] = float_map_ps.float_bitmap[i];
          if (float_map_ps.float_count) {
            cbuffer_binding_float_pixel_.up_to_date = false;
          }
        }
      }
    } else {
      std::memset(current_float_constant_map_pixel_, 0,
                  sizeof(current_float_constant_map_pixel_));
    }
  }

  const auto& texture_bindings_vertex =
      vertex_translation_metadata
          ? vertex_translation_metadata->texture_bindings
          : metal_vertex_shader->GetTextureBindingsAfterTranslation();
  const auto& sampler_bindings_vertex =
      vertex_translation_metadata
          ? vertex_translation_metadata->sampler_bindings
          : metal_vertex_shader->GetSamplerBindingsAfterTranslation();
  const size_t texture_count_vertex = texture_bindings_vertex.size();
  const size_t sampler_count_vertex = sampler_bindings_vertex.size();
  auto get_native_texture_layout_uid = [&](size_t stage, const auto& bindings,
                                           uint64_t empty_hash) -> size_t {
    NativeMslBindingLayoutUidCache& cache =
        native_msl_binding_layout_uid_cache_[stage];
    const void* data = bindings.empty() ? nullptr : bindings.data();
    const size_t count = bindings.size();
    if (cache.texture_bindings_data == data &&
        cache.texture_binding_count == count) {
      return cache.texture_layout_uid;
    }
    const size_t uid = MetalNativeBindingLayoutHash(bindings, empty_hash);
    cache.texture_bindings_data = data;
    cache.texture_binding_count = count;
    cache.texture_layout_uid = uid;
    return uid;
  };
  auto get_native_sampler_layout_uid = [&](size_t stage, const auto& bindings,
                                           uint64_t empty_hash) -> size_t {
    NativeMslBindingLayoutUidCache& cache =
        native_msl_binding_layout_uid_cache_[stage];
    const void* data = bindings.empty() ? nullptr : bindings.data();
    const size_t count = bindings.size();
    if (cache.sampler_bindings_data == data &&
        cache.sampler_binding_count == count) {
      return cache.sampler_layout_uid;
    }
    const size_t uid = MetalNativeBindingLayoutHash(bindings, empty_hash);
    cache.sampler_bindings_data = data;
    cache.sampler_binding_count = count;
    cache.sampler_layout_uid = uid;
    return uid;
  };
  auto get_sampler_parameters_cached =
      [&](const DxbcShader::SamplerBinding& binding,
          std::vector<SamplerParameterInputCache>& input_cache, size_t index) {
        if (input_cache.size() <= index) {
          input_cache.resize(index + 1);
        }
        SamplerParameterInputCache& cache = input_cache[index];
        const xenos::xe_gpu_texture_fetch_t fetch =
            regs.GetTextureFetch(binding.fetch_constant);
        const std::array<uint32_t, 6> fetch_dwords = {
            fetch.dword_0, fetch.dword_1, fetch.dword_2,
            fetch.dword_3, fetch.dword_4, fetch.dword_5};
        if (cache.valid && cache.fetch_constant == binding.fetch_constant &&
            cache.mag_filter == binding.mag_filter &&
            cache.min_filter == binding.min_filter &&
            cache.mip_filter == binding.mip_filter &&
            cache.aniso_filter == binding.aniso_filter &&
            cache.fetch_dwords == fetch_dwords) {
          return cache.parameters;
        }
        MetalTextureCache::SamplerParameters parameters =
            texture_cache_->GetSamplerParameters(binding);
        cache.fetch_constant = binding.fetch_constant;
        cache.mag_filter = binding.mag_filter;
        cache.min_filter = binding.min_filter;
        cache.mip_filter = binding.mip_filter;
        cache.aniso_filter = binding.aniso_filter;
        cache.fetch_dwords = fetch_dwords;
        cache.parameters = parameters;
        cache.valid = true;
        return parameters;
      };
  size_t texture_layout_uid_vertex =
      vertex_translation_metadata
          ? get_native_texture_layout_uid(kStageVertex, texture_bindings_vertex,
                                          0x564D534C54657830ull)
          : metal_vertex_shader->GetTextureBindingLayoutUserUID();
  size_t sampler_layout_uid_vertex =
      vertex_translation_metadata
          ? get_native_sampler_layout_uid(kStageVertex, sampler_bindings_vertex,
                                          0x564D534C536D7030ull)
          : metal_vertex_shader->GetSamplerBindingLayoutUserUID();
  auto& next_texture_bindless_indices_vertex =
      scratch_texture_bindless_indices_vertex_;
  auto& next_texture_bindless_resources_vertex =
      scratch_texture_bindless_resources_vertex_;
  auto& next_sampler_bindless_indices_vertex =
      scratch_sampler_bindless_indices_vertex_;
  next_texture_bindless_indices_vertex.clear();
  next_texture_bindless_resources_vertex.clear();
  next_sampler_bindless_indices_vertex.clear();
  if (sampler_count_vertex) {
    if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
      current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
    current_samplers_vertex_.resize(
        std::max(current_samplers_vertex_.size(), sampler_count_vertex));
    for (size_t i = 0; i < sampler_count_vertex; ++i) {
      auto parameters = get_sampler_parameters_cached(
          sampler_bindings_vertex[i], current_sampler_parameter_inputs_vertex_,
          i);
      if (current_samplers_vertex_[i] != parameters) {
        current_samplers_vertex_[i] = parameters;
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      }
    }
  } else if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
    current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
    cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
  }
  bool vertex_texture_layout_changed =
      current_texture_layout_uid_vertex_ != texture_layout_uid_vertex;
  if (vertex_texture_layout_changed && !texture_count_vertex) {
    cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
  } else if (texture_count_vertex &&
             cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
    bool vertex_texture_srv_changed =
        !vertex_texture_layout_changed &&
        !texture_cache_->AreActiveTextureSRVKeysUpToDate(
            current_texture_srv_keys_vertex_.data(),
            texture_bindings_vertex.data(), texture_count_vertex);
    if (vertex_texture_layout_changed || vertex_texture_srv_changed) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
  }
  const bool descriptor_indices_vertex_active =
      cbv_active(kStageVertex, kCbvSlotDescriptorIndices);
  const bool descriptor_indices_pixel_active =
      cbv_active(kStagePixel, kCbvSlotDescriptorIndices);

  if (descriptor_indices_vertex_active &&
      !cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
    next_texture_bindless_indices_vertex.reserve(texture_count_vertex);
    next_texture_bindless_resources_vertex.reserve(texture_count_vertex);
    for (const auto& binding : texture_bindings_vertex) {
      MTL::Texture* texture_for_encoder = nullptr;
      next_texture_bindless_indices_vertex.push_back(
          texture_cache_->GetBindlessSRVIndexForBinding(
              binding.fetch_constant, binding.dimension, binding.is_signed,
              &texture_for_encoder));
      next_texture_bindless_resources_vertex.push_back(texture_for_encoder);
    }
    next_sampler_bindless_indices_vertex.reserve(sampler_count_vertex);
    for (const auto& binding : sampler_bindings_vertex) {
      next_sampler_bindless_indices_vertex.push_back(
          texture_cache_->GetBindlessSamplerIndexForBinding(binding));
    }
  }

  size_t texture_layout_uid_pixel = 0;
  size_t sampler_layout_uid_pixel = 0;
  const std::vector<DxbcShader::TextureBinding>* texture_bindings_pixel_ptr =
      nullptr;
  const std::vector<DxbcShader::SamplerBinding>* sampler_bindings_pixel_ptr =
      nullptr;
  auto& next_texture_bindless_indices_pixel =
      scratch_texture_bindless_indices_pixel_;
  auto& next_texture_bindless_resources_pixel =
      scratch_texture_bindless_resources_pixel_;
  auto& next_sampler_bindless_indices_pixel =
      scratch_sampler_bindless_indices_pixel_;
  next_texture_bindless_indices_pixel.clear();
  next_texture_bindless_resources_pixel.clear();
  next_sampler_bindless_indices_pixel.clear();
  if (metal_pixel_shader) {
    const auto& texture_bindings_pixel =
        pixel_translation_metadata
            ? pixel_translation_metadata->texture_bindings
            : metal_pixel_shader->GetTextureBindingsAfterTranslation();
    const auto& sampler_bindings_pixel =
        pixel_translation_metadata
            ? pixel_translation_metadata->sampler_bindings
            : metal_pixel_shader->GetSamplerBindingsAfterTranslation();
    const size_t texture_count_pixel = texture_bindings_pixel.size();
    const size_t sampler_count_pixel = sampler_bindings_pixel.size();
    texture_bindings_pixel_ptr = &texture_bindings_pixel;
    sampler_bindings_pixel_ptr = &sampler_bindings_pixel;
    texture_layout_uid_pixel =
        pixel_translation_metadata
            ? get_native_texture_layout_uid(kStagePixel, texture_bindings_pixel,
                                            0x504D534C54657830ull)
            : metal_pixel_shader->GetTextureBindingLayoutUserUID();
    sampler_layout_uid_pixel =
        pixel_translation_metadata
            ? get_native_sampler_layout_uid(kStagePixel, sampler_bindings_pixel,
                                            0x504D534C536D7030ull)
            : metal_pixel_shader->GetSamplerBindingLayoutUserUID();
    if (sampler_count_pixel) {
      if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
        current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
      }
      current_samplers_pixel_.resize(
          std::max(current_samplers_pixel_.size(), sampler_count_pixel));
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        auto parameters = get_sampler_parameters_cached(
            sampler_bindings_pixel[i], current_sampler_parameter_inputs_pixel_,
            i);
        if (current_samplers_pixel_[i] != parameters) {
          current_samplers_pixel_[i] = parameters;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
        }
      }
    } else if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
      current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }
    bool pixel_texture_layout_changed =
        current_texture_layout_uid_pixel_ != texture_layout_uid_pixel;
    if (pixel_texture_layout_changed && !texture_count_pixel) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else if (texture_count_pixel &&
               cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      bool pixel_texture_srv_changed =
          !pixel_texture_layout_changed &&
          !texture_cache_->AreActiveTextureSRVKeysUpToDate(
              current_texture_srv_keys_pixel_.data(),
              texture_bindings_pixel.data(), texture_count_pixel);
      if (pixel_texture_layout_changed || pixel_texture_srv_changed) {
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
      }
    }
    if (!cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      next_texture_bindless_indices_pixel.reserve(texture_count_pixel);
      next_texture_bindless_resources_pixel.reserve(texture_count_pixel);
      for (const auto& binding : texture_bindings_pixel) {
        MTL::Texture* texture_for_encoder = nullptr;
        next_texture_bindless_indices_pixel.push_back(
            texture_cache_->GetBindlessSRVIndexForBinding(
                binding.fetch_constant, binding.dimension, binding.is_signed,
                &texture_for_encoder));
        next_texture_bindless_resources_pixel.push_back(texture_for_encoder);
      }
      next_sampler_bindless_indices_pixel.reserve(sampler_count_pixel);
      for (const auto& binding : sampler_bindings_pixel) {
        next_sampler_bindless_indices_pixel.push_back(
            texture_cache_->GetBindlessSamplerIndexForBinding(binding));
      }
    }
  }

  bool descriptor_indices_vertex_written = false;
  bool descriptor_indices_pixel_written = false;

  auto upload_binding = [&](ConstantBufferBinding& binding, size_t cbv_slot,
                            size_t size, const char* name,
                            auto&& writer) -> bool {
    constexpr size_t kConstantBufferAlignment = 256;
    size = std::max(size, size_t(16));
    MTL::Buffer* buffer = nullptr;
    size_t offset = 0;
    uint64_t gpu_address = 0;
    uint8_t* data = constant_buffer_pool_->Request(
        frame_current_, size, kConstantBufferAlignment, &buffer, offset,
        gpu_address);
    if (!data) {
      XELOGE("IssueDraw: {} constant buffer pool allocation failed", name);
      return false;
    }
    writer(data, size);
    binding.buffer = buffer;
    binding.offset = static_cast<NS::UInteger>(offset);
    binding.gpu_address = gpu_address;
    binding.size = size;
    binding.upload_frame = frame_current_;
    binding.up_to_date = true;
    if (cbv_slot < backend_telemetry_.cbv_uploads.size()) {
      ++backend_telemetry_.cbv_uploads[cbv_slot];
    }
    return true;
  };
  if (!cbuffer_binding_system_.up_to_date) {
    if (!upload_binding(cbuffer_binding_system_, kCbvSlotSystem,
                        sizeof(DxbcShaderTranslator::SystemConstants), "system",
                        [&](uint8_t* data, size_t) {
                          std::memcpy(
                              data, &system_constants_,
                              sizeof(DxbcShaderTranslator::SystemConstants));
                        })) {
      return false;
    }
  } else {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotSystem];
  }

  auto write_packed_float_constants =
      [&](uint8_t* dst, size_t dst_size, const Shader::ConstantRegisterMap* map,
          uint32_t regs_base) {
        std::memset(dst, 0, dst_size);
        if (!map || !map->float_count) {
          return;
        }
        uint8_t* out = dst;
        uint8_t* end = dst + dst_size;
        for (uint32_t i = 0; i < 4; ++i) {
          uint64_t bits = map->float_bitmap[i];
          uint32_t constant_index;
          while (xe::bit_scan_forward(bits, &constant_index)) {
            bits &= ~(uint64_t(1) << constant_index);
            if (out + 4 * sizeof(uint32_t) > end) {
              return;
            }
            std::memcpy(
                out, &regs.values[regs_base + (i << 8) + (constant_index << 2)],
                4 * sizeof(uint32_t));
            out += 4 * sizeof(uint32_t);
          }
        }
      };

  const Shader::ConstantRegisterMap& float_map_vertex =
      vertex_shader->constant_register_map();
  if (cbv_active(kStageVertex, kCbvSlotFloat) &&
      !cbuffer_binding_float_vertex_.up_to_date) {
    const size_t float_size =
        sizeof(float) * 4 * std::max(float_map_vertex.float_count, uint32_t(1));
    if (!upload_binding(
            cbuffer_binding_float_vertex_, kCbvSlotFloat, float_size,
            "vertex float", [&](uint8_t* data, size_t size) {
              write_packed_float_constants(data, size, &float_map_vertex,
                                           XE_GPU_REG_SHADER_CONSTANT_000_X);
            })) {
      return false;
    }
  } else if (cbv_active(kStageVertex, kCbvSlotFloat)) {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotFloat];
  }

  if (cbv_active(kStagePixel, kCbvSlotFloat) &&
      !cbuffer_binding_float_pixel_.up_to_date) {
    const Shader::ConstantRegisterMap* float_map_pixel =
        pixel_shader ? &pixel_shader->constant_register_map() : nullptr;
    const size_t float_size =
        sizeof(float) * 4 *
        std::max(float_map_pixel ? float_map_pixel->float_count : uint32_t(0),
                 uint32_t(1));
    if (!upload_binding(cbuffer_binding_float_pixel_, kCbvSlotFloat, float_size,
                        "pixel float", [&](uint8_t* data, size_t size) {
                          write_packed_float_constants(
                              data, size, float_map_pixel,
                              XE_GPU_REG_SHADER_CONSTANT_256_X);
                        })) {
      return false;
    }
  } else if (pixel_shader && cbv_active(kStagePixel, kCbvSlotFloat)) {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotFloat];
  }

  const bool bool_loop_active = cbv_active(kStageVertex, kCbvSlotBoolLoop) ||
                                cbv_active(kStagePixel, kCbvSlotBoolLoop);
  if (bool_loop_active && !cbuffer_binding_bool_loop_.up_to_date) {
    if (!upload_binding(
            cbuffer_binding_bool_loop_, kCbvSlotBoolLoop,
            kBoolLoopConstantsSize, "bool loop", [&](uint8_t* data, size_t) {
              std::memcpy(data,
                          &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                          kBoolLoopConstantsSize);
            })) {
      return false;
    }
  } else if (bool_loop_active) {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotBoolLoop];
  }

  bool fetch_binding_active = false;
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    if (!(active_cbv_masks[stage] & (uint32_t(1) << kCbvSlotFetch))) {
      continue;
    }
    fetch_binding_active = true;
  }
  const size_t fetch_size = kFetchConstantDwordCount * sizeof(uint32_t);
  const uint32_t* fetch_constants =
      &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
  if (fetch_binding_active) {
    if (!cbuffer_binding_fetch_.up_to_date) {
      const bool can_reuse_fetch_payload =
          cbuffer_binding_fetch_.buffer &&
          cbuffer_binding_fetch_.upload_frame == frame_current_ &&
          current_fetch_constant_payload_valid_ &&
          std::memcmp(current_fetch_constant_payload_.data(), fetch_constants,
                      fetch_size) == 0;
      if (can_reuse_fetch_payload) {
        cbuffer_binding_fetch_.up_to_date = true;
        ++backend_telemetry_.cbv_reuse_hits[kCbvSlotFetch];
      } else {
        if (!upload_binding(cbuffer_binding_fetch_, kCbvSlotFetch, fetch_size,
                            "fetch", [&](uint8_t* data, size_t) {
                              std::memcpy(data, fetch_constants, fetch_size);
                            })) {
          return false;
        }
        std::memcpy(current_fetch_constant_payload_.data(), fetch_constants,
                    fetch_size);
        current_fetch_constant_payload_valid_ = true;
      }
    } else {
      ++backend_telemetry_.cbv_reuse_hits[kCbvSlotFetch];
    }
  }

  const bool use_native_msl_descriptor_indices =
      vertex_translation_metadata || pixel_translation_metadata;
  auto write_descriptor_indices =
      [&](uint32_t* words, uint32_t word_count,
          const std::vector<DxbcShader::TextureBinding>& texture_bindings,
          const std::vector<DxbcShader::SamplerBinding>& sampler_bindings,
          const std::vector<uint32_t>& texture_indices,
          const std::vector<uint32_t>& sampler_indices) {
        std::fill_n(words, std::max(word_count, uint32_t(1)), 0u);
        if (!texture_cache_) {
          return;
        }
        auto map_native_texture_index = [&](uint32_t index) {
          if (!use_native_msl_descriptor_indices) {
            return index;
          }
          const uint32_t native_index =
              texture_cache_->GetNativeMslSRVIndexForBindlessIndex(index);
          if (native_index != UINT32_MAX) {
            return native_index;
          }
          static bool logged_native_texture_index_failure = false;
          if (!logged_native_texture_index_failure) {
            XELOGW("Native MSL texture bindless index {} is not mapped", index);
            logged_native_texture_index_failure = true;
          }
          return 0u;
        };
        auto map_native_sampler_index = [&](uint32_t index) {
          if (!use_native_msl_descriptor_indices) {
            return index;
          }
          const uint32_t native_index =
              texture_cache_->GetNativeMslSamplerIndexForBindlessIndex(index);
          if (native_index != UINT32_MAX) {
            return native_index;
          }
          static bool logged_native_sampler_index_failure = false;
          if (!logged_native_sampler_index_failure) {
            XELOGW("Native MSL sampler bindless index {} is not mapped", index);
            logged_native_sampler_index_failure = true;
          }
          return 0u;
        };
        for (size_t i = 0;
             i < texture_bindings.size() && i < texture_indices.size(); ++i) {
          uint32_t d = texture_bindings[i].bindless_descriptor_index;
          assert_true(d < word_count);
          if (d >= word_count) {
            continue;
          }
          words[d] = map_native_texture_index(texture_indices[i]);
        }
        for (size_t i = 0;
             i < sampler_bindings.size() && i < sampler_indices.size(); ++i) {
          uint32_t d = sampler_bindings[i].bindless_descriptor_index;
          assert_true(d < word_count);
          if (d >= word_count) {
            continue;
          }
          words[d] = map_native_sampler_index(sampler_indices[i]);
        }
      };

  auto upload_descriptor_indices = [&](ConstantBufferBinding& binding,
                                       uint32_t word_count,
                                       auto&& writer,
                                       size_t stage, const char* name) -> bool {
    const size_t descriptor_indices_bytes =
        std::max(word_count, uint32_t(1)) * sizeof(uint32_t);
    if (stage < backend_telemetry_.descriptor_index_uploads.size()) {
      ++backend_telemetry_.descriptor_index_uploads[stage];
    }
    if (!upload_binding(
            binding, kCbvSlotDescriptorIndices, descriptor_indices_bytes, name,
            [&](uint8_t* data, size_t size) {
              writer(reinterpret_cast<uint32_t*>(data), word_count);
            })) {
      return false;
    }
    return true;
  };

  if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
    uint32_t descriptor_indices_word_count =
        vertex_translation_metadata
            ? MetalNativeDescriptorIndicesWordCount(
                  *vertex_translation_metadata)
            : vertex_draw_metadata.descriptor_indices_word_count;
    if (!upload_descriptor_indices(
            cbuffer_binding_descriptor_indices_vertex_,
            descriptor_indices_word_count,
            [&](uint32_t* words, uint32_t word_count) {
              write_descriptor_indices(words, word_count, texture_bindings_vertex,
                                       sampler_bindings_vertex,
                                       next_texture_bindless_indices_vertex,
                                       next_sampler_bindless_indices_vertex);
            },
            kStageVertex, "vertex descriptor indices")) {
      return false;
    }
    descriptor_indices_vertex_written = true;
  } else if (descriptor_indices_vertex_active) {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotDescriptorIndices];
  }

  if (metal_pixel_shader && descriptor_indices_pixel_active &&
      !cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
    uint32_t descriptor_indices_word_count =
        pixel_translation_metadata
            ? MetalNativeDescriptorIndicesWordCount(*pixel_translation_metadata)
            : (pixel_draw_metadata
                   ? pixel_draw_metadata->descriptor_indices_word_count
                   : 1);
    static const std::vector<DxbcShader::TextureBinding> kNoTextureBindings;
    static const std::vector<DxbcShader::SamplerBinding> kNoSamplerBindings;
    if (!upload_descriptor_indices(
            cbuffer_binding_descriptor_indices_pixel_,
            descriptor_indices_word_count,
            [&](uint32_t* words, uint32_t word_count) {
              write_descriptor_indices(
                  words, word_count,
                  texture_bindings_pixel_ptr ? *texture_bindings_pixel_ptr
                                             : kNoTextureBindings,
                  sampler_bindings_pixel_ptr ? *sampler_bindings_pixel_ptr
                                             : kNoSamplerBindings,
                  next_texture_bindless_indices_pixel,
                  next_sampler_bindless_indices_pixel);
            },
            kStagePixel, "pixel descriptor indices")) {
      return false;
    }
    descriptor_indices_pixel_written = true;
  } else if (metal_pixel_shader && descriptor_indices_pixel_active) {
    ++backend_telemetry_.cbv_reuse_hits[kCbvSlotDescriptorIndices];
  }

  if (descriptor_indices_vertex_written) {
    current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
    current_texture_bindless_resources_vertex_.swap(
        next_texture_bindless_resources_vertex);
    if (texture_count_vertex) {
      current_texture_srv_keys_vertex_.resize(std::max(
          current_texture_srv_keys_vertex_.size(), texture_count_vertex));
      texture_cache_->WriteActiveTextureSRVKeys(
          current_texture_srv_keys_vertex_.data(),
          texture_bindings_vertex.data(), texture_count_vertex);
    }
  }
  if (descriptor_indices_pixel_written) {
    current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
    current_texture_bindless_resources_pixel_.swap(
        next_texture_bindless_resources_pixel);
    if (texture_bindings_pixel_ptr && !texture_bindings_pixel_ptr->empty()) {
      current_texture_srv_keys_pixel_.resize(
          std::max(current_texture_srv_keys_pixel_.size(),
                   texture_bindings_pixel_ptr->size()));
      texture_cache_->WriteActiveTextureSRVKeys(
          current_texture_srv_keys_pixel_.data(),
          texture_bindings_pixel_ptr->data(),
          texture_bindings_pixel_ptr->size());
    }
  }
  if (descriptor_indices_vertex_written || descriptor_indices_pixel_written) {
    PublishBindlessTextureResourceSet();
  }

  uniforms_out = {};
  auto set_uniform_cbv = [&](UniformBufferInfo::Cbv& cbv,
                             const ConstantBufferBinding& binding,
                             bool active) {
    cbv.active = active;
    if (!active) {
      return;
    }
    assert_true(binding.up_to_date);
    assert_true(binding.upload_frame == frame_current_);
    cbv.buffer = binding.buffer;
    cbv.offset = binding.offset;
    cbv.gpu_address = binding.gpu_address;
    cbv.size = binding.size;
  };
  uniforms_out.active_cbv_masks = active_cbv_masks;
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotSystem],
                  cbuffer_binding_system_,
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotSystem));
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotFloat],
                  cbuffer_binding_float_vertex_,
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotFloat));
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotBoolLoop],
                  cbuffer_binding_bool_loop_,
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotBoolLoop));
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotFetch],
                  cbuffer_binding_fetch_,
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotFetch));
  set_uniform_cbv(uniforms_out.cbvs[kStageVertex][kCbvSlotDescriptorIndices],
                  cbuffer_binding_descriptor_indices_vertex_,
                  uniforms_out.active_cbv_masks[kStageVertex] &
                      (uint32_t(1) << kCbvSlotDescriptorIndices));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotSystem],
                  cbuffer_binding_system_,
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotSystem));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotFloat],
                  cbuffer_binding_float_pixel_,
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotFloat));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotBoolLoop],
                  cbuffer_binding_bool_loop_,
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotBoolLoop));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotFetch],
                  cbuffer_binding_fetch_,
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotFetch));
  set_uniform_cbv(uniforms_out.cbvs[kStagePixel][kCbvSlotDescriptorIndices],
                  cbuffer_binding_descriptor_indices_pixel_,
                  uniforms_out.active_cbv_masks[kStagePixel] &
                      (uint32_t(1) << kCbvSlotDescriptorIndices));
  uniforms_out.fetch_constant_dword_masks = fetch_constant_dword_masks;
  return true;
}

void MetalCommandProcessor::ApplyDrawDynamicState(
    const DrawDynamicState& dynamic_state) {
  if (viewport_dirty_ || std::memcmp(&dynamic_state.viewport, &cached_viewport_,
                                     sizeof(MTL::Viewport)) != 0) {
    current_render_encoder_->setViewport(dynamic_state.viewport);
    cached_viewport_ = dynamic_state.viewport;
    viewport_dirty_ = false;
  }

  if (scissor_dirty_ || std::memcmp(&dynamic_state.scissor, &cached_scissor_,
                                    sizeof(MTL::ScissorRect)) != 0) {
    current_render_encoder_->setScissorRect(dynamic_state.scissor);
    cached_scissor_ = dynamic_state.scissor;
    scissor_dirty_ = false;
  }

  if (dynamic_state.rasterization_enabled) {
    ApplyRasterizerState(dynamic_state);
  }

  // Fixed-function depth/stencil state is not part of the pipeline state in
  // Metal, so update it per draw.
  ApplyDepthStencilState(dynamic_state);

  bool blend_factor_update_needed =
      !ff_blend_factor_valid_ ||
      std::memcmp(ff_blend_factor_, dynamic_state.blend_constants,
                  sizeof(float) * 4) != 0;
  if (blend_factor_update_needed) {
    std::memcpy(ff_blend_factor_, dynamic_state.blend_constants,
                sizeof(float) * 4);
    ff_blend_factor_valid_ = true;
    current_render_encoder_->setBlendColor(
        dynamic_state.blend_constants[0], dynamic_state.blend_constants[1],
        dynamic_state.blend_constants[2], dynamic_state.blend_constants[3]);
  }
}

bool MetalCommandProcessor::BindNativeMslDrawResources(
    const PreparedDraw& draw) {
  if (!current_render_encoder_ || !constant_buffer_pool_ || !shared_memory_ ||
      !texture_cache_ || !null_buffer_) {
    XELOGE("Native MSL draw binding requested before Metal resources exist");
    return false;
  }

  constexpr size_t kNativeRuntimeInfoAlignment = 256;
  const bool draw_needs_primitive_index_constants =
      draw.native_vertex_metadata.uses_primitive_index_constants ||
      (draw.native_pixel_metadata_valid &&
       draw.native_pixel_metadata.uses_primitive_index_constants);
  MTL::Buffer* primitive_index_buffer = nullptr;
  size_t primitive_index_offset = 0;
  uint64_t primitive_index_gpu_address = 0;
  if (draw_needs_primitive_index_constants) {
    NativeMslPrimitiveIndexUploadCache& primitive_index_cache =
        native_msl_primitive_index_upload_cache_;
    const bool can_reuse_primitive_index =
        primitive_index_cache.payload_valid && primitive_index_cache.buffer &&
        primitive_index_cache.upload_frame == frame_current_ &&
        primitive_index_cache.payload == draw.native_primitive_index_constants;
    if (can_reuse_primitive_index) {
      primitive_index_buffer = primitive_index_cache.buffer;
      primitive_index_offset = primitive_index_cache.offset;
      primitive_index_gpu_address = primitive_index_cache.gpu_address;
    } else {
      uint8_t* primitive_index_data = constant_buffer_pool_->Request(
          frame_current_, sizeof(draw.native_primitive_index_constants),
          kNativeRuntimeInfoAlignment, &primitive_index_buffer,
          primitive_index_offset, primitive_index_gpu_address);
      if (!primitive_index_data || !primitive_index_buffer) {
        XELOGE("Native MSL primitive-index constant allocation failed");
        return false;
      }
      std::memcpy(primitive_index_data,
                  draw.native_primitive_index_constants.data(),
                  sizeof(draw.native_primitive_index_constants));
      primitive_index_cache.payload = draw.native_primitive_index_constants;
      primitive_index_cache.buffer = primitive_index_buffer;
      primitive_index_cache.offset =
          static_cast<NS::UInteger>(primitive_index_offset);
      primitive_index_cache.gpu_address = primitive_index_gpu_address;
      primitive_index_cache.payload_valid = true;
      primitive_index_cache.upload_frame = frame_current_;
    }
  }

  auto cbv_for_stage = [&](size_t stage,
                           CbvSlot slot) -> const UniformBufferInfo::Cbv& {
    return draw.uniforms.cbvs[stage][slot];
  };

  auto require_cbv = [&](const UniformBufferInfo::Cbv& cbv,
                         const char* name) -> bool {
    if (cbv.active && cbv.buffer) {
      return true;
    }
    XELOGE("Native MSL draw missing active {} CBV", name);
    return false;
  };

  auto bind_stage = [&](const DxbcShader::TranslationMetadata& metadata,
                        size_t stage, bool vertex_stage, bool fragment_stage,
                        bool mesh_stage, bool object_stage) -> bool {
    const auto& system_cbv = cbv_for_stage(stage, kCbvSlotSystem);
    const auto& float_cbv = cbv_for_stage(stage, kCbvSlotFloat);
    const auto& bool_loop_cbv = cbv_for_stage(stage, kCbvSlotBoolLoop);
    const auto& fetch_cbv = cbv_for_stage(stage, kCbvSlotFetch);
    const auto& descriptor_indices_cbv =
        cbv_for_stage(stage, kCbvSlotDescriptorIndices);
    auto metadata_uses_cbv = [&](CbvSlot slot) {
      return (metadata.used_cbuffer_mask & (uint32_t(1) << slot)) != 0;
    };
    const bool needs_system = metadata_uses_cbv(kCbvSlotSystem);
    const bool needs_float = metadata_uses_cbv(kCbvSlotFloat);
    const bool needs_bool_loop = metadata_uses_cbv(kCbvSlotBoolLoop);
    const bool needs_fetch = metadata_uses_cbv(kCbvSlotFetch);
    const bool needs_descriptor_indices =
        metadata_uses_cbv(kCbvSlotDescriptorIndices);
    if ((needs_system && !require_cbv(system_cbv, "system")) ||
        (needs_float && !require_cbv(float_cbv, "float")) ||
        (needs_bool_loop && !require_cbv(bool_loop_cbv, "bool/loop")) ||
        (needs_fetch && !require_cbv(fetch_cbv, "fetch")) ||
        (needs_descriptor_indices &&
         !require_cbv(descriptor_indices_cbv, "descriptor-indices"))) {
      return false;
    }

    MTL::RenderStages stage_render_stages = MTL::RenderStages(0);
    auto add_render_stage = [&](MTL::RenderStages stage_bits) {
      stage_render_stages =
          MTL::RenderStages(NS::UInteger(stage_render_stages) |
                            NS::UInteger(stage_bits));
    };
    if (vertex_stage) {
      add_render_stage(MTL::RenderStageVertex);
    }
    if (fragment_stage) {
      add_render_stage(MTL::RenderStageFragment);
    }
    if (mesh_stage) {
      add_render_stage(MTL::RenderStageMesh);
    }
    if (object_stage) {
      add_render_stage(MTL::RenderStageObject);
    }

    const bool needs_runtime_info =
        native_msl::UsesTextureRuntimeInfo(metadata);
    const uint32_t runtime_row_count =
        needs_runtime_info
            ? std::max<uint32_t>(uint32_t(metadata.texture_bindings.size()), 1)
            : 0;
    const native_msl::NativeMslStageBindings& native_bindings =
        stage == kStagePixel ? draw.native_pixel_bindings
                             : draw.native_vertex_bindings;
    if (needs_runtime_info &&
        native_bindings.runtime_info.size() < runtime_row_count) {
      XELOGE("Native MSL draw missing captured texture runtime info");
      return false;
    }
    bool needs_texture_2d_array_heap = false;
    bool needs_texture_3d_heap = false;
    bool needs_texture_cube_heap = false;
    for (const DxbcShader::TextureBinding& binding :
         metadata.texture_bindings) {
      switch (binding.dimension) {
        case xenos::FetchOpDimension::kCube:
          needs_texture_cube_heap = true;
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          needs_texture_2d_array_heap = true;
          needs_texture_3d_heap = true;
          break;
        case xenos::FetchOpDimension::k1D:
        case xenos::FetchOpDimension::k2D:
        default:
          needs_texture_2d_array_heap = true;
          break;
      }
    }
    if ((needs_texture_2d_array_heap && !native_msl_texture_2d_array_heap_) ||
        (needs_texture_3d_heap && !native_msl_texture_3d_heap_) ||
        (needs_texture_cube_heap && !native_msl_texture_cube_heap_)) {
      XELOGE("Native MSL draw missing required texture argument heap buffer");
      return false;
    }
    if (!metadata.sampler_bindings.empty() && !native_msl_sampler_heap_) {
      XELOGE("Native MSL draw missing sampler argument heap buffer");
      return false;
    }

    MTL::Buffer* runtime_info_buffer = nullptr;
    size_t runtime_info_offset = 0;
    if (needs_runtime_info) {
      const size_t runtime_info_size =
          runtime_row_count * sizeof(native_msl::NativeMslTextureRuntimeInfo);
      NativeMslRuntimeInfoUploadCache& runtime_cache =
          native_msl_runtime_info_upload_cache_[stage];
      const bool can_reuse_runtime_info =
          runtime_cache.buffer &&
          runtime_cache.upload_frame == frame_current_ &&
          runtime_cache.size == runtime_info_size &&
          runtime_cache.payload.size() == runtime_row_count &&
          std::memcmp(runtime_cache.payload.data(),
                      native_bindings.runtime_info.data(),
                      runtime_info_size) == 0;
      if (can_reuse_runtime_info) {
        runtime_info_buffer = runtime_cache.buffer;
        runtime_info_offset = runtime_cache.offset;
      } else {
        uint64_t runtime_info_gpu_address = 0;
        uint8_t* runtime_info_data = constant_buffer_pool_->Request(
            frame_current_, runtime_info_size, kNativeRuntimeInfoAlignment,
            &runtime_info_buffer, runtime_info_offset,
            runtime_info_gpu_address);
        (void)runtime_info_gpu_address;
        if (!runtime_info_data || !runtime_info_buffer) {
          XELOGE("Native MSL texture runtime-info allocation failed");
          return false;
        }
        std::memcpy(runtime_info_data, native_bindings.runtime_info.data(),
                    runtime_info_size);
        runtime_cache.payload.resize(runtime_row_count);
        std::memcpy(runtime_cache.payload.data(),
                    native_bindings.runtime_info.data(), runtime_info_size);
        runtime_cache.buffer = runtime_info_buffer;
        runtime_cache.offset = static_cast<NS::UInteger>(runtime_info_offset);
        runtime_cache.size = runtime_info_size;
        runtime_cache.upload_frame = frame_current_;
      }
    }

    MTL::Buffer* shared_memory_buffer = nullptr;
    if (metadata.uses_shared_memory) {
      shared_memory_buffer = shared_memory_->GetBuffer();
      if (!shared_memory_buffer) {
        XELOGE("Native MSL draw missing shared-memory buffer");
        return false;
      }
    }

    const uint64_t null_gpu_address = null_buffer_->gpuAddress();
    auto cbv_gpu_address_or_null = [&](const UniformBufferInfo::Cbv& cbv,
                                       bool needed) -> uint64_t {
      if (!needed) {
        return null_gpu_address;
      }
      return cbv.gpu_address ? cbv.gpu_address : null_gpu_address;
    };
    native_msl::NativeMslDrawConstantPointers draw_constants = {};
    draw_constants.system =
        cbv_gpu_address_or_null(system_cbv, needs_system);
    draw_constants.float_constants_data =
        cbv_gpu_address_or_null(float_cbv, needs_float);
    draw_constants.bool_loop_constants_data =
        cbv_gpu_address_or_null(bool_loop_cbv, needs_bool_loop);
    draw_constants.fetch_constants_data =
        cbv_gpu_address_or_null(fetch_cbv, needs_fetch);
    draw_constants.descriptor_indices =
        cbv_gpu_address_or_null(descriptor_indices_cbv,
                                needs_descriptor_indices);
    draw_constants.primitive_index =
        metadata.uses_primitive_index_constants && primitive_index_gpu_address
            ? primitive_index_gpu_address
            : null_gpu_address;

    MTL::Buffer* draw_constants_buffer = nullptr;
    size_t draw_constants_offset = 0;
    NativeMslDrawConstantsUploadCache& draw_constants_cache =
        native_msl_draw_constants_upload_cache_[stage];
    const bool can_reuse_draw_constants =
        draw_constants_cache.payload_valid && draw_constants_cache.buffer &&
        draw_constants_cache.upload_frame == frame_current_ &&
        std::memcmp(&draw_constants_cache.payload, &draw_constants,
                    sizeof(draw_constants)) == 0;
    auto compute_draw_constants_change_mask = [&]() {
      uint32_t changed_mask = 0;
      auto mark_changed = [&](bool changed, uint32_t bit) {
        if (changed) {
          changed_mask |= bit;
        }
      };
      mark_changed(draw_constants_cache.payload.system !=
                       draw_constants.system,
                   kNativeMslDrawConstantsChangeSystem);
      mark_changed(draw_constants_cache.payload.float_constants_data !=
                       draw_constants.float_constants_data,
                   kNativeMslDrawConstantsChangeFloat);
      mark_changed(draw_constants_cache.payload.bool_loop_constants_data !=
                       draw_constants.bool_loop_constants_data,
                   kNativeMslDrawConstantsChangeBoolLoop);
      mark_changed(draw_constants_cache.payload.fetch_constants_data !=
                       draw_constants.fetch_constants_data,
                   kNativeMslDrawConstantsChangeFetch);
      mark_changed(draw_constants_cache.payload.descriptor_indices !=
                       draw_constants.descriptor_indices,
                   kNativeMslDrawConstantsChangeDescriptorIndices);
      mark_changed(draw_constants_cache.payload.primitive_index !=
                       draw_constants.primitive_index,
                   kNativeMslDrawConstantsChangePrimitiveIndex);
      return changed_mask;
    };
    auto record_draw_constants_reason =
        [&](RenderEncoderBufferStage encoder_stage,
            NativeMslDrawConstantsRebuildReason reason) {
          const size_t stage_index = size_t(encoder_stage);
          if (stage_index >= kRenderEncoderBufferStageTelemetryCount) {
            return;
          }
          const size_t reason_index =
              stage_index * kNativeMslDrawConstantsRebuildReasonCount +
              size_t(reason);
          if (reason_index <
              backend_telemetry_
                  .native_msl_draw_constants_rebuild_reasons.size()) {
            ++backend_telemetry_
                  .native_msl_draw_constants_rebuild_reasons[reason_index];
          }
        };
    auto record_draw_constants_change_mask =
        [&](RenderEncoderBufferStage encoder_stage, uint32_t changed_mask) {
          const size_t stage_index = size_t(encoder_stage);
          if (stage_index >= kRenderEncoderBufferStageTelemetryCount ||
              changed_mask >= kNativeMslDrawConstantsChangeMaskCount) {
            return;
          }
          const size_t mask_index =
              stage_index * kNativeMslDrawConstantsChangeMaskCount +
              changed_mask;
          if (mask_index <
              backend_telemetry_.native_msl_draw_constants_change_masks
                  .size()) {
            ++backend_telemetry_.native_msl_draw_constants_change_masks
                  [mask_index];
          }
        };
    auto record_draw_constants_reason_for_bound_stages =
        [&](NativeMslDrawConstantsRebuildReason reason) {
          if (vertex_stage) {
            record_draw_constants_reason(RenderEncoderBufferStage::kVertex,
                                         reason);
          }
          if (fragment_stage) {
            record_draw_constants_reason(RenderEncoderBufferStage::kFragment,
                                         reason);
          }
          if (mesh_stage) {
            record_draw_constants_reason(RenderEncoderBufferStage::kMesh,
                                         reason);
          }
          if (object_stage) {
            record_draw_constants_reason(RenderEncoderBufferStage::kObject,
                                         reason);
          }
        };
    auto record_draw_constants_change_mask_for_bound_stages =
        [&](uint32_t changed_mask) {
          if (vertex_stage) {
            record_draw_constants_change_mask(RenderEncoderBufferStage::kVertex,
                                              changed_mask);
          }
          if (fragment_stage) {
            record_draw_constants_change_mask(
                RenderEncoderBufferStage::kFragment, changed_mask);
          }
          if (mesh_stage) {
            record_draw_constants_change_mask(RenderEncoderBufferStage::kMesh,
                                              changed_mask);
          }
          if (object_stage) {
            record_draw_constants_change_mask(RenderEncoderBufferStage::kObject,
                                              changed_mask);
          }
        };
    const bool can_record_draw_constants_change_mask =
        !can_reuse_draw_constants && draw_constants_cache.payload_valid &&
        draw_constants_cache.buffer &&
        draw_constants_cache.upload_frame == frame_current_;
    const uint32_t draw_constants_change_mask =
        can_record_draw_constants_change_mask
            ? compute_draw_constants_change_mask()
            : 0;
    if (draw_constants_change_mask) {
      record_draw_constants_change_mask_for_bound_stages(
          draw_constants_change_mask);
    }
    auto classify_draw_constants_reason =
        [&]() -> NativeMslDrawConstantsRebuildReason {
      if (can_reuse_draw_constants) {
        return kNativeMslDrawConstantsReuse;
      }
      if (!draw_constants_cache.payload_valid || !draw_constants_cache.buffer) {
        return kNativeMslDrawConstantsInitial;
      }
      if (draw_constants_cache.upload_frame != frame_current_) {
        return kNativeMslDrawConstantsFrameOpen;
      }
      const uint32_t changed_mask = draw_constants_change_mask;
      if (!changed_mask || (changed_mask & (changed_mask - 1))) {
        return kNativeMslDrawConstantsMixed;
      }
      switch (changed_mask) {
        case kNativeMslDrawConstantsChangeSystem:
          return kNativeMslDrawConstantsSystemChanged;
        case kNativeMslDrawConstantsChangeFloat:
          return kNativeMslDrawConstantsFloatChanged;
        case kNativeMslDrawConstantsChangeBoolLoop:
          return kNativeMslDrawConstantsBoolLoopChanged;
        case kNativeMslDrawConstantsChangeFetch:
          return kNativeMslDrawConstantsFetchChanged;
        case kNativeMslDrawConstantsChangeDescriptorIndices:
          return kNativeMslDrawConstantsDescriptorIndicesChanged;
        case kNativeMslDrawConstantsChangePrimitiveIndex:
          return kNativeMslDrawConstantsPrimitiveIndexChanged;
        default:
          return kNativeMslDrawConstantsMixed;
      }
    };
    record_draw_constants_reason_for_bound_stages(
        classify_draw_constants_reason());
    if (can_reuse_draw_constants) {
      draw_constants_buffer = draw_constants_cache.buffer;
      draw_constants_offset = draw_constants_cache.offset;
    } else {
      uint64_t draw_constants_gpu_address = 0;
      uint8_t* draw_constants_data = constant_buffer_pool_->Request(
          frame_current_, sizeof(draw_constants), kNativeRuntimeInfoAlignment,
          &draw_constants_buffer, draw_constants_offset,
          draw_constants_gpu_address);
      (void)draw_constants_gpu_address;
      if (!draw_constants_data || !draw_constants_buffer) {
        XELOGE("Native MSL draw-constant pointer allocation failed");
        return false;
      }
      std::memcpy(draw_constants_data, &draw_constants,
                  sizeof(draw_constants));
      draw_constants_cache.payload = draw_constants;
      draw_constants_cache.buffer = draw_constants_buffer;
      draw_constants_cache.offset =
          static_cast<NS::UInteger>(draw_constants_offset);
      draw_constants_cache.payload_valid = true;
      draw_constants_cache.upload_frame = frame_current_;
    }

    auto bind_native_buffer_for_stage =
        [&](RenderEncoderBufferStage stage, MTL::Buffer* buffer,
            NS::UInteger offset, NS::UInteger slot) {
          if (RenderEncoderBufferBindingMatches(stage, buffer, offset, slot)) {
            return;
          }
          SetRenderEncoderBuffer(stage, buffer, offset, slot);
        };
    auto bind_native_buffer = [&](MTL::Buffer* buffer, NS::UInteger offset,
                                  NS::UInteger slot) {
      if (!buffer) {
        return;
      }
      if (vertex_stage) {
        bind_native_buffer_for_stage(RenderEncoderBufferStage::kVertex, buffer,
                                     offset, slot);
      }
      if (fragment_stage) {
        bind_native_buffer_for_stage(RenderEncoderBufferStage::kFragment,
                                     buffer, offset, slot);
      }
      if (mesh_stage) {
        bind_native_buffer_for_stage(RenderEncoderBufferStage::kMesh, buffer,
                                     offset, slot);
      }
      if (object_stage) {
        bind_native_buffer_for_stage(RenderEncoderBufferStage::kObject, buffer,
                                     offset, slot);
      }
    };
    bind_native_buffer(draw_constants_buffer, draw_constants_offset,
                       kNativeBufferDrawConstants);
    if (needs_runtime_info) {
      bind_native_buffer(runtime_info_buffer, runtime_info_offset,
                         kNativeBufferTextureRuntimeInfo);
    }
    if (needs_texture_2d_array_heap) {
      bind_native_buffer(native_msl_texture_2d_array_heap_, 0,
                         kNativeBufferTexture2DArrayHeap);
    }
    if (needs_texture_3d_heap) {
      bind_native_buffer(native_msl_texture_3d_heap_, 0,
                         kNativeBufferTexture3DHeap);
    }
    if (needs_texture_cube_heap) {
      bind_native_buffer(native_msl_texture_cube_heap_, 0,
                         kNativeBufferTextureCubeHeap);
    }
    if (!metadata.sampler_bindings.empty()) {
      bind_native_buffer(native_msl_sampler_heap_, 0, kNativeBufferSamplerHeap);
    }
    if (metadata.uses_shared_memory) {
      bind_native_buffer(shared_memory_buffer, 0, kNativeBufferSharedMemory);
    }

    std::array<const MTL::Resource*, 8> draw_constant_resources;
    uint32_t draw_constant_resource_count = 0;
    auto add_draw_constant_resource = [&](MTL::Resource* resource) {
      if (!resource) {
        return;
      }
      for (uint32_t i = 0; i < draw_constant_resource_count; ++i) {
        if (draw_constant_resources[i] == resource) {
          return;
        }
      }
      assert_true(draw_constant_resource_count <
                  draw_constant_resources.size());
      if (draw_constant_resource_count < draw_constant_resources.size()) {
        draw_constant_resources[draw_constant_resource_count++] = resource;
      }
    };
    if (needs_system) {
      add_draw_constant_resource(system_cbv.buffer);
    }
    if (needs_float) {
      add_draw_constant_resource(float_cbv.buffer);
    }
    if (needs_bool_loop) {
      add_draw_constant_resource(bool_loop_cbv.buffer);
    }
    if (needs_fetch) {
      add_draw_constant_resource(fetch_cbv.buffer);
    }
    if (needs_descriptor_indices) {
      add_draw_constant_resource(descriptor_indices_cbv.buffer);
    }
    if (metadata.uses_primitive_index_constants) {
      add_draw_constant_resource(primitive_index_buffer);
    }
    if (draw_constant_resource_count) {
      UseRenderEncoderResources(draw_constant_resources.data(),
                                draw_constant_resource_count,
                                MTL::ResourceUsageRead, stage_render_stages);
    }

    if (metadata.uses_shared_memory) {
      if (!IsResidencySetResourceCovered(shared_memory_buffer)) {
        UseRenderEncoderResource(shared_memory_buffer,
                                 draw.shared_memory_usage,
                                 stage_render_stages);
      }
    }
    return true;
  };

  const bool native_vertex_stage =
      !draw.use_native_msl_primitive_mesh && !draw.use_native_msl_tessellation;
  const bool native_mesh_stage =
      draw.use_native_msl_primitive_mesh || draw.use_native_msl_tessellation;
  const bool native_object_stage = draw.use_native_msl_tessellation;
  if (!bind_stage(draw.native_vertex_metadata, kStageVertex,
                  native_vertex_stage, false, native_mesh_stage,
                  native_object_stage)) {
    return false;
  }
  if (draw.native_pixel_metadata_valid &&
      !bind_stage(draw.native_pixel_metadata, kStagePixel, false, true, false,
                  false)) {
    return false;
  }
  return true;
}

bool MetalCommandProcessor::MaterializeGraphicsRootArguments(
    GraphicsRootArgumentState& state) {
  MTL::Buffer* top_level_buffer = nullptr;
  size_t top_level_offset = 0;
  uint64_t top_level_gpu_address = 0;
  auto* top_level_entries =
      reinterpret_cast<uint64_t*>(constant_buffer_pool_->Request(
          frame_current_, kTopLevelABBytesPerTable, kTopLevelABBytesPerTable,
          &top_level_buffer, top_level_offset, top_level_gpu_address));
  if (!top_level_entries) {
    XELOGE("IssueDraw: bindless table allocation failed");
    return false;
  }

  std::memcpy(top_level_entries, state.entries.data(),
              kTopLevelABBytesPerTable);

  state.allocation = {top_level_buffer,
                      static_cast<NS::UInteger>(top_level_offset),
                      top_level_gpu_address, frame_current_, true};
  state.valid = true;
  state.frame_open_rebuild_pending = false;
  state.dirty_slot_mask = 0;
  ++state.serial;
  if (!state.serial) {
    state.serial = 1;
  }
  return true;
}

bool MetalCommandProcessor::PopulateBindlessTables(
    bool shared_memory_is_uav, MTL::ResourceUsage shared_memory_usage,
    bool use_geometry_emulation, bool use_tessellation_emulation,
    const UniformBufferInfo& uniforms) {
  constexpr size_t kGraphicsRootStage = kStageVertex;
  GraphicsRootArgumentState& root_state = graphics_root_argument_state_;
  if (root_state.valid) {
    assert_true(root_state.allocation.upload_frame == frame_current_);
  }
  const bool entries_were_initialized = root_state.entries_initialized;
  const bool bindless_shared_memory_uav_mismatch =
      current_bindless_shared_memory_is_uav_ != shared_memory_is_uav;
  const bool root_rebuild_detail_telemetry =
      cvars::metal_root_rebuild_detail_telemetry;
  const uint64_t null_gpu = null_buffer_->gpuAddress();
  struct RootUpdateStats {
    size_t slots_patched = 0;
    bool descriptor_indices_pointer_mismatch = false;
    bool other_cbv_pointer_mismatch = false;
    bool root_resource_identity_changed = false;
    bool root_resource_identity_same = false;
  } root_update_stats;
  auto root_slot_is_descriptor_indices_cbv = [](size_t slot) {
    switch (slot) {
      case kGraphicsRootABSlotCBVVertexDescriptorIndices:
      case kGraphicsRootABSlotCBVHullDescriptorIndices:
      case kGraphicsRootABSlotCBVDomainDescriptorIndices:
      case kGraphicsRootABSlotCBVPixelDescriptorIndices:
        return true;
      default:
        return false;
    }
  };
  auto root_slot_is_other_cbv = [](size_t slot) {
    switch (slot) {
      case kGraphicsRootABSlotCBVSystem:
      case kGraphicsRootABSlotCBVVertexFloat:
      case kGraphicsRootABSlotCBVBoolLoop:
      case kGraphicsRootABSlotCBVVertexFetch:
      case kGraphicsRootABSlotCBVHullFloat:
      case kGraphicsRootABSlotCBVHullFetch:
      case kGraphicsRootABSlotCBVDomainFloat:
      case kGraphicsRootABSlotCBVDomainFetch:
      case kGraphicsRootABSlotCBVPixelFloat:
      case kGraphicsRootABSlotCBVPixelFetch:
        return true;
      default:
        return false;
    }
  };
  auto uniform_cbv_identity = [&](size_t stage, size_t cbv) {
    const uint32_t cbv_bit = uint32_t(1) << cbv;
    if (!(uniforms.active_cbv_masks[stage] & cbv_bit)) {
      return RootSlotIdentity{true, false, nullptr, 0};
    }
    const UniformBufferInfo::Cbv& uniform_cbv = uniforms.cbvs[stage][cbv];
    return RootSlotIdentity{true, true, uniform_cbv.buffer, uniform_cbv.offset};
  };
  auto cbv_gpu_address = [&](size_t stage, size_t cbv) -> uint64_t {
    const uint32_t cbv_bit = uint32_t(1) << cbv;
    if (!(uniforms.active_cbv_masks[stage] & cbv_bit)) {
      return null_gpu;
    }
    const UniformBufferInfo::Cbv& uniform_cbv = uniforms.cbvs[stage][cbv];
    return uniform_cbv.gpu_address ? uniform_cbv.gpu_address : null_gpu;
  };
  auto common_cbv_gpu_address = [&](size_t cbv) -> uint64_t {
    uint64_t vertex_address = cbv_gpu_address(kStageVertex, cbv);
    if (vertex_address != null_gpu) {
      return vertex_address;
    }
    return cbv_gpu_address(kStagePixel, cbv);
  };
  auto select_common_uniform_cbv_identity = [&](size_t cbv) {
    RootSlotIdentity vertex_identity = uniform_cbv_identity(kStageVertex, cbv);
    if (vertex_identity.active) {
      return vertex_identity;
    }
    return uniform_cbv_identity(kStagePixel, cbv);
  };
  auto set_root_slot = [&](size_t slot, uint64_t gpu_address,
                           RootSlotIdentity identity) {
    assert_true(slot < root_state.entries.size());
    if (slot >= root_state.entries.size()) {
      return;
    }
    const uint64_t previous_gpu_address = root_state.entries[slot];
    const RootSlotIdentity previous_identity = root_state.identities[slot];
    const bool slot_changed = entries_were_initialized
                                  ? previous_gpu_address != gpu_address
                                  : gpu_address != 0;
    if (!slot_changed) {
      root_state.identities[slot] = identity;
      return;
    }
    root_state.entries[slot] = gpu_address;
    root_state.identities[slot] = identity;
    if (slot < 64) {
      root_state.dirty_slot_mask |= uint64_t(1) << slot;
    }
    ++root_update_stats.slots_patched;
    if (slot < backend_telemetry_.bindless_root_arg_slot_patches.size()) {
      ++backend_telemetry_.bindless_root_arg_slot_patches[slot];
    }
    if (root_rebuild_detail_telemetry) {
      const bool previous_slot_active =
          entries_were_initialized && previous_gpu_address != null_gpu;
      const bool new_slot_active = gpu_address != null_gpu;
      if (previous_identity.is_cbv || identity.is_cbv) {
        const bool previous_cbv_active =
            previous_identity.active && previous_slot_active;
        const bool new_cbv_active = identity.active && new_slot_active;
        MTL::Buffer* previous_buffer =
            previous_cbv_active ? previous_identity.buffer : nullptr;
        MTL::Buffer* new_buffer = new_cbv_active ? identity.buffer : nullptr;
        if (previous_buffer == new_buffer && previous_buffer &&
            previous_identity.offset != identity.offset) {
          ++backend_telemetry_.bindless_root_rebuild_details
                [kBindlessRootDetailSameBufferOffsetChanged];
          root_update_stats.root_resource_identity_same = true;
        } else if (previous_buffer != new_buffer) {
          ++backend_telemetry_.bindless_root_rebuild_details
                [kBindlessRootDetailDifferentBuffer];
          root_update_stats.root_resource_identity_changed = true;
        } else if (previous_buffer) {
          root_update_stats.root_resource_identity_same = true;
        }
      } else {
        root_update_stats.root_resource_identity_changed = true;
      }
    }
    if (entries_were_initialized) {
      if (root_slot_is_descriptor_indices_cbv(slot)) {
        root_update_stats.descriptor_indices_pointer_mismatch = true;
      } else if (root_slot_is_other_cbv(slot)) {
        root_update_stats.other_cbv_pointer_mismatch = true;
      }
    }
  };
  auto set_cbv_slot = [&](size_t root_slot, size_t stage, size_t cbv) {
    set_root_slot(root_slot, cbv_gpu_address(stage, cbv),
                  uniform_cbv_identity(stage, cbv));
  };
  auto set_common_cbv_slot = [&](size_t root_slot, size_t cbv) {
    set_root_slot(root_slot, common_cbv_gpu_address(cbv),
                  select_common_uniform_cbv_identity(cbv));
  };

  constexpr uint64_t kDescriptorEntrySize = sizeof(IRDescriptorTableEntry);
  const uint64_t view_heap_gpu = view_bindless_heap_->gpuAddress();
  const uint64_t sampler_heap_gpu = sampler_bindless_heap_->gpuAddress();
  const uint64_t system_view_gpu = system_view_tables_->gpuAddress();
  const uint64_t srv_space0_gpu =
      system_view_gpu + (shared_memory_is_uav
                             ? kSystemViewTableSRVNull
                             : kSystemViewTableSRVSharedMemory) *
                            kDescriptorEntrySize;
  const uint64_t uav_space0_gpu =
      system_view_gpu + (shared_memory_is_uav
                             ? kSystemViewTableUAVSharedMemoryStart
                             : kSystemViewTableUAVNullStart) *
                            kDescriptorEntrySize;
  const uint64_t null_uav_gpu =
      system_view_gpu + kSystemViewTableUAVNullStart * kDescriptorEntrySize;
  set_root_slot(kGraphicsRootABSlotSRVSpace0, srv_space0_gpu, {});
  set_root_slot(kGraphicsRootABSlotSRVSpace1, view_heap_gpu, {});
  set_root_slot(kGraphicsRootABSlotSRVSpace2, view_heap_gpu, {});
  set_root_slot(kGraphicsRootABSlotSRVSpace3, view_heap_gpu, {});
  set_root_slot(kGraphicsRootABSlotSRVSpace10, view_heap_gpu, {});
  set_root_slot(kGraphicsRootABSlotUAVSpace0, uav_space0_gpu, {});
  set_root_slot(kGraphicsRootABSlotUAVSpace1, null_uav_gpu, {});
  set_root_slot(kGraphicsRootABSlotUAVSpace2, null_uav_gpu, {});
  set_root_slot(kGraphicsRootABSlotUAVSpace3, null_uav_gpu, {});
  set_root_slot(kGraphicsRootABSlotSamplerSpace0, sampler_heap_gpu, {});
  set_common_cbv_slot(kGraphicsRootABSlotCBVSystem, kCbvSlotSystem);
  set_common_cbv_slot(kGraphicsRootABSlotCBVBoolLoop, kCbvSlotBoolLoop);
  set_cbv_slot(kGraphicsRootABSlotCBVVertexFloat, kStageVertex, kCbvSlotFloat);
  set_cbv_slot(kGraphicsRootABSlotCBVVertexFetch, kStageVertex, kCbvSlotFetch);
  set_cbv_slot(kGraphicsRootABSlotCBVVertexDescriptorIndices, kStageVertex,
               kCbvSlotDescriptorIndices);
  if (use_tessellation_emulation) {
    // Hull and domain generated stages consume the same guest constant state as
    // the vertex/domain translation they expand. Keep these slots null on
    // ordinary draws so normal vertex root churn doesn't patch inactive stages.
    set_cbv_slot(kGraphicsRootABSlotCBVHullFloat, kStageVertex, kCbvSlotFloat);
    set_cbv_slot(kGraphicsRootABSlotCBVHullFetch, kStageVertex, kCbvSlotFetch);
    set_cbv_slot(kGraphicsRootABSlotCBVHullDescriptorIndices, kStageVertex,
                 kCbvSlotDescriptorIndices);
    set_cbv_slot(kGraphicsRootABSlotCBVDomainFloat, kStageVertex,
                 kCbvSlotFloat);
    set_cbv_slot(kGraphicsRootABSlotCBVDomainFetch, kStageVertex,
                 kCbvSlotFetch);
    set_cbv_slot(kGraphicsRootABSlotCBVDomainDescriptorIndices, kStageVertex,
                 kCbvSlotDescriptorIndices);
  } else {
    const RootSlotIdentity inactive_cbv{true, false, nullptr, 0};
    set_root_slot(kGraphicsRootABSlotCBVHullFloat, null_gpu, inactive_cbv);
    set_root_slot(kGraphicsRootABSlotCBVHullFetch, null_gpu, inactive_cbv);
    set_root_slot(kGraphicsRootABSlotCBVHullDescriptorIndices, null_gpu,
                  inactive_cbv);
    set_root_slot(kGraphicsRootABSlotCBVDomainFloat, null_gpu, inactive_cbv);
    set_root_slot(kGraphicsRootABSlotCBVDomainFetch, null_gpu, inactive_cbv);
    set_root_slot(kGraphicsRootABSlotCBVDomainDescriptorIndices, null_gpu,
                  inactive_cbv);
  }
  set_cbv_slot(kGraphicsRootABSlotCBVPixelFloat, kStagePixel, kCbvSlotFloat);
  set_cbv_slot(kGraphicsRootABSlotCBVPixelFetch, kStagePixel, kCbvSlotFetch);
  set_cbv_slot(kGraphicsRootABSlotCBVPixelDescriptorIndices, kStagePixel,
               kCbvSlotDescriptorIndices);
  root_state.entries_initialized = true;

  const bool root_valid = root_state.valid && root_state.allocation.valid &&
                          root_state.allocation.upload_frame == frame_current_;
  const bool root_update_needed = !root_valid || root_state.dirty_slot_mask;
  if (root_update_needed) {
    if (root_rebuild_detail_telemetry) {
      const size_t slots_changed_bin =
          std::min(root_update_stats.slots_patched,
                   backend_telemetry_.bindless_root_slots_changed.size() - 1);
      ++backend_telemetry_.bindless_root_slots_changed[slots_changed_bin];
      if (root_update_stats.descriptor_indices_pointer_mismatch &&
          !root_update_stats.other_cbv_pointer_mismatch) {
        ++backend_telemetry_.bindless_root_rebuild_details
              [kBindlessRootDetailDescriptorIndicesOnly];
      } else if (!root_update_stats.descriptor_indices_pointer_mismatch &&
                 root_update_stats.other_cbv_pointer_mismatch) {
        ++backend_telemetry_
              .bindless_root_rebuild_details[kBindlessRootDetailOtherCbvOnly];
      } else if (root_update_stats.descriptor_indices_pointer_mismatch &&
                 root_update_stats.other_cbv_pointer_mismatch) {
        ++backend_telemetry_.bindless_root_rebuild_details
              [kBindlessRootDetailMixedDescriptorAndOther];
      }
      if (root_update_stats.root_resource_identity_changed) {
        ++backend_telemetry_.bindless_root_rebuild_details
              [kBindlessRootDetailResourceIdentityChanged];
      } else if (root_update_stats.root_resource_identity_same ||
                 root_update_stats.slots_patched) {
        ++backend_telemetry_.bindless_root_rebuild_details
              [kBindlessRootDetailResourceIdentitySame];
      }
    }
    const bool frame_open_rebuild = root_state.frame_open_rebuild_pending;
    if (frame_open_rebuild) {
      ++backend_telemetry_
            .bindless_root_rebuild_reasons[kBindlessRootRebuildFrameOpen];
    }
    if (!frame_open_rebuild &&
        root_update_stats.descriptor_indices_pointer_mismatch) {
      ++backend_telemetry_.bindless_root_rebuild_reasons
            [kBindlessRootRebuildDescriptorIndicesPointerChange];
    }
    if (!frame_open_rebuild && root_update_stats.other_cbv_pointer_mismatch) {
      ++backend_telemetry_.bindless_root_rebuild_reasons
            [kBindlessRootRebuildOtherCbvPointerChange];
    }
    if (bindless_shared_memory_uav_mismatch) {
      ++backend_telemetry_.bindless_root_rebuild_reasons
            [kBindlessRootRebuildSharedMemoryUavChange];
    }
    if (entries_were_initialized && !root_update_stats.slots_patched &&
        root_valid) {
      ++backend_telemetry_.bindless_root_arg_noop_updates[kGraphicsRootStage];
    } else {
      if (!MaterializeGraphicsRootArguments(root_state)) {
        return false;
      }
      ++backend_telemetry_.bindless_root_allocations[kGraphicsRootStage];
      backend_telemetry_.bindless_root_arg_slots_patched[kGraphicsRootStage] +=
          root_update_stats.slots_patched;
      backend_telemetry_.bindless_root_arg_bytes_copied[kGraphicsRootStage] +=
          kTopLevelABBytesPerTable;
    }
  } else {
    ++backend_telemetry_.bindless_root_reuse_hits[kGraphicsRootStage];
  }

  for (size_t stage = 0; stage < kStageCount; ++stage) {
    for (size_t cbv = 0; cbv < kCbvSlotCount; ++cbv) {
      const UniformBufferInfo::Cbv& uniform_cbv = uniforms.cbvs[stage][cbv];
      const bool cbv_active =
          uniforms.active_cbv_masks[stage] & (uint32_t(1) << cbv);
      current_bindless_cbv_gpu_addresses_[stage][cbv] =
          cbv_active ? (uniform_cbv.gpu_address ? uniform_cbv.gpu_address
                                                : null_buffer_->gpuAddress())
                     : null_buffer_->gpuAddress();
      current_bindless_cbv_buffers_[stage][cbv] =
          cbv_active ? uniform_cbv.buffer : nullptr;
      current_bindless_cbv_offsets_[stage][cbv] =
          cbv_active ? uniform_cbv.offset : 0;
    }
    current_bindless_active_cbv_masks_[stage] =
        uniforms.active_cbv_masks[stage];
  }
  current_bindless_shared_memory_is_uav_ = shared_memory_is_uav;

  PublishBindlessFixedResourceSet(shared_memory_usage);
  PublishBindlessRootResourceSet(uniforms);
  ApplyRenderEncoderResourceSets();

  const bool use_mesh_path =
      use_geometry_emulation || use_tessellation_emulation;
  const bool root_argument_path_needs_update =
      render_encoder_bindless_table_bind_mesh_path_ != use_mesh_path ||
      render_encoder_bindless_table_bind_tessellation_ !=
          use_tessellation_emulation;
  {
    const StageRootArgumentAllocation& graphics_root_arguments =
        graphics_root_argument_state_.allocation;
    const uint64_t graphics_root_serial = graphics_root_argument_state_.serial;
    assert_true(graphics_root_arguments.valid);
    assert_true(graphics_root_arguments.upload_frame == frame_current_);
    const bool vertex_root_argument_binding_needs_update =
        render_encoder_bindless_stage_root_bind_serials_[kStageVertex] !=
            graphics_root_serial ||
        root_argument_path_needs_update;
    const bool pixel_root_argument_binding_needs_update =
        render_encoder_bindless_stage_root_bind_serials_[kStagePixel] !=
        graphics_root_serial;
    const bool root_argument_bindings_need_update =
        vertex_root_argument_binding_needs_update ||
        pixel_root_argument_binding_needs_update;
    if (use_mesh_path) {
      if (vertex_root_argument_binding_needs_update) {
        SetRenderEncoderObjectBuffer(graphics_root_arguments.buffer,
                                     graphics_root_arguments.offset,
                                     kIRArgumentBufferBindPoint);
        SetRenderEncoderMeshBuffer(graphics_root_arguments.buffer,
                                   graphics_root_arguments.offset,
                                   kIRArgumentBufferBindPoint);

        if (use_tessellation_emulation) {
          SetRenderEncoderObjectBuffer(graphics_root_arguments.buffer,
                                       graphics_root_arguments.offset,
                                       kIRArgumentBufferHullDomainBindPoint);
          SetRenderEncoderMeshBuffer(graphics_root_arguments.buffer,
                                     graphics_root_arguments.offset,
                                     kIRArgumentBufferHullDomainBindPoint);
        }
      }
      if (pixel_root_argument_binding_needs_update) {
        SetRenderEncoderFragmentBuffer(graphics_root_arguments.buffer,
                                       graphics_root_arguments.offset,
                                       kIRArgumentBufferBindPoint);
      }

      if (!heap_binds_set_on_encoder_) {
        SetRenderEncoderObjectBuffer(view_bindless_heap_, 0,
                                     kIRDescriptorHeapBindPoint);
        SetRenderEncoderMeshBuffer(view_bindless_heap_, 0,
                                   kIRDescriptorHeapBindPoint);
        SetRenderEncoderFragmentBuffer(view_bindless_heap_, 0,
                                       kIRDescriptorHeapBindPoint);
        SetRenderEncoderObjectBuffer(sampler_bindless_heap_, 0,
                                     kIRSamplerHeapBindPoint);
        SetRenderEncoderMeshBuffer(sampler_bindless_heap_, 0,
                                   kIRSamplerHeapBindPoint);
        SetRenderEncoderFragmentBuffer(sampler_bindless_heap_, 0,
                                       kIRSamplerHeapBindPoint);
        heap_binds_set_on_encoder_ = true;
      }
    } else {
      if (vertex_root_argument_binding_needs_update) {
        SetRenderEncoderVertexBuffer(graphics_root_arguments.buffer,
                                     graphics_root_arguments.offset,
                                     kIRArgumentBufferBindPoint);
      }
      if (pixel_root_argument_binding_needs_update) {
        SetRenderEncoderFragmentBuffer(graphics_root_arguments.buffer,
                                       graphics_root_arguments.offset,
                                       kIRArgumentBufferBindPoint);
      }

      if (!heap_binds_set_on_encoder_) {
        SetRenderEncoderVertexBuffer(view_bindless_heap_, 0,
                                     kIRDescriptorHeapBindPoint);
        SetRenderEncoderFragmentBuffer(view_bindless_heap_, 0,
                                       kIRDescriptorHeapBindPoint);
        SetRenderEncoderVertexBuffer(sampler_bindless_heap_, 0,
                                     kIRSamplerHeapBindPoint);
        SetRenderEncoderFragmentBuffer(sampler_bindless_heap_, 0,
                                       kIRSamplerHeapBindPoint);
        heap_binds_set_on_encoder_ = true;
      }
    }
    if (vertex_root_argument_binding_needs_update) {
      render_encoder_bindless_stage_root_bind_serials_[kStageVertex] =
          graphics_root_serial;
    }
    if (pixel_root_argument_binding_needs_update) {
      render_encoder_bindless_stage_root_bind_serials_[kStagePixel] =
          graphics_root_serial;
    }
    if (root_argument_bindings_need_update) {
      render_encoder_bindless_table_bind_mesh_path_ = use_mesh_path;
      render_encoder_bindless_table_bind_tessellation_ =
          use_tessellation_emulation;
    }
  }

  return true;
}

bool MetalCommandProcessor::PrepareGuestDMAIndexBufferForMemexport(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    PreparedIndexBuffer& prepared_index_buffer_out) {
  prepared_index_buffer_out = {};
  if (primitive_processing_result.index_buffer_type !=
      PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
    return true;
  }
  if (!shared_memory_) {
    XELOGE("IssueDraw: shared memory is unavailable for guest DMA index copy");
    return false;
  }

  MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer();
  if (!shared_mem_buffer) {
    XELOGE("IssueDraw: shared memory buffer is unavailable for index copy");
    return false;
  }

  uint32_t index_stride = primitive_processing_result.host_index_format ==
                                  xenos::IndexFormat::kInt16
                              ? sizeof(uint16_t)
                              : sizeof(uint32_t);
  uint64_t index_bytes =
      uint64_t(primitive_processing_result.host_draw_vertex_count) *
      index_stride;
  uint64_t guest_index_base = primitive_processing_result.guest_index_base;
  if (guest_index_base > SharedMemory::kBufferSize ||
      SharedMemory::kBufferSize - guest_index_base < index_bytes) {
    XELOGW("Index buffer range out of bounds (base=0x{:08X} size={} count={})",
           static_cast<uint32_t>(guest_index_base), index_bytes,
           primitive_processing_result.host_draw_vertex_count);
    return false;
  }

  // Metal's buffer-to-buffer blit requires 4-byte offsets and sizes on macOS.
  uint64_t source_copy_offset = guest_index_base & ~uint64_t(3);
  uint64_t source_delta = guest_index_base - source_copy_offset;
  uint64_t copy_size = xe::align(source_delta + index_bytes, uint64_t(4));
  if (source_copy_offset > SharedMemory::kBufferSize ||
      SharedMemory::kBufferSize - source_copy_offset < copy_size) {
    XELOGW(
        "Aligned index buffer copy range out of bounds "
        "(base=0x{:08X} size={})",
        static_cast<uint32_t>(source_copy_offset), copy_size);
    return false;
  }
  if (!RequestSharedMemoryRange(SharedMemoryRequestReason::kIndexCopySource,
                                static_cast<uint32_t>(source_copy_offset),
                                static_cast<uint32_t>(copy_size))) {
    XELOGE(
        "IssueDraw: failed to request guest DMA index copy range at 0x{:08X} "
        "(size {})",
        static_cast<uint32_t>(source_copy_offset), copy_size);
    return false;
  }

  MTL::CommandBuffer* command_buffer =
      RequestTransferCommandBuffer(TransferRequestSource::kGuestIndexCopy);
  if (!command_buffer) {
    XELOGE("IssueDraw: failed to get command buffer for index copy");
    return false;
  }
  shared_memory_->MarkGpuAccess(static_cast<uint32_t>(source_copy_offset),
                                static_cast<uint32_t>(copy_size),
                                GetCurrentSubmission());

  MTL::Buffer* scratch_buffer = nullptr;
  size_t scratch_offset = 0;
  uint64_t scratch_gpu_address = 0;
  MTL::Buffer* direct_scratch_buffer = nullptr;
  if (constant_buffer_pool_ &&
      copy_size <= ui::GraphicsUploadBufferPool::kDefaultPageSize) {
    (void)constant_buffer_pool_->Request(
        frame_current_, static_cast<size_t>(copy_size), 4, &scratch_buffer,
        scratch_offset, scratch_gpu_address);
  }
  if (!scratch_buffer) {
    direct_scratch_buffer = device_->newBuffer(
        static_cast<NS::UInteger>(copy_size), MTL::ResourceStorageModePrivate);
    if (!direct_scratch_buffer) {
      XELOGE(
          "IssueDraw: failed to allocate scratch index buffer for guest DMA "
          "memexport draw");
      return false;
    }
    scratch_buffer = direct_scratch_buffer;
    scratch_offset = 0;
  }

  MTL::BlitCommandEncoder* blit_encoder = command_buffer->blitCommandEncoder();
  if (!blit_encoder) {
    if (direct_scratch_buffer) {
      direct_scratch_buffer->release();
    }
    XELOGE("IssueDraw: failed to create blit encoder for index copy");
    return false;
  }
  blit_encoder->setLabel(
      NS::String::string("XeniaGuestDMAIndexCopy", NS::UTF8StringEncoding));
  if (!EncodeSharedMemoryBlitReadDependency(
          blit_encoder, static_cast<uint32_t>(source_copy_offset),
          static_cast<uint32_t>(copy_size))) {
    blit_encoder->endEncoding();
    if (direct_scratch_buffer) {
      direct_scratch_buffer->release();
    }
    return false;
  }
  blit_encoder->copyFromBuffer(
      shared_mem_buffer, static_cast<NS::UInteger>(source_copy_offset),
      scratch_buffer, static_cast<NS::UInteger>(scratch_offset),
      static_cast<NS::UInteger>(copy_size));
  blit_encoder->endEncoding();

  if (direct_scratch_buffer) {
    retired_memexport_index_buffers_.push_back(
        {direct_scratch_buffer, GetCurrentSubmission()});
  }
  prepared_index_buffer_out.buffer = scratch_buffer;
  prepared_index_buffer_out.offset = scratch_offset + source_delta;
  return true;
}

MetalCommandProcessor::PreparedDrawRenderTargetKey
MetalCommandProcessor::BuildPreparedDrawRenderTargetKey(
    const RegisterFile& regs, bool is_rasterization_done,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) const {
  PreparedDrawRenderTargetKey key;
  key.rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>().value;
  key.rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>().value;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    key.rb_color_info[i] =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i])
            .value;
  }
  key.normalized_depth_control = normalized_depth_control.value;
  key.normalized_color_mask = normalized_color_mask;
  key.is_rasterization_done = is_rasterization_done;
  return key;
}

bool MetalCommandProcessor::EncodePreparedDraw(const PreparedDraw& draw) {
  if (!draw.pipeline) {
    static bool null_pipeline_logged = false;
    if (!null_pipeline_logged) {
      null_pipeline_logged = true;
      XELOGW("Metal: prepared draw had no pipeline; skipping draw");
    }
    return true;
  }
  if (!BeginRenderEncoderForDraw(draw.fallback_depth_attachment_required)) {
    static bool no_command_buffer_logged = false;
    if (!no_command_buffer_logged) {
      no_command_buffer_logged = true;
      XELOGE(
          "IssueDraw: failed to begin Metal command buffer/render encoder; "
          "skipping draws until uniforms buffer allocation recovers");
    }
    return false;
  }
  if (render_target_cache_ &&
      render_target_cache_->HasPendingDrawPassTransfers()) {
    MetalRenderTargetCache::DrawPassTransferEncoderMutationMask
        transfer_mutations =
            MetalRenderTargetCache::kDrawPassTransferEncoderMutationNone;
    if (!render_target_cache_->EncodePendingDrawPassTransfers(
            current_render_encoder_, current_render_pass_descriptor_,
            &transfer_mutations)) {
      if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
        return false;
      }
      if (!BeginRenderEncoderForDraw(draw.fallback_depth_attachment_required)) {
        return false;
      }
      transfer_mutations =
          MetalRenderTargetCache::kDrawPassTransferEncoderMutationNone;
    }
    InvalidateRenderEncoderStateAfterDrawPassTransfers(transfer_mutations);
  }

  if (draw.shared_memory_hazard_range_count &&
      PendingSharedMemoryWritesOverlapRanges(
          draw.shared_memory_hazard_ranges.data(),
          draw.shared_memory_hazard_range_count)) {
    if (!EncodeSharedMemoryRenderReadDependencies(
            draw.shared_memory_hazard_ranges.data(),
            draw.shared_memory_hazard_range_count,
            draw.shared_memory_consumer_stages)) {
      return false;
    }
    if (!current_render_encoder_) {
      return false;
    }
  }
  if (shared_memory_ && draw.shared_memory_hazard_range_count) {
    uint64_t submission = GetCurrentSubmission();
    for (uint32_t i = 0; i < draw.shared_memory_hazard_range_count; ++i) {
      const SharedMemoryRange& range = draw.shared_memory_hazard_ranges[i];
      shared_memory_->MarkGpuAccess(range.start, range.length, submission);
    }
  }

  if (current_render_pipeline_state_ != draw.pipeline) {
    current_render_encoder_->setRenderPipelineState(draw.pipeline);
    current_render_pipeline_state_ = draw.pipeline;
    ++backend_telemetry_.pipeline_sets;
  } else {
    ++backend_telemetry_.pipeline_set_skips;
  }
  if (draw.use_tessellation_emulation && !draw.use_native_msl_tessellation) {
    if (!tessellator_tables_buffer_) {
      XELOGE("Tessellation emulation requires tessellator tables buffer");
      return false;
    }
    SetRenderEncoderObjectBuffer(tessellator_tables_buffer_, 0,
                                 kIRRuntimeTessellatorTablesBindPoint);
    SetRenderEncoderMeshBuffer(tessellator_tables_buffer_, 0,
                               kIRRuntimeTessellatorTablesBindPoint);
    if (!IsResidencySetResourceCovered(tessellator_tables_buffer_)) {
      UseRenderEncoderResource(tessellator_tables_buffer_,
                               MTL::ResourceUsageRead);
    }
  }

  if (draw.prepare_uniforms) {
    ApplyDrawDynamicState(draw.dynamic_state);

    if (draw.use_native_msl) {
      RestoreBindlessTextureResourceSet(draw.texture_resource_set);
      ApplyRenderEncoderResourceSets();
      if (!BindNativeMslDrawResources(draw)) {
        return false;
      }
    } else {
      RestoreBindlessTextureResourceSet(draw.texture_resource_set);

      if (!PopulateBindlessTables(
              draw.shared_memory_is_uav, draw.shared_memory_usage,
              draw.use_geometry_emulation, draw.use_tessellation_emulation,
              draw.uniforms)) {
        return false;
      }
    }
  }

  OpenQuerySegment(false);

  return DispatchDraw(
      draw.primitive_processing_result, draw.use_tessellation_emulation,
      draw.tessellation_pipeline_state, draw.use_geometry_emulation,
      draw.geometry_pipeline_state, draw.native_mesh_pipeline_state,
      draw.use_native_msl_tessellation, draw.shared_memory_is_uav,
      draw.shared_memory_usage, draw.memexport_used,
      draw.memexport_write_stages, draw.uses_vertex_fetch,
      draw.prepare_uniforms,
      draw.prepared_guest_dma_index_buffer.buffer
          ? &draw.prepared_guest_dma_index_buffer
          : nullptr,
      draw.vertex_bindings, draw.vertex_ranges.data(), draw.vertex_range_count,
      draw.has_index_buffer_info ? &draw.index_buffer_info : nullptr,
      draw.memexport_ranges);
}

bool MetalCommandProcessor::DispatchDraw(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool use_tessellation_emulation,
    MetalPipelineCache::TessellationPipelineState* tessellation_pipeline_state,
    bool use_geometry_emulation,
    MetalPipelineCache::GeometryPipelineState* geometry_pipeline_state,
    MetalPipelineCache::NativeMeshPipelineState* native_mesh_pipeline_state,
    bool use_native_msl_tessellation, bool shared_memory_is_uav,
    MTL::ResourceUsage shared_memory_usage, bool memexport_used,
    MTL::RenderStages memexport_write_stages, bool uses_vertex_fetch,
    bool shared_memory_resource_registered,
    const PreparedIndexBuffer* prepared_guest_dma_index_buffer,
    PreparedDrawSpan<Shader::VertexBinding> vb_bindings,
    const VertexBindingRange* vertex_ranges, uint32_t vertex_range_count,
    const IndexBufferInfo* index_buffer_info,
    PreparedDrawSpan<draw_util::MemExportRange> memexport_ranges) {
  const bool use_native_msl_primitive_mesh =
      native_mesh_pipeline_state != nullptr;
  auto use_resource_if_not_residency_covered = [&](MTL::Resource* resource,
                                                   MTL::ResourceUsage usage) {
    if (!resource || IsResidencySetResourceCovered(resource)) {
      return;
    }
    UseRenderEncoderResource(resource, usage);
  };
  auto use_shared_memory_resource_if_needed = [&](MTL::Buffer* buffer) {
    if (!buffer || shared_memory_resource_registered) {
      return;
    }
    use_resource_if_not_residency_covered(buffer, shared_memory_usage);
  };

  // Bind vertex buffers / descriptors.
  if (use_geometry_emulation || use_tessellation_emulation) {
    IRRuntimeVertexBuffers vertex_buffers = {};
    MTL::Buffer* shared_mem_buffer =
        shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
    if (shared_mem_buffer) {
      use_shared_memory_resource_if_needed(shared_mem_buffer);
      for (uint32_t i = 0; i < vertex_range_count; ++i) {
        const auto& range = vertex_ranges[i];
        size_t binding_index = range.binding_index;
        if (binding_index <
            (sizeof(vertex_buffers) / sizeof(vertex_buffers[0]))) {
          vertex_buffers[binding_index].addr =
              shared_mem_buffer->gpuAddress() + range.offset;
          vertex_buffers[binding_index].length = range.length;
          vertex_buffers[binding_index].stride = range.stride;
        }
      }
    }
    // MSC manual: bind IRRuntimeVertexBuffers at kIRVertexBufferBindPoint (6)
    // for the object stage when using geometry emulation.
    current_render_encoder_->setObjectBytes(
        vertex_buffers, sizeof(vertex_buffers), kIRVertexBufferBindPoint);
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kObject,
                                         kIRVertexBufferBindPoint);
  } else if (uses_vertex_fetch) {
    // Vertex fetch shaders read directly from shared memory via SRV, so avoid
    // stage-in bindings that can trigger invalid buffer loads.
    if (shared_memory_) {
      if (MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer()) {
        use_shared_memory_resource_if_needed(shared_mem_buffer);
      }
    }
  } else {
    // Bind vertex buffers at kIRVertexBufferBindPoint (index 6+) for stage-in.
    // The pipeline's vertex descriptor expects buffers at these indices,
    // populated from the vertex fetch constants. The buffer addresses come from
    // shared memory.
    if (shared_memory_ && !vb_bindings.empty()) {
      MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer();
      if (shared_mem_buffer) {
        // Mark shared memory as used for reading
        use_shared_memory_resource_if_needed(shared_mem_buffer);

        // Bind vertex buffers for each binding
        for (uint32_t i = 0; i < vertex_range_count; ++i) {
          const auto& range = vertex_ranges[i];
          uint64_t buffer_index =
              kIRVertexBufferBindPoint + uint64_t(range.binding_index);
          SetRenderEncoderVertexBuffer(shared_mem_buffer, range.offset,
                                       buffer_index);
        }
      }
    } else if (shared_memory_) {
      // No vertex bindings, but still mark shared memory as resident
      if (MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer()) {
        use_shared_memory_resource_if_needed(shared_mem_buffer);
      }
    }
  }

  auto validate_guest_index_range = [&](uint64_t index_base,
                                        uint32_t index_count,
                                        MTL::IndexType index_type) -> bool {
    if (!shared_memory_) {
      return false;
    }
    uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                ? sizeof(uint16_t)
                                : sizeof(uint32_t);
    uint64_t index_length = uint64_t(index_count) * index_stride;
    if (index_base > SharedMemory::kBufferSize ||
        SharedMemory::kBufferSize - index_base < index_length) {
      XELOGW(
          "Index buffer range out of bounds (base=0x{:08X} size={} count={})",
          static_cast<uint32_t>(index_base), index_length, index_count);
      return false;
    }
    return true;
  };
  auto resolve_guest_dma_index_buffer =
      [&](uint64_t guest_index_base, uint32_t index_count,
          MTL::IndexType index_type, MTL::Buffer*& index_buffer_out,
          uint64_t& index_offset_out) -> bool {
    index_buffer_out = nullptr;
    index_offset_out = 0;
    if (memexport_used) {
      if (!prepared_guest_dma_index_buffer ||
          !prepared_guest_dma_index_buffer->buffer) {
        XELOGE(
            "IssueDraw: guest DMA memexport index draw was not prepared for "
            "GPU copy");
        return false;
      }
      index_buffer_out = prepared_guest_dma_index_buffer->buffer;
      index_offset_out = prepared_guest_dma_index_buffer->offset;
      return true;
    }
    if (!validate_guest_index_range(guest_index_base, index_count,
                                    index_type)) {
      return false;
    }
    MTL::Buffer* shared_mem_buffer =
        shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
    if (!shared_mem_buffer) {
      return false;
    }
    index_buffer_out = shared_mem_buffer;
    index_offset_out = guest_index_base;
    return true;
  };

  // Shared index buffer resolution used by tessellation, geometry, and
  // standard indexed draw paths.  Returns false on fatal error.
  auto resolve_index_buffer = [&](MTL::IndexType index_type,
                                  MTL::Buffer*& index_buffer_out,
                                  uint64_t& index_offset_out) -> bool {
    index_buffer_out = nullptr;
    index_offset_out = 0;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        if (!resolve_guest_dma_index_buffer(
                primitive_processing_result.guest_index_base,
                primitive_processing_result.host_draw_vertex_count, index_type,
                index_buffer_out, index_offset_out)) {
          XELOGE("IssueDraw: failed to resolve guest DMA index buffer");
          return false;
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        if (primitive_processor_) {
          index_buffer_out = primitive_processor_->GetConvertedIndexBuffer(
              primitive_processing_result.host_index_buffer_handle,
              index_offset_out);
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        if (primitive_processor_) {
          index_buffer_out = primitive_processor_->GetBuiltinIndexBuffer();
          index_offset_out =
              primitive_processing_result.host_index_buffer_handle;
        }
        break;
      default:
        XELOGE("Unsupported index buffer type {}",
               uint32_t(primitive_processing_result.index_buffer_type));
        return false;
    }
    if (!index_buffer_out) {
      XELOGE("IssueDraw: index buffer is null for type {}",
             uint32_t(primitive_processing_result.index_buffer_type));
      return false;
    }
    use_resource_if_not_residency_covered(index_buffer_out,
                                          MTL::ResourceUsageRead);
    return true;
  };

  if (use_native_msl_primitive_mesh) {
    uint32_t mesh_threadgroup_count = 0;
    switch (native_mesh_pipeline_state->type) {
      case MetalPipelineCache::NativeMeshPipelineType::kPointList:
        mesh_threadgroup_count =
            primitive_processing_result.guest_draw_vertex_count;
        if (!mesh_threadgroup_count) {
          mesh_threadgroup_count =
              primitive_processing_result.host_draw_vertex_count;
        }
        break;
      case MetalPipelineCache::NativeMeshPipelineType::kRectangleList:
        mesh_threadgroup_count =
            primitive_processing_result.guest_draw_vertex_count / 3u;
        if (!mesh_threadgroup_count) {
          mesh_threadgroup_count =
              primitive_processing_result.host_draw_vertex_count / 4u;
        }
        break;
      case MetalPipelineCache::NativeMeshPipelineType::kQuadList:
        mesh_threadgroup_count =
            primitive_processing_result.guest_draw_vertex_count / 4u;
        if (!mesh_threadgroup_count) {
          mesh_threadgroup_count =
              primitive_processing_result.host_draw_vertex_count / 4u;
        }
        break;
    }
    if (!mesh_threadgroup_count) {
      return true;
    }
    current_render_encoder_->drawMeshThreadgroups(
        MTL::Size::Make(mesh_threadgroup_count, 1, 1), MTL::Size::Make(1, 1, 1),
        MTL::Size::Make(1, 1, 1));
  } else if (use_tessellation_emulation) {
    if (use_native_msl_tessellation) {
      if (!tessellation_pipeline_state ||
          !tessellation_pipeline_state->native_msl) {
        XELOGE("Native MSL tessellation draw missing native pipeline state");
        return false;
      }
      uint32_t control_point_count =
          tessellation_pipeline_state->control_point_count;
      if (!control_point_count) {
        XELOGE("Native MSL tessellation draw has no control point count");
        return false;
      }
      uint32_t patch_count =
          primitive_processing_result.guest_draw_vertex_count /
          control_point_count;
      if (!patch_count) {
        patch_count = primitive_processing_result.host_draw_vertex_count /
                      control_point_count;
      }
      if (!patch_count) {
        return true;
      }
      current_render_encoder_->drawMeshThreadgroups(
          MTL::Size::Make(patch_count, 1, 1), MTL::Size::Make(1, 1, 1),
          MTL::Size::Make(1, 1, 1));
      return true;
    }

    IRRuntimePrimitiveType tess_primitive = IRRuntimePrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
        tess_primitive = IRRuntimePrimitiveType3ControlPointPatchlist;
        break;
      case xenos::PrimitiveType::kQuadList:
        tess_primitive = IRRuntimePrimitiveType4ControlPointPatchlist;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        tess_primitive = (primitive_processing_result.tessellation_mode ==
                          xenos::TessellationMode::kAdaptive)
                             ? IRRuntimePrimitiveType3ControlPointPatchlist
                             : IRRuntimePrimitiveType1ControlPointPatchlist;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        tess_primitive = (primitive_processing_result.tessellation_mode ==
                          xenos::TessellationMode::kAdaptive)
                             ? IRRuntimePrimitiveType4ControlPointPatchlist
                             : IRRuntimePrimitiveType1ControlPointPatchlist;
        break;
      default:
        XELOGE(
            "Host tessellated primitive type {} returned by the primitive "
            "processor is not supported by the Metal tessellation path",
            uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    const IRRuntimeTessellationPipelineConfig& tess_config =
        tessellation_pipeline_state->config;

    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      IRRuntimeDrawPatchesTessellationEmulation(
          current_render_encoder_, tess_primitive, tess_config, 1,
          primitive_processing_result.host_draw_vertex_count, 0, 0);
    } else {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      if (!resolve_index_buffer(index_type, index_buffer, index_offset)) {
        return false;
      }
      uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t);
      uint32_t start_index =
          index_stride ? uint32_t(index_offset / index_stride) : 0;
      IRRuntimeDrawIndexedPatchesTessellationEmulation(
          current_render_encoder_, tess_primitive, index_type, index_buffer,
          tess_config, 1, primitive_processing_result.host_draw_vertex_count, 0,
          0, start_index);
    }
  } else if (use_geometry_emulation) {
    IRRuntimePrimitiveType geometry_primitive = IRRuntimePrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        geometry_primitive = IRRuntimePrimitiveTypePoint;
        break;
      case xenos::PrimitiveType::kRectangleList:
        geometry_primitive = IRRuntimePrimitiveTypeTriangle;
        break;
      case xenos::PrimitiveType::kQuadList:
        geometry_primitive = IRRuntimePrimitiveTypeLineWithAdj;
        break;
      default:
        XELOGE(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Metal geometry path",
            uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    IRRuntimeGeometryPipelineConfig geometry_config = {};
    geometry_config.gsVertexSizeInBytes =
        geometry_pipeline_state->gs_vertex_size_in_bytes;
    geometry_config.gsMaxInputPrimitivesPerMeshThreadgroup =
        geometry_pipeline_state->gs_max_input_primitives_per_mesh_threadgroup;

    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      IRRuntimeDrawPrimitivesGeometryEmulation(
          current_render_encoder_, geometry_primitive, geometry_config, 1,
          primitive_processing_result.host_draw_vertex_count, 0, 0);
    } else {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      if (!resolve_index_buffer(index_type, index_buffer, index_offset)) {
        return false;
      }
      uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t);
      uint32_t start_index =
          index_stride ? uint32_t(index_offset / index_stride) : 0;
      IRRuntimeDrawIndexedPrimitivesGeometryEmulation(
          current_render_encoder_, geometry_primitive, index_type, index_buffer,
          geometry_config, 1,
          primitive_processing_result.host_draw_vertex_count, start_index, 0,
          0);
    }
  } else {
    // Primitive topology - from primitive processor, like D3D12.
    MTL::PrimitiveType mtl_primitive = MTL::PrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        mtl_primitive = MTL::PrimitiveTypePoint;
        break;
      case xenos::PrimitiveType::kLineList:
        mtl_primitive = MTL::PrimitiveTypeLine;
        break;
      case xenos::PrimitiveType::kLineStrip:
        mtl_primitive = MTL::PrimitiveTypeLineStrip;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        mtl_primitive = MTL::PrimitiveTypeTriangle;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        mtl_primitive = MTL::PrimitiveTypeTriangleStrip;
        break;
      default:
        XELOGE(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Metal command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    // Draw using primitive processor output.
    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      IRRuntimeDrawPrimitives(
          current_render_encoder_, mtl_primitive, NS::UInteger(0),
          NS::UInteger(primitive_processing_result.host_draw_vertex_count));
    } else {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      if (!resolve_index_buffer(index_type, index_buffer, index_offset)) {
        return false;
      }
      IRRuntimeDrawIndexedPrimitives(
          current_render_encoder_, mtl_primitive,
          NS::UInteger(primitive_processing_result.host_draw_vertex_count),
          index_type, index_buffer, index_offset, NS::UInteger(1), 0, 0);
    }
  }

  if (memexport_used && shared_memory_) {
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges) {
      shared_memory_->RangeWrittenByGpu(
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
      if (NS::UInteger(memexport_write_stages)) {
        active_render_encoder_shared_memory_write_stages_ = MTL::RenderStages(
            NS::UInteger(active_render_encoder_shared_memory_write_stages_) |
            NS::UInteger(memexport_write_stages));
        MarkSharedMemoryWritePending(memexport_range.base_address_dwords << 2,
                                     memexport_range.size_bytes,
                                     memexport_write_stages, true, false);
      }
    }
  }

  submission_has_draws_ = true;

  return true;
}

bool MetalCommandProcessor::IssueCopy() {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kIssueCopy)) {
    return false;
  }
  if (!render_target_cache_) {
    XELOGW("MetalCommandProcessor::IssueCopy - No render target cache");
    return true;
  }

  MetalRenderTargetCache::ResolvePlan resolve_plan;
  if (!render_target_cache_->PrepareResolvePlan(*memory_, resolve_plan)) {
    XELOGE("MetalCommandProcessor::IssueCopy - Resolve planning failed");
    return false;
  }

  uint32_t written_address = 0;
  uint32_t written_length = 0;

  MTL::CommandBuffer* copy_command_buffer = nullptr;
  if (resolve_plan.needs_render_encoder_end) {
    // End only the render encoder here. The resolve/transfer work may still
    // reuse the current Metal command buffer and submission ordering.
    EndRenderEncoder(RenderEncoderEndReason::kResolveNeedsBoundary);
    copy_command_buffer = EnsureCommandBuffer();
    if (!copy_command_buffer) {
      XELOGE("MetalCommandProcessor::IssueCopy: failed to get command buffer");
      return false;
    }
  }

  if (!render_target_cache_->Resolve(*memory_, written_address, written_length,
                                     copy_command_buffer, &resolve_plan)) {
    XELOGE("MetalCommandProcessor::IssueCopy - Resolve failed");
    return false;
  }

  return true;
}

void MetalCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void MetalCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

namespace {

bool RegisterRangeContains(uint32_t start, uint32_t end, uint32_t range_start,
                           uint32_t range_end) {
  return start >= range_start && end <= range_end;
}

bool RegisterRangeOverlaps(uint32_t start, uint32_t end, uint32_t range_start,
                           uint32_t range_end) {
  return start < range_end && range_start < end;
}

}  // namespace

void MetalCommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (TryWriteKnownRegisterRangeFromMem(start_index, base, num_registers)) {
    return;
  }
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void MetalCommandProcessor::WriteRegisterRangeFromRing(xe::RingBuffer* ring,
                                                       uint32_t base,
                                                       uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (CanFastWriteRegisterRange(base, num_registers)) {
    WriteFastRegisterRangeFromRing(ring, base, num_registers);
    return;
  }
  CommandProcessor::WriteRegisterRangeFromRing(ring, base, num_registers);
}

bool MetalCommandProcessor::CanFastWriteRegisterRange(
    uint32_t start_index, uint32_t num_registers) const {
  if (!num_registers) {
    return true;
  }
  const uint32_t end = start_index + num_registers;
  if (end < start_index || end > RegisterFile::kRegisterCount) {
    return false;
  }
  if (RegisterRangeContains(start_index, end, XE_GPU_REG_SHADER_CONSTANT_000_X,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) ||
      RegisterRangeContains(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1) ||
      RegisterRangeContains(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
                            XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1)) {
    return true;
  }

  const bool overlaps_special =
      RegisterRangeOverlaps(start_index, end, XE_GPU_REG_SCRATCH_REG0,
                            XE_GPU_REG_SCRATCH_REG7 + 1) ||
      RegisterRangeOverlaps(start_index, end, XE_GPU_REG_COHER_STATUS_HOST,
                            XE_GPU_REG_COHER_STATUS_HOST + 1) ||
      RegisterRangeOverlaps(start_index, end, XE_GPU_REG_DC_LUT_RW_INDEX,
                            XE_GPU_REG_DC_LUT_30_COLOR + 1);
  const bool overlaps_shader_constants =
      RegisterRangeOverlaps(start_index, end, XE_GPU_REG_SHADER_CONSTANT_000_X,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) ||
      RegisterRangeOverlaps(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1) ||
      RegisterRangeOverlaps(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
                            XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1);
  return !overlaps_special && !overlaps_shader_constants;
}

bool MetalCommandProcessor::TryWriteKnownRegisterRangeFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (!CanFastWriteRegisterRange(start_index, num_registers)) {
    return false;
  }
  const uint32_t end = start_index + num_registers;
  if (RegisterRangeContains(start_index, end, XE_GPU_REG_SHADER_CONSTANT_000_X,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0)) {
    WriteShaderConstantsFromMem(start_index, base, num_registers);
    return true;
  }
  if (RegisterRangeContains(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                            XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1)) {
    WriteFetchConstantsFromMem(start_index, base, num_registers);
    return true;
  }
  if (RegisterRangeContains(start_index, end,
                            XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
                            XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1)) {
    WriteBoolLoopConstantsFromMem(start_index, base, num_registers);
    return true;
  }

  xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);
  return true;
}

void MetalCommandProcessor::WriteFastRegisterRangeFromRing(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));
  if (!range.second) {
    TryWriteKnownRegisterRangeFromMem(
        base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
        num_registers);
    ring->EndRead(range);
    return;
  }

  uint32_t first_registers =
      static_cast<uint32_t>(range.first_length / sizeof(uint32_t));
  TryWriteKnownRegisterRangeFromMem(
      base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
      first_registers);
  TryWriteKnownRegisterRangeFromMem(
      base + first_registers,
      reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
      num_registers - first_registers);
  ring->EndRead(range);
}

void MetalCommandProcessor::WriteShaderConstantsFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (cbuffer_binding_float_vertex_.up_to_date &&
      FloatConstantRangeNeedsDirty(start_index, base, num_registers,
                                   current_float_constant_map_vertex_, 0)) {
    cbuffer_binding_float_vertex_.up_to_date = false;
  }
  if (cbuffer_binding_float_pixel_.up_to_date &&
      FloatConstantRangeNeedsDirty(start_index, base, num_registers,
                                   current_float_constant_map_pixel_, 256)) {
    cbuffer_binding_float_pixel_.up_to_date = false;
  }
  xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);
}

void MetalCommandProcessor::WriteBoolLoopConstantsFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);
  cbuffer_binding_bool_loop_.up_to_date = false;
}

void MetalCommandProcessor::WriteFetchConstantsFromMem(uint32_t start_index,
                                                       uint32_t* base,
                                                       uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  DxbcShader::FetchConstantDwordMask written_fetch_dword_mask = {};
  const uint32_t dword_start =
      start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
  const uint32_t dword_end = dword_start + num_registers;
  for (uint32_t dword = dword_start; dword < dword_end; ++dword) {
    MarkFetchConstantDword(written_fetch_dword_mask, dword);
  }

  xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);

  DirtyFetchConstantDwords(written_fetch_dword_mask);
  if (texture_cache_) {
    texture_cache_->TextureFetchConstantsWritten(dword_start / 6,
                                                 (dword_end - 1) / 6);
  }
}

bool MetalCommandProcessor::FloatConstantRangeNeedsDirty(
    uint32_t start_index, const uint32_t* base, uint32_t num_registers,
    const uint64_t* constant_map, uint32_t stage_first_constant) const {
  constexpr uint32_t kStageFloatConstantCount = 256;
  const uint32_t written_dword_start =
      start_index - XE_GPU_REG_SHADER_CONSTANT_000_X;
  const uint32_t written_dword_end = written_dword_start + num_registers;
  const uint32_t stage_dword_start = stage_first_constant * 4;
  const uint32_t stage_dword_end =
      (stage_first_constant + kStageFloatConstantCount) * 4;
  uint32_t dword = std::max(written_dword_start, stage_dword_start);
  const uint32_t dword_end = std::min(written_dword_end, stage_dword_end);
  const bool dirty_on_change = ::cvars::metal_float_constants_dirty_on_change;

  while (dword < dword_end) {
    const uint32_t relative_constant = (dword - stage_dword_start) >> 2;
    const uint64_t live_constant = constant_map[relative_constant >> 6] &
                                   (uint64_t(1) << (relative_constant & 63));
    const uint32_t constant_dword_end =
        std::min(dword_end, stage_dword_start + ((relative_constant + 1) << 2));
    if (!live_constant) {
      dword = constant_dword_end;
      continue;
    }
    if (!dirty_on_change) {
      return true;
    }
    for (; dword < constant_dword_end; ++dword) {
      const uint32_t value =
          xe::load_and_swap<uint32_t>(base + (dword - written_dword_start));
      if (register_file_->values[XE_GPU_REG_SHADER_CONSTANT_000_X + dword] !=
          value) {
        return true;
      }
    }
  }
  return false;
}

void MetalCommandProcessor::DirtyFetchConstantDwords(
    const DxbcShader::FetchConstantDwordMask& dirty_mask) {
  (void)dirty_mask;
  cbuffer_binding_fetch_.up_to_date = false;
}

void MetalCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  uint32_t old_value = 0;
  bool valid_register = index < RegisterFile::kRegisterCount;
  if (valid_register) {
    old_value = register_file_->values[index];
  }
  CommandProcessor::WriteRegister(index, value);
  if (!valid_register) {
    return;
  }
  bool value_changed = old_value != value;

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (::cvars::metal_float_constants_dirty_on_change && !value_changed) {
      return;
    }
    uint32_t float_constant_index =
        (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    if (float_constant_index >= 256) {
      uint32_t rel = float_constant_index - 256;
      if (current_float_constant_map_pixel_[rel >> 6] &
          (uint64_t(1) << (rel & 63))) {
        cbuffer_binding_float_pixel_.up_to_date = false;
      }
    } else {
      if (current_float_constant_map_vertex_[float_constant_index >> 6] &
          (uint64_t(1) << (float_constant_index & 63))) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    const uint32_t fetch_dword = index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    DxbcShader::FetchConstantDwordMask changed_fetch_dword_mask = {};
    MarkFetchConstantDword(changed_fetch_dword_mask, fetch_dword);
    DirtyFetchConstantDwords(changed_fetch_dword_mask);
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten(fetch_dword / 6);
    }
  }
}

MTL::CommandBuffer* MetalCommandProcessor::EnsureCommandBuffer() {
  ProcessCompletedSubmissions();
  if (current_command_buffer_) {
    return current_command_buffer_;
  }
  if (!command_queue_) {
    XELOGE("EnsureCommandBuffer: no command queue");
    return nullptr;
  }
  const bool is_opening_frame = !frame_open_;
  if (is_opening_frame) {
    OpenFrameLifetime();
  }

  EnsureCommandBufferAutoreleasePool();

  // Note: commandBuffer() returns an autoreleased object, we must retain it.
  current_command_buffer_ = command_queue_->commandBuffer();
  if (!current_command_buffer_) {
    XELOGE("EnsureCommandBuffer: failed to create command buffer");
    DrainCommandBufferAutoreleasePool();
    return nullptr;
  }
  current_command_buffer_->retain();

  ++submission_current_;

  current_command_buffer_->setLabel(
      NS::String::string("XeniaCommandBuffer", NS::UTF8StringEncoding));

  pending_completion_handlers_.fetch_add(1, std::memory_order_relaxed);
  current_command_buffer_->addCompletedHandler(
      [this](MTL::CommandBuffer* completed_cmd) {
        if (completed_cmd->status() == MTL::CommandBufferStatusError) {
          NS::Error* error = completed_cmd->error();
          if (error) {
            XELOGE("Metal command buffer error: {}",
                   error->localizedDescription()->utf8String());
          }
        }
        {
          std::lock_guard<std::mutex> lock(completion_mutex_);
          completed_command_buffers_.fetch_add(1, std::memory_order_release);
          pending_completion_handlers_.fetch_sub(1, std::memory_order_release);
        }
        completion_cond_.notify_all();
      });

  if (texture_cache_) {
    texture_cache_->BeginSubmission(submission_current_);
  }
  submission_has_draws_ = false;
  if (is_opening_frame) {
    if (primitive_processor_) {
      primitive_processor_->BeginFrame();
    }
    if (render_target_cache_) {
      render_target_cache_->BeginFrame();
    }
    if (texture_cache_) {
      texture_cache_->BeginFrame();
    }
    frame_open_ = true;
  }

  return current_command_buffer_;
}

void MetalCommandProcessor::ProcessCompletedSubmissions() {
  const uint64_t completed =
      completed_command_buffers_.load(std::memory_order_relaxed);
  if (completed <= submission_completed_processed_) {
    return;
  }
  submission_completed_processed_ = completed;
  if (texture_cache_) {
    texture_cache_->CompletedSubmissionUpdated(completed);
  }
  PruneCompletedSharedMemoryWrites(completed);
  while (!retired_view_bindless_indices_.empty() &&
         retired_view_bindless_indices_.front().submission_id <= completed) {
    FreeViewBindlessIndexNow(retired_view_bindless_indices_.front().index);
    retired_view_bindless_indices_.pop_front();
  }
  while (!retired_sampler_bindless_indices_.empty() &&
         retired_sampler_bindless_indices_.front().submission_id <= completed) {
    FreeSamplerBindlessIndexNow(
        retired_sampler_bindless_indices_.front().index);
    retired_sampler_bindless_indices_.pop_front();
  }
  while (!retired_memexport_index_buffers_.empty() &&
         retired_memexport_index_buffers_.front().submission_id <= completed) {
    if (retired_memexport_index_buffers_.front().buffer) {
      retired_memexport_index_buffers_.front().buffer->release();
    }
    retired_memexport_index_buffers_.pop_front();
  }
}

void MetalCommandProcessor::WaitForFrameSlotSubmission(
    uint64_t awaited_submission) {
  if (!awaited_submission) {
    return;
  }
  if (completed_command_buffers_.load(std::memory_order_acquire) >=
      awaited_submission) {
    return;
  }
  ++backend_telemetry_.frame_slot_waits;
  ++backend_telemetry_.frame_slot_wait_submission_count;
  backend_telemetry_.frame_slot_wait_submission_last = awaited_submission;
  std::unique_lock<std::mutex> lock(completion_mutex_);
  completion_cond_.wait(lock, [&]() {
    return completed_command_buffers_.load(std::memory_order_acquire) >=
           awaited_submission;
  });
}

void MetalCommandProcessor::OpenFrameLifetime() {
  const uint64_t awaited_submission =
      closed_frame_submissions_[frame_current_ % kMaxFramesInFlight];
  WaitForFrameSlotSubmission(awaited_submission);
  ProcessCompletedSubmissions();

  const uint64_t completed_submission =
      completed_command_buffers_.load(std::memory_order_acquire);
  frame_completed_ = std::max(frame_current_, uint64_t(kMaxFramesInFlight)) -
                     kMaxFramesInFlight;
  for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
    if (closed_frame_submissions_[frame % kMaxFramesInFlight] >
        completed_submission) {
      break;
    }
    frame_completed_ = frame;
  }

  if (constant_buffer_pool_) {
    constant_buffer_pool_->Reclaim(frame_completed_);
  }
  InvalidateFrameTransientBindings();
}

void MetalCommandProcessor::InvalidateFrameTransientBindings() {
  cbuffer_binding_system_.up_to_date = false;
  cbuffer_binding_float_vertex_.up_to_date = false;
  cbuffer_binding_float_pixel_.up_to_date = false;
  cbuffer_binding_bool_loop_.up_to_date = false;
  cbuffer_binding_fetch_.up_to_date = false;
  current_fetch_constant_payload_valid_ = false;
  cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
  cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
  std::memset(current_float_constant_map_vertex_, 0,
              sizeof(current_float_constant_map_vertex_));
  std::memset(current_float_constant_map_pixel_, 0,
              sizeof(current_float_constant_map_pixel_));
  for (NativeMslRuntimeInfoUploadCache& cache :
       native_msl_runtime_info_upload_cache_) {
    cache.buffer = nullptr;
    cache.offset = 0;
    cache.size = 0;
    cache.upload_frame = 0;
  }
  for (NativeMslDrawConstantsUploadCache& cache :
      native_msl_draw_constants_upload_cache_) {
    cache.payload = {};
    cache.buffer = nullptr;
    cache.offset = 0;
    cache.payload_valid = false;
    cache.upload_frame = 0;
  }
  native_msl_primitive_index_upload_cache_ = {};

  graphics_root_argument_state_ = {};
  current_bindless_cbv_gpu_addresses_ = {};
  current_bindless_cbv_buffers_ = {};
  current_bindless_cbv_offsets_ = {};
  current_bindless_active_cbv_masks_.fill(0);
  current_bindless_root_resource_set_ = {};
  current_bindless_root_resource_source_serial_ = 0;
  render_encoder_bindless_root_resources_serial_ = 0;
  current_bindless_shared_memory_is_uav_ = false;
}

void MetalCommandProcessor::CloseFrameLifetime() {
  if (!frame_open_) {
    return;
  }
  if (primitive_processor_) {
    primitive_processor_->EndFrame();
  }
  frame_open_ = false;

  const uint32_t frame_slot = uint32_t(frame_current_ % kMaxFramesInFlight);
  const uint64_t previous_slot_submission =
      closed_frame_submissions_[frame_slot];
  assert_true(!previous_slot_submission ||
              previous_slot_submission <=
                  completed_command_buffers_.load(std::memory_order_acquire));
  closed_frame_submissions_[frame_slot] = submission_current_;
  ++frame_current_;
}

void MetalCommandProcessor::MaybeDumpBackendTelemetry(const char* reason,
                                                      bool force) {
  if (!::cvars::metal_backend_telemetry) {
    return;
  }
  int32_t interval_config = ::cvars::metal_backend_telemetry_interval;
  uint64_t interval = interval_config > 0 ? uint64_t(interval_config) : 0;
  if (!force &&
      (!interval || backend_telemetry_.swaps <
                        backend_telemetry_last_dump_swap_ + interval)) {
    return;
  }

  MetalRenderTargetCache::TelemetryStats rt_stats = {};
  if (render_target_cache_) {
    rt_stats = render_target_cache_->GetAndResetTelemetryStats();
  }

  const std::string end_reasons = MetalFormatNamedCounts(
      backend_telemetry_.end_reasons, MetalRenderEncoderEndReasonName);
  const std::string transfer_request_sources = MetalFormatNamedTriplets(
      backend_telemetry_.transfer_request_sources_total,
      backend_telemetry_.transfer_request_sources_active,
      backend_telemetry_.transfer_request_sources_no_active,
      MetalTransferRequestSourceName);
  const std::string transfer_request_render_end_sources =
      MetalFormatNamedCounts(
          backend_telemetry_.transfer_request_render_encoder_ends,
          MetalTransferRequestSourceName);
  const std::string shared_memory_request_upload_bytes = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_request_upload_bytes,
      MetalSharedMemoryRequestReasonName);
  const std::string shared_memory_request_failures =
      MetalFormatNamedCounts(backend_telemetry_.shared_memory_request_failures,
                             MetalSharedMemoryRequestReasonName);
  const std::string shared_memory_request_outcomes =
      MetalFormatNamedCounts(backend_telemetry_.shared_memory_request_outcomes,
                             MetalSharedMemoryRequestOutcomeName);
  const std::string shared_memory_upload_route_counts = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_upload_route_counts,
      MetalSharedMemoryUploadRouteName);
  const std::string shared_memory_upload_route_bytes = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_upload_route_bytes,
      MetalSharedMemoryUploadRouteName);
  const std::string shared_memory_direct_write_rejects = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_direct_write_reject_counts,
      MetalSharedMemoryDirectWriteRejectReasonName);
  const std::string shared_memory_direct_write_reject_bytes =
      MetalFormatNamedCounts(
          backend_telemetry_.shared_memory_direct_write_reject_bytes,
          MetalSharedMemoryDirectWriteRejectReasonName);
  const std::string texture_upload_source_route_counts = MetalFormatNamedCounts(
      backend_telemetry_.texture_upload_source_route_counts,
      MetalTextureUploadSourceRouteName);
  const std::string texture_upload_source_route_bytes = MetalFormatNamedCounts(
      backend_telemetry_.texture_upload_source_route_bytes,
      MetalTextureUploadSourceRouteName);
  const std::string texture_upload_source_fallback_reasons =
      MetalFormatNamedCounts(
          backend_telemetry_.texture_upload_source_fallback_reasons,
          MetalTextureUploadSourceFallbackReasonName);
  const std::string texture_upload_compatibility_counts =
      MetalFormatNamedCounts(
          backend_telemetry_.texture_upload_compatibility_counts,
          MetalTextureUploadCompatibilityClassName);
  const std::string texture_upload_compatibility_bytes = MetalFormatNamedCounts(
      backend_telemetry_.texture_upload_compatibility_bytes,
      MetalTextureUploadCompatibilityClassName);
  const std::string texture_upload_compute_blockers = MetalFormatNamedCounts(
      backend_telemetry_.texture_upload_compute_blocker_counts,
      MetalTextureUploadComputeBlockerName);
  const std::string texture_upload_compute_blocker_bytes =
      MetalFormatNamedCounts(
          backend_telemetry_.texture_upload_compute_blocker_bytes,
          MetalTextureUploadComputeBlockerName);
  const std::string texture_upload_execution_details = MetalFormatNamedCounts(
      backend_telemetry_.texture_upload_execution_details,
      MetalTextureUploadExecutionDetailName);
  const std::string texture_reload_reason_counts =
      MetalFormatNamedCounts(backend_telemetry_.texture_reload_reason_counts,
                             MetalTextureReloadReasonName);
  const std::string texture_reload_reason_bytes =
      MetalFormatNamedCounts(backend_telemetry_.texture_reload_reason_bytes,
                             MetalTextureReloadReasonName);
  const std::string texture_watch_invalidation_counts = MetalFormatNamedCounts(
      backend_telemetry_.texture_watch_invalidation_counts,
      MetalTextureWatchInvalidationReasonName);
  const std::string texture_watch_invalidation_bytes = MetalFormatNamedCounts(
      backend_telemetry_.texture_watch_invalidation_bytes,
      MetalTextureWatchInvalidationReasonName);
  const std::string texture_resolve_reload_counts =
      MetalFormatNamedCounts(backend_telemetry_.texture_resolve_reload_counts,
                             MetalTextureResolveReloadReasonName);
  const std::string texture_resolve_reload_bytes =
      MetalFormatNamedCounts(backend_telemetry_.texture_resolve_reload_bytes,
                             MetalTextureResolveReloadReasonName);
  const std::string shared_memory_upload_encoder_end_reasons =
      MetalFormatNamedCounts(
          backend_telemetry_.shared_memory_upload_encoder_end_reasons,
          MetalSharedMemoryUploadEncoderEndReasonName);
  const std::string draw_materialization_source_ranges = MetalFormatNamedCounts(
      backend_telemetry_.draw_materialization_source_ranges,
      MetalTelemetryDrawMaterializationSourceName);
  const std::string draw_materialization_source_bytes = MetalFormatNamedCounts(
      backend_telemetry_.draw_materialization_source_bytes,
      MetalTelemetryDrawMaterializationSourceName);
  const std::string draw_materialization_source_invalid_ranges =
      MetalFormatNamedCounts(
          backend_telemetry_.draw_materialization_source_invalid_ranges,
          MetalTelemetryDrawMaterializationSourceName);
  const std::string draw_materialization_source_invalid_bytes =
      MetalFormatNamedCounts(
          backend_telemetry_.draw_materialization_source_invalid_bytes,
          MetalTelemetryDrawMaterializationSourceName);
  const std::string prepared_draw_queue_flush_reasons = MetalFormatNamedCounts(
      backend_telemetry_.prepared_draw_queue_flush_reasons,
      MetalPreparedDrawFlushReasonName);
  const std::string prepared_draw_queue_reject_reasons = MetalFormatNamedCounts(
      backend_telemetry_.prepared_draw_queue_reject_reasons,
      MetalPreparedDrawQueueRejectReasonName);
  const std::string cbv_uploads = MetalFormatNamedCounts(
      backend_telemetry_.cbv_uploads, MetalTelemetryCbvSlotName);
  const std::string cbv_reuse_hits = MetalFormatNamedCounts(
      backend_telemetry_.cbv_reuse_hits, MetalTelemetryCbvSlotName);
  const std::string descriptor_index_uploads =
      MetalFormatNamedCounts(backend_telemetry_.descriptor_index_uploads,
                             MetalTelemetryShaderStageName);
  const std::string bindless_root_allocations =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_allocations,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_reuse_hits = MetalFormatNamedCounts(
      backend_telemetry_.bindless_root_reuse_hits, MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_noop_updates =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_noop_updates,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_slots_patched =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_slots_patched,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_bytes_copied =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_bytes_copied,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_slot_patches =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_slot_patches,
                             MetalTelemetryRootArgSlotName);
  const std::string bindless_root_rebuild_reasons =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_rebuild_reasons,
                             MetalTelemetryRootRebuildReasonName);
  const std::string native_msl_draw_constants_rebuild_reasons =
      MetalFormatNamedCounts(
          backend_telemetry_.native_msl_draw_constants_rebuild_reasons,
          MetalTelemetryNativeMslDrawConstantsRebuildReasonName);
  const std::string native_msl_draw_constants_change_masks =
      MetalFormatNamedCounts(
          backend_telemetry_.native_msl_draw_constants_change_masks,
          MetalTelemetryNativeMslDrawConstantsChangeMaskName);
  const std::string encoder_buffer_full_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_full_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_offset_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_offset_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_noop_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_noop_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_null_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_null_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_untracked_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_untracked_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_slot_full_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_slot_full_binds,
      MetalTelemetryRenderEncoderBufferSlotName);
  const std::string encoder_buffer_slot_offset_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_slot_offset_binds,
      MetalTelemetryRenderEncoderBufferSlotName);
  const std::string encoder_buffer_slot_noop_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_slot_noop_binds,
      MetalTelemetryRenderEncoderBufferSlotName);
  const uint64_t encoder_buffer_full_bind_total = MetalTelemetrySumCounts(
      backend_telemetry_.render_encoder_buffer_full_binds);
  const uint64_t encoder_buffer_offset_bind_total = MetalTelemetrySumCounts(
      backend_telemetry_.render_encoder_buffer_offset_binds);
  const uint64_t encoder_buffer_noop_bind_total = MetalTelemetrySumCounts(
      backend_telemetry_.render_encoder_buffer_noop_binds);
  const uint64_t encoder_buffer_slot_full_bind_total = MetalTelemetrySumCounts(
      backend_telemetry_.render_encoder_buffer_slot_full_binds);
  const uint64_t encoder_buffer_slot_offset_bind_total = MetalTelemetrySumCounts(
      backend_telemetry_.render_encoder_buffer_slot_offset_binds);
  const uint64_t encoder_buffer_slot_noop_bind_total = MetalTelemetrySumCounts(
      backend_telemetry_.render_encoder_buffer_slot_noop_binds);
  const std::string render_resource_set_applies =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_set_applies,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_set_skips =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_set_skips,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_set_resources =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_set_resources,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_registry_serial_skips =
      MetalFormatNamedCounts(
          backend_telemetry_.render_resource_registry_serial_skips,
          MetalTelemetryRenderResourceSetName);
  const std::string render_resource_registry_builds =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_registry_builds,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_registry_registers = MetalFormatNamedCounts(
      backend_telemetry_.render_resource_registry_registers,
      MetalTelemetryRenderResourceSetName);
  const auto& direct_host_stats = rt_stats.resolve_direct_host;
  MetalStageCompileCacheStats stage_compile_stats = {};
  MetalPipelineRuntimeStats pipeline_runtime_stats = {};
  if (pipeline_cache_) {
    stage_compile_stats = pipeline_cache_->GetAndResetStageCompileStats();
    pipeline_runtime_stats = pipeline_cache_->GetAndResetRuntimeStats();
  }

  XELOGI(
      "MetalTelemetry[{}]: work swaps={} draws={} pipelines set/skip={}/{} "
      "texture_requests before/after_encoder={}/{}",
      reason, backend_telemetry_.swaps - backend_telemetry_last_dump_swap_,
      backend_telemetry_.draw_calls, backend_telemetry_.pipeline_sets,
      backend_telemetry_.pipeline_set_skips,
      backend_telemetry_.texture_requests_before_encoder,
      backend_telemetry_.texture_requests_after_encoder_begin);
  XELOGI(
      "MetalTelemetry[{}]: render_encoder begin_calls={} reused={} created={} "
      "descriptor_restarts={} resource_resets={} desc_fail={} create_fail={} "
      "end active/no_active={}/{} reasons={{ {} }}",
      reason, backend_telemetry_.begin_encoder_calls,
      backend_telemetry_.begin_encoder_reused_compatible,
      backend_telemetry_.begin_encoder_created,
      backend_telemetry_.begin_encoder_descriptor_restarts,
      backend_telemetry_.begin_encoder_resource_usage_resets,
      backend_telemetry_.begin_encoder_descriptor_failures,
      backend_telemetry_.begin_encoder_creation_failures,
      backend_telemetry_.end_encoder_active,
      backend_telemetry_.end_encoder_no_active, end_reasons);
  XELOGI(
      "MetalTelemetry[{}]: transfer_request sources "
      "total/active/no_active={{ {} }} render_end={{ {} }}",
      reason, transfer_request_sources, transfer_request_render_end_sources);
  XELOGI("MetalTelemetry[{}]: shared_memory request failures={{ {} }}", reason,
         shared_memory_request_failures);
  XELOGI(
      "MetalTelemetry[{}]: shared_memory upload bytes={{ {} }} "
      "outcomes={{ {} }}",
      reason, shared_memory_request_upload_bytes,
      shared_memory_request_outcomes);
  XELOGI(
      "MetalTelemetry[{}]: shared_memory upload_batches requests={} "
      "ranges input/coalesced={}/{} bytes={} upload_encoder "
      "acquire/reuse/copies={}/{}/{} closes={{ {} }} routes counts={{ {} }} "
      "bytes={{ {} }}",
      reason, backend_telemetry_.shared_memory_upload_batches,
      backend_telemetry_.shared_memory_upload_batch_input_ranges,
      backend_telemetry_.shared_memory_upload_batch_coalesced_ranges,
      backend_telemetry_.shared_memory_upload_batch_bytes,
      backend_telemetry_.shared_memory_upload_encoder_acquisitions,
      backend_telemetry_.shared_memory_upload_encoder_reuses,
      backend_telemetry_.shared_memory_upload_encoder_copies,
      shared_memory_upload_encoder_end_reasons,
      shared_memory_upload_route_counts, shared_memory_upload_route_bytes);
  XELOGI(
      "MetalTelemetry[{}]: shared_memory direct_write "
      "eligible/staged_bytes={}/{} rejects={{ {} }} reject_bytes={{ {} }}",
      reason, backend_telemetry_.shared_memory_direct_write_eligible_bytes,
      backend_telemetry_.shared_memory_direct_write_staged_required_bytes,
      shared_memory_direct_write_rejects,
      shared_memory_direct_write_reject_bytes);
  XELOGI(
      "MetalTelemetry[{}]: shared_memory lazy_upload batches "
      "no_upload/direct_only/mixed/staged_only={}/{}/{}/{} "
      "render_active direct_only/mixed/staged_only={}/{}/{}",
      reason, backend_telemetry_.shared_memory_lazy_upload_no_upload_batches,
      backend_telemetry_.shared_memory_lazy_upload_direct_only_batches,
      backend_telemetry_.shared_memory_lazy_upload_mixed_batches,
      backend_telemetry_.shared_memory_lazy_upload_staged_only_batches,
      backend_telemetry_.shared_memory_lazy_upload_direct_only_active,
      backend_telemetry_.shared_memory_lazy_upload_mixed_active,
      backend_telemetry_.shared_memory_lazy_upload_staged_only_active);
  XELOGI(
      "MetalTelemetry[{}]: texture_upload_source routes counts={{ {} }} "
      "bytes={{ {} }} fallback_reasons={{ {} }}",
      reason, texture_upload_source_route_counts,
      texture_upload_source_route_bytes,
      texture_upload_source_fallback_reasons);
  XELOGI(
      "MetalTelemetry[{}]: texture_upload_classification "
      "classes counts={{ {} }} bytes={{ {} }} blockers={{ {} }} "
      "blocker_bytes={{ {} }} execution={{ {} }}",
      reason, texture_upload_compatibility_counts,
      texture_upload_compatibility_bytes, texture_upload_compute_blockers,
      texture_upload_compute_blocker_bytes, texture_upload_execution_details);
  XELOGI(
      "MetalTelemetry[{}]: texture_reload reasons counts={{ {} }} "
      "bytes={{ {} }}",
      reason, texture_reload_reason_counts, texture_reload_reason_bytes);
  XELOGI(
      "MetalTelemetry[{}]: texture_watch_invalidations counts={{ {} }} "
      "bytes={{ {} }}",
      reason, texture_watch_invalidation_counts,
      texture_watch_invalidation_bytes);
  XELOGI(
      "MetalTelemetry[{}]: texture_resolve_reload counts={{ {} }} "
      "bytes={{ {} }}",
      reason, texture_resolve_reload_counts, texture_resolve_reload_bytes);
  XELOGI(
      "MetalTelemetry[{}]: draw_materialization ranges={{ {} }} bytes={{ {} }} "
      "invalid_ranges={{ {} }} invalid_bytes={{ {} }} "
      "per_draw requests/invalid/resident_skip={}/{}/{}",
      reason, draw_materialization_source_ranges,
      draw_materialization_source_bytes,
      draw_materialization_source_invalid_ranges,
      draw_materialization_source_invalid_bytes,
      backend_telemetry_.draw_materialization_per_draw_requests,
      backend_telemetry_.draw_materialization_per_draw_invalid_requests,
      backend_telemetry_.draw_materialization_per_draw_resident_skips);
  XELOGI(
      "MetalTelemetry[{}]: prepared_draw_queue appends={} flushes={} "
      "single_draw_flushes={} draws_flushed={} ranges_flushed={} "
      "bytes_flushed={} invalid_flushes={} texture_plans={} "
      "texture_loads planned/executed={}/{} flush_reasons={{ {} }} "
      "rejects={{ {} }}",
      reason, backend_telemetry_.prepared_draw_queue_appends,
      backend_telemetry_.prepared_draw_queue_flushes,
      backend_telemetry_.prepared_draw_queue_single_draw_flushes,
      backend_telemetry_.prepared_draw_queue_draws_flushed,
      backend_telemetry_.prepared_draw_queue_ranges_flushed,
      backend_telemetry_.prepared_draw_queue_bytes_flushed,
      backend_telemetry_.prepared_draw_queue_invalid_flushes,
      backend_telemetry_.prepared_draw_queue_texture_plans_flushed,
      backend_telemetry_.prepared_draw_queue_texture_loads_planned,
      backend_telemetry_.prepared_draw_queue_texture_loads_executed,
      prepared_draw_queue_flush_reasons, prepared_draw_queue_reject_reasons);
  XELOGI(
      "MetalTelemetry[{}]: constants cbv_uploads={{ {} }} "
      "cbv_reuse={{ {} }} descriptor_index_uploads={{ {} }} "
      "root_allocations={{ {} }} root_reuse={{ {} }} "
      "root_rebuild_reasons={{ {} }} frame_slot_waits={} "
      "waited_submissions={} last_wait_submission={}",
      reason, cbv_uploads, cbv_reuse_hits, descriptor_index_uploads,
      bindless_root_allocations, bindless_root_reuse_hits,
      bindless_root_rebuild_reasons, backend_telemetry_.frame_slot_waits,
      backend_telemetry_.frame_slot_wait_submission_count,
      backend_telemetry_.frame_slot_wait_submission_last);
  XELOGI(
      "MetalTelemetry[{}]: root_args allocs={{ {} }} reuse={{ {} }} "
      "noop={{ {} }} slots_patched={{ {} }} bytes_copied={{ {} }} "
      "slot_patches={{ {} }} rebuild_reasons={{ {} }}",
      reason, bindless_root_allocations, bindless_root_reuse_hits,
      bindless_root_arg_noop_updates, bindless_root_arg_slots_patched,
      bindless_root_arg_bytes_copied, bindless_root_arg_slot_patches,
      bindless_root_rebuild_reasons);
  if (cvars::metal_root_rebuild_detail_telemetry) {
    const std::string bindless_root_slots_changed =
        MetalFormatNamedCounts(backend_telemetry_.bindless_root_slots_changed,
                               MetalTelemetryRootSlotsChangedName);
    const std::string bindless_root_rebuild_details =
        MetalFormatNamedCounts(backend_telemetry_.bindless_root_rebuild_details,
                               MetalTelemetryRootRebuildDetailName);
    XELOGI(
        "MetalTelemetry[{}]: root_rebuild_detail slots_changed={{ {} }} "
        "details={{ {} }}",
        reason, bindless_root_slots_changed, bindless_root_rebuild_details);
  }
  XELOGI(
      "MetalTelemetry[{}]: encoder_bindings full={{ {} }} offset={{ {} }} "
      "noop={{ {} }} null={{ {} }} untracked={{ {} }} resource_use "
      "calls/skips/upgrades={}/{}/{} batches/resources/skips={}/{}/{} "
      "resource_sets apply={{ {} }} skip={{ {} }} resources={{ {} }}",
      reason, encoder_buffer_full_binds, encoder_buffer_offset_binds,
      encoder_buffer_noop_binds, encoder_buffer_null_binds,
      encoder_buffer_untracked_binds,
      backend_telemetry_.render_encoder_use_resource_calls,
      backend_telemetry_.render_encoder_use_resource_skips,
      backend_telemetry_.render_encoder_use_resource_upgrades,
      backend_telemetry_.render_encoder_use_resources_batches,
      backend_telemetry_.render_encoder_use_resources_requested,
      backend_telemetry_.render_encoder_use_resources_skips,
      render_resource_set_applies, render_resource_set_skips,
      render_resource_set_resources);
  XELOGI(
      "MetalTelemetry[{}]: encoder_binding_slots full={{ {} }} offset={{ {} }} "
      "noop={{ {} }}",
      reason, encoder_buffer_slot_full_binds, encoder_buffer_slot_offset_binds,
      encoder_buffer_slot_noop_binds);
  XELOGI(
      "MetalTelemetry[{}]: native_msl_draw_constants reasons={{ {} }}",
      reason, native_msl_draw_constants_rebuild_reasons);
  XELOGI(
      "MetalTelemetry[{}]: native_msl_draw_constants change_masks={{ {} }}",
      reason, native_msl_draw_constants_change_masks);
  XELOGI(
      "MetalTelemetry[{}]: encoder_binding_slot_totals "
      "stage_full/slot_full={} / {} stage_offset/slot_offset={} / {} "
      "stage_noop/slot_noop={} / {}",
      reason, encoder_buffer_full_bind_total,
      encoder_buffer_slot_full_bind_total, encoder_buffer_offset_bind_total,
      encoder_buffer_slot_offset_bind_total, encoder_buffer_noop_bind_total,
      encoder_buffer_slot_noop_bind_total);
  XELOGI(
      "MetalTelemetry[{}]: resource_registry serial_skip={{ {} }} "
      "build={{ {} }} register={{ {} }}",
      reason, render_resource_registry_serial_skips,
      render_resource_registry_builds, render_resource_registry_registers);
  XELOGI(
      "MetalTelemetry[{}]: residency_set supported/enabled/attached={}/{}/{} "
      "allocations added/duplicates/commits={}/{}/{} "
      "resource_refs covered/fallback={}/{} "
      "use_resource covered/fallback={}/{} "
      "use_heap covered/fallback={}/{} live_allocations={}",
      reason, residency_set_supported_ ? 1 : 0, residency_set_enabled_ ? 1 : 0,
      residency_set_attached_ ? 1 : 0,
      backend_telemetry_.residency_set_allocations_added,
      backend_telemetry_.residency_set_allocation_duplicates,
      backend_telemetry_.residency_set_commits,
      backend_telemetry_.residency_set_resource_refs_covered,
      backend_telemetry_.residency_set_resource_refs_fallback,
      backend_telemetry_.residency_set_use_resources_covered,
      backend_telemetry_.residency_set_use_resources_fallback,
      backend_telemetry_.residency_set_use_heaps_covered,
      backend_telemetry_.residency_set_use_heaps_fallback,
      residency_set_ ? uint64_t(residency_set_->allocationCount()) : 0);
  XELOGI(
      "MetalTelemetry[{}]: resolve_direct_host attempt/success={}/{} "
      "reject gamma/exp_bias/format/sample/depth_no_fast={}/{}/{}/{}/{}",
      reason, direct_host_stats.direct_host_attempt,
      direct_host_stats.direct_host_success,
      direct_host_stats.direct_host_reject_gamma,
      direct_host_stats.direct_host_reject_exp_bias,
      direct_host_stats.direct_host_reject_format_mismatch,
      direct_host_stats.direct_host_reject_sample_select,
      direct_host_stats.direct_host_reject_depth_no_fast);
  XELOGI(
      "MetalTelemetry[{}]: stage_compile requests={} hits/misses={}/{} "
      "waits={} failures={} persistent hits/misses={}/{} "
      "bytes dxil/metallib={}/{} owner_ms total/max={}/{} "
      "wait_ms total/max={}/{}",
      reason, stage_compile_stats.requests, stage_compile_stats.memory_hits,
      stage_compile_stats.memory_misses, stage_compile_stats.waits,
      stage_compile_stats.failures, stage_compile_stats.persistent_hits,
      stage_compile_stats.persistent_misses, stage_compile_stats.dxil_bytes,
      stage_compile_stats.metallib_bytes,
      stage_compile_stats.owner_compile_ms_total,
      stage_compile_stats.owner_compile_ms_max,
      stage_compile_stats.wait_ms_total, stage_compile_stats.wait_ms_max);
  XELOGI(
      "MetalTelemetry[{}]: shader_prep dxil requests/hits/misses/failures="
      "{}/{}/{}/{} "
      "bytes dxbc/dxil={}/{} ms total/max={}/{} libraries "
      "requests/failures={}/{} bytes={} ms total/max={}/{} render_pso "
      "requests/failures={}/{} ms total/max={}/{}",
      reason, pipeline_runtime_stats.dxil_convert_requests,
      pipeline_runtime_stats.dxil_cache_hits,
      pipeline_runtime_stats.dxil_cache_misses,
      pipeline_runtime_stats.dxil_convert_failures,
      pipeline_runtime_stats.dxil_convert_dxbc_bytes,
      pipeline_runtime_stats.dxil_convert_dxil_bytes,
      pipeline_runtime_stats.dxil_convert_ms_total,
      pipeline_runtime_stats.dxil_convert_ms_max,
      pipeline_runtime_stats.library_requests,
      pipeline_runtime_stats.library_failures,
      pipeline_runtime_stats.library_bytes,
      pipeline_runtime_stats.library_ms_total,
      pipeline_runtime_stats.library_ms_max,
      pipeline_runtime_stats.render_pipeline_requests,
      pipeline_runtime_stats.render_pipeline_failures,
      pipeline_runtime_stats.render_pipeline_ms_total,
      pipeline_runtime_stats.render_pipeline_ms_max);
  ResetBackendTelemetry();
}

void MetalCommandProcessor::ResetBackendTelemetry() {
  backend_telemetry_last_dump_swap_ = backend_telemetry_.swaps;
  backend_telemetry_ = BackendTelemetryStats();
  backend_telemetry_.swaps = backend_telemetry_last_dump_swap_;
}

void MetalCommandProcessor::EnsureCommandBufferAutoreleasePool() {
  if (command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_ = NS::AutoreleasePool::alloc()->init();
}

void MetalCommandProcessor::DrainCommandBufferAutoreleasePool() {
  if (!command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_->release();
  command_buffer_autorelease_pool_ = nullptr;
}

void MetalCommandProcessor::EndRenderEncoder() {
  EndRenderEncoder(RenderEncoderEndReason::kUnknown);
}

void MetalCommandProcessor::EndRenderEncoder(RenderEncoderEndReason reason) {
  if (!flushing_prepared_draw_queue_ && !prepared_draw_queue_.empty()) {
    if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kRenderEncoderEnd)) {
      XELOGE("Metal EndRenderEncoder: failed to flush prepared draw queue");
    }
  }
  size_t reason_index = static_cast<size_t>(reason);
  if (reason_index < backend_telemetry_.end_reasons.size()) {
    ++backend_telemetry_.end_reasons[reason_index];
  }
  if (!current_render_encoder_) {
    ++backend_telemetry_.end_encoder_no_active;
    current_render_encoder_has_zpd_visibility_ = false;
    if (current_render_pass_descriptor_) {
      current_render_pass_descriptor_->release();
      current_render_pass_descriptor_ = nullptr;
    }
    ResetRenderEncoderBufferBindings();
    ResetRenderEncoderResourceUsage();
    return;
  }
  ++backend_telemetry_.end_encoder_active;
  // Close any active ZPD query segment before ending the encoder.
  // Metal visibility results are scoped to the render encoder; the offset
  // cannot be reused after the encoder is ended.
  if (GetZPDMode() != ZPDMode::kFake) {
    CloseQuerySegment();
  }
  UpdateSharedMemoryFenceForActiveRenderEncoder();
  current_render_encoder_->endEncoding();
  current_render_encoder_->release();
  current_render_encoder_ = nullptr;
  current_render_encoder_has_zpd_visibility_ = false;
  ResetRenderEncoderBufferBindings();
  ResetRenderEncoderResourceUsage();
  if (current_render_pass_descriptor_) {
    current_render_pass_descriptor_->release();
    current_render_pass_descriptor_ = nullptr;
  }
  current_render_pipeline_state_ = nullptr;
  rasterizer_state_valid_ = false;
  current_depth_stencil_state_ = nullptr;
  stencil_reference_valid_ = false;
  heap_binds_set_on_encoder_ = false;
  ResetRenderEncoderBufferBindings();
}

void MetalCommandProcessor::InvalidateRenderEncoderStateAfterDrawPassTransfers(
    MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations) {
  if (!mutations) {
    return;
  }
  using RTC = MetalRenderTargetCache;
  if (mutations & RTC::kDrawPassTransferEncoderMutationPipeline) {
    current_render_pipeline_state_ = nullptr;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationDepthStencil) {
    current_depth_stencil_state_ = nullptr;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationStencilReference) {
    stencil_reference_valid_ = false;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationViewport) {
    viewport_dirty_ = true;
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationScissor) {
    scissor_dirty_ = true;
  }

  constexpr RTC::DrawPassTransferEncoderMutationMask kTransferBufferMutations =
      RTC::kDrawPassTransferEncoderMutationVertexSlot0 |
      RTC::kDrawPassTransferEncoderMutationVertexSlot1 |
      RTC::kDrawPassTransferEncoderMutationFragmentSlot0 |
      RTC::kDrawPassTransferEncoderMutationFragmentSlot1;
  if (mutations & RTC::kDrawPassTransferEncoderMutationVertexSlot0) {
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kVertex, 0);
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationVertexSlot1) {
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kVertex, 1);
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationFragmentSlot0) {
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kFragment,
                                         0);
  }
  if (mutations & RTC::kDrawPassTransferEncoderMutationFragmentSlot1) {
    InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage::kFragment,
                                         1);
  }
  if (mutations & kTransferBufferMutations) {
    heap_binds_set_on_encoder_ = false;
  }
}

bool MetalCommandProcessor::RequestSharedMemoryRange(
    SharedMemoryRequestReason reason, uint32_t start, uint32_t length) {
  SharedMemory::Range range = {start, length};
  return RequestSharedMemoryRanges(reason, &range, 1);
}

bool MetalCommandProcessor::RequestSharedMemoryRangeBeforeDrawPass(
    SharedMemoryRequestReason reason, uint32_t start, uint32_t length) {
  SharedMemory::Range range = {start, length};
  PrepareSharedMemoryUploadBeforeDrawPass(&range, 1);
  return RequestSharedMemoryRanges(reason, &range, 1);
}

bool MetalCommandProcessor::RequestSharedMemoryRanges(
    SharedMemoryRequestReason reason, const SharedMemory::Range* ranges,
    uint32_t range_count) {
  const bool was_active = current_render_encoder_ != nullptr;
  const size_t reason_index = static_cast<size_t>(reason);
  const bool reason_valid = reason_index < kSharedMemoryRequestReasonCount;
  if (!shared_memory_) {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kNoSharedMemory);
    if (reason_valid) {
      ++backend_telemetry_.shared_memory_request_failures[reason_index];
    }
    return false;
  }
  if (range_count && !ranges) {
    if (reason_valid) {
      ++backend_telemetry_.shared_memory_request_failures[reason_index];
    }
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kRequestFailed);
    return false;
  }

  std::vector<SharedMemory::Range> coalesced_ranges;
  const SharedMemory::Range* request_ranges = ranges;
  uint32_t request_range_count = range_count;
  if (ranges && range_count) {
    coalesced_ranges.reserve(range_count);
    for (uint32_t i = 0; i < range_count; ++i) {
      const SharedMemory::Range& range = ranges[i];
      if (!range.length) {
        continue;
      }
      coalesced_ranges.push_back(range);
    }
    if (!coalesced_ranges.empty()) {
      std::sort(coalesced_ranges.begin(), coalesced_ranges.end(),
                [](const SharedMemory::Range& a, const SharedMemory::Range& b) {
                  return a.start < b.start;
                });
      size_t coalesced_count = 0;
      for (SharedMemory::Range range : coalesced_ranges) {
        uint64_t range_end = uint64_t(range.start) + range.length;
        if (range_end > SharedMemory::kBufferSize) {
          coalesced_ranges[coalesced_count++] = range;
          continue;
        }
        if (!coalesced_count) {
          coalesced_ranges[coalesced_count++] = range;
          continue;
        }
        SharedMemory::Range& previous = coalesced_ranges[coalesced_count - 1];
        uint64_t previous_end = uint64_t(previous.start) + previous.length;
        if (range.start <= previous_end) {
          uint64_t merged_end = std::max(previous_end, range_end);
          previous.length = static_cast<uint32_t>(merged_end - previous.start);
        } else {
          coalesced_ranges[coalesced_count++] = range;
        }
      }
      coalesced_ranges.resize(coalesced_count);
    }
    request_ranges = coalesced_ranges.data();
    request_range_count = static_cast<uint32_t>(coalesced_ranges.size());
    if (request_range_count) {
      ++backend_telemetry_.shared_memory_upload_batches;
      backend_telemetry_.shared_memory_upload_batch_input_ranges += range_count;
      backend_telemetry_.shared_memory_upload_batch_coalesced_ranges +=
          request_range_count;
    }
  }

  SharedMemory::RequestRangeStats stats;
  SharedMemoryRequestReason previous_upload_reason =
      current_shared_memory_upload_reason_;
  current_shared_memory_upload_reason_ = reason;
  const bool success = shared_memory_->RequestRanges(
      request_ranges, request_range_count, &stats);
  current_shared_memory_upload_reason_ = previous_upload_reason;
  backend_telemetry_.shared_memory_upload_batch_bytes += stats.upload_bytes;
  if (reason_valid) {
    backend_telemetry_.shared_memory_request_upload_bytes[reason_index] +=
        stats.upload_bytes;
    if (!success) {
      ++backend_telemetry_.shared_memory_request_failures[reason_index];
    }
  }

  if (!success) {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kRequestFailed);
  } else if (!stats.upload_bytes) {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kAlreadyResident);
  } else if (was_active) {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kUploadInsideRenderEncoder);
  } else {
    RecordSharedMemoryRequestOutcome(
        SharedMemoryRequestOutcome::kUploadBeforeRenderEncoder);
  }

  return success;
}

void MetalCommandProcessor::RecordSharedMemoryRequestOutcome(
    SharedMemoryRequestOutcome outcome) {
  const size_t outcome_index = static_cast<size_t>(outcome);
  if (outcome_index < kSharedMemoryRequestOutcomeCount) {
    ++backend_telemetry_.shared_memory_request_outcomes[outcome_index];
  }
}

bool MetalCommandProcessor::AnySharedMemoryRangeInvalid(
    const SharedMemory::Range* ranges, uint32_t range_count) const {
  if (!shared_memory_ || !ranges || !range_count) {
    return false;
  }
  for (uint32_t i = 0; i < range_count; ++i) {
    if (!shared_memory_->IsRangeValid(ranges[i].start, ranges[i].length)) {
      return true;
    }
  }
  return false;
}

void MetalCommandProcessor::RecordSharedMemoryLazyUploadRoute(
    const MetalSharedMemory::UploadRouteInfo& route_info,
    bool render_encoder_active) {
  if (!route_info.upload_bytes) {
    ++backend_telemetry_.shared_memory_lazy_upload_no_upload_batches;
    return;
  }
  if (route_info.direct_bytes && !route_info.staged_bytes) {
    ++backend_telemetry_.shared_memory_lazy_upload_direct_only_batches;
    if (render_encoder_active) {
      ++backend_telemetry_.shared_memory_lazy_upload_direct_only_active;
    }
    return;
  }
  if (route_info.direct_bytes && route_info.staged_bytes) {
    ++backend_telemetry_.shared_memory_lazy_upload_mixed_batches;
    if (render_encoder_active) {
      ++backend_telemetry_.shared_memory_lazy_upload_mixed_active;
    }
    return;
  }
  ++backend_telemetry_.shared_memory_lazy_upload_staged_only_batches;
  if (render_encoder_active) {
    ++backend_telemetry_.shared_memory_lazy_upload_staged_only_active;
  }
}

void MetalCommandProcessor::PrepareSharedMemoryUploadBeforeDrawPass(
    const SharedMemory::Range* ranges, uint32_t range_count) {
  if (!shared_memory_ || !ranges || !range_count) {
    return;
  }
  const bool render_encoder_active = current_render_encoder_ != nullptr;
  MetalSharedMemory::UploadRouteInfo route_info =
      shared_memory_->GetUploadRouteInfo(ranges, range_count);
  RecordSharedMemoryLazyUploadRoute(route_info, render_encoder_active);
  if (render_encoder_active && route_info.staged_bytes) {
    EndRenderEncoder(RenderEncoderEndReason::kSharedMemoryUploadBeforeDrawPass);
  }
}

bool MetalCommandProcessor::HasActiveSharedMemoryWritePending() const {
  if (NS::UInteger(active_render_encoder_shared_memory_write_stages_)) {
    return true;
  }
  for (const PendingSharedMemoryWrite& pending :
       pending_shared_memory_writes_) {
    if (pending.active_render_encoder) {
      return true;
    }
  }
  return false;
}

MTL::CommandBuffer* MetalCommandProcessor::RequestTransferCommandBuffer(
    TransferRequestSource source) {
  if (!flushing_prepared_draw_queue_ && !prepared_draw_queue_.empty()) {
    if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kTransferRequest)) {
      return nullptr;
    }
  }
  const size_t source_index = static_cast<size_t>(source);
  const bool ends_render_encoder = current_render_encoder_ != nullptr;
  if (source_index < kTransferRequestSourceCount) {
    ++backend_telemetry_.transfer_request_sources_total[source_index];
    if (ends_render_encoder) {
      ++backend_telemetry_.transfer_request_sources_active[source_index];
      ++backend_telemetry_.transfer_request_render_encoder_ends[source_index];
    } else {
      ++backend_telemetry_.transfer_request_sources_no_active[source_index];
    }
  }
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kTransferRequest);
  if (current_render_encoder_) {
    EndRenderEncoder(RenderEncoderEndReason::kRequestTransferCommandBuffer);
  }
  if (texture_cache_ &&
      !texture_cache_->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
    return nullptr;
  }
  return EnsureCommandBuffer();
}

void MetalCommandProcessor::RecordSharedMemoryUploadRoute(
    SharedMemoryUploadRoute route, uint64_t bytes) {
  const size_t route_index = static_cast<size_t>(route);
  if (route_index >= kSharedMemoryUploadRouteCount) {
    return;
  }
  ++backend_telemetry_.shared_memory_upload_route_counts[route_index];
  backend_telemetry_.shared_memory_upload_route_bytes[route_index] += bytes;
}

void MetalCommandProcessor::RecordSharedMemoryDirectWriteEligibility(
    uint64_t direct_bytes, uint64_t staged_bytes) {
  backend_telemetry_.shared_memory_direct_write_eligible_bytes += direct_bytes;
  backend_telemetry_.shared_memory_direct_write_staged_required_bytes +=
      staged_bytes;
}

void MetalCommandProcessor::RecordSharedMemoryDirectWriteReject(
    SharedMemoryDirectWriteRejectReason reason, uint64_t bytes) {
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index >= kSharedMemoryDirectWriteRejectReasonCount) {
    return;
  }
  ++backend_telemetry_.shared_memory_direct_write_reject_counts[reason_index];
  backend_telemetry_.shared_memory_direct_write_reject_bytes[reason_index] +=
      bytes;
}

void MetalCommandProcessor::RecordTextureUploadSourceRoute(
    TextureUploadSourceRoute route, uint64_t bytes) {
  const size_t route_index = static_cast<size_t>(route);
  if (route_index >= kTextureUploadSourceRouteCount) {
    return;
  }
  ++backend_telemetry_.texture_upload_source_route_counts[route_index];
  backend_telemetry_.texture_upload_source_route_bytes[route_index] += bytes;
}

void MetalCommandProcessor::RecordTextureUploadSourceFallback(
    TextureUploadSourceFallbackReason reason) {
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index < kTextureUploadSourceFallbackReasonCount) {
    ++backend_telemetry_.texture_upload_source_fallback_reasons[reason_index];
  }
}

void MetalCommandProcessor::RecordTextureUploadCompatibility(
    TextureUploadCompatibilityClass type, uint64_t bytes) {
  const size_t type_index = static_cast<size_t>(type);
  if (type_index >= kTextureUploadCompatibilityClassCount) {
    return;
  }
  ++backend_telemetry_.texture_upload_compatibility_counts[type_index];
  backend_telemetry_.texture_upload_compatibility_bytes[type_index] += bytes;
}

void MetalCommandProcessor::RecordTextureUploadComputeBlocker(
    TextureUploadComputeBlocker blocker, uint64_t bytes) {
  const size_t blocker_index = static_cast<size_t>(blocker);
  if (blocker_index >= kTextureUploadComputeBlockerCount) {
    return;
  }
  ++backend_telemetry_.texture_upload_compute_blocker_counts[blocker_index];
  backend_telemetry_.texture_upload_compute_blocker_bytes[blocker_index] +=
      bytes;
}

void MetalCommandProcessor::RecordTextureUploadExecutionDetail(
    TextureUploadExecutionDetail detail, uint64_t count) {
  const size_t detail_index = static_cast<size_t>(detail);
  if (detail_index < kTextureUploadExecutionDetailCount) {
    backend_telemetry_.texture_upload_execution_details[detail_index] += count;
  }
}

void MetalCommandProcessor::RecordTextureReloadReason(
    TextureReloadReason reason, uint64_t bytes, uint64_t count) {
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index >= kTextureReloadReasonCount) {
    return;
  }
  backend_telemetry_.texture_reload_reason_counts[reason_index] += count;
  backend_telemetry_.texture_reload_reason_bytes[reason_index] += bytes;
}

void MetalCommandProcessor::RecordTextureWatchInvalidation(
    TextureWatchInvalidationReason reason, uint64_t bytes, uint64_t count) {
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index >= kTextureWatchInvalidationReasonCount) {
    return;
  }
  backend_telemetry_.texture_watch_invalidation_counts[reason_index] += count;
  backend_telemetry_.texture_watch_invalidation_bytes[reason_index] += bytes;
}

void MetalCommandProcessor::RecordTextureResolveReload(
    TextureResolveReloadReason reason, uint64_t bytes, uint64_t count) {
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index >= kTextureResolveReloadReasonCount) {
    return;
  }
  backend_telemetry_.texture_resolve_reload_counts[reason_index] += count;
  backend_telemetry_.texture_resolve_reload_bytes[reason_index] += bytes;
}

void MetalCommandProcessor::RecordSharedMemoryUploadEncoderCopy() {
  ++backend_telemetry_.shared_memory_upload_encoder_copies;
}

MTL::BlitCommandEncoder*
MetalCommandProcessor::GetSharedMemoryUploadBlitEncoder() {
  if (shared_memory_upload_blit_encoder_) {
    ++backend_telemetry_.shared_memory_upload_encoder_reuses;
    return shared_memory_upload_blit_encoder_;
  }

  MTL::CommandBuffer* command_buffer =
      RequestTransferCommandBuffer(TransferRequestSource::kSharedMemoryUpload);
  if (!command_buffer) {
    return nullptr;
  }

  shared_memory_upload_blit_encoder_ = command_buffer->blitCommandEncoder();
  if (!shared_memory_upload_blit_encoder_) {
    XELOGE("Metal: failed to create shared-memory upload blit encoder");
    return nullptr;
  }
  ++backend_telemetry_.shared_memory_upload_encoder_acquisitions;
  shared_memory_upload_blit_encoder_->retain();
  shared_memory_upload_blit_encoder_->setLabel(
      NS::String::string("XeniaSharedMemoryUpload", NS::UTF8StringEncoding));
  return shared_memory_upload_blit_encoder_;
}

void MetalCommandProcessor::EndSharedMemoryUploadBlitEncoder(
    SharedMemoryUploadEncoderEndReason reason) {
  if (!shared_memory_upload_blit_encoder_) {
    return;
  }
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index < kSharedMemoryUploadEncoderEndReasonCount) {
    ++backend_telemetry_.shared_memory_upload_encoder_end_reasons[reason_index];
  }
  shared_memory_upload_blit_encoder_->endEncoding();
  shared_memory_upload_blit_encoder_->release();
  shared_memory_upload_blit_encoder_ = nullptr;
}

MTL::CommandBuffer*
MetalCommandProcessor::CreateStandaloneTransferCommandBuffer(
    const char* label) {
  if (!command_queue_) {
    return nullptr;
  }
  MTL::CommandBuffer* cmd = command_queue_->commandBuffer();
  if (!cmd) {
    return nullptr;
  }
  cmd->retain();
  if (label) {
    cmd->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
  }
  return cmd;
}

void MetalCommandProcessor::CommitStandaloneAsync(MTL::CommandBuffer* cmd) {
  if (!cmd) {
    return;
  }
  cmd->addCompletedHandler(^(MTL::CommandBuffer* completed_cmd) {
    completed_cmd->release();
  });
  cmd->commit();
}

void MetalCommandProcessor::CommitStandaloneAndWait(MTL::CommandBuffer* cmd) {
  if (!cmd) {
    return;
  }
  cmd->commit();
  cmd->waitUntilCompleted();
  cmd->release();
}

void MetalCommandProcessor::ResetRenderEncoderResourceUsage() {
  for (EncoderResourceUsageTableEntry& entry :
       render_encoder_resource_usage_table_) {
    entry = {};
  }
  render_encoder_resource_usage_count_ = 0;
  render_encoder_heap_usage_.clear();
  render_encoder_bindless_fixed_resources_serial_ = 0;
  render_encoder_bindless_texture_resources_serial_ = 0;
  render_encoder_bindless_root_resources_serial_ = 0;
}

void MetalCommandProcessor::AddRenderHeapRef(RenderResourceSet& set,
                                             MTL::Heap* heap) {
  if (!heap) {
    return;
  }
  for (MTL::Heap* existing : set.heaps) {
    if (existing == heap) {
      return;
    }
  }
  set.heaps.push_back(heap);
}

void MetalCommandProcessor::AddRenderResourceRef(RenderResourceSet& set,
                                                 MTL::Resource* resource,
                                                 MTL::ResourceUsage usage,
                                                 MTL::RenderStages stages) {
  if (!resource) {
    return;
  }
  if (IsResidencySetResourceCovered(resource)) {
    ++backend_telemetry_.residency_set_resource_refs_covered;
    return;
  }
  ++backend_telemetry_.residency_set_resource_refs_fallback;
  uint32_t stage_bits = MetalRenderStageBits(stages);
  if (!stage_bits) {
    stages = MetalAllGraphicsRenderStages();
    stage_bits = MetalRenderStageBits(stages);
  }
  const uint32_t usage_bits = MetalResourceUsageBits(usage);
  for (RenderResourceRef& existing : set.resources) {
    if (existing.resource != resource) {
      continue;
    }
    existing.usage =
        MTL::ResourceUsage(MetalResourceUsageBits(existing.usage) | usage_bits);
    existing.stages =
        MTL::RenderStages(MetalRenderStageBits(existing.stages) | stage_bits);
    return;
  }
  set.resources.push_back({resource, usage, stages});
}

void MetalCommandProcessor::RestoreRenderResourceSet(
    RenderResourceSetKind kind, RenderResourceSet& current,
    const RenderResourceSet& snapshot) {
  const size_t kind_index = static_cast<size_t>(kind);
  if (snapshot.source_serial &&
      current.source_serial == snapshot.source_serial) {
    if (kind_index <
        backend_telemetry_.render_resource_registry_serial_skips.size()) {
      ++backend_telemetry_.render_resource_registry_serial_skips[kind_index];
    }
    return;
  }

  bool same = current.heaps.size() == snapshot.heaps.size() &&
              current.resources.size() == snapshot.resources.size();
  if (same) {
    for (size_t i = 0; i < current.heaps.size(); ++i) {
      if (current.heaps[i] != snapshot.heaps[i]) {
        same = false;
        break;
      }
    }
  }
  if (same) {
    for (size_t i = 0; i < current.resources.size(); ++i) {
      const RenderResourceRef& current_ref = current.resources[i];
      const RenderResourceRef& snapshot_ref = snapshot.resources[i];
      if (current_ref.resource != snapshot_ref.resource ||
          MetalResourceUsageBits(current_ref.usage) !=
              MetalResourceUsageBits(snapshot_ref.usage) ||
          MetalRenderStageBits(current_ref.stages) !=
              MetalRenderStageBits(snapshot_ref.stages)) {
        same = false;
        break;
      }
    }
  }
  if (same) {
    current.source_serial = snapshot.source_serial;
    return;
  }

  if (kind_index < backend_telemetry_.render_resource_registry_builds.size()) {
    ++backend_telemetry_.render_resource_registry_builds[kind_index];
  }
  uint64_t next_serial = current.serial + 1;
  if (!next_serial) {
    next_serial = 1;
  }
  current.heaps = snapshot.heaps;
  current.resources = snapshot.resources;
  current.serial = next_serial;
  current.source_serial = snapshot.source_serial;
}

void MetalCommandProcessor::PublishRenderResourceSet(RenderResourceSet& current,
                                                     RenderResourceSet&& next) {
  if (next.source_serial && current.source_serial == next.source_serial) {
    return;
  }
  bool same = current.heaps.size() == next.heaps.size() &&
              current.resources.size() == next.resources.size();
  if (same) {
    for (size_t i = 0; i < current.heaps.size(); ++i) {
      if (current.heaps[i] != next.heaps[i]) {
        same = false;
        break;
      }
    }
  }
  if (same) {
    for (size_t i = 0; i < current.resources.size(); ++i) {
      const RenderResourceRef& current_ref = current.resources[i];
      const RenderResourceRef& next_ref = next.resources[i];
      if (current_ref.resource != next_ref.resource ||
          MetalResourceUsageBits(current_ref.usage) !=
              MetalResourceUsageBits(next_ref.usage) ||
          MetalRenderStageBits(current_ref.stages) !=
              MetalRenderStageBits(next_ref.stages)) {
        same = false;
        break;
      }
    }
  }
  if (same) {
    current.source_serial = next.source_serial;
    return;
  }
  uint64_t next_serial = current.serial + 1;
  if (!next_serial) {
    next_serial = 1;
  }
  current.heaps = std::move(next.heaps);
  current.resources = std::move(next.resources);
  current.serial = next_serial;
  current.source_serial = next.source_serial;
}

uint64_t MetalCommandProcessor::GetBindlessFixedResourceSourceSerial(
    MTL::ResourceUsage shared_memory_usage) const {
  const uint64_t shared_memory_usage_bits =
      uint64_t(MetalResourceUsageBits(shared_memory_usage));
  const uint64_t rt_serial =
      render_target_cache_ ? render_target_cache_->GetBindlessResourcesSerial()
                           : 0;
  return (rt_serial << 8) ^ shared_memory_usage_bits;
}

uint64_t MetalCommandProcessor::GetBindlessTextureResourceInputSerial() const {
  uint64_t hash = 1469598103934665603ull;
  auto hash_value = [&](uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  hash_value(current_texture_bindless_resources_vertex_.size());
  for (MTL::Texture* texture : current_texture_bindless_resources_vertex_) {
    hash_value(reinterpret_cast<uintptr_t>(texture));
  }
  hash_value(0x9e3779b97f4a7c15ull);
  hash_value(current_texture_bindless_resources_pixel_.size());
  for (MTL::Texture* texture : current_texture_bindless_resources_pixel_) {
    hash_value(reinterpret_cast<uintptr_t>(texture));
  }
  return hash ? hash : 1;
}

uint64_t MetalCommandProcessor::GetRenderResourceSetSourceSerial(
    const RenderResourceSet& set) const {
  uint64_t hash = 1469598103934665603ull;
  auto hash_value = [&](uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  hash_value(set.heaps.size());
  for (MTL::Heap* heap : set.heaps) {
    hash_value(reinterpret_cast<uintptr_t>(heap));
  }
  hash_value(0x9e3779b97f4a7c15ull);
  hash_value(set.resources.size());
  for (const RenderResourceRef& ref : set.resources) {
    hash_value(reinterpret_cast<uintptr_t>(ref.resource));
    hash_value(MetalResourceUsageBits(ref.usage));
    hash_value(MetalRenderStageBits(ref.stages));
  }
  return hash ? hash : 1;
}

void MetalCommandProcessor::BuildBindlessTextureResourceSet(
    RenderResourceSet& set) {
  set.heaps.clear();
  set.resources.clear();
  set.serial = 0;
  set.source_serial = 0;
  const MTL::RenderStages stages = MetalAllGraphicsRenderStages();
  auto add_texture_residency_ref = [&](MTL::Texture* texture) {
    if (!texture) {
      return;
    }
    // Bindless texture paths sample through argument-buffer resource IDs. For
    // read-only heap-backed texture-cache allocations, Apple's model is to make
    // the containing heap resident instead of declaring every texture
    // individually for every encoder.
    if (MTL::Heap* heap = texture->heap()) {
      AddRenderHeapRef(set, heap);
      return;
    }
    AddRenderResourceRef(set, texture, MTL::ResourceUsageRead, stages);
  };
  for (MTL::Texture* texture : current_texture_bindless_resources_vertex_) {
    add_texture_residency_ref(texture);
  }
  for (MTL::Texture* texture : current_texture_bindless_resources_pixel_) {
    add_texture_residency_ref(texture);
  }
  std::sort(set.heaps.begin(), set.heaps.end(),
            [](MTL::Heap* a, MTL::Heap* b) {
              return reinterpret_cast<uintptr_t>(a) <
                     reinterpret_cast<uintptr_t>(b);
            });
  std::sort(set.resources.begin(), set.resources.end(),
            [](const RenderResourceRef& a, const RenderResourceRef& b) {
              const uintptr_t a_resource =
                  reinterpret_cast<uintptr_t>(a.resource);
              const uintptr_t b_resource =
                  reinterpret_cast<uintptr_t>(b.resource);
              if (a_resource != b_resource) {
                return a_resource < b_resource;
              }
              const uint32_t a_usage = MetalResourceUsageBits(a.usage);
              const uint32_t b_usage = MetalResourceUsageBits(b.usage);
              if (a_usage != b_usage) {
                return a_usage < b_usage;
              }
              return MetalRenderStageBits(a.stages) <
                     MetalRenderStageBits(b.stages);
            });
}

uint64_t MetalCommandProcessor::GetBindlessRootResourceSourceSerial(
    const UniformBufferInfo& uniforms) const {
  uint64_t hash = 1469598103934665603ull;
  auto hash_value = [&](uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  const StageRootArgumentAllocation& allocation =
      graphics_root_argument_state_.allocation;
  hash_value(reinterpret_cast<uintptr_t>(allocation.valid ? allocation.buffer
                                                          : nullptr));
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    hash_value(uniforms.active_cbv_masks[stage]);
    for (size_t cbv = 0; cbv < kCbvSlotCount; ++cbv) {
      const bool active =
          uniforms.active_cbv_masks[stage] & (uint32_t(1) << cbv);
      hash_value(reinterpret_cast<uintptr_t>(
          active ? uniforms.cbvs[stage][cbv].buffer : nullptr));
    }
  }
  return hash ? hash : 1;
}

void MetalCommandProcessor::PublishBindlessFixedResourceSet(
    MTL::ResourceUsage shared_memory_usage) {
  constexpr size_t kFixedKindIndex = size_t(RenderResourceSetKind::kFixed);
  const uint64_t source_serial =
      GetBindlessFixedResourceSourceSerial(shared_memory_usage);
  if (source_serial &&
      current_bindless_fixed_resource_source_serial_ == source_serial) {
    ++backend_telemetry_.render_resource_registry_serial_skips[kFixedKindIndex];
    return;
  }
  ++backend_telemetry_.render_resource_registry_builds[kFixedKindIndex];
  RenderResourceSet next;
  next.source_serial = source_serial;
  const MTL::RenderStages stages = MetalAllGraphicsRenderStages();
  AddRenderResourceRef(next,
                       shared_memory_ ? shared_memory_->GetBuffer() : nullptr,
                       shared_memory_usage, stages);
  std::vector<MTL::Resource*> render_target_resources;
  if (render_target_cache_) {
    render_target_cache_->CollectBindlessResources(render_target_resources);
  }
  for (MTL::Resource* resource : render_target_resources) {
    AddRenderResourceRef(next, resource,
                         MTL::ResourceUsageRead | MTL::ResourceUsageWrite,
                         stages);
  }
  AddRenderResourceRef(next, null_buffer_, MTL::ResourceUsageRead, stages);
  AddRenderResourceRef(next, view_bindless_heap_, MTL::ResourceUsageRead,
                       stages);
  AddRenderResourceRef(next, sampler_bindless_heap_, MTL::ResourceUsageRead,
                       stages);
  AddRenderResourceRef(next, system_view_tables_, MTL::ResourceUsageRead,
                       stages);
  PublishRenderResourceSet(current_bindless_fixed_resource_set_,
                           std::move(next));
  current_bindless_fixed_resource_source_serial_ =
      current_bindless_fixed_resource_set_.source_serial;
}

void MetalCommandProcessor::RestoreBindlessTextureResourceSet(
    const RenderResourceSet& set) {
  RestoreRenderResourceSet(RenderResourceSetKind::kTexture,
                           current_bindless_texture_resource_set_, set);
  current_bindless_texture_resource_input_serial_ = 0;
  current_bindless_texture_resource_source_serial_ =
      current_bindless_texture_resource_set_.source_serial;
}

void MetalCommandProcessor::PublishBindlessTextureResourceSet() {
  constexpr size_t kTextureKindIndex = size_t(RenderResourceSetKind::kTexture);
  const uint64_t input_serial = GetBindlessTextureResourceInputSerial();
  if (input_serial &&
      current_bindless_texture_resource_input_serial_ == input_serial) {
    ++backend_telemetry_
          .render_resource_registry_serial_skips[kTextureKindIndex];
    return;
  }
  RenderResourceSet next;
  BuildBindlessTextureResourceSet(next);
  next.source_serial = GetRenderResourceSetSourceSerial(next);
  if (next.source_serial &&
      current_bindless_texture_resource_source_serial_ == next.source_serial) {
    current_bindless_texture_resource_input_serial_ = input_serial;
    ++backend_telemetry_
          .render_resource_registry_serial_skips[kTextureKindIndex];
    return;
  }
  ++backend_telemetry_.render_resource_registry_builds[kTextureKindIndex];
  PublishRenderResourceSet(current_bindless_texture_resource_set_,
                           std::move(next));
  current_bindless_texture_resource_input_serial_ = input_serial;
  current_bindless_texture_resource_source_serial_ =
      current_bindless_texture_resource_set_.source_serial;
}

void MetalCommandProcessor::PublishBindlessRootResourceSet(
    const UniformBufferInfo& uniforms) {
  constexpr size_t kRootKindIndex = size_t(RenderResourceSetKind::kRoot);
  const uint64_t root_source_serial =
      GetBindlessRootResourceSourceSerial(uniforms);
  if (root_source_serial &&
      current_bindless_root_resource_source_serial_ == root_source_serial) {
    ++backend_telemetry_.render_resource_registry_serial_skips[kRootKindIndex];
    return;
  }
  ++backend_telemetry_.render_resource_registry_builds[kRootKindIndex];
  RenderResourceSet next;
  next.source_serial = root_source_serial;
  const MTL::RenderStages stages = MetalAllGraphicsRenderStages();
  const StageRootArgumentAllocation& allocation =
      graphics_root_argument_state_.allocation;
  if (allocation.valid) {
    AddRenderResourceRef(next, allocation.buffer, MTL::ResourceUsageRead,
                         stages);
  }
  for (size_t stage = 0; stage < kStageCount; ++stage) {
    for (size_t cbv = 0; cbv < kCbvSlotCount; ++cbv) {
      if (uniforms.cbvs[stage][cbv].active) {
        AddRenderResourceRef(next, uniforms.cbvs[stage][cbv].buffer,
                             MTL::ResourceUsageRead, stages);
      }
    }
  }
  PublishRenderResourceSet(current_bindless_root_resource_set_,
                           std::move(next));
  current_bindless_root_resource_source_serial_ =
      current_bindless_root_resource_set_.source_serial;
}

void MetalCommandProcessor::ApplyRenderEncoderResourceSets() {
  ApplyRenderEncoderResourceSet(
      RenderResourceSetKind::kFixed, current_bindless_fixed_resource_set_,
      render_encoder_bindless_fixed_resources_serial_);
  ApplyRenderEncoderResourceSet(
      RenderResourceSetKind::kTexture, current_bindless_texture_resource_set_,
      render_encoder_bindless_texture_resources_serial_);
  ApplyRenderEncoderResourceSet(RenderResourceSetKind::kRoot,
                                current_bindless_root_resource_set_,
                                render_encoder_bindless_root_resources_serial_);
}

void MetalCommandProcessor::ApplyRenderEncoderResourceSet(
    RenderResourceSetKind kind, const RenderResourceSet& set,
    uint64_t& applied_serial) {
  const size_t kind_index = static_cast<size_t>(kind);
  if (!current_render_encoder_) {
    return;
  }
  if (applied_serial == set.serial) {
    if (kind_index < backend_telemetry_.render_resource_set_skips.size()) {
      ++backend_telemetry_.render_resource_set_skips[kind_index];
    }
    return;
  }
  if (kind_index < backend_telemetry_.render_resource_set_applies.size()) {
    ++backend_telemetry_.render_resource_set_applies[kind_index];
    backend_telemetry_.render_resource_set_resources[kind_index] +=
        set.heaps.size() + set.resources.size();
    ++backend_telemetry_.render_resource_registry_registers[kind_index];
  }
  for (MTL::Heap* heap : set.heaps) {
    UseRenderEncoderHeap(heap);
  }
  constexpr size_t kResourceBatchSize = 128;
  std::array<const MTL::Resource*, kResourceBatchSize> batch;
  uint32_t batch_count = 0;
  MTL::ResourceUsage batch_usage = MTL::ResourceUsageRead;
  MTL::RenderStages batch_stages = MTL::RenderStages(0);
  auto flush_batch = [&]() {
    if (!batch_count) {
      return;
    }
    UseRenderEncoderResources(batch.data(), batch_count, batch_usage,
                              batch_stages);
    batch_count = 0;
  };
  for (const RenderResourceRef& ref : set.resources) {
    if (!ref.resource) {
      continue;
    }
    if (!batch_count) {
      batch_usage = ref.usage;
      batch_stages = ref.stages;
    } else if (MetalResourceUsageBits(batch_usage) !=
                   MetalResourceUsageBits(ref.usage) ||
               MetalRenderStageBits(batch_stages) !=
                   MetalRenderStageBits(ref.stages)) {
      flush_batch();
      batch_usage = ref.usage;
      batch_stages = ref.stages;
    }
    batch[batch_count++] = ref.resource;
    if (batch_count == batch.size()) {
      flush_batch();
    }
  }
  flush_batch();
  applied_serial = set.serial;
}

void MetalCommandProcessor::GrowRenderEncoderResourceUsageTable(
    size_t min_capacity) {
  size_t capacity = render_encoder_resource_usage_table_.size();
  if (!capacity) {
    capacity = 256;
  }
  while (capacity < min_capacity) {
    capacity <<= 1;
  }

  std::vector<EncoderResourceUsageTableEntry> old_entries =
      std::move(render_encoder_resource_usage_table_);
  render_encoder_resource_usage_table_.assign(capacity, {});
  render_encoder_resource_usage_count_ = 0;
  for (const EncoderResourceUsageTableEntry& old_entry : old_entries) {
    if (!old_entry.resource) {
      continue;
    }
    bool inserted = false;
    EncoderResourceUsageState* state =
        FindOrInsertRenderEncoderResourceUsage(old_entry.resource, inserted);
    if (state) {
      *state = old_entry.state;
    }
  }
}

MetalCommandProcessor::EncoderResourceUsageState*
MetalCommandProcessor::FindOrInsertRenderEncoderResourceUsage(
    MTL::Resource* resource, bool& inserted) {
  inserted = false;
  if (!resource) {
    return nullptr;
  }
  if (render_encoder_resource_usage_table_.empty() ||
      (render_encoder_resource_usage_count_ + 1) * 4 >=
          render_encoder_resource_usage_table_.size() * 3) {
    GrowRenderEncoderResourceUsageTable(
        std::max<size_t>(render_encoder_resource_usage_table_.size() * 2, 256));
  }

  const size_t mask = render_encoder_resource_usage_table_.size() - 1;
  size_t index =
      ((reinterpret_cast<uintptr_t>(resource) >> 4) * 11400714819323198485ull) &
      mask;
  for (;;) {
    EncoderResourceUsageTableEntry& entry =
        render_encoder_resource_usage_table_[index];
    if (!entry.resource) {
      entry.resource = resource;
      entry.state = {};
      ++render_encoder_resource_usage_count_;
      inserted = true;
      return &entry.state;
    }
    if (entry.resource == resource) {
      return &entry.state;
    }
    index = (index + 1) & mask;
  }
}

void MetalCommandProcessor::ResetRenderEncoderBufferBindings() {
  for (auto& stage_bindings : render_encoder_buffer_bindings_) {
    for (auto& binding : stage_bindings) {
      binding = {};
    }
  }
  render_encoder_bindless_stage_root_bind_serials_.fill(0);
  render_encoder_bindless_table_bind_mesh_path_ = false;
  render_encoder_bindless_table_bind_tessellation_ = false;
}

void MetalCommandProcessor::InvalidateRenderEncoderBufferBinding(
    RenderEncoderBufferStage stage, NS::UInteger index) {
  const size_t stage_index = size_t(stage);
  if (stage_index >= render_encoder_buffer_bindings_.size() ||
      index >= kTrackedRenderEncoderBufferBindingCount) {
    return;
  }
  render_encoder_buffer_bindings_[stage_index][index] = {};
  if (index == kIRArgumentBufferBindPoint ||
      index == kIRArgumentBufferHullDomainBindPoint) {
    if (stage == RenderEncoderBufferStage::kFragment) {
      render_encoder_bindless_stage_root_bind_serials_[kStagePixel] = 0;
    } else {
      render_encoder_bindless_stage_root_bind_serials_[kStageVertex] = 0;
      render_encoder_bindless_table_bind_mesh_path_ = false;
      render_encoder_bindless_table_bind_tessellation_ = false;
    }
  }
}

bool MetalCommandProcessor::RenderEncoderBufferBindingMatches(
    RenderEncoderBufferStage stage, MTL::Buffer* buffer, NS::UInteger offset,
    NS::UInteger index) const {
  const size_t stage_index = size_t(stage);
  if (stage_index >= render_encoder_buffer_bindings_.size() ||
      index >= kTrackedRenderEncoderBufferBindingCount) {
    return false;
  }
  const RenderEncoderBufferBinding& binding =
      render_encoder_buffer_bindings_[stage_index][index];
  return binding.valid && binding.buffer == buffer && binding.offset == offset;
}

void MetalCommandProcessor::SetRenderEncoderBuffer(
    RenderEncoderBufferStage stage, MTL::Buffer* buffer, NS::UInteger offset,
    NS::UInteger index) {
  if (!current_render_encoder_ || stage == RenderEncoderBufferStage::kCount) {
    return;
  }
  size_t stage_index = static_cast<size_t>(stage);
  auto set_buffer = [&](MTL::Buffer* buffer_to_set,
                        NS::UInteger offset_to_set) {
    switch (stage) {
      case RenderEncoderBufferStage::kVertex:
        current_render_encoder_->setVertexBuffer(buffer_to_set, offset_to_set,
                                                 index);
        break;
      case RenderEncoderBufferStage::kFragment:
        current_render_encoder_->setFragmentBuffer(buffer_to_set, offset_to_set,
                                                   index);
        break;
      case RenderEncoderBufferStage::kObject:
        current_render_encoder_->setObjectBuffer(buffer_to_set, offset_to_set,
                                                 index);
        break;
      case RenderEncoderBufferStage::kMesh:
        current_render_encoder_->setMeshBuffer(buffer_to_set, offset_to_set,
                                               index);
        break;
      case RenderEncoderBufferStage::kCount:
        break;
    }
  };
  auto set_buffer_offset = [&]() {
    switch (stage) {
      case RenderEncoderBufferStage::kVertex:
        current_render_encoder_->setVertexBufferOffset(offset, index);
        break;
      case RenderEncoderBufferStage::kFragment:
        current_render_encoder_->setFragmentBufferOffset(offset, index);
        break;
      case RenderEncoderBufferStage::kObject:
        current_render_encoder_->setObjectBufferOffset(offset, index);
        break;
      case RenderEncoderBufferStage::kMesh:
        current_render_encoder_->setMeshBufferOffset(offset, index);
        break;
      case RenderEncoderBufferStage::kCount:
        break;
    }
  };
  if (!buffer) {
    if (stage_index <
        backend_telemetry_.render_encoder_buffer_null_binds.size()) {
      ++backend_telemetry_.render_encoder_buffer_null_binds[stage_index];
    }
    set_buffer(nullptr, 0);
    InvalidateRenderEncoderBufferBinding(stage, index);
    return;
  }
  if (index >= kTrackedRenderEncoderBufferBindingCount) {
    if (stage_index <
        backend_telemetry_.render_encoder_buffer_untracked_binds.size()) {
      ++backend_telemetry_.render_encoder_buffer_untracked_binds[stage_index];
    }
    set_buffer(buffer, offset);
    return;
  }
  const size_t stage_slot_index =
      stage_index * kTrackedRenderEncoderBufferBindingCount + size_t(index);
  auto increment_slot_count =
      [stage_slot_index](std::array<uint64_t,
                                    kRenderEncoderBufferSlotTelemetryCount>&
                             counts) {
    if (stage_slot_index < counts.size()) {
      ++counts[stage_slot_index];
    }
  };
  auto& binding = render_encoder_buffer_bindings_[stage_index][index];
  if (binding.valid && binding.buffer == buffer) {
    if (binding.offset != offset) {
      if (stage_index <
          backend_telemetry_.render_encoder_buffer_offset_binds.size()) {
        ++backend_telemetry_.render_encoder_buffer_offset_binds[stage_index];
      }
      increment_slot_count(
          backend_telemetry_.render_encoder_buffer_slot_offset_binds);
      set_buffer_offset();
      binding.offset = offset;
    } else if (stage_index <
               backend_telemetry_.render_encoder_buffer_noop_binds.size()) {
      ++backend_telemetry_.render_encoder_buffer_noop_binds[stage_index];
      increment_slot_count(
          backend_telemetry_.render_encoder_buffer_slot_noop_binds);
    }
    return;
  }
  if (stage_index <
      backend_telemetry_.render_encoder_buffer_full_binds.size()) {
    ++backend_telemetry_.render_encoder_buffer_full_binds[stage_index];
  }
  increment_slot_count(
      backend_telemetry_.render_encoder_buffer_slot_full_binds);
  set_buffer(buffer, offset);
  binding = {buffer, offset, true};
}

void MetalCommandProcessor::SetRenderEncoderVertexBuffer(MTL::Buffer* buffer,
                                                         NS::UInteger offset,
                                                         NS::UInteger index) {
  SetRenderEncoderBuffer(RenderEncoderBufferStage::kVertex, buffer, offset,
                         index);
}

void MetalCommandProcessor::SetRenderEncoderFragmentBuffer(MTL::Buffer* buffer,
                                                           NS::UInteger offset,
                                                           NS::UInteger index) {
  SetRenderEncoderBuffer(RenderEncoderBufferStage::kFragment, buffer, offset,
                         index);
}

void MetalCommandProcessor::SetRenderEncoderObjectBuffer(MTL::Buffer* buffer,
                                                         NS::UInteger offset,
                                                         NS::UInteger index) {
  SetRenderEncoderBuffer(RenderEncoderBufferStage::kObject, buffer, offset,
                         index);
}

void MetalCommandProcessor::SetRenderEncoderMeshBuffer(MTL::Buffer* buffer,
                                                       NS::UInteger offset,
                                                       NS::UInteger index) {
  SetRenderEncoderBuffer(RenderEncoderBufferStage::kMesh, buffer, offset,
                         index);
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Resource* resource,
                                                     MTL::ResourceUsage usage) {
  UseRenderEncoderResource(resource, usage, MetalAllGraphicsRenderStages());
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Resource* resource,
                                                     MTL::ResourceUsage usage,
                                                     MTL::RenderStages stages) {
  if (!current_render_encoder_ || !resource) {
    return;
  }
  if (IsResidencySetResourceCovered(resource)) {
    ++backend_telemetry_.residency_set_use_resources_covered;
    return;
  }
  ++backend_telemetry_.residency_set_use_resources_fallback;
  ++backend_telemetry_.render_encoder_use_resource_calls;

  const uint32_t usage_bits = MetalResourceUsageBits(usage);
  uint32_t stage_bits = MetalRenderStageBits(stages);
  if (!stage_bits) {
    stages = MetalAllGraphicsRenderStages();
    stage_bits = MetalRenderStageBits(stages);
  }

  auto usage_state_covers = [&](const EncoderResourceUsageState& state) {
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageRead)) &&
        ((state.read_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageWrite)) &&
        ((state.write_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageSample)) &&
        ((state.sample_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    return (state.usage_bits & usage_bits) == usage_bits;
  };
  auto add_usage_to_state = [&](EncoderResourceUsageState& state) {
    state.usage_bits |= usage_bits;
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageRead)) {
      state.read_stage_bits |= stage_bits;
    }
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageWrite)) {
      state.write_stage_bits |= stage_bits;
    }
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageSample)) {
      state.sample_stage_bits |= stage_bits;
    }
  };

  bool inserted = false;
  EncoderResourceUsageState* state =
      FindOrInsertRenderEncoderResourceUsage(resource, inserted);
  if (!state) {
    return;
  }
  if (!inserted) {
    if (usage_state_covers(*state)) {
      ++backend_telemetry_.render_encoder_use_resource_skips;
      return;
    }
    ++backend_telemetry_.render_encoder_use_resource_upgrades;
  }

  add_usage_to_state(*state);
  current_render_encoder_->useResource(resource, usage, stages);
}

void MetalCommandProcessor::UseRenderEncoderResources(
    const MTL::Resource* const resources[], uint32_t count,
    MTL::ResourceUsage usage) {
  UseRenderEncoderResources(resources, count, usage,
                            MetalAllGraphicsRenderStages());
}

void MetalCommandProcessor::UseRenderEncoderResources(
    const MTL::Resource* const resources[], uint32_t count,
    MTL::ResourceUsage usage, MTL::RenderStages stages) {
  if (!current_render_encoder_ || !resources || !count) {
    return;
  }
  const uint32_t usage_bits = MetalResourceUsageBits(usage);
  uint32_t stage_bits = MetalRenderStageBits(stages);
  if (!stage_bits) {
    stages = MetalAllGraphicsRenderStages();
    stage_bits = MetalRenderStageBits(stages);
  }
  auto usage_state_covers = [&](const EncoderResourceUsageState& state) {
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageRead)) &&
        ((state.read_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageWrite)) &&
        ((state.write_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    if ((usage_bits & MetalResourceUsageBits(MTL::ResourceUsageSample)) &&
        ((state.sample_stage_bits & stage_bits) != stage_bits)) {
      return false;
    }
    return (state.usage_bits & usage_bits) == usage_bits;
  };
  auto add_usage_to_state = [&](EncoderResourceUsageState& state) {
    state.usage_bits |= usage_bits;
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageRead)) {
      state.read_stage_bits |= stage_bits;
    }
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageWrite)) {
      state.write_stage_bits |= stage_bits;
    }
    if (usage_bits & MetalResourceUsageBits(MTL::ResourceUsageSample)) {
      state.sample_stage_bits |= stage_bits;
    }
  };

  constexpr size_t kResourceBatchSize = 128;
  std::array<const MTL::Resource*, kResourceBatchSize> resource_batch;
  uint32_t resource_batch_count = 0;
  auto flush_batch = [&]() {
    if (!resource_batch_count) {
      return;
    }
    current_render_encoder_->useResources(resource_batch.data(),
                                          resource_batch_count, usage, stages);
    ++backend_telemetry_.render_encoder_use_resources_batches;
    resource_batch_count = 0;
  };
  for (uint32_t i = 0; i < count; ++i) {
    const MTL::Resource* const_resource = resources[i];
    if (!const_resource) {
      continue;
    }
    MTL::Resource* resource = const_cast<MTL::Resource*>(const_resource);
    if (IsResidencySetResourceCovered(resource)) {
      ++backend_telemetry_.residency_set_use_resources_covered;
      continue;
    }
    ++backend_telemetry_.residency_set_use_resources_fallback;
    ++backend_telemetry_.render_encoder_use_resources_requested;
    bool inserted = false;
    EncoderResourceUsageState* state =
        FindOrInsertRenderEncoderResourceUsage(resource, inserted);
    if (!state) {
      continue;
    }
    if (!inserted) {
      if (usage_state_covers(*state)) {
        ++backend_telemetry_.render_encoder_use_resources_skips;
        continue;
      }
      ++backend_telemetry_.render_encoder_use_resource_upgrades;
    }

    add_usage_to_state(*state);
    resource_batch[resource_batch_count++] = const_resource;
    if (resource_batch_count == resource_batch.size()) {
      flush_batch();
    }
  }
  flush_batch();
}

void MetalCommandProcessor::UseRenderEncoderHeap(MTL::Heap* heap) {
  if (!current_render_encoder_ || !heap) {
    return;
  }
  // See IsResidencySetResourceCovered: heap-backed textures and render targets
  // still need this usage declaration until texture / RT hazards are tracked
  // explicitly by the backend.
  for (MTL::Heap* used_heap : render_encoder_heap_usage_) {
    if (used_heap == heap) {
      return;
    }
  }
  ++backend_telemetry_.residency_set_use_heaps_fallback;
  render_encoder_heap_usage_.push_back(heap);
  current_render_encoder_->useHeap(heap);
}

void MetalCommandProcessor::UseRenderEncoderAttachmentHeaps(
    MTL::RenderPassDescriptor* descriptor) {
  if (!current_render_encoder_ || !descriptor) {
    return;
  }
  auto* color_attachments = descriptor->colorAttachments();
  for (uint32_t i = 0; i < 8; ++i) {
    auto* attachment = color_attachments->object(i);
    if (!attachment) {
      continue;
    }
    MTL::Texture* texture = attachment->texture();
    if (texture) {
      UseRenderEncoderHeap(texture->heap());
    }
  }
  auto* depth_attachment = descriptor->depthAttachment();
  if (depth_attachment && depth_attachment->texture()) {
    UseRenderEncoderHeap(depth_attachment->texture()->heap());
  }
  auto* stencil_attachment = descriptor->stencilAttachment();
  if (stencil_attachment && stencil_attachment->texture()) {
    UseRenderEncoderHeap(stencil_attachment->texture()->heap());
  }
}

MTL::RenderPassDescriptor* MetalCommandProcessor::GetDrawRenderPassDescriptor(
    bool fallback_depth_attachment_required) {
  if (render_target_cache_) {
    if (MTL::RenderPassDescriptor* cache_desc =
            render_target_cache_->GetRenderPassDescriptor(
                1, fallback_depth_attachment_required)) {
      // Attach the ZPD visibility buffer so occlusion queries can write results
      // directly into shared memory without an explicit resolve step.
      if (GetZPDMode() != ZPDMode::kFake && zpd_visibility_pool_ &&
          zpd_visibility_pool_->is_initialized()) {
        cache_desc->setVisibilityResultBuffer(
            zpd_visibility_pool_->visibility_buffer());
        cache_desc->setVisibilityResultType(MTL::VisibilityResultTypeReset);
      } else {
        cache_desc->setVisibilityResultBuffer(nullptr);
      }
      return cache_desc;
    }
  }
  return nullptr;
}

bool MetalCommandProcessor::BeginRenderEncoderForDraw(
    bool fallback_depth_attachment_required) {
  ++backend_telemetry_.begin_encoder_calls;
  if (!EnsureCommandBuffer()) {
    return false;
  }
  const bool zpd_segment_pending = GetZPDMode() != ZPDMode::kFake &&
                                   zpd_active_segment_.logical_active &&
                                   zpd_active_segment_.segment_pending_begin;
  if (zpd_segment_pending) {
    EnsureZPDQueryResources();
  }
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kRenderBegin);
  if (texture_cache_ &&
      !texture_cache_->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
    return false;
  }

  if (!current_render_encoder_ && (render_encoder_resource_usage_count_ ||
                                   !render_encoder_heap_usage_.empty())) {
    ++backend_telemetry_.begin_encoder_resource_usage_resets;
    ResetRenderEncoderResourceUsage();
  }

  if (current_render_encoder_ && zpd_segment_pending && IsZPDQueryPoolReady() &&
      !current_render_encoder_has_zpd_visibility_) {
    EndRenderEncoder(RenderEncoderEndReason::kUnknown);
  }

  // Obtain the render pass descriptor from MetalRenderTargetCache (host
  // render-target path). If an encoder is already active, keep using its
  // descriptor while the attachment textures still match the current binding.
  // The cache may be dirty only because a clear load action was consumed and
  // the next new pass needs a refreshed descriptor.
  if (current_render_encoder_ && render_target_cache_ &&
      render_target_cache_->IsRenderPassDescriptorCompatible(
          current_render_pass_descriptor_, 1,
          fallback_depth_attachment_required)) {
    ++backend_telemetry_.begin_encoder_reused_compatible;
    return true;
  }

  MTL::RenderPassDescriptor* pass_descriptor =
      GetDrawRenderPassDescriptor(fallback_depth_attachment_required);
  if (!pass_descriptor) {
    ++backend_telemetry_.begin_encoder_descriptor_failures;
    XELOGE("BeginRenderEncoderForDraw: No render pass descriptor available");
    return false;
  }

  // Keep first-use depth clears tied to the guest depth format. D24FS8 uses
  // 0.0 as the far value, unlike fixed-point D24S8.
  if (render_target_cache_) {
    if (auto* da = pass_descriptor->depthAttachment()) {
      da->setClearDepth(render_target_cache_->GetDepthTargetClearDepth());
    }
  }

  // If the render pass configuration has changed since the current render
  // encoder was created (e.g. dummy RT0 -> real RTs, depth/stencil binding),
  // restart the render encoder with the updated descriptor.
  if (current_render_encoder_ &&
      current_render_pass_descriptor_ != pass_descriptor) {
    ++backend_telemetry_.begin_encoder_descriptor_restarts;
    EndRenderEncoder(
        RenderEncoderEndReason::kBeginRenderEncoderDescriptorChanged);
  }

  if (!current_render_encoder_) {
    EndSharedMemoryUploadBlitEncoder(
        SharedMemoryUploadEncoderEndReason::kRenderBegin);
    // If some path cleared the encoder without going through EndRenderEncoder,
    // avoid leaking cached binding state into the new encoder.
    // Note: renderCommandEncoder() returns an autoreleased object, we must
    // retain it.
    current_render_encoder_ =
        current_command_buffer_->renderCommandEncoder(pass_descriptor);
    if (!current_render_encoder_) {
      ++backend_telemetry_.begin_encoder_creation_failures;
      XELOGE("Failed to create render command encoder");
      return false;
    }
    ++backend_telemetry_.begin_encoder_created;
    current_render_encoder_->retain();
    current_render_encoder_has_zpd_visibility_ =
        zpd_visibility_pool_ && zpd_visibility_pool_->is_initialized() &&
        pass_descriptor->visibilityResultBuffer() ==
            zpd_visibility_pool_->visibility_buffer();
    ResetRenderEncoderBufferBindings();
    current_render_encoder_->setLabel(
        NS::String::string("XeniaRenderEncoder", NS::UTF8StringEncoding));
    current_render_pipeline_state_ = nullptr;
    ff_blend_factor_valid_ = false;
    rasterizer_state_valid_ = false;
    viewport_dirty_ = true;
    scissor_dirty_ = true;
    current_depth_stencil_state_ = nullptr;
    stencil_reference_valid_ = false;
    heap_binds_set_on_encoder_ = false;
    if (current_render_pass_descriptor_ != pass_descriptor) {
      if (current_render_pass_descriptor_) {
        current_render_pass_descriptor_->release();
      }
      current_render_pass_descriptor_ = pass_descriptor;
      current_render_pass_descriptor_->retain();
    }
    UseRenderEncoderAttachmentHeaps(pass_descriptor);

    // Derive the initial viewport/scissor from the active render pass texture.
    // Metal validates scissor rectangles against the descriptor attachments,
    // not against the cache's logical RT0 binding.
    uint32_t rt_width = 1;
    uint32_t rt_height = 1;
    GetActiveRenderTargetSize(pass_descriptor, render_target_cache_.get(), 1280,
                              720, rt_width, rt_height);

    MTL::Viewport viewport = {
        0.0, 0.0, static_cast<double>(rt_width), static_cast<double>(rt_height),
        0.0, 1.0};
    current_render_encoder_->setViewport(viewport);

    MTL::ScissorRect scissor = {0, 0, rt_width, rt_height};
    current_render_encoder_->setScissorRect(scissor);

    // IssueDraw applies the guest viewport/scissor before dispatch.
    viewport_dirty_ = true;
    scissor_dirty_ = true;
  }
  return true;
}

void MetalCommandProcessor::EndCommandBuffer() {
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kCommandBufferEnd)) {
    XELOGE("Metal EndCommandBuffer: failed to flush prepared draw queue");
  }
  TryTrimPreparedDrawRetainedStorage();
  EndRenderEncoder(RenderEncoderEndReason::kCommandBufferEnd);
  EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason::kCommandBufferEnd);
  if (texture_cache_ &&
      !texture_cache_->FlushPendingUploadEncodersForCommandEncoderBoundary()) {
    XELOGE(
        "Metal: failed to flush texture upload encoder before command buffer "
        "end");
  }
  if (pipeline_cache_) {
    pipeline_cache_->EndSubmission();
  }

  if (current_command_buffer_) {
    current_command_buffer_->commit();
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
    submission_has_draws_ = false;
  }
  DrainCommandBufferAutoreleasePool();
}

void MetalCommandProcessor::ApplyDepthStencilState(
    const DrawDynamicState& dynamic_state) {
  if (!current_render_encoder_ || !device_) {
    return;
  }

  bool primitive_polygonal =
      dynamic_state.rasterization_enabled && dynamic_state.primitive_polygonal;
  auto stencil_ref_mask_front = dynamic_state.stencil_ref_mask_front;
  auto stencil_ref_mask_back = dynamic_state.stencil_ref_mask_back;
  auto depth_control = dynamic_state.depth_control;

  bool has_stencil_attachment = false;
  if (current_render_pass_descriptor_) {
    if (auto* stencil_attachment =
            current_render_pass_descriptor_->stencilAttachment()) {
      has_stencil_attachment = stencil_attachment->texture() != nullptr;
    }
  }

  if (!has_stencil_attachment && depth_control.stencil_enable) {
    static bool no_stencil_logged = false;
    if (!no_stencil_logged) {
      no_stencil_logged = true;
      XELOGW(
          "Metal: stencil enabled but no stencil attachment bound; disabling "
          "stencil for this pass");
    }
    depth_control.stencil_enable = 0;
    depth_control.backface_enable = 0;
    depth_control.stencilfunc = xenos::CompareFunction::kAlways;
    depth_control.stencilfail = xenos::StencilOp::kKeep;
    depth_control.stencilzpass = xenos::StencilOp::kKeep;
    depth_control.stencilzfail = xenos::StencilOp::kKeep;
    depth_control.stencilfunc_bf = xenos::CompareFunction::kAlways;
    depth_control.stencilfail_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzpass_bf = xenos::StencilOp::kKeep;
    depth_control.stencilzfail_bf = xenos::StencilOp::kKeep;
    stencil_ref_mask_front.value = 0;
    stencil_ref_mask_back.value = 0;
  }

  DepthStencilStateKey key;
  key.depth_control = depth_control.value;
  key.stencil_ref_mask_front = stencil_ref_mask_front.value;
  key.stencil_ref_mask_back = stencil_ref_mask_back.value;
  key.polygonal_and_backface = (primitive_polygonal ? 1u : 0u) |
                               (depth_control.backface_enable ? 2u : 0u);

  MTL::DepthStencilState* state = nullptr;
  auto it = depth_stencil_state_cache_.find(key);
  if (it != depth_stencil_state_cache_.end()) {
    state = it->second;
  } else {
    MTL::DepthStencilDescriptor* ds_desc =
        MTL::DepthStencilDescriptor::alloc()->init();
    if (depth_control.z_enable) {
      ds_desc->setDepthCompareFunction(
          ToMetalCompareFunction(depth_control.zfunc));
      ds_desc->setDepthWriteEnabled(depth_control.z_write_enable != 0);
    } else {
      ds_desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
      ds_desc->setDepthWriteEnabled(false);
    }

    if (depth_control.stencil_enable) {
      auto* front = MTL::StencilDescriptor::alloc()->init();
      front->setStencilCompareFunction(
          ToMetalCompareFunction(depth_control.stencilfunc));
      front->setStencilFailureOperation(
          ToMetalStencilOperation(depth_control.stencilfail));
      front->setDepthFailureOperation(
          ToMetalStencilOperation(depth_control.stencilzfail));
      front->setDepthStencilPassOperation(
          ToMetalStencilOperation(depth_control.stencilzpass));
      front->setReadMask(stencil_ref_mask_front.stencilmask);
      front->setWriteMask(stencil_ref_mask_front.stencilwritemask);

      ds_desc->setFrontFaceStencil(front);

      if (primitive_polygonal && depth_control.backface_enable) {
        auto* back = MTL::StencilDescriptor::alloc()->init();
        back->setStencilCompareFunction(
            ToMetalCompareFunction(depth_control.stencilfunc_bf));
        back->setStencilFailureOperation(
            ToMetalStencilOperation(depth_control.stencilfail_bf));
        back->setDepthFailureOperation(
            ToMetalStencilOperation(depth_control.stencilzfail_bf));
        back->setDepthStencilPassOperation(
            ToMetalStencilOperation(depth_control.stencilzpass_bf));
        back->setReadMask(stencil_ref_mask_back.stencilmask);
        back->setWriteMask(stencil_ref_mask_back.stencilwritemask);
        ds_desc->setBackFaceStencil(back);
        back->release();
      } else {
        ds_desc->setBackFaceStencil(front);
      }

      front->release();
    }

    state = device_->newDepthStencilState(ds_desc);
    ds_desc->release();

    if (!state) {
      XELOGE("Failed to create Metal depth/stencil state");
      return;
    }
    depth_stencil_state_cache_.emplace(key, state);
  }

  if (current_depth_stencil_state_ != state) {
    current_render_encoder_->setDepthStencilState(state);
    current_depth_stencil_state_ = state;
  }

  if (depth_control.stencil_enable) {
    uint32_t ref_front = stencil_ref_mask_front.stencilref;
    uint32_t ref_back = stencil_ref_mask_back.stencilref;
    uint32_t ref = ref_front;
    if (primitive_polygonal && depth_control.backface_enable &&
        dynamic_state.pa_su_sc_mode_cntl.cull_front &&
        !dynamic_state.pa_su_sc_mode_cntl.cull_back) {
      ref = ref_back;
    } else if (primitive_polygonal && depth_control.backface_enable &&
               ref_front != ref_back) {
      static bool mismatch_logged = false;
      if (!mismatch_logged) {
        mismatch_logged = true;
        XELOGW(
            "Metal: front/back stencil ref differ (front={}, back={}); using "
            "front for both",
            ref_front, ref_back);
      }
    }
    if (!stencil_reference_valid_ || current_stencil_reference_ != ref) {
      current_render_encoder_->setStencilReferenceValue(ref);
      current_stencil_reference_ = ref;
      stencil_reference_valid_ = true;
    }
  }
}

void MetalCommandProcessor::ApplyRasterizerState(
    const DrawDynamicState& dynamic_state) {
  if (!current_render_encoder_ || !render_target_cache_) {
    return;
  }

  if (!rasterizer_state_valid_ ||
      current_cull_mode_ != dynamic_state.cull_mode) {
    current_render_encoder_->setCullMode(dynamic_state.cull_mode);
    current_cull_mode_ = dynamic_state.cull_mode;
  }

  if (!rasterizer_state_valid_ ||
      current_front_facing_winding_ != dynamic_state.front_facing_winding) {
    current_render_encoder_->setFrontFacingWinding(
        dynamic_state.front_facing_winding);
    current_front_facing_winding_ = dynamic_state.front_facing_winding;
  }

  if (!rasterizer_state_valid_ ||
      current_triangle_fill_mode_ != dynamic_state.triangle_fill_mode) {
    current_render_encoder_->setTriangleFillMode(
        dynamic_state.triangle_fill_mode);
    current_triangle_fill_mode_ = dynamic_state.triangle_fill_mode;
  }

  float depth_bias_values[] = {dynamic_state.depth_bias_constant,
                               dynamic_state.depth_bias_slope, 0.0f};
  if (!rasterizer_state_valid_ ||
      std::memcmp(current_depth_bias_values_, depth_bias_values,
                  sizeof(depth_bias_values)) != 0) {
    current_render_encoder_->setDepthBias(dynamic_state.depth_bias_constant,
                                          dynamic_state.depth_bias_slope, 0.0f);
    std::memcpy(current_depth_bias_values_, depth_bias_values,
                sizeof(depth_bias_values));
  }

  if (!rasterizer_state_valid_ ||
      current_depth_clip_mode_ != dynamic_state.depth_clip_mode) {
    current_render_encoder_->setDepthClipMode(dynamic_state.depth_clip_mode);
    current_depth_clip_mode_ = dynamic_state.depth_clip_mode;
  }
  rasterizer_state_valid_ = true;
}

void MetalCommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal,
    uint32_t line_loop_closing_index, xenos::Endian index_endian,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  uint32_t dirty = 0u;
  ArchFloatMask dirty_float_mask = floatmask_zero;

  auto update_dirty_floatmask = [&dirty_float_mask](float x, float y) {
    dirty_float_mask =
        ArchORFloatMask(dirty_float_mask, ArchCmpneqFloatMask(x, y));
  };
  auto update_dirty_uint32_cmp = [&dirty](uint32_t x, uint32_t y) {
    dirty |= (x ^ y);
  };

  // Get color info for each render target
  reg::RB_COLOR_INFO color_infos[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
  }

  // Build flags
  uint32_t flags = 0;

  // Shared memory mode - determines whether shaders read from SRV (T0) or UAV
  // (U0)
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }

  // W0 division control from PA_CL_VTE_CNTL
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }

  // Primitive type flags
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }

  // Depth format
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }

  // Alpha test - encode compare function in flags
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;

  // Gamma conversion flags for render targets
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }

  update_dirty_uint32_cmp(system_constants_.flags, flags);
  system_constants_.flags = flags;

  // Tessellation factor range
  float tessellation_factor_min =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  update_dirty_floatmask(system_constants_.tessellation_factor_range_min,
                         tessellation_factor_min);
  update_dirty_floatmask(system_constants_.tessellation_factor_range_max,
                         tessellation_factor_max);
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index
  update_dirty_uint32_cmp(system_constants_.line_loop_closing_index,
                          line_loop_closing_index);
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Vertex index configuration
  update_dirty_uint32_cmp(
      static_cast<uint32_t>(system_constants_.vertex_index_endian),
      static_cast<uint32_t>(index_endian));
  update_dirty_uint32_cmp(system_constants_.vertex_index_offset,
                          vgt_indx_offset);
  update_dirty_uint32_cmp(system_constants_.vertex_index_min, vgt_min_vtx_indx);
  update_dirty_uint32_cmp(system_constants_.vertex_index_max, vgt_max_vtx_indx);
  system_constants_.vertex_index_endian = index_endian;
  system_constants_.vertex_index_offset = vgt_indx_offset;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (when not CLIP_DISABLE)
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (xe::bit_scan_forward(user_clip_planes_remaining,
                                &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const float* user_clip_plane_regs = reinterpret_cast<const float*>(
          &regs.values[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4]);
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs,
                      4 * sizeof(float)) != 0) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs,
                    4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // NDC scale and offset from viewport info
  for (uint32_t i = 0; i < 3; ++i) {
    update_dirty_floatmask(system_constants_.ndc_scale[i],
                           viewport_info.ndc_scale[i]);
    update_dirty_floatmask(system_constants_.ndc_offset[i],
                           viewport_info.ndc_offset[i]);
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size parameters
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min =
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max =
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x =
        float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y =
        float(pa_su_point_size.height) * (2.0f / 16.0f);
    update_dirty_floatmask(system_constants_.point_vertex_diameter_min,
                           point_vertex_diameter_min);
    update_dirty_floatmask(system_constants_.point_vertex_diameter_max,
                           point_vertex_diameter_max);
    update_dirty_floatmask(system_constants_.point_constant_diameter[0],
                           point_constant_diameter_x);
    update_dirty_floatmask(system_constants_.point_constant_diameter[1],
                           point_constant_diameter_y);
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // Screen to NDC radius conversion.
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader. Include draw_resolution_scale to
    // match D3D12 behavior.
    uint32_t point_draw_resolution_scale_x =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_x()
                             : 1;
    uint32_t point_draw_resolution_scale_y =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_y()
                             : 1;
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(point_draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(point_draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    update_dirty_floatmask(
        system_constants_.point_screen_diameter_to_ndc_radius[0],
        point_screen_diameter_to_ndc_radius_x);
    update_dirty_floatmask(
        system_constants_.point_screen_diameter_to_ndc_radius[1],
        point_screen_diameter_to_ndc_radius_y);
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / resolution scaling (mirror D3D12 logic).
  // Always update textures_resolution_scaled, even when used_texture_mask is 0,
  // to avoid stale values from previous draws.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    if (texture_cache_) {
      uint32_t& texture_signs_uint =
          system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = (texture_index & 3) * 8;
      uint8_t texture_signs =
          texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs)
                                       << texture_signs_shift;
      uint32_t texture_signs_mask = uint32_t(0xFF) << texture_signs_shift;
      update_dirty_uint32_cmp((texture_signs_uint & texture_signs_mask),
                              texture_signs_shifted);
      texture_signs_uint =
          (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      textures_resolution_scaled |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
  }
  update_dirty_uint32_cmp(system_constants_.textures_resolution_scaled,
                          textures_resolution_scaled);
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Sample count log2 for alpha to mask
  uint32_t sample_count_log2_x =
      rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y =
      rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  update_dirty_uint32_cmp(system_constants_.sample_count_log2[0],
                          sample_count_log2_x);
  update_dirty_uint32_cmp(system_constants_.sample_count_log2[1],
                          sample_count_log2_y);
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test reference
  update_dirty_floatmask(system_constants_.alpha_test_reference, rb_alpha_ref);
  system_constants_.alpha_test_reference = rb_alpha_ref;

  // Alpha to mask
  uint32_t alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                               ? (rb_colorcontrol.value >> 24) | (1 << 8)
                               : 0;
  update_dirty_uint32_cmp(system_constants_.alpha_to_mask, alpha_to_mask);
  system_constants_.alpha_to_mask = alpha_to_mask;

  // Color exponent bias
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t color_exp_bias = color_infos[i].color_exp_bias;
    // Fixed-point render targets (k_16_16 / k_16_16_16_16) are backed by
    // *_SNORM in the host render targets path. If full-range emulation is
    // requested, remap from -32...32 to -1...1 by dividing the output values
    // by 32.
    if (color_infos[i].color_format ==
        xenos::ColorRenderTargetFormat::k_16_16) {
      if (!render_target_cache_->IsFixedRG16TruncatedToMinus1To1()) {
        color_exp_bias -= 5;
      }
    } else if (color_infos[i].color_format ==
               xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (!render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1()) {
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale = xe::memory::Reinterpret<float>(
        int32_t(0x3F800000 + (color_exp_bias << 23)));
    update_dirty_floatmask(system_constants_.color_exp_bias[i],
                           color_exp_bias_scale);
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }

  // Blend constants (used by EDRAM and for host blending)
  float blend_red = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
  float blend_green = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
  float blend_blue = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
  float blend_alpha = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  update_dirty_floatmask(system_constants_.edram_blend_constant[0], blend_red);
  update_dirty_floatmask(system_constants_.edram_blend_constant[1],
                         blend_green);
  update_dirty_floatmask(system_constants_.edram_blend_constant[2], blend_blue);
  update_dirty_floatmask(system_constants_.edram_blend_constant[3],
                         blend_alpha);
  system_constants_.edram_blend_constant[0] = blend_red;
  system_constants_.edram_blend_constant[1] = blend_green;
  system_constants_.edram_blend_constant[2] = blend_blue;
  system_constants_.edram_blend_constant[3] = blend_alpha;

  dirty |= ArchFloatMaskSignbit(dirty_float_mask);
  cbuffer_binding_system_.up_to_date &= !dirty;
}

#define COMMAND_PROCESSOR MetalCommandProcessor
#include "../pm4_command_processor_implement.h"
#undef COMMAND_PROCESSOR

}  // namespace metal
}  // namespace gpu
}  // namespace xe
