/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XUI_PACKAGE_H_
#define XENIA_KERNEL_XAM_XUI_PACKAGE_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {

// A file in an XUIZ package, the archive XUI resources keep their files in.
struct XuiPackageFile {
  std::string name;
  std::span<const uint8_t> data;
};

// The files of an XUIZ package. Version 1 names them in UTF-16 and version 3
// in UTF-8, with the same entries otherwise. Nothing if it isn't one, or an
// entry runs past its end.
std::optional<std::vector<XuiPackageFile>> ReadXuiPackage(
    std::span<const uint8_t> package);

// One file of an XUIZ package by name, in any case.
std::optional<std::span<const uint8_t>> FindXuiPackageFile(
    std::span<const uint8_t> package, std::string_view name);

// length big-endian UTF-16 code units at *offset, which moves past them.
std::optional<std::u16string> ReadXuiString(std::span<const uint8_t> data,
                                            size_t* offset, size_t length);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XUI_PACKAGE_H_
