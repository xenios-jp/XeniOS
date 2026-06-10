/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shared_memory.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/metal/metal_command_processor.h"

DEFINE_bool(
    metal_shared_memory_direct_write, true,
    "Directly copy safe Metal shared-memory uploads into the shared storage "
    "buffer instead of staging them through a blit.",
    "Metal");

namespace xe {
namespace gpu {
namespace metal {

MetalSharedMemory::MetalSharedMemory(MetalCommandProcessor& command_processor,
                                     Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

MetalSharedMemory::~MetalSharedMemory() { Shutdown(); }

bool MetalSharedMemory::Initialize() {
  if (!InitializeCommon()) {
    return false;
  }

  const ui::metal::MetalProvider& provider =
      command_processor_.GetMetalProvider();
  MTL::Device* device = provider.GetDevice();

  if (!device) {
    XELOGE("Metal device is null in MetalSharedMemory::Initialize");
    return false;
  }

  // Create Metal buffer - similar to D3D12's approach
  // On Apple Silicon, ResourceStorageModeShared gives CPU/GPU access
  void* xbox_ram = memory().TranslatePhysical(0);
  if (!xbox_ram) {
    XELOGE("Metal shared memory: Xbox RAM is null");
    return false;
  }

  buffer_ = device->newBuffer(kBufferSize, MTL::ResourceStorageModeShared);
  if (!buffer_) {
    XELOGE("Failed to create Metal shared memory buffer");
    return false;
  }

  upload_buffer_pool_ = std::make_unique<MetalUploadBufferPool>(
      device, xe::align(ui::GraphicsUploadBufferPool::kDefaultPageSize,
                        size_t(1) << page_size_log2()));
  page_last_main_gpu_access_submission_.assign(
      kBufferSize >> page_size_log2(), 0);
  page_standalone_gpu_access_counts_.assign(kBufferSize >> page_size_log2(),
                                            0);

  return true;
}

void MetalSharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  if (upload_buffer_pool_) {
    upload_buffer_pool_->ClearCache();
  }
}

void MetalSharedMemory::MarkGpuAccess(uint32_t start, uint32_t length,
                                      uint64_t submission_index) {
  if (!length || !submission_index ||
      page_last_main_gpu_access_submission_.empty()) {
    return;
  }
  if (start >= kBufferSize) {
    return;
  }
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  if (end <= start) {
    return;
  }
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  page_last = std::min<uint32_t>(
      page_last,
      static_cast<uint32_t>(page_last_main_gpu_access_submission_.size() - 1));
  for (uint32_t page = page_first; page <= page_last; ++page) {
    page_last_main_gpu_access_submission_[page] =
        std::max(page_last_main_gpu_access_submission_[page],
                 submission_index);
  }
}

void MetalSharedMemory::TrackStandaloneGpuAccess(
    MTL::CommandBuffer* command_buffer,
    const std::pair<uint32_t, uint32_t>* ranges, uint32_t range_count) {
  if (!command_buffer || !ranges || !range_count ||
      page_standalone_gpu_access_counts_.empty()) {
    return;
  }
  auto tracked_page_ranges =
      std::make_shared<std::vector<std::pair<uint32_t, uint32_t>>>();
  tracked_page_ranges->reserve(range_count);
  for (uint32_t i = 0; i < range_count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length || start >= kBufferSize) {
      continue;
    }
    uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
    if (end <= start) {
      continue;
    }
    uint32_t page_first = start >> page_size_log2();
    uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
    page_last = std::min<uint32_t>(
        page_last,
        static_cast<uint32_t>(page_standalone_gpu_access_counts_.size() - 1));
    tracked_page_ranges->push_back({page_first, page_last});
  }
  if (tracked_page_ranges->empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
    for (const auto& page_range : *tracked_page_ranges) {
      for (uint32_t page = page_range.first; page <= page_range.second;
           ++page) {
        ++page_standalone_gpu_access_counts_[page];
      }
    }
  }
  MetalSharedMemory* shared_memory = this;
  command_buffer->addCompletedHandler(^(MTL::CommandBuffer*) {
    std::lock_guard<std::mutex> lock(
        shared_memory->standalone_gpu_access_mutex_);
    for (const auto& page_range : *tracked_page_ranges) {
      for (uint32_t page = page_range.first; page <= page_range.second;
           ++page) {
        if (page < shared_memory->page_standalone_gpu_access_counts_.size() &&
            shared_memory->page_standalone_gpu_access_counts_[page]) {
          --shared_memory->page_standalone_gpu_access_counts_[page];
        }
      }
    }
  });
}

MetalSharedMemory::UploadRouteInfo MetalSharedMemory::GetUploadRouteInfo(
    const SharedMemory::Range* ranges, uint32_t range_count) {
  UploadRouteInfo info;
  if (!ranges || !range_count) {
    return info;
  }

  const uint32_t page_size = 1u << page_size_log2();
  const uint64_t completed_submission =
      command_processor_.GetCompletedSubmission();
  const bool direct_write_enabled =
      ::cvars::metal_shared_memory_direct_write && buffer_ &&
      buffer_->contents() != nullptr;

  std::lock_guard<std::mutex> standalone_lock(standalone_gpu_access_mutex_);
  for (uint32_t i = 0; i < range_count; ++i) {
    const SharedMemory::Range& range = ranges[i];
    if (!range.length || range.start >= kBufferSize) {
      continue;
    }
    uint64_t end =
        std::min(uint64_t(range.start) + range.length, uint64_t(kBufferSize));
    if (end <= range.start) {
      continue;
    }
    uint32_t page_first = range.start >> page_size_log2();
    uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
    for (uint32_t page = page_first; page <= page_last; ++page) {
      uint64_t page_start = uint64_t(page) << page_size_log2();
      uint32_t page_length = static_cast<uint32_t>(
          std::min<uint64_t>(page_size, uint64_t(kBufferSize) - page_start));
      if (!page_length ||
          IsRangeValid(static_cast<uint32_t>(page_start), page_length)) {
        continue;
      }

      info.upload_bytes += page_length;
      bool direct_safe = direct_write_enabled;
      if (page < page_standalone_gpu_access_counts_.size() &&
          page_standalone_gpu_access_counts_[page]) {
        direct_safe = false;
      }
      uint64_t page_last_access = 0;
      if (page < page_last_main_gpu_access_submission_.size()) {
        page_last_access = page_last_main_gpu_access_submission_[page];
      }
      if (page_last_access > completed_submission) {
        direct_safe = false;
      }
      if (direct_safe) {
        info.direct_bytes += page_length;
      } else {
        info.staged_bytes += page_length;
      }
    }
  }
  return info;
}

bool MetalSharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  static bool first_upload = true;
  if (first_upload) {
    first_upload = false;
    const uint32_t page_size = 1u << page_size_log2();
    XELOGD("MetalSharedMemory::UploadRanges: page_size={}, {} ranges to upload",
           page_size, num_upload_ranges);
    for (uint32_t i = 0; i < std::min(5u, num_upload_ranges); i++) {
      uint32_t start_byte = upload_page_ranges[i].first * page_size;
      uint32_t length_bytes = upload_page_ranges[i].second * page_size;
      XELOGD("  Range[{}]: page={} count={} -> byte offset=0x{:08X} length={}",
             i, upload_page_ranges[i].first, upload_page_ranges[i].second,
             start_byte, length_bytes);
    }
  }

  if (!buffer_ || num_upload_ranges == 0) {
    return true;
  }

  void* xbox_ram = memory().TranslatePhysical(0);
  if (!xbox_ram) {
    XELOGE("MetalSharedMemory::UploadRanges: Xbox RAM is null");
    return false;
  }
  uint8_t* xbox_data = static_cast<uint8_t*>(xbox_ram);

  const uint32_t page_size = 1u << page_size_log2();
  if (!upload_buffer_pool_) {
    XELOGE("MetalSharedMemory::UploadRanges: upload buffer pool is null");
    return false;
  }
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());

  MTL::BlitCommandEncoder* blit_encoder = nullptr;
  auto get_blit_encoder = [&]() -> MTL::BlitCommandEncoder* {
    if (!blit_encoder) {
      blit_encoder = command_processor_.GetSharedMemoryUploadBlitEncoder();
    }
    return blit_encoder;
  };

  uint32_t merged_start = 0;
  uint32_t merged_end = 0;
  bool have_merged = false;

  auto copy_guest_bytes = [&](uint8_t* dest, uint32_t offset, size_t size) {
    if (size < (1ULL << 32) && size > 8192) {
      memory::vastcpy(dest, xbox_data + offset, static_cast<uint32_t>(size));
      swcache::WriteFence();
    } else {
      std::memcpy(dest, xbox_data + offset, size);
    }
  };

  uint64_t direct_eligible_bytes = 0;
  uint64_t staged_required_bytes = 0;
  uint8_t* shared_buffer_contents = static_cast<uint8_t*>(buffer_->contents());
  const bool has_shared_buffer_contents = shared_buffer_contents != nullptr;
  const uint64_t completed_submission =
      command_processor_.GetCompletedSubmission();

  auto record_direct_write_reject =
      [&](MetalCommandProcessor::SharedMemoryDirectWriteRejectReason reason,
          uint64_t bytes) {
        command_processor_.RecordSharedMemoryDirectWriteReject(reason, bytes);
      };

  struct UploadRun {
    uint32_t start;
    uint32_t end;
    bool direct_safe;
  };

  auto append_upload_run = [](std::vector<UploadRun>& runs, uint32_t start,
                              uint32_t end, bool direct_safe) {
    if (end <= start) {
      return;
    }
    if (!runs.empty() && runs.back().direct_safe == direct_safe &&
        runs.back().end == start) {
      runs.back().end = end;
      return;
    }
    runs.push_back({start, end, direct_safe});
  };

  auto build_upload_runs = [&](uint32_t start, uint32_t end,
                               std::vector<UploadRun>& runs) {
    if (end <= start) {
      return;
    }
    if (!has_shared_buffer_contents) {
      uint64_t bytes = uint64_t(end) - start;
      staged_required_bytes += bytes;
      record_direct_write_reject(
          MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
              kNoSharedBufferContents,
          bytes);
      append_upload_run(runs, start, end, false);
      return;
    }
    bool saw_direct = false;
    bool saw_staged = false;
    uint32_t page_first = start >> page_size_log2();
    uint32_t page_last = (end - 1) >> page_size_log2();
    std::lock_guard<std::mutex> standalone_lock(standalone_gpu_access_mutex_);
    for (uint32_t page = page_first; page <= page_last; ++page) {
      uint64_t page_start = uint64_t(page) << page_size_log2();
      uint64_t page_end = page_start + page_size;
      uint64_t byte_start = std::max<uint64_t>(page_start, start);
      uint64_t byte_end = std::min<uint64_t>(page_end, end);
      uint64_t bytes = byte_end - byte_start;
      if (!bytes) {
        continue;
      }

      if (page < page_standalone_gpu_access_counts_.size() &&
          page_standalone_gpu_access_counts_[page]) {
        staged_required_bytes += bytes;
        saw_staged = true;
        append_upload_run(runs, static_cast<uint32_t>(byte_start),
                          static_cast<uint32_t>(byte_end), false);
        record_direct_write_reject(
            MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
                kStandaloneAccessInFlight,
            bytes);
        continue;
      }

      uint64_t page_last_access = 0;
      if (page < page_last_main_gpu_access_submission_.size()) {
        page_last_access = page_last_main_gpu_access_submission_[page];
      }
      if (page_last_access > completed_submission) {
        staged_required_bytes += bytes;
        saw_staged = true;
        append_upload_run(runs, static_cast<uint32_t>(byte_start),
                          static_cast<uint32_t>(byte_end), false);
        record_direct_write_reject(
            MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
                kMainGpuAccessInFlight,
            bytes);
        continue;
      }

      direct_eligible_bytes += bytes;
      saw_direct = true;
      append_upload_run(runs, static_cast<uint32_t>(byte_start),
                        static_cast<uint32_t>(byte_end), true);
    }

    if (saw_direct && saw_staged) {
      record_direct_write_reject(
          MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
              kMixedRangeSplit,
          uint64_t(end) - start);
    }
  };

  auto direct_write_run = [&](uint32_t start, uint32_t end) {
    uint32_t size = end - start;
    MakeRangeValid(start, size, false);
    copy_guest_bytes(shared_buffer_contents + start, start, size);
    command_processor_.RecordSharedMemoryUploadRoute(
        MetalCommandProcessor::SharedMemoryUploadRoute::kDirectWrite, size);
  };

  auto stage_upload_run = [&](uint32_t start, uint32_t end) -> bool {
    if (end <= start) {
      return true;
    }
    uint32_t offset = start;
    uint32_t remaining = end - start;
    while (remaining) {
      MTL::BlitCommandEncoder* encoder = get_blit_encoder();
      if (!encoder) {
        XELOGE("MetalSharedMemory::UploadRanges: failed to get blit encoder");
        return false;
      }

      MTL::Buffer* upload_buffer = nullptr;
      size_t upload_offset = 0;
      uint64_t upload_gpu_address = 0;
      size_t upload_size = 0;
      uint8_t* upload_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), remaining, page_size,
          &upload_buffer, upload_offset, upload_gpu_address, upload_size);
      if (!upload_mapping || !upload_buffer || !upload_size) {
        XELOGE(
            "MetalSharedMemory::UploadRanges: failed to allocate upload "
            "staging buffer");
        return false;
      }

      MakeRangeValid(offset, static_cast<uint32_t>(upload_size), false);
      copy_guest_bytes(upload_mapping, offset, upload_size);
      encoder->copyFromBuffer(upload_buffer,
                              static_cast<NS::UInteger>(upload_offset), buffer_,
                              static_cast<NS::UInteger>(offset),
                              static_cast<NS::UInteger>(upload_size));
      command_processor_.RecordSharedMemoryUploadEncoderCopy();
      command_processor_.RecordSharedMemoryUploadRoute(
          MetalCommandProcessor::SharedMemoryUploadRoute::kStagedBlit,
          upload_size);

      offset += static_cast<uint32_t>(upload_size);
      remaining -= static_cast<uint32_t>(upload_size);
    }
    return true;
  };

  auto flush_merged_range = [&](uint32_t start, uint32_t end) -> bool {
    if (end <= start) {
      return true;
    }
    std::vector<UploadRun> upload_runs;
    build_upload_runs(start, end, upload_runs);
    for (const UploadRun& run : upload_runs) {
      if (::cvars::metal_shared_memory_direct_write && run.direct_safe) {
        direct_write_run(run.start, run.end);
      } else if (!stage_upload_run(run.start, run.end)) {
        return false;
      }
    }
    return true;
  };

  for (uint32_t i = 0; i < num_upload_ranges; ++i) {
    const auto& range = upload_page_ranges[i];
    uint32_t start = range.first * page_size;
    uint32_t end = start + range.second * page_size;
    if (start >= kBufferSize) {
      continue;
    }
    if (end > kBufferSize) {
      end = kBufferSize;
    }

    if (!have_merged) {
      merged_start = start;
      merged_end = end;
      have_merged = true;
      continue;
    }

    // Merge overlapping/adjacent ranges.
    if (start <= merged_end) {
      if (end > merged_end) {
        merged_end = end;
      }
    } else {
      if (!flush_merged_range(merged_start, merged_end)) {
        command_processor_.EndSharedMemoryUploadBlitEncoder(
            MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
                kUploadFailure);
        return false;
      }
      merged_start = start;
      merged_end = end;
    }
  }

  if (have_merged) {
    if (!flush_merged_range(merged_start, merged_end)) {
      command_processor_.EndSharedMemoryUploadBlitEncoder(
          MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
              kUploadFailure);
      return false;
    }
  }

  XELOGD("MetalSharedMemory::UploadRanges: Staged {} ranges to Metal buffer",
         num_upload_ranges);
  command_processor_.RecordSharedMemoryDirectWriteEligibility(
      direct_eligible_bytes, staged_required_bytes);

  return true;
}

void MetalSharedMemory::Shutdown() {
  upload_buffer_pool_.reset();
  if (buffer_) {
    buffer_->release();
    buffer_ = nullptr;
  }

  ShutdownCommon();  // Base class cleanup
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
