/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_geometry_shader.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace xe {
namespace gpu {
namespace metal {

// Metal-local cache of generated DXBC geometry shaders for the helper MSC path.
static std::unordered_map<GeometryShaderKey, std::vector<uint32_t>,
                          GeometryShaderKey::Hasher>
    geometry_shaders_;

const std::vector<uint32_t>& GetGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }
  std::vector<uint32_t> shader;
  CreateDxbcGeometryShader(key, shader);
  return geometry_shaders_.emplace(key, std::move(shader)).first->second;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
