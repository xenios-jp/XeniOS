// Exact format-3/12 source, one sample, RGB10A2 destination, read straight
// from the 1x host render target instead of the EDRAM buffer.
// XE_MSL_ONLY
// Each pixel is packed into the dword the EDRAM dump would store for it, then
// the copy is the generic 32bpp one specialized for this case (the resolve info
// fixed as for resolve_full_32bpp_rgb10f_1x), with the same unpack, exponent
// bias and pack code.
#include "endian.xesli"
#include "pixel_formats.xesli"
// The EDRAM binding slot carries the source texel origin instead: two dwords,
// the host render target texel of the resolve area's first pixel. With
// XE_RESOLVE_DIRECT_REFRESH, two more follow: the refreshed texture's width
// with the texture row of the resolve's first row in the upper 16 bits, and
// its height with the number of its aliases in the upper 16 bits.
#define XE_RESOLVE_COPY_EDRAM_BYTE_BUFFER_ALIGNMENT 4
#include "resolve.xesli"

byte_buffer_align16_wo_declare_xe(xe_resolve_dest, 1, 0, u0, space0, 1)
#ifdef XE_RESOLVE_DIRECT_REFRESH
#include "resolve_direct_refresh.xesli"
#endif
#define LOCAL_SIZE_X_XE 8
#define LOCAL_SIZE_Y_XE 8
#define LOCAL_SIZE_Z_XE 1

// The dump's 7e3 packing of a host color component (UnclampedFloat32To7e3):
// NClamp to [0, 31.875], then PreClampedFloat32To7e3, rounding to the nearest
// even. Done on the bits so fast math can't change the clamp: as unsigned,
// everything above +infinity is a NaN or has the sign bit set, -0.0 included,
// and those all become 0. Above 0, the order of the bits is that of the values.
uint XeResolveDirectFloat32To7e3(float value) {
  uint f32 = float_bits_to_uint_xe(value);
  if (f32 > 0x7F800000u) {
    f32 = 0u;
  }
  // 31.875.
  f32 = min(f32, 0x41FF0000u);
  uint biased_f32;
  if (f32 < 0x3E800000u) {
    // Denormal 7e3, below 0.25. The shift is at least 1 here.
    biased_f32 = ((f32 & 0x7FFFFFu) | 0x800000u) >>
                 min(125u - (f32 >> 23u), 24u);
  } else {
    biased_f32 = f32 - (124u << 23u);
  }
  return ((biased_f32 + 0x7FFFu + ((biased_f32 >> 16u) & 1u)) >> 16u) &
         0x3FFu;
}

// The dump's 2_10_10_10_FLOAT packing of a host RGBA16Float sample: 7e3 RGB,
// and alpha NClamped to [0, 1] (with the same handling of NaN and the sign) as
// a 2-bit unsigned fraction.
uint XeResolveDirectPackRGB10A2Float(float4_xe sample) {
  uint alpha_bits = float_bits_to_uint_xe(sample.a);
  if (alpha_bits > 0x7F800000u) {
    alpha_bits = 0u;
  }
  float alpha = uint_bits_to_float_xe(min(alpha_bits, 0x3F800000u));
  return XeResolveDirectFloat32To7e3(sample.r) |
         (XeResolveDirectFloat32To7e3(sample.g) << 10u) |
         (XeResolveDirectFloat32To7e3(sample.b) << 20u) |
         (uint(alpha * 3.0f + 0.5f) << 30u);
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
  resolve_info.edram_format =
      kXenosColorRenderTargetFormat_2_10_10_10_FLOAT;
  resolve_info.edram_format_ints_log2 = 0u;
  resolve_info.edram_msaa_samples = 0u;
  resolve_info.edram_is_depth = false;
  resolve_info.decode_pwl_gamma = false;
  resolve_info.dest_format = 7u;
  resolve_info.dest_num_format = 0u;
  resolve_info.sample_select = 0u;

  uint2_xe pixel_index = in_global_thread_id_xe.xy << uint2_xe(2u, 0u);
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
  XeResolveDecodePWLGammaSource(resolve_info, pixel_0);
  XeResolveDecodePWLGammaSource(resolve_info, pixel_1);
  XeResolveDecodePWLGammaSource(resolve_info, pixel_2);
  XeResolveDecodePWLGammaSource(resolve_info, pixel_3);
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

  packed = XePack32bpp4Pixels(pixel_0, pixel_1, pixel_2, pixel_3,
                              resolve_info.dest_format,
                              resolve_info.dest_num_format);
  dont_flatten_xe
  if (pixel_index.x == 0u &&
      resolve_info.half_pixel_offset_fill_source.x != 0u) {
    if (resolve_info.half_pixel_offset_fill_source.x >= 2u) {
      if (resolve_info.half_pixel_offset_fill_source.x >= 3u) {
        packed.z = packed.w;
      }
      packed.y = packed.z;
    }
    packed.x = packed.y;
  }
#ifdef XE_RESOLVE_DIRECT_REFRESH
  XeResolveDirectRefreshWrite4(pixel_index, packed);
#endif
  byte_buffer_align16_store16_xe(
      xe_resolve_dest, XeResolveDestPixelAddress(resolve_info, pixel_index, 2u),
      XeEndianSwap32(packed, resolve_info.dest_endian_128));
}
entry_code_end_compute_xe
