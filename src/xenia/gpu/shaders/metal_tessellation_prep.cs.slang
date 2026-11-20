// XE_MSL_ONLY
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

#include "endian.xesli"

// mode_flags layout:
//   bits 0..1: index source (0 = auto-index, 1 = uint16, 2 = uint32)
//   bits 2..3: index endian (xenos::Endian: 0..3)
//   bit 4:     1 = adaptive (fill patch index + per-patch half factors)
//   bit 5:     1 = quad domain (4 edge factors), 0 = triangle domain (3)
push_const_begin_xe(b0, space0)
  uint xe_tess_prep_count;
  uint xe_tess_prep_mode_flags;
  uint xe_tess_prep_index_offset;
  uint xe_tess_prep_min_index;
  uint xe_tess_prep_max_index;
  uint xe_tess_prep_src_byte_offset;
  float xe_tess_prep_factor_min;
  float xe_tess_prep_factor_max;
push_const_end_xe

byte_buffer_align4_wo_declare_xe(xe_tess_control_points, 0, 0, u0, space0, 1)
byte_buffer_align4_declare_xe(xe_tess_indices, 0, 1, t0, space0, 2)
byte_buffer_align4_wo_declare_xe(xe_tess_factors, 0, 2, u1, space0, 3)

#define LOCAL_SIZE_X_XE 64
#define LOCAL_SIZE_Y_XE 1
#define LOCAL_SIZE_Z_XE 1
entry_bindings_begin_compute_xe
  push_const_binding_xe(buffer(0))
  entry_binding_next_xe
  byte_buffer_wo_binding_xe(xe_tess_control_points, buffer(1))
  entry_binding_next_xe
  byte_buffer_binding_xe(xe_tess_indices, buffer(2))
  entry_binding_next_xe
  byte_buffer_wo_binding_xe(xe_tess_factors, buffer(3))
entry_bindings_end_inputs_begin_compute_xe
  entry_in_global_thread_id_xe
entry_inputs_end_code_begin_compute_xe
{
  uint tid = in_global_thread_id_xe.x;
  if (tid >= push_const_xe(xe_tess_prep_count)) {
    return;
  }

  uint flags = push_const_xe(xe_tess_prep_mode_flags);
  uint index_source = flags & 3u;
  uint index_endian = (flags >> 2u) & 3u;
  bool is_adaptive = ((flags >> 4u) & 1u) != 0u;
  bool is_quad = ((flags >> 5u) & 1u) != 0u;

  uint index_offset = push_const_xe(xe_tess_prep_index_offset);
  uint min_index = push_const_xe(xe_tess_prep_min_index);
  uint max_index = push_const_xe(xe_tess_prep_max_index);
  uint src_byte_offset = push_const_xe(xe_tess_prep_src_byte_offset);

  if (!is_adaptive) {
    uint raw_index;
    if (index_source == 0u) {
      raw_index = tid;
    } else if (index_source == 1u) {
      uint byte_pos = src_byte_offset + (tid << 1u);
      uint word = byte_buffer_align4_load4_xe(xe_tess_indices, byte_pos & ~3u);
      raw_index = (word >> ((byte_pos & 2u) << 3u)) & 0xFFFFu;
    } else {
      raw_index = byte_buffer_align4_load4_xe(
          xe_tess_indices, src_byte_offset + (tid << 2u));
    }
    uint swapped = XeEndianSwap32(raw_index, index_endian);
    uint clamped = clamp((swapped + index_offset) & 0xFFFFFFu, min_index,
                         max_index);
    byte_buffer_align4_store4_xe(xe_tess_control_points, tid << 2u,
                                 float_bits_to_uint_xe(float(clamped)));
    return;
  }

  // Adaptive tessellation is always patch-indexed (N = 1 control point per
  // patch).
  uint patch_index = clamp((tid + index_offset) & 0xFFFFFFu, min_index,
                           max_index);
  byte_buffer_align4_store4_xe(xe_tess_control_points, tid << 2u,
                               float_bits_to_uint_xe(float(patch_index)));

  float factor_min = push_const_xe(xe_tess_prep_factor_min);
  float factor_max = push_const_xe(xe_tess_prep_factor_max);

  if (!is_quad) {
    // Triangle patch: 3 float32 edge factors -> MTLTriangleTessellationFactorsHalf.
    // Match adaptive_triangle.hs.slang: edges[i] = input[(i + 1) % 3],
    // inside = min(edges[0], edges[1], edges[2]).
    uint src_base = src_byte_offset + tid * 12u;
    float f0 = clamp(
        uint_bits_to_float_xe(XeEndianSwap32(
            byte_buffer_align4_load4_xe(xe_tess_indices, src_base),
            index_endian)) +
            1.0f,
        factor_min, factor_max);
    float f1 = clamp(
        uint_bits_to_float_xe(XeEndianSwap32(
            byte_buffer_align4_load4_xe(xe_tess_indices, src_base + 4u),
            index_endian)) +
            1.0f,
        factor_min, factor_max);
    float f2 = clamp(
        uint_bits_to_float_xe(XeEndianSwap32(
            byte_buffer_align4_load4_xe(xe_tess_indices, src_base + 8u),
            index_endian)) +
            1.0f,
        factor_min, factor_max);
    float e0 = f1;
    float e1 = f2;
    float e2 = f0;
    float inside = min(min(e0, e1), e2);
    uint dst_base = tid << 3u;
    byte_buffer_align4_store4_xe(xe_tess_factors, dst_base,
                                 pack_half_2x16_xe(float2_xe(e0, e1)));
    byte_buffer_align4_store4_xe(xe_tess_factors, dst_base + 4u,
                                 pack_half_2x16_xe(float2_xe(e2, inside)));
  } else {
    // Quad patch: 4 float32 edge factors -> MTLQuadTessellationFactorsHalf.
    // Match adaptive_quad.hs.slang: edges[i] = input[(i + 3) & 3],
    // inside[0] = min(edges[1], edges[3]), inside[1] = min(edges[0], edges[2]).
    uint src_base = src_byte_offset + (tid << 4u);
    float f0 = clamp(
        uint_bits_to_float_xe(XeEndianSwap32(
            byte_buffer_align4_load4_xe(xe_tess_indices, src_base),
            index_endian)) +
            1.0f,
        factor_min, factor_max);
    float f1 = clamp(
        uint_bits_to_float_xe(XeEndianSwap32(
            byte_buffer_align4_load4_xe(xe_tess_indices, src_base + 4u),
            index_endian)) +
            1.0f,
        factor_min, factor_max);
    float f2 = clamp(
        uint_bits_to_float_xe(XeEndianSwap32(
            byte_buffer_align4_load4_xe(xe_tess_indices, src_base + 8u),
            index_endian)) +
            1.0f,
        factor_min, factor_max);
    float f3 = clamp(
        uint_bits_to_float_xe(XeEndianSwap32(
            byte_buffer_align4_load4_xe(xe_tess_indices, src_base + 12u),
            index_endian)) +
            1.0f,
        factor_min, factor_max);
    float e0 = f3;
    float e1 = f0;
    float e2 = f1;
    float e3 = f2;
    float in0 = min(e1, e3);
    float in1 = min(e0, e2);
    uint dst_base = tid * 12u;
    byte_buffer_align4_store4_xe(xe_tess_factors, dst_base,
                                 pack_half_2x16_xe(float2_xe(e0, e1)));
    byte_buffer_align4_store4_xe(xe_tess_factors, dst_base + 4u,
                                 pack_half_2x16_xe(float2_xe(e2, e3)));
    byte_buffer_align4_store4_xe(xe_tess_factors, dst_base + 8u,
                                 pack_half_2x16_xe(float2_xe(in0, in1)));
  }
}
entry_code_end_compute_xe
