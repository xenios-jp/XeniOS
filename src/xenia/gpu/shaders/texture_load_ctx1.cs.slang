/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "pixel_formats.xesli"
#include "texture_load.xesli"

// http://fileadmin.cs.lth.se/cs/Personal/Michael_Doggett/talks/unc-xenos-doggett.pdf
// CXT1 is like DXT3/5 color, but 2-component and with 8:8 endpoints rather than
// 5:6:5.
//
// Dword 1:
// rrrrrrrrgggggggg
// RRRRRRRRGGGGGGGG
// (R is in the higher bits, according to how this format is used in 4D5307E6).
// Dword 2:
// AA BB CC DD
// EE FF GG HH
// II JJ KK LL
// MM NN OO PP

// The four R8G8 colors of a CTX1 block for its codes 0 to 3, codes 0 and 1 in
// x, 2 and 3 in y, 16 bits each. Codes 2 and 3 weigh the endpoints 2:1 and 1:2,
// rounding down like XeCTX1TwoBlocksRowToR8G8, dividing by 3 as * 683 >> 11
// (exact up to 3 * 255).
uint2_xe XeCTX1Palette(uint endpoints) {
  // 0x00g000r0 and 0x00G100R1.
  uint end_0 = ((endpoints >> 8u) & 0xFFu) | ((endpoints & 0xFFu) << 16u);
  uint end_1 = (endpoints >> 24u) | (endpoints & 0xFF0000u);
  uint2_xe mix_3x = uint2_xe(2u * end_0 + end_1, end_0 + 2u * end_1);
  uint2_xe mix = (((mix_3x & 0xFFFFu) * 683u) >> 11u) |
                 ((((mix_3x >> 16u) * 683u) >> 11u) << 8u);
  // 0xRRGGrrgg in memory order (g0 r0 g1 r1) to r0 g0 r1 g1.
  return uint2_xe(((endpoints >> 8u) & 0x00FF00FFu) |
                      ((endpoints << 8u) & 0xFF00FF00u),
                  mix.x | (mix.y << 16u));
}

// One row of four texels of a CTX1 block as R8G8, from its palette and the
// row's 8 bits of codes.
uint2_xe XeCTX1RowToR8G8(uint2_xe palette, uint codes) {
  uint4_xe texel_codes = (uint_x4_xe(codes) >> uint4_xe(0u, 2u, 4u, 6u)) & 3u;
  uint4_xe texels =
      (select_xe((texel_codes & 2u) != uint_x4_xe(0u), palette.yyyy,
                 palette.xxxx) >>
       ((texel_codes & 1u) << 4u)) &
      0xFFFFu;
  return texels.xz | (texels.yw << 16u);
}

XE_TEXTURE_LOAD_ENTRY(16)
{
  // 1 thread = 4 CTX1 blocks to 16x4 R8G8 texels.
  XeTextureLoadInfo load_info = XeTextureLoadGetInfo(pass_push_consts_xe);
  uint3_xe block_index = in_global_thread_id_xe << uint3_xe(2u, 0u, 0u);
  dont_flatten_xe
  if (any(greater_than_equal_xe(block_index.xy, load_info.size_blocks.xy))) {
    return;
  }
  uint3_xe texel_index_host = block_index << uint3_xe(2u, 2u, 0u);
  uint block_offset_host =
      load_info.host_offset +
      uint(XeTextureHostLinearOffset(int3_xe(texel_index_host),
                                     load_info.host_pitch,
                                     load_info.height_texels, 2u));
  uint block_offset_guest =
      XeTextureLoadSourceAddress(load_info, block_index, 3u);
  uint i;
  unroll_xe for (i = 0u; i < 2u; ++i) {
    if (i != 0u) {
      block_offset_host += 16u;
      block_offset_guest += XeTextureLoadLocalXAddressXor(2u, 3u,
                                                          load_info.is_tiled);
    }
    // Two blocks.
    uint4_xe blocks = XeEndianSwap32(
        byte_buffer_align16_load16_xe(xe_texture_load_source,
                                      block_offset_guest),
        load_info.endian_32);
    uint2_xe palette_0 = XeCTX1Palette(blocks.x);
    uint2_xe palette_1 = XeCTX1Palette(blocks.z);
    byte_buffer_align16_store16_xe(
        xe_texture_load_dest, block_offset_host,
        uint4_xe(XeCTX1RowToR8G8(palette_0, blocks.y),
                 XeCTX1RowToR8G8(palette_1, blocks.w)));
    dont_flatten_xe if (texel_index_host.y + 1u < load_info.height_texels) {
      byte_buffer_align16_store16_xe(
          xe_texture_load_dest, block_offset_host + load_info.host_pitch,
          uint4_xe(XeCTX1RowToR8G8(palette_0, blocks.y >> 8u),
                   XeCTX1RowToR8G8(palette_1, blocks.w >> 8u)));
      dont_flatten_xe if (texel_index_host.y + 2u < load_info.height_texels) {
        byte_buffer_align16_store16_xe(
            xe_texture_load_dest, block_offset_host + 2u * load_info.host_pitch,
            uint4_xe(XeCTX1RowToR8G8(palette_0, blocks.y >> 16u),
                     XeCTX1RowToR8G8(palette_1, blocks.w >> 16u)));
        dont_flatten_xe
        if (texel_index_host.y + 3u < load_info.height_texels) {
          byte_buffer_align16_store16_xe(
              xe_texture_load_dest,
              block_offset_host + 3u * load_info.host_pitch,
              uint4_xe(XeCTX1RowToR8G8(palette_0, blocks.y >> 24u),
                       XeCTX1RowToR8G8(palette_1, blocks.w >> 24u)));
        }
      }
    }
  }
}
entry_code_end_compute_xe
