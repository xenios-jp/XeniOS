/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_command_processor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/gpu/metal/metal_texture_cache.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

template <typename T>
void TrimRetainedVector(std::vector<T>& vector, size_t max_capacity) {
  if (vector.capacity() <= max_capacity) {
    return;
  }
  std::vector<T>().swap(vector);
}

}  // namespace

MetalCommandProcessor::PreparedDraw*
MetalCommandProcessor::AcquirePreparedDraw() {
  PreparedDraw* draw = nullptr;
  if (!prepared_draw_recycle_pool_.empty()) {
    draw = prepared_draw_recycle_pool_.back();
    prepared_draw_recycle_pool_.pop_back();
  } else {
    prepared_draw_storage_.push_back(std::make_unique<PreparedDraw>());
    draw = prepared_draw_storage_.back().get();
  }
  return draw;
}

template <typename T>
MetalCommandProcessor::PreparedDrawSpan<T>
MetalCommandProcessor::StorePreparedDrawPayload(
    PreparedDrawPayloadStorage<T>& storage, const T* data, size_t count) {
  if (!count) {
    return {};
  }
  assert_true(data != nullptr);
  assert_true(count <= UINT32_MAX);
  constexpr size_t kPayloadChunkElementCount = 1024;
  for (;;) {
    if (storage.current_chunk >= storage.chunks.size()) {
      auto& chunk = storage.chunks.emplace_back();
      chunk.values.reserve(std::max(count, kPayloadChunkElementCount));
    }
    auto& values = storage.chunks[storage.current_chunk].values;
    if (values.capacity() - values.size() >= count) {
      break;
    }
    ++storage.current_chunk;
  }
  auto& values = storage.chunks[storage.current_chunk].values;
  const size_t offset = values.size();
  values.insert(values.end(), data, data + count);
  return {values.data() + offset, static_cast<uint32_t>(count)};
}

MetalCommandProcessor::PreparedDrawSpan<SharedMemory::Range>
MetalCommandProcessor::StorePreparedDrawMaterializationRanges(
    const std::vector<SharedMemory::Range>& ranges) {
  return StorePreparedDrawPayload(prepared_draw_materialization_range_storage_,
                                  ranges.data(), ranges.size());
}

MetalCommandProcessor::PreparedDrawSpan<draw_util::MemExportRange>
MetalCommandProcessor::StorePreparedDrawMemexportRanges(
    const std::vector<draw_util::MemExportRange>& ranges) {
  return StorePreparedDrawPayload(prepared_draw_memexport_range_storage_,
                                  ranges.data(), ranges.size());
}

void MetalCommandProcessor::ResetPreparedDrawPayloadArena() {
  auto reset_storage = [](auto& storage) {
    for (auto& chunk : storage.chunks) {
      chunk.values.clear();
    }
    storage.current_chunk = 0;
  };
  reset_storage(prepared_draw_materialization_range_storage_);
  reset_storage(prepared_draw_memexport_range_storage_);
}

void MetalCommandProcessor::TryResetPreparedDrawPayloadArena() {
  if (!prepared_draw_queue_.empty() || !prepared_draw_flush_draws_.empty() ||
      flushing_prepared_draw_queue_) {
    return;
  }
  ResetPreparedDrawPayloadArena();
}

void MetalCommandProcessor::TryTrimPreparedDrawRetainedStorage() {
  if (!prepared_draw_queue_.empty() || !prepared_draw_flush_draws_.empty() ||
      flushing_prepared_draw_queue_) {
    return;
  }

  ResetPreparedDrawPayloadArena();
  for (auto& draw_storage : prepared_draw_storage_) {
    PreparedDraw& draw = *draw_storage;
    TrimRetainedVector(draw.texture_resource_set.heaps, 64);
    TrimRetainedVector(draw.texture_resource_set.resources, 512);
    TrimRetainedVector(draw.texture_materialization_plan.source_ranges,
                       kPreparedDrawQueueMaxRanges * 2);
    TrimRetainedVector(draw.native_vertex_bindings.runtime_info, 64);
    TrimRetainedVector(draw.native_pixel_bindings.runtime_info, 64);
  }
  TrimRetainedVector(prepared_draw_flush_draws_,
                     kPreparedDrawQueueMaxDraws * 2);
  TrimRetainedVector(prepared_draw_flush_texture_plans_,
                     kPreparedDrawQueueMaxDraws * 2);
  TrimRetainedVector(prepared_draw_flush_materialization_ranges_,
                     kPreparedDrawQueueMaxRanges * 2);
  auto trim_payload_storage = [](auto& storage, size_t max_capacity) {
    for (auto& chunk : storage.chunks) {
      TrimRetainedVector(chunk.values, max_capacity);
    }
  };
  trim_payload_storage(prepared_draw_materialization_range_storage_,
                       kPreparedDrawQueueMaxRanges * 2);
  trim_payload_storage(prepared_draw_memexport_range_storage_, 1024);
}

void MetalCommandProcessor::RecyclePreparedDraw(PreparedDraw* draw) {
  if (!draw) {
    return;
  }
  ResetPreparedDrawForReuse(*draw);
  prepared_draw_recycle_pool_.push_back(draw);
}

void MetalCommandProcessor::ResetPreparedDrawForReuse(PreparedDraw& draw) {
  // This is intentionally a field reset rather than `draw = PreparedDraw{}`.
  // The latter forces the compiler to synthesize a temporary draw, run the old
  // draw destructor, and potentially free retained vectors on every recycle.
  draw.pipeline = nullptr;
  draw.tessellation_pipeline_state = nullptr;
  draw.geometry_pipeline_state = nullptr;
  draw.native_mesh_pipeline_state = nullptr;

  draw.has_index_buffer_info = false;

  draw.vertex_bindings = {};
  draw.vertex_range_count = 0;
  draw.materialization_ranges = {};
  draw.texture_source_range_count = 0;
  draw.has_invalid_shared_memory = false;
  draw.shared_memory_hazard_range_count = 0;
  draw.shared_memory_consumer_stages = MTL::RenderStages(0);

  draw.memexport_ranges = {};
  draw.memexport_write_stages = MTL::RenderStages(0);
  draw.shared_memory_usage = MTL::ResourceUsageRead;
  draw.render_target_key = {};
  draw.texture_resource_set.heaps.clear();
  draw.texture_resource_set.resources.clear();
  draw.texture_resource_set.serial = 0;
  draw.texture_resource_set.source_serial = 0;
  draw.texture_materialization_plan.Reset();

  draw.native_vertex_bindings.Clear();
  draw.native_pixel_bindings.Clear();
  draw.native_pixel_metadata_valid = false;
  draw.native_primitive_index_constants = {};

  draw.use_tessellation_emulation = false;
  draw.use_geometry_emulation = false;
  draw.use_native_msl = false;
  draw.use_native_msl_primitive_mesh = false;
  draw.use_native_msl_tessellation = false;
  draw.shared_memory_is_uav = false;
  draw.memexport_used = false;
  draw.uses_vertex_fetch = false;
  draw.prepare_uniforms = false;
  draw.fallback_depth_attachment_required = false;
  draw.texture_upload_needed = false;
  draw.may_texture_request_load_data = false;
  draw.has_pending_draw_pass_transfers = false;
}

bool MetalCommandProcessor::PreparedDrawQueueHasActiveZPD() const {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }
  return zpd_active_segment_.logical_active ||
         zpd_active_segment_.segment_active ||
         zpd_active_segment_.segment_pending_begin ||
         zpd_active_query_.is_open();
}

void MetalCommandProcessor::RecordPreparedDrawQueueReject(
    PreparedDrawQueueRejectReason reject_reason) {
  const size_t reject_index = static_cast<size_t>(reject_reason);
  if (reject_index <
      backend_telemetry_.prepared_draw_queue_reject_reasons.size()) {
    ++backend_telemetry_.prepared_draw_queue_reject_reasons[reject_index];
  }
}

bool MetalCommandProcessor::CanQueuePreparedDraw(
    const PreparedDraw& draw,
    PreparedDrawQueueRejectReason& reject_reason) const {
  reject_reason = PreparedDrawQueueRejectReason::kNone;
  if (draw.memexport_used) {
    reject_reason = PreparedDrawQueueRejectReason::kMemexport;
    return false;
  }
  if (draw.has_pending_draw_pass_transfers) {
    reject_reason = PreparedDrawQueueRejectReason::kPendingDrawPassTransfers;
    return false;
  }
  if (PreparedDrawQueueHasActiveZPD()) {
    reject_reason = PreparedDrawQueueRejectReason::kZPDActive;
    return false;
  }
  if (draw.materialization_ranges.empty() && !draw.texture_upload_needed) {
    reject_reason = PreparedDrawQueueRejectReason::kNoSharedMemoryRanges;
    return false;
  }
  if (!draw.has_invalid_shared_memory && prepared_draw_queue_.empty() &&
      current_render_encoder_ && !draw.texture_upload_needed) {
    reject_reason = PreparedDrawQueueRejectReason::kResidentWithoutActiveQueue;
    return false;
  }
  if (prepared_draw_queue_render_target_key_valid_ &&
      draw.render_target_key != prepared_draw_queue_render_target_key_) {
    reject_reason = PreparedDrawQueueRejectReason::kRenderTargetKeyMismatch;
    return false;
  }

  size_t range_count = draw.materialization_ranges.size();
  uint64_t byte_count = 0;
  for (const SharedMemory::Range& range : draw.materialization_ranges) {
    byte_count += range.length;
  }
  for (const PreparedDraw* queued_draw : prepared_draw_queue_) {
    range_count += queued_draw->materialization_ranges.size();
    for (const SharedMemory::Range& range :
         queued_draw->materialization_ranges) {
      byte_count += range.length;
    }
  }
  if (prepared_draw_queue_.size() + 1 > kPreparedDrawQueueMaxDraws ||
      range_count > kPreparedDrawQueueMaxRanges ||
      byte_count > kPreparedDrawQueueMaxBytes) {
    reject_reason = PreparedDrawQueueRejectReason::kQueueBudget;
    return false;
  }
  return true;
}

bool MetalCommandProcessor::FlushPreparedDrawQueue(
    PreparedDrawFlushReason reason) {
  if (prepared_draw_queue_.empty()) {
    return true;
  }
  const size_t reason_index = static_cast<size_t>(reason);
  if (reason_index <
      backend_telemetry_.prepared_draw_queue_flush_reasons.size()) {
    ++backend_telemetry_.prepared_draw_queue_flush_reasons[reason_index];
  }
  ++backend_telemetry_.prepared_draw_queue_flushes;
  if (prepared_draw_queue_.size() == 1) {
    ++backend_telemetry_.prepared_draw_queue_single_draw_flushes;
  }

  std::vector<PreparedDraw*>& draws = prepared_draw_flush_draws_;
  draws.clear();
  draws.swap(prepared_draw_queue_);
  prepared_draw_queue_.clear();
  prepared_draw_queue_render_target_key_valid_ = false;
  prepared_draw_queue_render_target_key_ = {};

  if (texture_cache_) {
    std::vector<MetalTextureCache::TextureMaterializationPlan*>& texture_plans =
        prepared_draw_flush_texture_plans_;
    texture_plans.clear();
    texture_plans.reserve(draws.size());
    for (PreparedDraw* draw : draws) {
      if (draw->texture_materialization_plan.NeedsTextureUpload()) {
        texture_plans.push_back(&draw->texture_materialization_plan);
      }
    }
    if (!texture_plans.empty()) {
      texture_cache_->RefreshTextureMaterializationPlans(
          texture_plans.data(), static_cast<uint32_t>(texture_plans.size()));
    }
  }

  std::vector<SharedMemory::Range>& ranges =
      prepared_draw_flush_materialization_ranges_;
  ranges.clear();
  size_t range_count = 0;
  uint64_t byte_count = 0;
  bool has_texture_materialization = false;
  for (const PreparedDraw* draw : draws) {
    const size_t texture_source_range_count = std::min<size_t>(
        draw->texture_source_range_count, draw->materialization_ranges.size());
    const bool skip_texture_source_ranges =
        !draw->texture_materialization_plan.NeedsTextureUpload() &&
        draw->texture_materialization_plan.source_ranges.empty();
    const size_t range_start =
        skip_texture_source_ranges ? texture_source_range_count : 0;
    range_count += draw->materialization_ranges.size() - range_start;
    has_texture_materialization =
        has_texture_materialization ||
        draw->texture_materialization_plan.NeedsTextureUpload();
    for (size_t i = range_start; i < draw->materialization_ranges.size(); ++i) {
      const SharedMemory::Range& range = draw->materialization_ranges[i];
      byte_count += range.length;
    }
  }
  ranges.reserve(range_count);
  for (const PreparedDraw* draw : draws) {
    const size_t texture_source_range_count = std::min<size_t>(
        draw->texture_source_range_count, draw->materialization_ranges.size());
    const bool skip_texture_source_ranges =
        !draw->texture_materialization_plan.NeedsTextureUpload() &&
        draw->texture_materialization_plan.source_ranges.empty();
    const size_t range_start =
        skip_texture_source_ranges ? texture_source_range_count : 0;
    if (range_start >= draw->materialization_ranges.size()) {
      continue;
    }
    ranges.insert(ranges.end(),
                  draw->materialization_ranges.begin() +
                      static_cast<std::ptrdiff_t>(range_start),
                  draw->materialization_ranges.end());
  }
  const bool has_invalid_shared_memory =
      shared_memory_ && !ranges.empty() &&
      AnySharedMemoryRangeInvalid(ranges.data(),
                                  static_cast<uint32_t>(ranges.size()));
  backend_telemetry_.prepared_draw_queue_draws_flushed += draws.size();
  backend_telemetry_.prepared_draw_queue_ranges_flushed += ranges.size();
  backend_telemetry_.prepared_draw_queue_bytes_flushed += byte_count;
  if (has_invalid_shared_memory) {
    ++backend_telemetry_.prepared_draw_queue_invalid_flushes;
  }

  const bool previous_flushing = flushing_prepared_draw_queue_;
  flushing_prepared_draw_queue_ = true;
  auto finish_flush = [&]() {
    for (PreparedDraw* draw : draws) {
      RecyclePreparedDraw(draw);
    }
    draws.clear();
    prepared_draw_flush_texture_plans_.clear();
    ranges.clear();
    TryResetPreparedDrawPayloadArena();
  };
  auto fail_flush = [&]() {
    flushing_prepared_draw_queue_ = previous_flushing;
    finish_flush();
    return false;
  };

  if (has_invalid_shared_memory && shared_memory_ && !ranges.empty()) {
    PrepareSharedMemoryUploadBeforeDrawPass(
        ranges.data(), static_cast<uint32_t>(ranges.size()));
    if (!RequestSharedMemoryRanges(
            SharedMemoryRequestReason::kDrawMaterialization, ranges.data(),
            static_cast<uint32_t>(ranges.size()))) {
      XELOGE("Failed to request {} prepared-draw shared-memory ranges",
             ranges.size());
      return fail_flush();
    }
    EndSharedMemoryUploadBlitEncoder(
        SharedMemoryUploadEncoderEndReason::kMaterializationDrain);
  }

  if (has_texture_materialization && texture_cache_) {
    if (current_render_encoder_) {
      EndRenderEncoder(RenderEncoderEndReason::kTextureUploadBeforeDrawPass);
    }
    if (!EnsureCommandBuffer()) {
      return fail_flush();
    }
    texture_cache_->BeginTextureUploadBatch();
    bool texture_materialization_succeeded = true;
    for (PreparedDraw* draw : draws) {
      if (!draw->texture_materialization_plan.NeedsTextureUpload()) {
        continue;
      }
      ++backend_telemetry_.prepared_draw_queue_texture_plans_flushed;
      backend_telemetry_.prepared_draw_queue_texture_loads_planned +=
          draw->texture_materialization_plan.planned_load_count;
      if (!texture_cache_->ExecuteTextureMaterialization(
              draw->texture_materialization_plan)) {
        texture_materialization_succeeded = false;
        break;
      }
      backend_telemetry_.prepared_draw_queue_texture_loads_executed +=
          draw->texture_materialization_plan.executed_load_count;
    }
    if (!texture_cache_->EndTextureUploadBatch()) {
      texture_materialization_succeeded = false;
    }
    if (!texture_materialization_succeeded) {
      return fail_flush();
    }
  }

  for (const PreparedDraw* draw : draws) {
    if (!EncodePreparedDraw(*draw)) {
      return fail_flush();
    }
  }

  flushing_prepared_draw_queue_ = previous_flushing;
  finish_flush();
  return true;
}

bool MetalCommandProcessor::SubmitPreparedDraw(PreparedDraw* draw) {
  if (!draw) {
    return false;
  }
  PreparedDrawQueueRejectReason reject_reason =
      PreparedDrawQueueRejectReason::kNone;
  if (CanQueuePreparedDraw(*draw, reject_reason)) {
    if (prepared_draw_queue_.empty()) {
      prepared_draw_queue_render_target_key_ = draw->render_target_key;
      prepared_draw_queue_render_target_key_valid_ = true;
    }
    prepared_draw_queue_.push_back(draw);
    ++backend_telemetry_.prepared_draw_queue_appends;
    if (prepared_draw_queue_.size() >= kPreparedDrawQueueMaxDraws) {
      return FlushPreparedDrawQueue(PreparedDrawFlushReason::kQueueBudget);
    }
    return true;
  }

  RecordPreparedDrawQueueReject(reject_reason);
  if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kQueueReject)) {
    RecyclePreparedDraw(draw);
    TryResetPreparedDrawPayloadArena();
    return false;
  }

  if (texture_cache_ &&
      draw->texture_materialization_plan.NeedsTextureUpload()) {
    texture_cache_->RefreshTextureMaterializationPlan(
        draw->texture_materialization_plan);
  }

  const size_t texture_source_range_count = std::min<size_t>(
      draw->texture_source_range_count, draw->materialization_ranges.size());
  const bool skip_texture_source_ranges =
      !draw->texture_materialization_plan.NeedsTextureUpload() &&
      draw->texture_materialization_plan.source_ranges.empty();
  const size_t materialization_range_start =
      skip_texture_source_ranges ? texture_source_range_count : 0;
  const uint32_t materialization_range_count = static_cast<uint32_t>(
      draw->materialization_ranges.size() - materialization_range_start);
  const SharedMemory::Range* materialization_ranges =
      materialization_range_count
          ? draw->materialization_ranges.data() + materialization_range_start
          : nullptr;
  const bool has_invalid_shared_memory =
      shared_memory_ && materialization_range_count &&
      AnySharedMemoryRangeInvalid(materialization_ranges,
                                  materialization_range_count);

  if (has_invalid_shared_memory && shared_memory_ && materialization_ranges) {
    PrepareSharedMemoryUploadBeforeDrawPass(materialization_ranges,
                                            materialization_range_count);
    if (!RequestSharedMemoryRanges(
            SharedMemoryRequestReason::kDrawMaterialization,
            materialization_ranges, materialization_range_count)) {
      XELOGE("Failed to request {} current-draw shared-memory ranges",
             materialization_range_count);
      RecyclePreparedDraw(draw);
      TryResetPreparedDrawPayloadArena();
      return false;
    }
    EndSharedMemoryUploadBlitEncoder(
        SharedMemoryUploadEncoderEndReason::kMaterializationDrain);
  }

  if (draw->texture_materialization_plan.NeedsTextureUpload() &&
      texture_cache_) {
    if (current_render_encoder_) {
      EndRenderEncoder(RenderEncoderEndReason::kTextureUploadBeforeDrawPass);
    }
    if (!EnsureCommandBuffer()) {
      RecyclePreparedDraw(draw);
      TryResetPreparedDrawPayloadArena();
      return false;
    }
    if (!texture_cache_->ExecuteTextureMaterialization(
            draw->texture_materialization_plan)) {
      RecyclePreparedDraw(draw);
      TryResetPreparedDrawPayloadArena();
      return false;
    }
  }

  const bool encoded = EncodePreparedDraw(*draw);
  RecyclePreparedDraw(draw);
  TryResetPreparedDrawPayloadArena();
  return encoded;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
