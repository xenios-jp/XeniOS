/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_GAMER_PICTURES_H_
#define XENIA_KERNEL_XAM_GAMER_PICTURES_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {

// A gamer picture the console comes with. xam keeps them in its shared
// resources (shrdres) as 64_<key>.png and 32_<key>.png, under the dashboard's
// keys (FFFE07D1 0002 000n 0001 000n).
struct GamerPicture {
  std::string key;  // 24 hex digits in upper case, as the profile stores it
  std::vector<uint8_t> large;  // 64x64 PNG
  std::vector<uint8_t> small;  // 32x32 PNG
};

// The built-in gamer pictures in an xam.xex (the system software's
// flash/xam.xex), in key order. Empty if it has none or doesn't decode.
std::vector<GamerPicture> ReadBuiltInGamerPictures(
    std::span<const uint8_t> xam_xex);


}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_GAMER_PICTURES_H_
