/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_
#define XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_

// Metal shared memory keeps a Metal-owned guest-memory buffer and updates dirty
// guest ranges through submission-owned upload staging buffers.

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "xenia/gpu/metal/metal_upload_buffer_pool.h"
#include "xenia/gpu/shared_memory.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;
class MetalSharedMemory : public SharedMemory {
 public:
  MetalSharedMemory(MetalCommandProcessor& command_processor, Memory& memory);
  ~MetalSharedMemory() override;
  bool Initialize();
  void Shutdown();
  void ClearCache() override;

  MTL::Buffer* GetBuffer() const { return buffer_; }
  const uint8_t* GetXboxRamBase() const {
    return static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  }
  void MarkGpuAccess(uint32_t start, uint32_t length,
                     uint64_t submission_index);
  void TrackStandaloneGpuAccess(
      MTL::CommandBuffer* command_buffer,
      const std::pair<uint32_t, uint32_t>* ranges, uint32_t range_count);
  struct UploadRouteInfo {
    uint64_t upload_bytes = 0;
    uint64_t direct_bytes = 0;
    uint64_t staged_bytes = 0;
  };
  UploadRouteInfo GetUploadRouteInfo(const SharedMemory::Range* ranges,
                                      uint32_t range_count);

  // For trace dump, simplified - just make buffer available for reading
  void UseForReading() {
    // No state transitions needed in Metal
  }
  // Override pure virtual function from SharedMemory
  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_upload_ranges) override;

 private:
  MetalCommandProcessor& command_processor_;
  std::unique_ptr<MetalUploadBufferPool> upload_buffer_pool_;
  std::vector<uint64_t> page_last_main_gpu_access_submission_;
  std::mutex standalone_gpu_access_mutex_;
  std::vector<uint32_t> page_standalone_gpu_access_counts_;
  MTL::Buffer* buffer_ = nullptr;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif
