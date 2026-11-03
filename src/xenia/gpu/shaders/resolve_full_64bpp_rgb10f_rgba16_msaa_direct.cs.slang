// Exact format-3/12 source, two-sample average of a 2x or four-sample average
// of a 4x surface, 16_16_16_16 or 16_16_16_16_FLOAT destination, read straight
// from the multisampled host render target instead of the EDRAM buffer.
// XE_MSL_ONLY
// Each sample is packed into the dword the EDRAM dump would store for it, then
// the copy is the generic 64bpp one specialized for this case (the resolve info
// fixed except the sample select, the destination format, number format and
// exponent bias left to the constants), with the same unpack, sum in the same
// order, exponent bias and pack code.
#include "endian.xesli"
#include "pixel_formats.xesli"
// The EDRAM binding slot carries the source texel origin instead: two dwords,
// the host render target texel of the resolve area's first pixel, then the
// host sample index of each guest sample in the order the copy adds them (0,
// 1, 2, 3), a byte each.
#define XE_RESOLVE_COPY_EDRAM_BYTE_BUFFER_ALIGNMENT 4
#include "resolve.xesli"
#include "resolve_direct_rgb10f.xesli"

byte_buffer_align16_wo_declare_xe(xe_resolve_dest, 1, 0, u0, space0, 1)
#define LOCAL_SIZE_X_XE 8
#define LOCAL_SIZE_Y_XE 8
#define LOCAL_SIZE_Z_XE 1

// XeResolveLoad4RGBAUnswappedPixelSamplesFromRaw for one guest sample of four
// adjacent pixels, read from the render target. Unpacking the packed dwords is
// faster than rounding the floats to 7e3 values directly.
void XeResolveDirectLoad4RGB10FSamples(
    texture_2d_ms_xe xe_resolve_source, int2_xe texel, int sample,
    XeResolveInfo resolve_info, out_param_xe(float4_xe, pixel_0),
    out_param_xe(float4_xe, pixel_1), out_param_xe(float4_xe, pixel_2),
    out_param_xe(float4_xe, pixel_3)) {
  uint4_xe packed;
  packed.x = XeResolveDirectPackRGB10A2Float(
      texel_fetch_2d_ms_xe(xe_resolve_source, texel, sample));
  packed.y = XeResolveDirectPackRGB10A2Float(
      texel_fetch_2d_ms_xe(xe_resolve_source, texel + int2_xe(1, 0), sample));
  packed.z = XeResolveDirectPackRGB10A2Float(
      texel_fetch_2d_ms_xe(xe_resolve_source, texel + int2_xe(2, 0), sample));
  packed.w = XeResolveDirectPackRGB10A2Float(
      texel_fetch_2d_ms_xe(xe_resolve_source, texel + int2_xe(3, 0), sample));
  XeResolveUnpack32bpp4Samples(packed, resolve_info.edram_format, pixel_0,
                               pixel_1, pixel_2, pixel_3);
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
  // 1 thread = 4 host pixels.
  XeResolveInfo resolve_info = XeResolveGetInfo(pass_push_consts_xe);
  resolve_info.edram_format =
      kXenosColorRenderTargetFormat_2_10_10_10_FLOAT;
  resolve_info.edram_format_ints_log2 = 0u;
  resolve_info.edram_is_depth = false;
  resolve_info.decode_pwl_gamma = false;

  uint2_xe pixel_index = in_global_thread_id_xe.xy << uint2_xe(2u, 0u);
  // Group height can't cross resolve granularity, Y overflow check not needed.
  dont_flatten_xe
  if (pixel_index.x >= resolve_info.width_div_8_scaled << 3u) {
    return;
  }
  // The EDRAM row XeResolveCopySourceFirstSampleAddressBytes reads.
  int2_xe texel = int2_xe(
      uint2_xe(pixel_index.x,
               max(pixel_index.y,
                   resolve_info.half_pixel_offset_fill_source.y)) +
      uint2_xe(byte_buffer_align4_load4_xe(xe_resolve_edram, 0u),
               byte_buffer_align4_load4_xe(xe_resolve_edram, 4u)));
  uint host_samples = byte_buffer_align4_load4_xe(xe_resolve_edram, 8u);

  // XeResolveLoad4RGBAColors for sample select 01 or 0123, with the samples
  // read from the render target.
  float4_xe pixel_0, pixel_1, pixel_2, pixel_3;
  XeResolveDirectLoad4RGB10FSamples(
      xe_resolve_source, texel, int(host_samples & 0xFFu), resolve_info,
      pixel_0, pixel_1, pixel_2, pixel_3);
  float exp_bias = resolve_info.dest_exp_bias_factor * 0.5f;
  float4_xe msaa_resolve_pixel_0, msaa_resolve_pixel_1, msaa_resolve_pixel_2,
      msaa_resolve_pixel_3;
  XeResolveDirectLoad4RGB10FSamples(
      xe_resolve_source, texel, int((host_samples >> 8u) & 0xFFu),
      resolve_info, msaa_resolve_pixel_0, msaa_resolve_pixel_1,
      msaa_resolve_pixel_2, msaa_resolve_pixel_3);
  pixel_0 += msaa_resolve_pixel_0;
  pixel_1 += msaa_resolve_pixel_1;
  pixel_2 += msaa_resolve_pixel_2;
  pixel_3 += msaa_resolve_pixel_3;
  dont_flatten_xe
  if (resolve_info.sample_select >= kXenosCopySampleSelect_0123) {
    exp_bias *= 0.5f;
    XeResolveDirectLoad4RGB10FSamples(
        xe_resolve_source, texel, int((host_samples >> 16u) & 0xFFu),
        resolve_info, msaa_resolve_pixel_0, msaa_resolve_pixel_1,
        msaa_resolve_pixel_2, msaa_resolve_pixel_3);
    pixel_0 += msaa_resolve_pixel_0;
    pixel_1 += msaa_resolve_pixel_1;
    pixel_2 += msaa_resolve_pixel_2;
    pixel_3 += msaa_resolve_pixel_3;
    XeResolveDirectLoad4RGB10FSamples(
        xe_resolve_source, texel, int(host_samples >> 24u), resolve_info,
        msaa_resolve_pixel_0, msaa_resolve_pixel_1, msaa_resolve_pixel_2,
        msaa_resolve_pixel_3);
    pixel_0 += msaa_resolve_pixel_0;
    pixel_1 += msaa_resolve_pixel_1;
    pixel_2 += msaa_resolve_pixel_2;
    pixel_3 += msaa_resolve_pixel_3;
  }
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

  uint4_xe packed_01, packed_23;
  XePack64bpp4Pixels(pixel_0, pixel_1, pixel_2, pixel_3,
                     resolve_info.dest_format, resolve_info.dest_num_format,
                     packed_01, packed_23);
  dont_flatten_xe
  if (pixel_index.x == 0u &&
      resolve_info.half_pixel_offset_fill_source.x != 0u) {
    if (resolve_info.half_pixel_offset_fill_source.x >= 2u) {
      if (resolve_info.half_pixel_offset_fill_source.x >= 3u) {
        packed_23.xy = packed_23.zw;
      }
      packed_01.zw = packed_23.xy;
    }
    packed_01.xy = packed_01.zw;
  }
  uint dest_address = XeResolveDestPixelAddress(resolve_info, pixel_index, 3u);
  byte_buffer_align16_store16_xe(
      xe_resolve_dest, dest_address,
      XeEndianSwap64(packed_01, resolve_info.dest_endian_128));
  dest_address += XeResolveLocalXAddressXor(2u, 3u);
  byte_buffer_align16_store16_xe(
      xe_resolve_dest, dest_address,
      XeEndianSwap64(packed_23, resolve_info.dest_endian_128));
}
entry_code_end_compute_xe
