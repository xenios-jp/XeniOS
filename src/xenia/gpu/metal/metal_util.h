/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_UTIL_H_
#define XENIA_GPU_METAL_METAL_UTIL_H_

#include <chrono>
#include <cstddef>
#include <cstdint>

#include <dispatch/dispatch.h>

#include "third_party/metal-cpp/Foundation/Foundation.hpp"
#include "third_party/metal-cpp/Metal/Metal.hpp"
#include "xenia/base/logging.h"

namespace xe {
namespace gpu {
namespace metal {

inline int64_t GetSteadyTimeNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class ScopedAutoreleasePool {
 public:
  ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}
  ~ScopedAutoreleasePool() {
    if (pool_) {
      pool_->release();
    }
  }

  ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
  ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

 private:
  NS::AutoreleasePool* pool_;
};

inline MTL::Library* CreateLibraryFromEmbeddedMetallib(
    MTL::Device* device, const void* metallib_data, size_t metallib_size,
    const char* debug_name) {
  if (!device || !metallib_data || !metallib_size) {
    return nullptr;
  }

  NS::Error* error = nullptr;
  dispatch_data_t data = dispatch_data_create(
      metallib_data, metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* lib = device->newLibrary(data, &error);
  dispatch_release(data);
  if (!lib) {
    XELOGE("Metal: failed to create {} library: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }
  return lib;
}

inline MTL::ComputePipelineState* CreateComputePipelineFromEmbeddedLibrary(
    MTL::Device* device, const void* metallib_data, size_t metallib_size,
    const char* debug_name, const char* entry_point_name = "entry_xe") {
  MTL::Library* lib = CreateLibraryFromEmbeddedMetallib(
      device, metallib_data, metallib_size, debug_name);
  if (!lib) {
    return nullptr;
  }

  NS::Error* error = nullptr;
  NS::String* entry_name =
      NS::String::string(entry_point_name, NS::UTF8StringEncoding);
  MTL::Function* fn = lib->newFunction(entry_name);
  if (!fn) {
    XELOGE("Metal: failed to find entry '{}' in {} library", entry_point_name,
           debug_name);
    lib->release();
    return nullptr;
  }

  MTL::ComputePipelineState* pso = device->newComputePipelineState(fn, &error);
  fn->release();
  lib->release();
  if (!pso) {
    XELOGE("Metal: failed to create {} compute pipeline: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }
  return pso;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_UTIL_H_
