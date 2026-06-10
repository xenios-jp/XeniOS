/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_HEAP_POOL_H_
#define XENIA_GPU_METAL_METAL_HEAP_POOL_H_

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace xe {
namespace gpu {
namespace metal {

class MetalHeapPool {
 public:
  // hazard_tracking_mode: Tracked keeps driver hazard tracking for the heap
  // and everything suballocated from it; Untracked requires the owner to
  // order GPU accesses itself (backend hazard model phases,
  // docs/metal_hazard_model_design.md).
  MetalHeapPool(MTL::Device* device, MTL::StorageMode storage_mode,
                size_t min_heap_size, const char* label_prefix,
                MTL::HazardTrackingMode hazard_tracking_mode =
                    MTL::HazardTrackingModeTracked);
  ~MetalHeapPool();

  using HeapCreatedCallback = std::function<void(MTL::Heap*)>;
  void SetHeapCreatedCallback(HeapCreatedCallback callback);

  MTL::Texture* CreateTexture(MTL::TextureDescriptor* descriptor);
  void Shutdown();

 private:
  struct HeapEntry {
    MTL::Heap* heap = nullptr;
    size_t size = 0;
  };

  MTL::Heap* GetHeapForSize(size_t size, size_t alignment);

  MTL::Device* device_ = nullptr;
  MTL::StorageMode storage_mode_ = MTL::StorageModePrivate;
  MTL::HazardTrackingMode hazard_tracking_mode_ =
      MTL::HazardTrackingModeTracked;
  size_t min_heap_size_ = 0;
  size_t max_heap_bytes_ = 0;
  size_t total_heap_bytes_ = 0;
  std::string label_prefix_;
  std::vector<HeapEntry> heaps_;
  HeapCreatedCallback heap_created_callback_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_HEAP_POOL_H_
