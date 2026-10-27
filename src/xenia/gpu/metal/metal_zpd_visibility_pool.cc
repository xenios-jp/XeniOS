/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_zpd_visibility_pool.h"

#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace xe {
namespace gpu {
namespace metal {

namespace {

bool SupportsAccumulation(MTL::Device* device) {
  // The accumulate result type exists from macOS and iOS 26, and is only used
  // on Apple7 and newer. Others keep real queries through per-pass readback.
  if (__builtin_available(macOS 26.0, iOS 26.0, *)) {
    return device && device->supportsFamily(MTL::GPUFamilyApple7);
  }
  return false;
}

}  // namespace

bool MetalZPDVisibilityPool::EnsureInitialized(MTL::Device* device,
                                               uint32_t requested_capacity) {
  const bool supports_accumulation = SupportsAccumulation(device);
  if (is_initialized() && capacity_ == requested_capacity &&
      supports_accumulation == (readback_buffer_ == nullptr)) {
    return true;
  }

  if (is_initialized()) {
    // Can't resize while resolves may be in flight. Caller must ensure
    // the pool is idle before requesting a different capacity.
    assert_true(free_indices_.size() == capacity_);
    Shutdown();
  }

  if (requested_capacity == 0) {
    return false;
  }

  size_t buffer_size =
      static_cast<size_t>(requested_capacity) * sizeof(uint64_t);
  visibility_buffer_ =
      device->newBuffer(buffer_size, MTL::ResourceStorageModeShared);
  if (!visibility_buffer_) {
    XELOGW(
        "MetalZPDVisibilityPool: Failed to allocate the ZPD visibility buffer "
        "(size={}), falling back to fake sample counts.",
        buffer_size);
    return false;
  }
  visibility_buffer_->setLabel(
      NS::String::string("XeniaZPDVisibility", NS::UTF8StringEncoding));

  if (!supports_accumulation) {
    // With the reset result type, a new render encoder resets the results.
    // Keep the CPU-visible results separate so later passes cannot erase
    // segments that have not been read back yet.
    readback_buffer_ =
        device->newBuffer(buffer_size, MTL::ResourceStorageModeShared);
    if (!readback_buffer_) {
      Shutdown();
      return false;
    }
    readback_buffer_->setLabel(
        NS::String::string("XeniaZPDReadback", NS::UTF8StringEncoding));
  }
  visibility_mapping_ = reinterpret_cast<uint64_t*>(
      (readback_buffer_ ? readback_buffer_ : visibility_buffer_)->contents());
  std::memset(visibility_mapping_, 0, buffer_size);

  capacity_ = requested_capacity;

  free_indices_.clear();
  free_indices_.reserve(requested_capacity);
  for (uint32_t i = requested_capacity; i > 0; --i) {
    free_indices_.push_back(i - 1);
  }
  generations_.assign(requested_capacity, 0);
  failed_results_.assign(requested_capacity, false);

  return true;
}

void MetalZPDVisibilityPool::Shutdown() {
  free_indices_.clear();
  generations_.clear();
  render_pass_indices_.clear();
  failed_results_.clear();

  capacity_ = 0;
  visibility_mapping_ = nullptr;

  if (visibility_buffer_) {
    visibility_buffer_->release();
    visibility_buffer_ = nullptr;
  }
  if (readback_buffer_) {
    readback_buffer_->release();
    readback_buffer_ = nullptr;
  }
}

bool MetalZPDVisibilityPool::Acquire(uint32_t& index, uint32_t& generation,
                                     size_t& offset) {
  if (free_indices_.empty()) {
    index = UINT32_MAX;
    generation = 0;
    offset = 0;
    return false;
  }

  index = free_indices_.back();
  free_indices_.pop_back();

  assert_true(index < generations_.size());
  generation = generations_[index];
  offset = static_cast<size_t>(index) * sizeof(uint64_t);
  visibility_mapping_[index] = 0;
  failed_results_[index] = false;
  if (readback_buffer_) {
    render_pass_indices_.push_back(index);
  }
  return true;
}

void MetalZPDVisibilityPool::EndRenderPass(MTL::CommandBuffer* command_buffer) {
  if (render_pass_indices_.empty()) {
    return;
  }
  assert_not_null(readback_buffer_);
  auto* blit = command_buffer ? command_buffer->blitCommandEncoder() : nullptr;
  if (!blit) {
    XELOGE("MetalZPDVisibilityPool: Failed to preserve visibility results");
    for (uint32_t index : render_pass_indices_) {
      failed_results_[index] = true;
    }
    render_pass_indices_.clear();
    return;
  }
  for (uint32_t index : render_pass_indices_) {
    const size_t offset = size_t(index) * sizeof(uint64_t);
    blit->copyFromBuffer(visibility_buffer_, offset, readback_buffer_, offset,
                         sizeof(uint64_t));
  }
  blit->endEncoding();
  render_pass_indices_.clear();
}

void MetalZPDVisibilityPool::Release(uint32_t index, uint32_t generation) {
  if (index >= capacity_) {
    return;
  }

  if (!IsGenerationCurrent(index, generation)) {
    XELOGW("MetalZPDVisibilityPool: stale release index={} gen={}", index,
           generation);
    return;
  }

  ++generations_[index];
  free_indices_.push_back(index);
}

bool MetalZPDVisibilityPool::IsGenerationCurrent(uint32_t index,
                                                 uint32_t generation) const {
  return index < generations_.size() && generations_[index] == generation;
}

bool MetalZPDVisibilityPool::Read(uint32_t index, uint64_t& samples) const {
  if (index >= capacity_ || !visibility_mapping_ || failed_results_[index]) {
    return false;
  }
  samples = visibility_mapping_[index];
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
