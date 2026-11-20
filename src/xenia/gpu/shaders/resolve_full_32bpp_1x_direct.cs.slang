// Exact 8_8_8_8, 2_10_10_10 or 2_10_10_10_FLOAT source, one sample, any 32bpp
// destination, read straight from the 1x host render target instead of the
// EDRAM buffer.
// XE_MSL_ONLY
#include "resolve_direct.slang"

// The dword the dump stores for a render target texel of the source format.
uint XeResolveDirectPack32bpp(float4_xe sample, uint format) {
  switch (format) {
    case kXenosColorRenderTargetFormat_2_10_10_10:
    case kXenosColorRenderTargetFormat_2_10_10_10_AS_10_10_10_10:
      return XeResolveDirectPackRGB10A2(sample);
    case kXenosColorRenderTargetFormat_2_10_10_10_FLOAT:
    case kXenosColorRenderTargetFormat_2_10_10_10_FLOAT_AS_16_16_16_16:
      return XeResolveDirectPackRGB10A2Float(sample);
    default:
      return XeResolveDirectPackRGBA8(sample);
  }
}

entry_bindings_begin_compute_xe
  XE_RESOLVE_PUSH_CONST_BINDING
  entry_binding_next_xe
  byte_buffer_wo_binding_xe(xe_resolve_dest, buffer(1))
  entry_binding_next_xe
  XE_RESOLVE_COPY_EDRAM_BINDING
  entry_binding_next_xe
  texture_xe(texture_2d_xe, xe_resolve_source, 1, 0, t0, space0, texture(0))
entry_bindings_end_inputs_begin_compute_xe
  entry_in_global_thread_id_xe
entry_inputs_end_code_begin_compute_xe
{
  XeResolveInfo resolve_info = XeResolveGetInfo(pass_push_consts_xe);
  resolve_info.edram_format_ints_log2 = 0u;
  resolve_info.edram_msaa_samples = 0u;
  resolve_info.edram_is_depth = false;
  resolve_info.sample_select = 0u;

  uint2_xe pixel_index = in_global_thread_id_xe.xy << uint2_xe(2u, 0u);
  dont_flatten_xe
  if (pixel_index.x >= resolve_info.width_div_8_scaled << 3u) {
    return;
  }
  int2_xe texel = XeResolveDirectSourceTexel(pixel_index, resolve_info, true);

  uint4_xe packed;
  packed.x = XeResolveDirectPack32bpp(
      texel_fetch_2d_xe(xe_resolve_source, texel, 0),
      resolve_info.edram_format);
  packed.y = XeResolveDirectPack32bpp(
      texel_fetch_2d_xe(xe_resolve_source, texel + int2_xe(1, 0), 0),
      resolve_info.edram_format);
  packed.z = XeResolveDirectPack32bpp(
      texel_fetch_2d_xe(xe_resolve_source, texel + int2_xe(2, 0), 0),
      resolve_info.edram_format);
  packed.w = XeResolveDirectPack32bpp(
      texel_fetch_2d_xe(xe_resolve_source, texel + int2_xe(3, 0), 0),
      resolve_info.edram_format);
  float4_xe pixel_0, pixel_1, pixel_2, pixel_3;
  XeResolveUnpack32bpp4Samples(packed, resolve_info.edram_format, pixel_0,
                               pixel_1, pixel_2, pixel_3);
  XeResolveDecodePWLGammaSource(resolve_info, pixel_0);
  XeResolveDecodePWLGammaSource(resolve_info, pixel_1);
  XeResolveDecodePWLGammaSource(resolve_info, pixel_2);
  XeResolveDecodePWLGammaSource(resolve_info, pixel_3);

  XeResolveDirectFinish32bpp(resolve_info, pixel_index,
                             resolve_info.dest_exp_bias_factor, pixel_0,
                             pixel_1, pixel_2, pixel_3, true);
}
entry_code_end_compute_xe
