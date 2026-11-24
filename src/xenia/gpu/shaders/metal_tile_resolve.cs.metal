// Open-pass TBDR tile resolve kernels (Apple4+ imageblock reads).
// Matches TileResolveConstants in src/xenia/gpu/metal/metal_resolve_encoder.cc.
// The dump shader's direct resolve per pixel (edram_dump_shader.cc), reading
// the render target from the pass's tiles: the render target's bits packed as
// the dump packs them, the red/blue swap of the resolve's format, the endian
// swap, and the store at XeResolveDestPixelAddress. One thread per pixel, one
// threadgroup per tile.

#include <metal_stdlib>
using namespace metal;

struct XeTileResolveConstants {
  uint2 origin;
  uint2 size;
  uint2 dest_offset;
  uint dest_pitch_macro_tiles;
  uint dest_slice_pitch_macro_tiles;
  uint dest_base;
  uint dest_array;
  uint dest_slice;
  uint swap;
  uint swap_bytes_in_16;
  uint swap_halves_in_32;
  uint exchange_dwords;
  uint bytes_per_block_log2;
  uint2 refresh_size;
  uint2 refresh_offset;
  uint num_format;
  uint exp_bias_factor_bits;
};

// XenosTextureTiledAddressCombine.
static uint XeTileResolveCombine(uint outer_inner_bytes, uint bank, uint pipe,
                                 uint y_lsb) {
  return (y_lsb << 4) | (pipe << 6) | (bank << 11) |
         (outer_inner_bytes & 0xFu) | (((outer_inner_bytes >> 4) & 0x1u) << 5) |
         (((outer_inner_bytes >> 5) & 0x7u) << 8) |
         (outer_inner_bytes >> 8 << 12);
}

// XenosTextureTiledAddress2D / 3D.
static uint XeTileResolveTiledAddress(uint2 p,
                                      constant XeTileResolveConstants& c) {
  if (c.dest_array != 0u) {
    uint outer = ((((c.dest_slice >> 2) * c.dest_slice_pitch_macro_tiles +
                    (p.y >> 4)) * c.dest_pitch_macro_tiles) + (p.x >> 5)) << 7;
    uint inner = ((c.dest_slice & 0x3u) << 5) | (((p.y >> 1) & 0x3u) << 3) |
                 (p.x & 0x7u);
    uint bank = ((p.y >> 3) ^ (c.dest_slice >> 2)) & 0x1u;
    return XeTileResolveCombine((outer | inner) << c.bytes_per_block_log2,
                                bank,
                                ((p.x >> 3) & 0x3u) ^ (bank << 1), p.y & 1u);
  }
  uint outer = ((p.y >> 5) * c.dest_pitch_macro_tiles + (p.x >> 5)) << 6;
  uint inner = (((p.y >> 1) & 0x7u) << 3) | (p.x & 0x7u);
  return XeTileResolveCombine((outer | inner) << c.bytes_per_block_log2,
                              (p.y >> 4) & 0x1u,
                              ((p.x >> 3) & 0x3u) ^ (((p.y >> 3) & 0x1u) << 1),
                              p.y & 1u);
}

// XeResolveSwapRedBlue_8_8_8_8 / XeResolveSwapRedBlue_2_10_10_10, keyed on the
// resolve's format as in the dump shader.
static uint XeTileResolveSwapRedBlue(constant XeTileResolveConstants& c,
                                     uint value) {
  if (c.swap == 1u) {
    value = (value & ~0xFF00FFu) | ((value & 0xFFu) << 16) |
            ((value >> 16) & 0xFFu);
  } else if (c.swap == 2u) {
    value = (value & ~0x3FF003FFu) | ((value & 0x3FFu) << 20) |
            ((value >> 20) & 0x3FFu);
  }
  return value;
}
// XeResolveSwap4PixelsRedBlue64bpp - the low halves of the two dwords.
static uint2 XeTileResolveSwapRedBlue(constant XeTileResolveConstants& c,
                                      uint2 value) {
  if (c.swap == 3u) {
    value = uint2((value.x & ~0xFFFFu) | (value.y & 0xFFFFu),
                  (value.y & ~0xFFFFu) | (value.x & 0xFFFFu));
  }
  return value;
}

// The texture-cache refresh texel: the swapped bits, not yet
// endian-transformed, as the ordinary loader would copy them from the
// resolved memory.
static uint4 XeTileResolveRefreshTexel(uint value) {
  return uint4(value, 0u, 0u, 0u);
}
static uint4 XeTileResolveRefreshTexel(uint2 value) {
  return uint4(value, 0u, 0u);
}

// XeEndianSwap32, and the store at XeResolveDestPixelAddress.
static void XeTileResolveStore(constant XeTileResolveConstants& c,
                               device uint* dest, uint2 p, uint value) {
  if (c.swap_bytes_in_16 != 0u) {
    value = ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
  }
  if (c.swap_halves_in_32 != 0u) {
    value = (value << 16) | (value >> 16);
  }
  dest[(c.dest_base + XeTileResolveTiledAddress(p + c.dest_offset, c)) >> 2] =
      value;
}
// XeEndianSwap64 - the dword exchange for 8in64, then XeEndianSwap32 on each
// dword - and the store of both.
static void XeTileResolveStore(constant XeTileResolveConstants& c,
                               device uint* dest, uint2 p, uint2 value) {
  if (c.exchange_dwords != 0u) {
    value = value.yx;
  }
  if (c.swap_bytes_in_16 != 0u) {
    value = ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
  }
  if (c.swap_halves_in_32 != 0u) {
    value = (value << 16) | (value >> 16);
  }
  uint index =
      (c.dest_base + XeTileResolveTiledAddress(p + c.dest_offset, c)) >> 2;
  dest[index] = value.x;
  dest[index + 1] = value.y;
}

// As the dump shader packs a k_8_8_8_8 render target: each component
// converted with OpConvertFToU(c * 255 + 0.5).
static uint XeTileResolvePack_8888(float4 color,
                                   constant XeTileResolveConstants& c) {
  uint4 bytes = uint4(color * 255.0f + 0.5f);
  return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}
// The ownership transfer view's bits. Every tile dispatch of a pass has to
// declare the same imageblock member type, so the attachments are read as
// half4 (16-bit float, exact, denormals included) or float4 (the rest; a 32-bit
// float is exact either way).
static uint XeTileResolvePack_16_16Float(half4 color,
                                         constant XeTileResolveConstants& c) {
  return as_type<uint>(color.xy);
}
static uint XeTileResolvePack_32Float(float4 color,
                                      constant XeTileResolveConstants& c) {
  return as_type<uint>(color.x);
}
// The RGBA16Uint ownership transfer view's components, two to a dword, red
// and blue in the low halves.
static uint2 XeTileResolvePack_16_16_16_16Float(
    half4 color, constant XeTileResolveConstants& c) {
  return as_type<uint2>(color);
}
// The RG32Uint ownership transfer view's.
static uint2 XeTileResolvePack_32_32Float(float4 color,
                                          constant XeTileResolveConstants& c) {
  return as_type<uint2>(color.xy);
}

// XePackFixed for 16 bits.
static uint XeTileResolvePackFixed16(float value, uint num_format) {
  uint unsigned_max = (1u << 16u) - 1u;
  uint packed = 0u;
  if (num_format == 1u) {
    // Signed fraction uses the positive endpoint, then keeps the destination
    // bit width.
    float positive_max = float((1u << (16u - 1u)) - 1u);
    packed = uint(int(min(max(value, -1.0f), 1.0f) * positive_max +
                      (value >= 0.0f ? 0.5f : -0.5f)));
  } else if (num_format == 2u) {
    packed = uint(min(max(value, 0.0f), float(unsigned_max)) + 0.5f);
  } else if (num_format == 3u) {
    int signed_min = -int(1u << (16u - 1u));
    int signed_max = int((1u << (16u - 1u)) - 1u);
    packed = uint(int(min(max(value, float(signed_min)), float(signed_max)) +
                      (value >= 0.0f ? 0.5f : -0.5f)));
  } else {
    // Unsigned fraction, float-on-fixed, or anything unexpected: keep the
    // usual unsigned-fraction pack.
    packed = uint(saturate(value) * float(unsigned_max) + 0.5f);
  }
  return packed & unsigned_max;
}
// resolve_full_64bpp_rgba16_1x_direct's conversion of one pixel of a
// 16_16_16_16 render target (RGBA16Snorm, read as float, whose stored bits are
// rint(v * 32767)): XeUnpackR16G16B16A16Edram, the exponent bias and
// XePackR16G16B16A16. The red/blue swap comes after, on the packed halves,
// which is the same as swapping the floats before packing.
static uint2 XeTileResolvePack_16_16_16_16(float4 color,
                                           constant XeTileResolveConstants& c) {
  float4 pixel = float4(int4(rint(color * 32767.0f))) * (1.0f / 1024.0f);
  pixel *= as_type<float>(c.exp_bias_factor_bits);
  uint4 n = uint4(XeTileResolvePackFixed16(pixel.x, c.num_format),
                  XeTileResolvePackFixed16(pixel.y, c.num_format),
                  XeTileResolvePackFixed16(pixel.z, c.num_format),
                  XeTileResolvePackFixed16(pixel.w, c.num_format));
  return n.xz | (n.yw << 16u);
}

// The console's average of 8-bit unorm samples, as the dump shader's
// load_average_rgba8 does it: each byte widened to 10 bits by replication,
// the sum divided by 4 truncating, then truncated to 8 bits.
static uint4 XeTileResolveWiden8(uint4 bytes) {
  return (bytes << 2u) | (bytes >> 6u);
}
static uint4 XeTileResolveBytes(float4 unorm) {
  return uint4(unorm * 255.0f + 0.5f);
}
static uint4 XeTileResolveBytes(uint packed) {
  return (uint4(packed) >> uint4(0u, 8u, 16u, 24u)) & 0xFFu;
}
static uint XeTileResolveAverage4Exact(uint4 sum10) {
  uint4 n = sum10 >> 4u;
  return n.x | (n.y << 8u) | (n.z << 16u) | (n.w << 24u);
}
static uint XeTileResolvePack_8888Average4(float4 s0, float4 s1, float4 s2,
                                           float4 s3,
                                           constant XeTileResolveConstants& c) {
  return XeTileResolveAverage4Exact(
      XeTileResolveWiden8(XeTileResolveBytes(s0)) +
      XeTileResolveWiden8(XeTileResolveBytes(s1)) +
      XeTileResolveWiden8(XeTileResolveBytes(s2)) +
      XeTileResolveWiden8(XeTileResolveBytes(s3)));
}
// Uncontracted multiply/add helpers matching NoContraction semantics.
static float XeTileResolveNoContractionMul(float l, float r) {
  return fma(l, r, 0.0f);
}
static float XeTileResolveNoContractionAdd(float l, float r) {
  return fma(1.0f, l, r);
}
// SpirvShaderTranslator::LinearToPWLGamma of a pre-saturated value, with its
// float constants' exact bits.
static float XeTileResolveLinearToPWLGamma(float linear) {
  bool is_piece_at_least_3 = linear >= as_type<float>(0x3F002008u);
  float scale_3_or_2 = is_piece_at_least_3 ? as_type<float>(0x42FFC000u)
                                           : as_type<float>(0x437FC000u);
  float offset_3_or_2 = is_piece_at_least_3 ? as_type<float>(0x3F008081u)
                                            : as_type<float>(0x3E808081u);
  bool is_piece_at_least_1 = linear >= as_type<float>(0x3D802008u);
  float scale_1_or_0 = is_piece_at_least_1 ? as_type<float>(0x43FFC000u)
                                           : 1023.0f;
  float offset_1_or_0 =
      is_piece_at_least_1 ? as_type<float>(0x3E008081u) : 0.0f;
  bool is_piece_at_least_2 = linear >= as_type<float>(0x3E002008u);
  float scale = is_piece_at_least_2 ? scale_3_or_2 : scale_1_or_0;
  float offset = is_piece_at_least_2 ? offset_3_or_2 : offset_1_or_0;
  return XeTileResolveNoContractionAdd(
      XeTileResolveNoContractionMul(
          trunc(XeTileResolveNoContractionMul(linear, scale)),
          as_type<float>(0x3B808081u)),
      offset);
}
// An 8_8_8_8_GAMMA render target's sample (linear, in RGBA16Unorm) as the
// dump shader's load_and_pack packs it.
static uint XeTileResolveGammaPack(float4 linear) {
  uint packed = uint(XeTileResolveLinearToPWLGamma(linear.x) * 255.0f + 0.5f);
  packed |= uint(XeTileResolveLinearToPWLGamma(linear.y) * 255.0f + 0.5f)
            << 8u;
  packed |= uint(XeTileResolveLinearToPWLGamma(linear.z) * 255.0f + 0.5f)
            << 16u;
  packed |= uint(linear.w * 255.0f + 0.5f) << 24u;
  return packed;
}
// The bit copy of a one-sample 8_8_8_8_GAMMA render target.
static uint XeTileResolvePack_8888Gamma(float4 color,
                                        constant XeTileResolveConstants& c) {
  return XeTileResolveGammaPack(color);
}
// As the dump shader packs a k_2_10_10_10 render target (RGB10A2Unorm): each
// component converted with OpConvertFToU(c * 1023 + 0.5), alpha with 3, and
// inserted 10 (alpha 2) bits wide.
static uint XeTileResolvePack_RGB10(float4 color,
                                    constant XeTileResolveConstants& c) {
  uint4 n = uint4(color * float4(1023.0f, 1023.0f, 1023.0f, 3.0f) + 0.5f) &
            uint4(1023u, 1023u, 1023u, 3u);
  return n.x | (n.y << 10u) | (n.z << 20u) | (n.w << 30u);
}
// The dump's 7e3 packing of a host color component, on the bits
// (XeResolveDirectFloat32To7e3): NaNs and everything with the sign bit become
// 0, the rest is clamped to 31.875 and rounded to the nearest even.
static uint XeTileResolveFloat32To7e3(float value) {
  uint f32 = as_type<uint>(value);
  if (f32 > 0x7F800000u) {
    f32 = 0u;
  }
  f32 = min(f32, 0x41FF0000u);
  uint biased_f32;
  if (f32 < 0x3E800000u) {
    biased_f32 = ((f32 & 0x7FFFFFu) | 0x800000u) >>
                 min(125u - (f32 >> 23u), 24u);
  } else {
    biased_f32 = f32 - (124u << 23u);
  }
  return ((biased_f32 + 0x7FFFu + ((biased_f32 >> 16u) & 1u)) >> 16u) &
         0x3FFu;
}
// The bit copy of a one-sample 2_10_10_10_FLOAT render target (RGBA16Float,
// read as float like the compute copies read it), as the dump packs it
// (XeResolveDirectPackRGB10A2Float): 7e3 RGB, and alpha clamped to [0, 1] the
// same way as a 2-bit unsigned fraction. Half denormals are below half the
// smallest 7e3 step, so they pack to 0 whether or not they are flushed.
static uint XeTileResolvePack_RGB10Float(float4 color,
                                         constant XeTileResolveConstants& c) {
  uint alpha_bits = as_type<uint>(color.w);
  if (alpha_bits > 0x7F800000u) {
    alpha_bits = 0u;
  }
  float alpha = as_type<float>(min(alpha_bits, 0x3F800000u));
  return XeTileResolveFloat32To7e3(color.x) |
         (XeTileResolveFloat32To7e3(color.y) << 10u) |
         (XeTileResolveFloat32To7e3(color.z) << 20u) |
         (uint(alpha * 3.0f + 0.5f) << 30u);
}
// XeUnpackR10G10B10A2Float's 7e3 component.
static float XeTileResolve7e3To32(uint f10) {
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint mantissa_lzcnt = 7u - (31u - clz(mantissa));
    exponent = 1u - mantissa_lzcnt;
    mantissa = (mantissa << mantissa_lzcnt) & 0x7Fu;
  }
  return as_type<float>(f10 != 0u ? ((exponent + 124u) << 23u) |
                                        (mantissa << 16u)
                                  : 0u);
}
// resolve_full_32bpp_rgb10f_1x_direct's conversion of one pixel: the dump's
// packing, XeUnpackR10G10B10A2Float, the exponent bias and XePackR10G10B10A2
// as an unsigned fraction. The red/blue swap comes after, on the packed
// fields, which is the same as swapping the floats before packing. The color
// products are exact; alpha's are exact or above a multiple of 1/3, never at
// a rounding boundary, so contraction changes none of them.
static uint XeTileResolvePack_RGB10FloatToRGB10(
    float4 color, constant XeTileResolveConstants& c) {
  uint packed = XeTileResolvePack_RGB10Float(color, c);
  float4 pixel = float4(XeTileResolve7e3To32(packed & 0x3FFu),
                        XeTileResolve7e3To32((packed >> 10u) & 0x3FFu),
                        XeTileResolve7e3To32((packed >> 20u) & 0x3FFu),
                        float(packed >> 30u) * (1.0f / 3.0f));
  pixel *= as_type<float>(c.exp_bias_factor_bits);
  uint4 n = uint4(saturate(pixel) * float4(1023.0f, 1023.0f, 1023.0f, 3.0f) +
                  0.5f) &
            uint4(1023u, 1023u, 1023u, 3u);
  return n.x | (n.y << 10u) | (n.z << 20u) | (n.w << 30u);
}
static uint XeTileResolvePack_8888GammaAverage4(
    float4 s0, float4 s1, float4 s2, float4 s3,
    constant XeTileResolveConstants& c) {
  return XeTileResolveAverage4Exact(
      XeTileResolveWiden8(XeTileResolveBytes(XeTileResolveGammaPack(s0))) +
      XeTileResolveWiden8(XeTileResolveBytes(XeTileResolveGammaPack(s1))) +
      XeTileResolveWiden8(XeTileResolveBytes(XeTileResolveGammaPack(s2))) +
      XeTileResolveWiden8(XeTileResolveBytes(XeTileResolveGammaPack(s3))));
}

// resolve_full_32bpp_rgb10_4x_direct's average of a 2_10_10_10 render target
// (RGB10A2Unorm), in its operation order: each sample packed as
// XeResolveDirectPackRGB10A2 packs it and unpacked as XeUnpackR10G10B10A2UNorm,
// with 1/1023 and 1/3's exact bits, summed in sample order, times
// dest_exp_bias_factor * 0.5 * 0.5 (no bias), and packed as XePackFixed packs
// unsigned fractions. Each multiply and add is rounded on its own (fma with a
// zero or unit operand), as the compute shader rounds them, so the output
// matches it bit for bit.
static float4 XeTileResolveRGB10Bits(float4 sample) {
  return float4(uint4(uint(sample.x * 1023.0f + 0.5f),
                      uint(sample.y * 1023.0f + 0.5f),
                      uint(sample.z * 1023.0f + 0.5f),
                      uint(sample.w * 3.0f + 0.5f)) &
                uint4(1023u, 1023u, 1023u, 3u));
}
static uint XeTileResolvePack_RGB10Average4(float4 s0, float4 s1, float4 s2,
                                            float4 s3,
                                            constant XeTileResolveConstants& c) {
  const float4 scale =
      float4(as_type<float>(0x3A802008u), as_type<float>(0x3A802008u),
             as_type<float>(0x3A802008u), as_type<float>(0x3EAAAAABu));
  float4 sum = fma(XeTileResolveRGB10Bits(s0), scale, float4(0.0f));
  sum = fma(float4(1.0f), sum,
            fma(XeTileResolveRGB10Bits(s1), scale, float4(0.0f)));
  sum = fma(float4(1.0f), sum,
            fma(XeTileResolveRGB10Bits(s2), scale, float4(0.0f)));
  sum = fma(float4(1.0f), sum,
            fma(XeTileResolveRGB10Bits(s3), scale, float4(0.0f)));
  float4 average = sum * 0.25f;
  uint4 n = uint4(saturate(average) * float4(1023.0f, 1023.0f, 1023.0f, 3.0f) +
                  0.5f) &
            uint4(1023u, 1023u, 1023u, 3u);
  return n.x | (n.y << 10u) | (n.z << 20u) | (n.w << 30u);
}

// The pixel's one value, or its four samples (the guest 4x sample numbering
// matches the host one).
#define XE_TILE_RESOLVE_READ_PIXEL(kernel_name)                               \
  XeTileResolvePack_##kernel_name(pixels.read(pixel_in_tile).color, constants)
#define XE_TILE_RESOLVE_READ_4_SAMPLES(kernel_name)                           \
  XeTileResolvePack_##kernel_name(                                            \
      pixels.read(pixel_in_tile, 0, imageblock_data_rate::sample).color,      \
      pixels.read(pixel_in_tile, 1, imageblock_data_rate::sample).color,      \
      pixels.read(pixel_in_tile, 2, imageblock_data_rate::sample).color,      \
      pixels.read(pixel_in_tile, 3, imageblock_data_rate::sample).color,      \
      constants)

// How many texture-cache textures the resolve refreshes, 0 to 4: the first
// and its aliases, R32Uint (32bpp) or RG32Uint (64bpp) 2D array views.
constant uint xe_tile_resolve_refresh_count [[function_constant(0)]];
constant bool xe_tile_resolve_refresh_0 = xe_tile_resolve_refresh_count > 0u;
constant bool xe_tile_resolve_refresh_1 = xe_tile_resolve_refresh_count > 1u;
constant bool xe_tile_resolve_refresh_2 = xe_tile_resolve_refresh_count > 2u;
constant bool xe_tile_resolve_refresh_3 = xe_tile_resolve_refresh_count > 3u;

#define XE_TILE_RESOLVE(kernel_name, member_type, read, index)                \
  struct XeTileResolvePixel_##kernel_name##_##index {                         \
    member_type color [[color(index)]];                                       \
  };                                                                          \
  kernel void xe_tile_resolve_##kernel_name##_##index(                        \
      imageblock<XeTileResolvePixel_##kernel_name##_##index,                  \
                 imageblock_layout_implicit> pixels,                          \
      constant XeTileResolveConstants& constants [[buffer(0)]],               \
      device uint* dest [[buffer(1)]],                                        \
      texture2d_array<uint, access::write> refresh_0                          \
      [[texture(0), function_constant(xe_tile_resolve_refresh_0)]],           \
      texture2d_array<uint, access::write> refresh_1                          \
      [[texture(1), function_constant(xe_tile_resolve_refresh_1)]],           \
      texture2d_array<uint, access::write> refresh_2                          \
      [[texture(2), function_constant(xe_tile_resolve_refresh_2)]],           \
      texture2d_array<uint, access::write> refresh_3                          \
      [[texture(3), function_constant(xe_tile_resolve_refresh_3)]],           \
      ushort2 pixel_in_tile [[thread_position_in_threadgroup]],               \
      uint2 pixel [[thread_position_in_grid]]) {                              \
    uint2 p = pixel - constants.origin;                                       \
    if (p.x >= constants.size.x || p.y >= constants.size.y) {                 \
      return;                                                                 \
    }                                                                         \
    auto value =                                                              \
        XeTileResolveSwapRedBlue(constants, read(kernel_name));               \
    if (xe_tile_resolve_refresh_0) {                                          \
      uint2 texel = p + constants.refresh_offset;                             \
      if (texel.x < constants.refresh_size.x &&                               \
          texel.y < constants.refresh_size.y) {                               \
        uint4 refresh = XeTileResolveRefreshTexel(value);                     \
        refresh_0.write(refresh, texel, 0);                                   \
        if (xe_tile_resolve_refresh_1) {                                      \
          refresh_1.write(refresh, texel, 0);                                 \
        }                                                                     \
        if (xe_tile_resolve_refresh_2) {                                      \
          refresh_2.write(refresh, texel, 0);                                 \
        }                                                                     \
        if (xe_tile_resolve_refresh_3) {                                      \
          refresh_3.write(refresh, texel, 0);                                 \
        }                                                                     \
      }                                                                       \
    }                                                                         \
    XeTileResolveStore(constants, dest, p, value);                            \
  }
#define XE_TILE_RESOLVE_ALL_INDICES(kernel_name, member_type, read)           \
  XE_TILE_RESOLVE(kernel_name, member_type, read, 0)                          \
  XE_TILE_RESOLVE(kernel_name, member_type, read, 1)                          \
  XE_TILE_RESOLVE(kernel_name, member_type, read, 2)                          \
  XE_TILE_RESOLVE(kernel_name, member_type, read, 3)

XE_TILE_RESOLVE_ALL_INDICES(8888, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(16_16Float, half4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(32Float, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(16_16_16_16Float, half4,
                            XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(32_32Float, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(16_16_16_16, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(8888Average4, float4,
                            XE_TILE_RESOLVE_READ_4_SAMPLES)
XE_TILE_RESOLVE_ALL_INDICES(8888GammaAverage4, float4,
                            XE_TILE_RESOLVE_READ_4_SAMPLES)
XE_TILE_RESOLVE_ALL_INDICES(RGB10Average4, float4,
                            XE_TILE_RESOLVE_READ_4_SAMPLES)
XE_TILE_RESOLVE_ALL_INDICES(8888Gamma, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(RGB10, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(RGB10Float, float4, XE_TILE_RESOLVE_READ_PIXEL)
XE_TILE_RESOLVE_ALL_INDICES(RGB10FloatToRGB10, float4,
                            XE_TILE_RESOLVE_READ_PIXEL)
