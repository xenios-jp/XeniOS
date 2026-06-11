/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_command_processor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_backend_telemetry.h"

DECLARE_bool(metal_backend_hazard_model);

#if XE_METAL_TELEMETRY

namespace xe {
namespace gpu {
namespace metal {

namespace {

constexpr uint32_t kNativeMslDrawConstantsChangeSystem = 1u << 0;
constexpr uint32_t kNativeMslDrawConstantsChangeFloat = 1u << 1;
constexpr uint32_t kNativeMslDrawConstantsChangeBoolLoop = 1u << 2;
constexpr uint32_t kNativeMslDrawConstantsChangeFetch = 1u << 3;
constexpr uint32_t kNativeMslDrawConstantsChangeDescriptorIndices = 1u << 4;
constexpr uint32_t kNativeMslDrawConstantsChangePrimitiveIndex = 1u << 5;

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

}  // namespace

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
  const uint64_t encoder_buffer_slot_offset_bind_total =
      MetalTelemetrySumCounts(
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
      "MetalTelemetry[{}]: work swaps={} draws={} submitted={} pipelines "
      "set/skip={}/{} texture_requests before/after_encoder={}/{}",
      reason, backend_telemetry_.swaps - backend_telemetry_last_dump_swap_,
      backend_telemetry_.draw_calls, backend_telemetry_.draws_submitted,
      backend_telemetry_.pipeline_sets, backend_telemetry_.pipeline_set_skips,
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
  XELOGI("MetalTelemetry[{}]: texture_upload execution={{ {} }}", reason,
         texture_upload_execution_details);
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
  XELOGI("MetalTelemetry[{}]: native_msl_draw_constants reasons={{ {} }}",
         reason, native_msl_draw_constants_rebuild_reasons);
  XELOGI("MetalTelemetry[{}]: native_msl_draw_constants change_masks={{ {} }}",
         reason, native_msl_draw_constants_change_masks);
  XELOGI(
      "MetalTelemetry[{}]: native_msl_texture_sign sign_only_switches={} "
      "live_variants={} (of pipeline_sets={})",
      reason, backend_telemetry_.pipeline_sets_sign_key_change,
      backend_telemetry_.native_msl_sign_variants_live,
      backend_telemetry_.pipeline_sets);
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
  if (shared_memory_hazard_fence_edges_) {
    XELOGI(
        "MetalTelemetry[{}]: hazard_model mode={} fence updates "
        "blit/compute={}/{} waits render/blit/compute={}/{}/{}",
        reason, cvars::metal_backend_hazard_model ? "untracked" : "validate",
        backend_telemetry_.hazard_fence_updates_blit,
        backend_telemetry_.hazard_fence_updates_compute,
        backend_telemetry_.hazard_fence_waits[0],
        backend_telemetry_.hazard_fence_waits[1],
        backend_telemetry_.hazard_fence_waits[2]);
  }
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
      "MetalTelemetry[{}]: resolve_clear load_action merged_pass/single={}/{} "
      "draw={}",
      reason, rt_stats.resolve_clear.load_action_merged_passes,
      rt_stats.resolve_clear.load_action_single_target,
      rt_stats.resolve_clear.draw_clears);
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
  // native_msl_sign_variants_live is a monotonic gauge (translations are never
  // freed per window), so carry it across resets like swaps.
  uint64_t native_msl_sign_variants_live =
      backend_telemetry_.native_msl_sign_variants_live;
  backend_telemetry_ = BackendTelemetryStats();
  backend_telemetry_.swaps = backend_telemetry_last_dump_swap_;
  backend_telemetry_.native_msl_sign_variants_live =
      native_msl_sign_variants_live;
}

void MetalCommandProcessor::RecordSharedMemoryRequestOutcome(
    SharedMemoryRequestOutcome outcome) {
  const size_t outcome_index = static_cast<size_t>(outcome);
  if (outcome_index < kSharedMemoryRequestOutcomeCount) {
    ++backend_telemetry_.shared_memory_request_outcomes[outcome_index];
  }
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
  shared_memory_upload_encoder_has_writes_ = true;
}

void MetalCommandProcessor::RecordHazardFenceUpdate(bool compute_encoder) {
  if (compute_encoder) {
    ++backend_telemetry_.hazard_fence_updates_compute;
  } else {
    ++backend_telemetry_.hazard_fence_updates_blit;
  }
}

void MetalCommandProcessor::RecordHazardFenceWait(uint32_t encoder_kind) {
  if (encoder_kind < 3) {
    ++backend_telemetry_.hazard_fence_waits[encoder_kind];
  }
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XE_METAL_TELEMETRY
