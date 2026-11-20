/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SHADERS_RESOLVE_DIRECT_SLANG_
#define XENIA_GPU_SHADERS_RESOLVE_DIRECT_SLANG_

// Shared setup and helpers for direct resolves that read straight from a host
// render target instead of the EDRAM buffer (XE_MSL_ONLY Slang compute
// shaders).
//
// The EDRAM binding slot carries the source texel origin instead: two dwords,
// the host render target texel of the resolve area's first pixel (optionally
// followed by one dword of packed host sample indices for MSAA averages). With
// XE_RESOLVE_DIRECT_REFRESH, two more follow: the refreshed texture's width
// with the texture row of the resolve's first row in the upper 16 bits, and
// its height with the number of its aliases in bits 16..23 and the column
// offset in 32-texel tiles in bits 24..31.

#include "endian.xesli"
#include "pixel_formats.xesli"
#define XE_RESOLVE_COPY_EDRAM_BYTE_BUFFER_ALIGNMENT 4
#include "resolve.xesli"

byte_buffer_align16_wo_declare_xe(xe_resolve_dest, 1, 0, u0, space0, 1)

#define LOCAL_SIZE_X_XE 8
#define LOCAL_SIZE_Y_XE 8
#define LOCAL_SIZE_Z_XE 1

// ----------------------------------------------------------------------------
// Direct render target sample packing (matching the EDRAM dump's packing).
// ----------------------------------------------------------------------------

// The dump's 7e3 packing of a host color component (UnclampedFloat32To7e3):
// NClamp to [0, 31.875], then PreClampedFloat32To7e3, rounding to the nearest
// even. Done on the bits so fast math can't change the clamp: as unsigned,
// everything above +infinity is a NaN or has the sign bit set, -0.0 included,
// and those all become 0. Above 0, the order of the bits is that of the values.
uint XeResolveDirectFloat32To7e3(float value) {
  uint f32 = asuint(value);
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
uint XeResolveDirectPackRGB10A2Float(float4 sample) {
  uint alpha_bits = asuint(sample.a);
  if (alpha_bits > 0x7F800000u) {
    alpha_bits = 0u;
  }
  float alpha = asfloat(min(alpha_bits, 0x3F800000u));
  return XeResolveDirectFloat32To7e3(sample.r) |
         (XeResolveDirectFloat32To7e3(sample.g) << 10u) |
         (XeResolveDirectFloat32To7e3(sample.b) << 20u) |
         (uint(alpha * 3.0f + 0.5f) << 30u);
}

// The dump's 8_8_8_8 packing of a host RGBA8Unorm sample.
uint XeResolveDirectPackRGBA8(float4 sample) {
  return uint(sample.r * 255.0f + 0.5f) |
         (uint(sample.g * 255.0f + 0.5f) << 8u) |
         (uint(sample.b * 255.0f + 0.5f) << 16u) |
         (uint(sample.a * 255.0f + 0.5f) << 24u);
}

// The dump's 2_10_10_10 packing of a host RGB10A2Unorm sample.
uint XeResolveDirectPackRGB10A2(float4 sample) {
  return uint(sample.r * 1023.0f + 0.5f) |
         (uint(sample.g * 1023.0f + 0.5f) << 10u) |
         (uint(sample.b * 1023.0f + 0.5f) << 20u) |
         (uint(sample.a * 3.0f + 0.5f) << 30u);
}

// The dump's 16_16_16_16 packing of a host RGBA16Uint texel: the components'
// bits, red and blue in the low halves of the two dwords.
uint2 XeResolveDirectPackRGBA16(uint4 texel) {
  return (texel.xz & 0xFFFFu) | (texel.yw << 16u);
}

// ----------------------------------------------------------------------------
// Optional texture-cache refresh output (up to 4 images: primary + 3 aliases).
// ----------------------------------------------------------------------------

#ifdef XE_RESOLVE_DIRECT_REFRESH
#ifdef XE_RESOLVE_DIRECT_REFRESH_64BPP
typedef uint2 XeResolveDirectRefreshTexel;
#else
typedef uint XeResolveDirectRefreshTexel;
#endif
RWTexture2DArray<XeResolveDirectRefreshTexel> xe_resolve_refresh_texture
    : register(u4);
RWTexture2DArray<XeResolveDirectRefreshTexel> xe_resolve_refresh_alias_0
    : register(u5);
RWTexture2DArray<XeResolveDirectRefreshTexel> xe_resolve_refresh_alias_1
    : register(u6);
RWTexture2DArray<XeResolveDirectRefreshTexel> xe_resolve_refresh_alias_2
    : register(u7);

void XeResolveDirectRefreshWrite(uint2 texel, XeResolveDirectRefreshTexel value,
                                 uint alias_count) {
  xe_resolve_refresh_texture[uint3(texel, 0u)] = value;
  if (alias_count >= 1u) {
    xe_resolve_refresh_alias_0[uint3(texel, 0u)] = value;
  }
  if (alias_count >= 2u) {
    xe_resolve_refresh_alias_1[uint3(texel, 0u)] = value;
  }
  if (alias_count >= 3u) {
    xe_resolve_refresh_alias_2[uint3(texel, 0u)] = value;
  }
}

#ifdef XE_RESOLVE_DIRECT_REFRESH_64BPP
[ForceInline]
void XeResolveDirectRefreshWrite4x64(uint2 pixel_index, uint4 packed_01,
                                     uint4 packed_23, uint constants_offset) {
  uint texture_width_and_offset_y =
      byte_buffer_align4_load4_xe(xe_resolve_edram, constants_offset);
  uint texture_width = texture_width_and_offset_y & 0xFFFFu;
  uint texture_height_and_alias_count =
      byte_buffer_align4_load4_xe(xe_resolve_edram, constants_offset + 4u);
  uint texture_height = texture_height_and_alias_count & 0xFFFFu;
  uint alias_count = (texture_height_and_alias_count >> 16u) & 0xFFu;
  uint texture_x =
      pixel_index.x + ((texture_height_and_alias_count >> 24u) << 5u);
  uint texture_y = pixel_index.y + (texture_width_and_offset_y >> 16u);
  if (texture_y < texture_height) {
    if (texture_x < texture_width) {
      XeResolveDirectRefreshWrite(uint2(texture_x, texture_y), packed_01.xy,
                                  alias_count);
    }
    if (texture_x + 1u < texture_width) {
      XeResolveDirectRefreshWrite(uint2(texture_x + 1u, texture_y),
                                  packed_01.zw, alias_count);
    }
    if (texture_x + 2u < texture_width) {
      XeResolveDirectRefreshWrite(uint2(texture_x + 2u, texture_y),
                                  packed_23.xy, alias_count);
    }
    if (texture_x + 3u < texture_width) {
      XeResolveDirectRefreshWrite(uint2(texture_x + 3u, texture_y),
                                  packed_23.zw, alias_count);
    }
  }
}
#else
[ForceInline]
void XeResolveDirectRefreshWrite4(uint2 pixel_index, uint4 packed) {
  uint texture_width_and_offset_y =
      byte_buffer_align4_load4_xe(xe_resolve_edram, 8u);
  uint texture_width = texture_width_and_offset_y & 0xFFFFu;
  uint texture_height_and_alias_count =
      byte_buffer_align4_load4_xe(xe_resolve_edram, 12u);
  uint texture_height = texture_height_and_alias_count & 0xFFFFu;
  uint alias_count = (texture_height_and_alias_count >> 16u) & 0xFFu;
  uint texture_x =
      pixel_index.x + ((texture_height_and_alias_count >> 24u) << 5u);
  uint texture_y = pixel_index.y + (texture_width_and_offset_y >> 16u);
  if (texture_y < texture_height) {
    if (texture_x < texture_width) {
      XeResolveDirectRefreshWrite(uint2(texture_x, texture_y), packed.x,
                                  alias_count);
    }
    if (texture_x + 1u < texture_width) {
      XeResolveDirectRefreshWrite(uint2(texture_x + 1u, texture_y), packed.y,
                                  alias_count);
    }
    if (texture_x + 2u < texture_width) {
      XeResolveDirectRefreshWrite(uint2(texture_x + 2u, texture_y), packed.z,
                                  alias_count);
    }
    if (texture_x + 3u < texture_width) {
      XeResolveDirectRefreshWrite(uint2(texture_x + 3u, texture_y), packed.w,
                                  alias_count);
    }
  }
}
#endif  // XE_RESOLVE_DIRECT_REFRESH_64BPP
#endif  // XE_RESOLVE_DIRECT_REFRESH

// ----------------------------------------------------------------------------
// Shared coordinate, bias/swap, half-pixel fill, refresh, and store tail.
// ----------------------------------------------------------------------------

[ForceInline]
int2 XeResolveDirectSourceTexel(uint2 pixel_index, XeResolveInfo resolve_info,
                                bool apply_half_pixel_y) {
  uint source_y =
      apply_half_pixel_y
          ? max(pixel_index.y, resolve_info.half_pixel_offset_fill_source.y)
          : pixel_index.y;
  return int2(
      uint2(pixel_index.x, source_y) +
      uint2(byte_buffer_align4_load4_xe(xe_resolve_edram, 0u),
            byte_buffer_align4_load4_xe(xe_resolve_edram, 4u)));
}

[ForceInline]
void XeResolveDirectBiasAndSwap(XeResolveInfo resolve_info, float exp_bias,
                                inout float4 pixel_0, inout float4 pixel_1,
                                inout float4 pixel_2, inout float4 pixel_3) {
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
}

#ifndef XE_RESOLVE_DIRECT_REFRESH_64BPP
[ForceInline]
void XeResolveDirectFinish32bpp(XeResolveInfo resolve_info, uint2 pixel_index,
                                float exp_bias, float4 pixel_0, float4 pixel_1,
                                float4 pixel_2, float4 pixel_3,
                                bool apply_half_pixel_x) {
  XeResolveDirectBiasAndSwap(resolve_info, exp_bias, pixel_0, pixel_1, pixel_2,
                             pixel_3);
  uint4 packed = XePack32bpp4Pixels(pixel_0, pixel_1, pixel_2, pixel_3,
                                    resolve_info.dest_format,
                                    resolve_info.dest_num_format);
  if (apply_half_pixel_x) {
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
  }
#ifdef XE_RESOLVE_DIRECT_REFRESH
  XeResolveDirectRefreshWrite4(pixel_index, packed);
#endif
  byte_buffer_align16_store16_xe(
      xe_resolve_dest, XeResolveDestPixelAddress(resolve_info, pixel_index, 2u),
      XeEndianSwap32(packed, resolve_info.dest_endian_128));
}
#endif  // !XE_RESOLVE_DIRECT_REFRESH_64BPP

#if !defined(XE_RESOLVE_DIRECT_REFRESH) || \
    defined(XE_RESOLVE_DIRECT_REFRESH_64BPP)
[ForceInline]
void XeResolveDirectFinish64bpp(XeResolveInfo resolve_info, uint2 pixel_index,
                                float exp_bias, float4 pixel_0, float4 pixel_1,
                                float4 pixel_2, float4 pixel_3,
                                uint refresh_constants_offset) {
  XeResolveDirectBiasAndSwap(resolve_info, exp_bias, pixel_0, pixel_1, pixel_2,
                             pixel_3);
  uint4 packed_01, packed_23;
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
  XeResolveDirectRefreshWrite4x64(pixel_index, packed_01, packed_23,
                                  refresh_constants_offset);
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
#endif  // !XE_RESOLVE_DIRECT_REFRESH || XE_RESOLVE_DIRECT_REFRESH_64BPP

#endif  // XENIA_GPU_SHADERS_RESOLVE_DIRECT_SLANG_
