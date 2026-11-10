// Exact format-3/12 source, one sample, 16_16_16_16 or 16_16_16_16_FLOAT
// destination, read straight from the 1x host render target instead of the
// EDRAM buffer.
// XE_MSL_ONLY
// Each pixel is packed into the dword the EDRAM dump would store for it, then
// the copy is the generic 64bpp one specialized for this case (the resolve info
// fixed as for resolve_full_64bpp_rgb10f_rgba16_1x and
// resolve_full_64bpp_rgb10f_rgba16f_1x, the destination format, number format
// and exponent bias left to the constants), with the same unpack, exponent bias
// and pack code.
#include "endian.xesli"
#include "pixel_formats.xesli"
// The EDRAM binding slot carries the source texel origin instead: two dwords,
// the host render target texel of the resolve area's first pixel. With
// XE_RESOLVE_DIRECT_REFRESH, two more follow: the refreshed texture's width
// with the texture row of the resolve's first row in the upper 16 bits, and
// its height with the number of its aliases in the upper 16 bits.
#define XE_RESOLVE_COPY_EDRAM_BYTE_BUFFER_ALIGNMENT 4
#include "resolve.xesli"
#include "resolve_direct_rgb10f.xesli"

byte_buffer_align16_wo_declare_xe(xe_resolve_dest, 1, 0, u0, space0, 1)
#ifdef XE_RESOLVE_DIRECT_REFRESH
#define XE_RESOLVE_DIRECT_REFRESH_64BPP 1
#include "resolve_direct_refresh.xesli"
#endif
#define LOCAL_SIZE_X_XE 8
#define LOCAL_SIZE_Y_XE 8
#define LOCAL_SIZE_Z_XE 1

entry_bindings_begin_compute_xe
  XE_RESOLVE_PUSH_CONST_BINDING
  entry_binding_next_xe
  byte_buffer_wo_binding_xe(xe_resolve_dest, buffer(1))
  entry_binding_next_xe
  XE_RESOLVE_COPY_EDRAM_BINDING
  entry_binding_next_xe
  texture_xe(texture_2d_xe, xe_resolve_source, 1, 0, t0, space0,
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
  resolve_info.edram_msaa_samples = 0u;
  resolve_info.edram_is_depth = false;
  resolve_info.decode_pwl_gamma = false;
  resolve_info.sample_select = 0u;

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

  // XeResolveLoad4RGBAColors for sample select 0, with the pixels read from
  // the render target.
  uint4_xe packed;
  packed.x = XeResolveDirectPackRGB10A2Float(
      texel_fetch_2d_xe(xe_resolve_source, texel, 0));
  packed.y = XeResolveDirectPackRGB10A2Float(
      texel_fetch_2d_xe(xe_resolve_source, texel + int2_xe(1, 0), 0));
  packed.z = XeResolveDirectPackRGB10A2Float(
      texel_fetch_2d_xe(xe_resolve_source, texel + int2_xe(2, 0), 0));
  packed.w = XeResolveDirectPackRGB10A2Float(
      texel_fetch_2d_xe(xe_resolve_source, texel + int2_xe(3, 0), 0));
  float4_xe pixel_0, pixel_1, pixel_2, pixel_3;
  XeResolveUnpack32bpp4Samples(packed, resolve_info.edram_format, pixel_0,
                               pixel_1, pixel_2, pixel_3);
  float exp_bias = resolve_info.dest_exp_bias_factor;
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
#ifdef XE_RESOLVE_DIRECT_REFRESH
  XeResolveDirectRefreshWrite4x64(pixel_index, packed_01, packed_23, 8u);
#endif
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
