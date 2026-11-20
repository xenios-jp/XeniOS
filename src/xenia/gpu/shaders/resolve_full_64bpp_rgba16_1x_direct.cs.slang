// Exact format-5 (16_16_16_16) source, one sample, 16_16_16_16 destination,
// read straight from the 1x host render target instead of the EDRAM buffer.
// XE_MSL_ONLY
#define XE_RESOLVE_DIRECT_REFRESH_64BPP 1
#include "resolve_direct.slang"

entry_bindings_begin_compute_xe
  XE_RESOLVE_PUSH_CONST_BINDING
  entry_binding_next_xe
  byte_buffer_wo_binding_xe(xe_resolve_dest, buffer(1))
  entry_binding_next_xe
  XE_RESOLVE_COPY_EDRAM_BINDING
  entry_binding_next_xe
  texture_xe(Texture2D<uint4_xe>, xe_resolve_source, 1, 0, t0, space0,
             texture(0))
entry_bindings_end_inputs_begin_compute_xe
  entry_in_global_thread_id_xe
entry_inputs_end_code_begin_compute_xe
{
  // 1 thread = 4 host pixels.
  XeResolveInfo resolve_info = XeResolveGetInfo(pass_push_consts_xe);
  resolve_info.edram_format = kXenosColorRenderTargetFormat_16_16_16_16;
  resolve_info.edram_format_ints_log2 = 1u;
  resolve_info.edram_msaa_samples = 0u;
  resolve_info.edram_is_depth = false;
  resolve_info.decode_pwl_gamma = false;
  resolve_info.dest_format = 26u;
  resolve_info.sample_select = 0u;

  uint2_xe pixel_index = in_global_thread_id_xe.xy << uint2_xe(2u, 0u);
  // Group height can't cross resolve granularity, Y overflow check not needed.
  dont_flatten_xe
  if (pixel_index.x >= resolve_info.width_div_8_scaled << 3u) {
    return;
  }
  int2_xe texel = XeResolveDirectSourceTexel(pixel_index, resolve_info, true);

  uint4_xe packed_01, packed_23;
  packed_01.xy =
      XeResolveDirectPackRGBA16(texel_fetch_2d_xe(xe_resolve_source, texel, 0));
  packed_01.zw = XeResolveDirectPackRGBA16(
      texel_fetch_2d_xe(xe_resolve_source, texel + int2_xe(1, 0), 0));
  packed_23.xy = XeResolveDirectPackRGBA16(
      texel_fetch_2d_xe(xe_resolve_source, texel + int2_xe(2, 0), 0));
  packed_23.zw = XeResolveDirectPackRGBA16(
      texel_fetch_2d_xe(xe_resolve_source, texel + int2_xe(3, 0), 0));
  float4_xe pixel_0, pixel_1, pixel_2, pixel_3;
  XeResolveUnpack64bpp4Samples(packed_01, packed_23, resolve_info.edram_format,
                               pixel_0, pixel_1, pixel_2, pixel_3);

  XeResolveDirectFinish64bpp(resolve_info, pixel_index,
                             resolve_info.dest_exp_bias_factor, pixel_0,
                             pixel_1, pixel_2, pixel_3, 8u);
}
entry_code_end_compute_xe
