// Exact format-2/10 source, four-sample average, RGB10A2 destination, read
// straight from the 4x host render target instead of the EDRAM buffer.
// XE_MSL_ONLY
// Each sample is packed into the dword the EDRAM dump would store for it, then
// the copy is the generic 32bpp one specialized for this case (resolve info
// fixed except the source format, which stays dynamic), with the same unpack,
// sum, exponent bias and pack code.
#include "endian.xesli"
#include "pixel_formats.xesli"
// The EDRAM binding slot carries the source texel origin instead: two dwords,
// the host render target texel of the resolve area's first pixel.
#define XE_RESOLVE_COPY_EDRAM_BYTE_BUFFER_ALIGNMENT 4
#include "resolve.xesli"

byte_buffer_align16_wo_declare_xe(xe_resolve_dest, 1, 0, u0, space0, 1)
#define LOCAL_SIZE_X_XE 8
#define LOCAL_SIZE_Y_XE 8
#define LOCAL_SIZE_Z_XE 1

// The dump's 2_10_10_10 packing of a host RGB10A2Unorm sample.
uint XeResolveDirectPackRGB10A2(float4_xe sample) {
  return uint(sample.r * 1023.0f + 0.5f) |
         (uint(sample.g * 1023.0f + 0.5f) << 10u) |
         (uint(sample.b * 1023.0f + 0.5f) << 20u) |
         (uint(sample.a * 3.0f + 0.5f) << 30u);
}

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
  int2_xe texel = int2_xe(
      pixel_index +
      uint2_xe(byte_buffer_align4_load4_xe(xe_resolve_edram, 0u),
               byte_buffer_align4_load4_xe(xe_resolve_edram, 4u)));

  // XeResolveLoad4RGBAColors for sample select 0123, with the samples read
  // from the render target.
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
      XeResolveDecodePWLGammaSource(resolve_info, pixel_0);
      XeResolveDecodePWLGammaSource(resolve_info, pixel_1);
      XeResolveDecodePWLGammaSource(resolve_info, pixel_2);
      XeResolveDecodePWLGammaSource(resolve_info, pixel_3);
    } else {
      XeResolveUnpack32bpp4Samples(packed, resolve_info.edram_format,
                                   msaa_resolve_pixel_0, msaa_resolve_pixel_1,
                                   msaa_resolve_pixel_2, msaa_resolve_pixel_3);
      XeResolveDecodePWLGammaSource(resolve_info, msaa_resolve_pixel_0);
      XeResolveDecodePWLGammaSource(resolve_info, msaa_resolve_pixel_1);
      XeResolveDecodePWLGammaSource(resolve_info, msaa_resolve_pixel_2);
      XeResolveDecodePWLGammaSource(resolve_info, msaa_resolve_pixel_3);
      pixel_0 += msaa_resolve_pixel_0;
      pixel_1 += msaa_resolve_pixel_1;
      pixel_2 += msaa_resolve_pixel_2;
      pixel_3 += msaa_resolve_pixel_3;
    }
  }
  float exp_bias = resolve_info.dest_exp_bias_factor * 0.5f * 0.5f;
  pixel_0 *= exp_bias;
  pixel_1 *= exp_bias;
  pixel_2 *= exp_bias;
  pixel_3 *= exp_bias;
  dont_flatten_xe if (resolve_info.dest_swap) {
    pixel_0 = pixel_0.bgra;
    pixel_1 = pixel_1.bgra;
    pixel_2 = pixel_2.bgra;
    pixel_3 = pixel_3.bgra;
  }

  packed = XePack32bpp4Pixels(pixel_0, pixel_1, pixel_2, pixel_3,
                              resolve_info.dest_format,
                              resolve_info.dest_num_format);
  byte_buffer_align16_store16_xe(
      xe_resolve_dest, XeResolveDestPixelAddress(resolve_info, pixel_index, 2u),
      XeEndianSwap32(packed, resolve_info.dest_endian_128));
}
entry_code_end_compute_xe
