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

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_command_processor.h"

namespace xe {
namespace gpu {
namespace metal {

MetalSharedMemory::MetalSharedMemory(MetalCommandProcessor& command_processor,
                                     Memory& memory, TraceWriter& trace_writer)
    : SharedMemory(memory),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {}

MetalSharedMemory::~MetalSharedMemory() { Shutdown(); }

bool MetalSharedMemory::Initialize() {
  // Try to alias guest memory on unified-memory devices and fall back to a
  // dedicated shared buffer when not supported.
  // Initialize base class
  InitializeCommon();

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

  const MTL::ResourceOptions buffer_options =
      MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked;
  hazards_ = std::make_unique<MetalBufferHazards>(device, kBufferSize);

  if (cvars::shared_memory_zero_copy) {
    size_t system_page_size = xe::memory::page_size();
    if (reinterpret_cast<uintptr_t>(xbox_ram) % system_page_size == 0) {
      buffer_ =
          device->newBuffer(xbox_ram, kBufferSize, buffer_options, nullptr);
      if (buffer_) {
        use_zero_copy_ = true;
        XELOGD("Metal shared memory: using bytes-no-copy buffer");
      } else {
        XELOGW("Metal shared memory: bytes-no-copy buffer creation failed");
      }
    } else {
      XELOGW(
          "Metal shared memory: Xbox RAM not page-aligned for bytes-no-copy");
    }
  }

  if (!buffer_) {
    buffer_ = device->newBuffer(kBufferSize, buffer_options);
  }
  if (!buffer_) {
    XELOGE("Failed to create Metal shared memory buffer");
    return false;
  }

  // For trace dump, do initial full copy; UploadRanges handles incremental
  // updates for normal runs.
  if (!use_zero_copy_) {
    if (xbox_ram) {
      memcpy(buffer_->contents(), xbox_ram, kBufferSize);
    }
  } else {
    XELOGD("Metal shared memory: skipping initial copy (zero-copy)");
  }

  return true;
}

void MetalSharedMemory::ClearCache() { SharedMemory::ClearCache(); }

bool MetalSharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  SCOPE_profile_cpu_f("gpu");
  // Copy modified ranges from Xbox memory to Metal buffer when not using
  // bytes-no-copy shared memory.
  if (!buffer_ || num_upload_ranges == 0) {
    return true;
  }

  uint8_t* xbox_data = nullptr;
  if (!use_zero_copy_) {
    void* xbox_ram = memory().TranslatePhysical(0);
    if (!xbox_ram) {
      XELOGE("MetalSharedMemory::UploadRanges: Xbox RAM is null");
      return false;
    }
    xbox_data = static_cast<uint8_t*>(xbox_ram);
  }

  const uint32_t page_size = 1u << page_size_log2();

  uint32_t merged_start = 0;
  uint32_t merged_end = 0;
  bool have_merged = false;

  auto flush_merged_range = [&](uint32_t start, uint32_t end) -> bool {
    if (end <= start) {
      return true;
    }
    uint32_t length = end - start;
    // Draws already encoded in the open command buffer may still read the old
    // contents, so the copy is encoded after them instead of made on the CPU.
    if (!use_zero_copy_ &&
        !CopyToBufferGpuOrdered(start, xbox_data + start, length)) {
      return false;
    }
    MakeRangeValid(start, length, false);
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
    trace_writer_.WriteMemoryRead(start, end - start);

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
        return false;
      }
      merged_start = start;
      merged_end = end;
    }
  }

  return !have_merged || flush_merged_range(merged_start, merged_end);
}

bool MetalSharedMemory::CopyToBufferGpuOrdered(uint32_t start, const void* data,
                                               uint32_t length,
                                               bool order_all_accesses) {
  MTL::CommandBuffer* command_buffer = command_processor_.EnsureCommandBuffer();
  if (!command_buffer) {
    return false;
  }
  command_processor_.EndEncodersForCommandBuffer(command_buffer);
  MTL::BlitCommandEncoder* encoder = command_buffer->blitCommandEncoder();
  if (!encoder) {
    return false;
  }
  MetalBufferHazards::Encoder hazards;
  if (hazards_) {
    hazards.Begin(*hazards_, command_buffer, encoder);
    if (order_all_accesses) {
      // Preserve the ordering of a guest fence publication, but track only
      // the bytes it writes, not a write to the whole buffer.
      hazards.OrderPreviousAccesses();
    }
  }
  // The source is in the command buffer's argument buffer pages, which are
  // recycled only after the command buffer completes.
  constexpr uint32_t kMaxCopyLength = UINT32_C(1) << 20;
  const uint8_t* source = static_cast<const uint8_t*>(data);
  bool copied = true;
  while (length) {
    uint32_t copy_length = std::min(length, kMaxCopyLength);
    MTL::Buffer* source_buffer = nullptr;
    NS::UInteger source_offset = 0;
    if (!command_processor_.AcquireSpirvArgumentBufferSlice(
            copy_length, 16, &source_buffer, &source_offset)) {
      copied = false;
      break;
    }
    std::memcpy(
        static_cast<uint8_t*>(source_buffer->contents()) + source_offset,
        source, copy_length);
    if (hazards.is_active()) {
      hazards.Access(start, copy_length, true);
    }
    encoder->copyFromBuffer(source_buffer, source_offset, buffer_, start,
                            copy_length);
    start += copy_length;
    source += copy_length;
    length -= copy_length;
  }
  hazards.End();
  encoder->endEncoding();
  return copied;
}

bool MetalSharedMemory::WriteGuestMemoryGpuOrdered(uint32_t start,
                                                   const void* data,
                                                   uint32_t length) {
  if (!use_zero_copy_ || !buffer_ || !data || !length || start >= kBufferSize ||
      length > kBufferSize - start || ((start | length) & 3) ||
      !RequestRange(start, length) ||
      !CopyToBufferGpuOrdered(start, data, length, true)) {
    return false;
  }
  RangeWrittenByGpu(start, length);
  return true;
}

bool MetalSharedMemory::InitializeTraceSubmitDownloads() {
  PrepareForTraceDownload();
  return trace_download_page_count() != 0;
}

void MetalSharedMemory::InitializeTraceCompleteDownloads() {
  if (buffer_) {
    const uint8_t* buffer_data =
        static_cast<const uint8_t*>(buffer_->contents());
    for (const auto& download_range : trace_download_ranges()) {
      trace_writer_.WriteMemoryRead(download_range.first, download_range.second,
                                    buffer_data + download_range.first);
    }
  }
  ReleaseTraceDownloadRanges();
}

void MetalSharedMemory::Shutdown() {
  hazards_.reset();
  if (buffer_) {
    buffer_->release();
    buffer_ = nullptr;
  }
  use_zero_copy_ = false;

  ShutdownCommon();  // Base class cleanup
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
