// Exact format-2/10 source, four-sample average, RGB10A2 destination, read
// straight from the 4x host render target instead of the EDRAM buffer.
// XE_MSL_ONLY
#include "resolve_direct.slang"

entry_bindings_begin_compute_xe
  XE_RESOLVE_PUSH_CONST_BINDING
  entry_binding_next_xe
  byte_buffer_wo_binding_xe(xe_resolve_dest, buffer(1))
  entry_binding_next_xe
  XE_RESOLVE_COPY_EDRAM_BINDING
  entry_binding_next_xe
  texture_xe(texture_2d_ms_xe, xe_resolve_source, 1, 0, t0, space0,
             texture(0))
entry_bindings_end_inputs_begin_compute_xe
  entry_in_global_thread_id_xe
entry_inputs_end_code_begin_compute_xe
{
  XeResolveInfo resolve_info = XeResolveGetInfo(pass_push_consts_xe);
  resolve_info.edram_format_ints_log2 = 0u;
  resolve_info.edram_msaa_samples = 2u;
  resolve_info.edram_is_depth = false;
  resolve_info.decode_pwl_gamma = false;
  resolve_info.dest_format = 7u;
  resolve_info.dest_num_format = 0u;
  resolve_info.sample_select = 6u;

  uint2_xe pixel_index = in_global_thread_id_xe.xy << uint2_xe(2u, 0u);
  dont_flatten_xe
  if (pixel_index.x >= resolve_info.width_div_8_scaled << 3u) {
    return;
  }
  int2_xe texel = XeResolveDirectSourceTexel(pixel_index, resolve_info, false);

  float4_xe pixel_0, pixel_1, pixel_2, pixel_3;
  float4_xe msaa_resolve_pixel_0, msaa_resolve_pixel_1, msaa_resolve_pixel_2,
      msaa_resolve_pixel_3;
  uint4_xe packed;
  for (int sample = 0; sample < 4; ++sample) {
    packed.x = XeResolveDirectPackRGB10A2(
        texel_fetch_2d_ms_xe(xe_resolve_source, texel, sample));
    packed.y = XeResolveDirectPackRGB10A2(
        texel_fetch_2d_ms_xe(xe_resolve_source, texel + int2_xe(1, 0),
                             sample));
    packed.z = XeResolveDirectPackRGB10A2(
        texel_fetch_2d_ms_xe(xe_resolve_source, texel + int2_xe(2, 0),
                             sample));
    packed.w = XeResolveDirectPackRGB10A2(
        texel_fetch_2d_ms_xe(xe_resolve_source, texel + int2_xe(3, 0),
                             sample));
    if (sample == 0) {
      XeResolveUnpack32bpp4Samples(packed, resolve_info.edram_format, pixel_0,
                                   pixel_1, pixel_2, pixel_3);
    } else {
      XeResolveUnpack32bpp4Samples(packed, resolve_info.edram_format,
                                   msaa_resolve_pixel_0, msaa_resolve_pixel_1,
                                   msaa_resolve_pixel_2, msaa_resolve_pixel_3);
      pixel_0 += msaa_resolve_pixel_0;
      pixel_1 += msaa_resolve_pixel_1;
      pixel_2 += msaa_resolve_pixel_2;
      pixel_3 += msaa_resolve_pixel_3;
    }
  }

  XeResolveDirectFinish32bpp(
      resolve_info, pixel_index,
      resolve_info.dest_exp_bias_factor * 0.5f * 0.5f, pixel_0, pixel_1,
      pixel_2, pixel_3, false);
}
entry_code_end_compute_xe
