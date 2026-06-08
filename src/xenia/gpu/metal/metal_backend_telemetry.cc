/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_backend_telemetry.h"

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace gpu {
namespace metal {

const char* MetalRenderEncoderEndReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "unknown";
    case 1:
      return "prepare_wait";
    case 2:
      return "swap";
    case 3:
      return "command_buffer_end";
    case 4:
      return "transfer_request";
    case 5:
      return "shared_memory_read";
    case 6:
      return "rt_update_descriptor_dirty";
    case 7:
      return "pipeline_descriptor_incompatible";
    case 8:
      return "texture_upload_before_draw_pass";
    case 9:
      return "shared_memory_upload_before_draw_pass";
    case 10:
      return "resolve_needs_boundary";
    case 11:
      return "begin_descriptor_changed";
    default:
      return "invalid";
  }
}

const char* MetalTransferRequestSourceName(size_t source) {
  switch (source) {
    case 0:
      return "unknown";
    case 1:
      return "shared_memory_upload";
    case 2:
      return "guest_index_copy";
    case 3:
      return "render_target_transfer";
    default:
      return "invalid";
  }
}

const char* MetalSharedMemoryRequestReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "unknown";
    case 1:
      return "vertex_fetch";
    case 2:
      return "memexport_stream";
    case 3:
      return "guest_index";
    case 4:
      return "shader_primitive_index";
    case 5:
      return "index_copy_source";
    case 6:
      return "texture_base";
    case 7:
      return "texture_mips";
    case 8:
      return "texture_base_and_mips";
    case 9:
      return "resolve_copy_dest";
    case 10:
      return "draw_materialization";
    default:
      return "invalid";
  }
}

const char* MetalSharedMemoryRequestOutcomeName(size_t outcome) {
  switch (outcome) {
    case 0:
      return "already_resident";
    case 1:
      return "upload_before_render_encoder";
    case 2:
      return "upload_inside_render_encoder";
    case 3:
      return "request_failed";
    case 4:
      return "no_shared_memory";
    case 5:
      return "texture_deferred_upload_flush";
    default:
      return "invalid";
  }
}

const char* MetalSharedMemoryUploadRouteName(size_t route) {
  switch (route) {
    case 0:
      return "staged_blit";
    case 1:
      return "direct_write";
    default:
      return "invalid";
  }
}

const char* MetalSharedMemoryDirectWriteRejectReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "main_gpu_access_in_flight";
    case 1:
      return "standalone_access_in_flight";
    case 2:
      return "no_shared_buffer_contents";
    case 3:
      return "mixed_range_split";
    default:
      return "invalid";
  }
}

const char* MetalSharedMemoryUploadEncoderEndReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "unknown";
    case 1:
      return "render_begin";
    case 2:
      return "transfer_request";
    case 3:
      return "texture_compute";
    case 4:
      return "texture_blit";
    case 5:
      return "texture_deferred_blit";
    case 6:
      return "scaled_resolve_blit";
    case 7:
      return "command_buffer_end";
    case 8:
      return "swap";
    case 9:
      return "upload_failure";
    case 10:
      return "shutdown";
    case 11:
      return "materialization_drain";
    default:
      return "invalid";
  }
}

const char* MetalTextureUploadSourceRouteName(size_t route) {
  switch (route) {
    case 0:
      return "cpu_guest";
    case 1:
      return "resident_shared_memory";
    case 2:
      return "scaled_resolve";
    default:
      return "invalid";
  }
}

const char* MetalTextureUploadSourceFallbackReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "mixed_validity";
    case 1:
      return "scaled_resolve";
    case 2:
      return "source_already_resident";
    case 3:
      return "cpu_source_load_failed";
    case 4:
      return "unknown";
    default:
      return "invalid";
  }
}

const char* MetalTextureUploadCompatibilityClassName(size_t type) {
  switch (type) {
    case 0:
      return "direct_copy_candidate";
    case 1:
      return "compute_required";
    default:
      return "invalid";
  }
}

const char* MetalTextureUploadComputeBlockerName(size_t blocker) {
  switch (blocker) {
    case 0:
      return "tiled";
    case 1:
      return "tiled_3d";
    case 2:
      return "endian_swap";
    case 3:
      return "format_conversion";
    case 4:
      return "bc_decompress";
    case 5:
      return "scaled_resolve";
    case 6:
      return "packed_mips";
    case 7:
      return "repack_alignment";
    case 8:
      return "unknown";
    default:
      return "invalid";
  }
}

const char* MetalTextureUploadExecutionDetailName(size_t detail) {
  switch (detail) {
    case 0:
      return "duplicate_planned_same_plan";
    case 1:
      return "fallback_already_current_lockless";
    case 2:
      return "cpu_source_already_current";
    case 3:
      return "pruned_already_current";
    case 4:
      return "pruned_duplicate_same_flush";
    default:
      return "invalid";
  }
}

const char* MetalTextureReloadReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "planned_base_only";
    case 1:
      return "planned_mips_only";
    case 2:
      return "planned_base_and_mips";
    case 3:
      return "planned_cpu_source";
    case 4:
      return "planned_resident_source";
    case 5:
      return "planned_again_same_frame";
    case 6:
      return "refresh_still_needed";
    case 7:
      return "refresh_became_current";
    case 8:
      return "execute_cpu_source";
    case 9:
      return "execute_resident_source";
    case 10:
      return "execute_again_same_frame";
    default:
      return "invalid";
  }
}

const char* MetalTextureWatchInvalidationReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "cpu_base";
    case 1:
      return "cpu_mips";
    case 2:
      return "gpu_other_base";
    case 3:
      return "gpu_other_mips";
    case 4:
      return "gpu_resolve_base";
    case 5:
      return "gpu_resolve_mips";
    default:
      return "invalid";
  }
}

const char* MetalTextureResolveReloadReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "candidate";
    case 1:
      return "exact_range";
    case 2:
      return "contained_range";
    case 3:
      return "partial_overlap";
    case 4:
      return "no_overlap";
    case 5:
      return "no_provenance";
    case 6:
      return "mips_requested";
    case 7:
      return "scaled_resolve";
    case 8:
      return "source_unknown";
    case 9:
      return "source_direct_host";
    case 10:
      return "source_render_target";
    default:
      return "invalid";
  }
}

std::string MetalFormatNamedCounts(const uint64_t* values, size_t count,
                                   MetalTelemetryNameCallback name_callback) {
  std::string formatted;
  for (size_t i = 0; i < count; ++i) {
    if (!values[i]) {
      continue;
    }
    if (!formatted.empty()) {
      formatted += ", ";
    }
    formatted += fmt::format("{}={}", name_callback(i), values[i]);
  }
  return formatted.empty() ? "none" : formatted;
}

std::string MetalFormatNamedTriplets(const uint64_t* total,
                                     const uint64_t* active,
                                     const uint64_t* no_active, size_t count,
                                     MetalTelemetryNameCallback name_callback) {
  std::string formatted;
  for (size_t i = 0; i < count; ++i) {
    if (!total[i]) {
      continue;
    }
    if (!formatted.empty()) {
      formatted += ", ";
    }
    formatted += fmt::format("{}={}/{}/{}", name_callback(i), total[i],
                             active[i], no_active[i]);
  }
  return formatted.empty() ? "none" : formatted;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
