/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_BACKEND_TELEMETRY_H_
#define XENIA_GPU_METAL_METAL_BACKEND_TELEMETRY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace xe {
namespace gpu {
namespace metal {

using MetalTelemetryNameCallback = const char* (*)(size_t);

const char* MetalRenderEncoderEndReasonName(size_t reason);
const char* MetalTransferRequestSourceName(size_t source);
const char* MetalSharedMemoryRequestReasonName(size_t reason);
const char* MetalSharedMemoryRequestOutcomeName(size_t outcome);
const char* MetalSharedMemoryUploadRouteName(size_t route);
const char* MetalSharedMemoryDirectWriteRejectReasonName(size_t reason);
const char* MetalSharedMemoryUploadEncoderEndReasonName(size_t reason);
const char* MetalTextureUploadSourceRouteName(size_t route);
const char* MetalTextureUploadSourceFallbackReasonName(size_t reason);
const char* MetalTextureUploadCompatibilityClassName(size_t type);
const char* MetalTextureUploadComputeBlockerName(size_t blocker);
const char* MetalTextureUploadExecutionDetailName(size_t detail);
const char* MetalTextureReloadReasonName(size_t reason);
const char* MetalTextureWatchInvalidationReasonName(size_t reason);
const char* MetalTextureResolveReloadReasonName(size_t reason);

std::string MetalFormatNamedCounts(const uint64_t* values, size_t count,
                                   MetalTelemetryNameCallback name_callback);
std::string MetalFormatNamedTriplets(const uint64_t* total,
                                     const uint64_t* active,
                                     const uint64_t* no_active, size_t count,
                                     MetalTelemetryNameCallback name_callback);

template <size_t Count>
std::string MetalFormatNamedCounts(const std::array<uint64_t, Count>& values,
                                   MetalTelemetryNameCallback name_callback) {
  return MetalFormatNamedCounts(values.data(), values.size(), name_callback);
}

template <size_t Count>
std::string MetalFormatNamedTriplets(
    const std::array<uint64_t, Count>& total,
    const std::array<uint64_t, Count>& active,
    const std::array<uint64_t, Count>& no_active,
    MetalTelemetryNameCallback name_callback) {
  return MetalFormatNamedTriplets(total.data(), active.data(), no_active.data(),
                                  total.size(), name_callback);
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_BACKEND_TELEMETRY_H_
