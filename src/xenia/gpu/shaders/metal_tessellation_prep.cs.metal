/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Prepares the per-draw buffers consumed by native Metal drawPatches +
// spirv2air TessellationEvaluation shaders:
// - Control point float buffer (spirv2air buffer(6)): consecutive float vertex
//   indices (p * N + k), endian-swapped, offset, masked to 24 bits, and
//   clamped to [min_index, max_index].
// - Adaptive tessellation factor buffer (when bit 4 of mode_flags is set):
//   MTLTriangleTessellationFactorsHalf (8 B/patch) or
//   MTLQuadTessellationFactorsHalf (12 B/patch), matching Xenia's
//   tessellation_adaptive.vs.slang + adaptive_{triangle,quad}.hs.slang.
//
// Discrete and continuous modes use MTL::TessellationFactorStepFunctionConstant
// with a single CPU-written factor struct, so only control points are filled
// here.

#include <metal_stdlib>
using namespace metal;

// mode_flags layout:
//   bits 0..1: index source (0 = auto-index, 1 = uint16, 2 = uint32)
//   bits 2..3: index endian (xenos::Endian: 0..3)
//   bit 4:     1 = adaptive (fill patch index + per-patch half factors)
//   bit 5:     1 = quad domain (4 edge factors), 0 = triangle domain (3)
struct TessPrepConstants {
  uint count;
  uint mode_flags;
  uint index_offset;
  uint min_index;
  uint max_index;
  uint src_byte_offset;
  float factor_min;
  float factor_max;
};

namespace {

constant constexpr uint kXenosEndian8in16 = 1u;
constant constexpr uint kXenosEndian8in32 = 2u;
constant constexpr uint kXenosEndian16in32 = 3u;

inline uint EndianSwap32(uint value, uint endian) {
  if (endian == kXenosEndian8in16 || endian == kXenosEndian8in32) {
    value = ((value & 0x00FF00FFu) << 8u) | ((value & 0xFF00FF00u) >> 8u);
  }
  if (endian == kXenosEndian8in32 || endian == kXenosEndian16in32) {
    value = (value << 16u) | (value >> 16u);
  }
  return value;
}

inline uint LoadUint32(const device uchar* buf, uint byte_offset) {
  return *reinterpret_cast<const device uint*>(buf + byte_offset);
}

}  // namespace

kernel void entry_xe(
    constant TessPrepConstants& params [[buffer(0)]],
    device float* control_points [[buffer(1)]],
    const device uchar* src_indices [[buffer(2)]],
    device uchar* dst_factors [[buffer(3)]],
    uint tid [[thread_position_in_grid]]) {
  if (tid >= params.count) {
    return;
  }

  const uint flags = params.mode_flags;
  const uint index_source = flags & 3u;
  const uint index_endian = (flags >> 2u) & 3u;
  const bool is_adaptive = ((flags >> 4u) & 1u) != 0u;
  const bool is_quad = ((flags >> 5u) & 1u) != 0u;

  if (!is_adaptive) {
    uint raw_index;
    if (index_source == 0u) {
      raw_index = tid;
    } else if (index_source == 1u) {
      const uint byte_pos = params.src_byte_offset + (tid << 1u);
      const uint word = LoadUint32(src_indices, byte_pos & ~3u);
      raw_index = (word >> ((byte_pos & 2u) << 3u)) & 0xFFFFu;
    } else {
      raw_index = LoadUint32(src_indices, params.src_byte_offset + (tid << 2u));
    }
    const uint swapped = EndianSwap32(raw_index, index_endian);
    const uint clamped = clamp((swapped + params.index_offset) & 0xFFFFFFu,
                               params.min_index, params.max_index);
    control_points[tid] = float(clamped);
    return;
  }

  // Adaptive tessellation is always patch-indexed (N = 1 control point per
  // patch).
  const uint patch_index = clamp((tid + params.index_offset) & 0xFFFFFFu,
                                 params.min_index, params.max_index);
  control_points[tid] = float(patch_index);

  if (!is_quad) {
    // Triangle patch: 3 float32 edge factors ->
    // MTLTriangleTessellationFactorsHalf.
    // Match adaptive_triangle.hs.slang: edges[i] = input[(i + 1) % 3],
    // inside = min(edges[0], edges[1], edges[2]).
    const uint src_base = params.src_byte_offset + tid * 12u;
    const float f0 = clamp(
        as_type<float>(EndianSwap32(LoadUint32(src_indices, src_base),
                                    index_endian)) +
            1.0f,
        params.factor_min, params.factor_max);
    const float f1 = clamp(
        as_type<float>(EndianSwap32(LoadUint32(src_indices, src_base + 4u),
                                    index_endian)) +
            1.0f,
        params.factor_min, params.factor_max);
    const float f2 = clamp(
        as_type<float>(EndianSwap32(LoadUint32(src_indices, src_base + 8u),
                                    index_endian)) +
            1.0f,
        params.factor_min, params.factor_max);
    const float e0 = f1;
    const float e1 = f2;
    const float e2 = f0;
    const float inside = min(min(e0, e1), e2);
    device MTLTriangleTessellationFactorsHalf& out =
        reinterpret_cast<device MTLTriangleTessellationFactorsHalf*>(
            dst_factors)[tid];
    out.edgeTessellationFactor[0] = half(e0);
    out.edgeTessellationFactor[1] = half(e1);
    out.edgeTessellationFactor[2] = half(e2);
    out.insideTessellationFactor = half(inside);
  } else {
    // Quad patch: 4 float32 edge factors -> MTLQuadTessellationFactorsHalf.
    // Match adaptive_quad.hs.slang: edges[i] = input[(i + 3) & 3],
    // inside[0] = min(edges[1], edges[3]), inside[1] = min(edges[0], edges[2]).
    const uint src_base = params.src_byte_offset + (tid << 4u);
    const float f0 = clamp(
        as_type<float>(EndianSwap32(LoadUint32(src_indices, src_base),
                                    index_endian)) +
            1.0f,
        params.factor_min, params.factor_max);
    const float f1 = clamp(
        as_type<float>(EndianSwap32(LoadUint32(src_indices, src_base + 4u),
                                    index_endian)) +
            1.0f,
        params.factor_min, params.factor_max);
    const float f2 = clamp(
        as_type<float>(EndianSwap32(LoadUint32(src_indices, src_base + 8u),
                                    index_endian)) +
            1.0f,
        params.factor_min, params.factor_max);
    const float f3 = clamp(
        as_type<float>(EndianSwap32(LoadUint32(src_indices, src_base + 12u),
                                    index_endian)) +
            1.0f,
        params.factor_min, params.factor_max);
    const float e0 = f3;
    const float e1 = f0;
    const float e2 = f1;
    const float e3 = f2;
    const float in0 = min(e1, e3);
    const float in1 = min(e0, e2);
    device MTLQuadTessellationFactorsHalf& out =
        reinterpret_cast<device MTLQuadTessellationFactorsHalf*>(
            dst_factors)[tid];
    out.edgeTessellationFactor[0] = half(e0);
    out.edgeTessellationFactor[1] = half(e1);
    out.edgeTessellationFactor[2] = half(e2);
    out.edgeTessellationFactor[3] = half(e3);
    out.insideTessellationFactor[0] = half(in0);
    out.insideTessellationFactor[1] = half(in1);
  }
}
