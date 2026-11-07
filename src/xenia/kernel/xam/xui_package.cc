/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xui_package.h"

#include "third_party/utfcpp/source/utf8.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"

namespace xe {
namespace kernel {
namespace xam {

std::optional<std::u16string> ReadXuiString(std::span<const uint8_t> data,
                                            size_t* offset, size_t length) {
  if (*offset > data.size() || length > (data.size() - *offset) / 2) {
    return std::nullopt;
  }
  std::u16string string(length, u'\0');
  for (size_t i = 0; i < length; ++i) {
    string[i] =
        char16_t(xe::load_and_swap<uint16_t>(data.data() + *offset + i * 2));
  }
  *offset += length * 2;
  return string;
}

std::optional<std::vector<XuiPackageFile>> ReadXuiPackage(
    std::span<const uint8_t> package) {
  constexpr uint32_t kXuiPackageMagic = 0x5855495A;  // XUIZ
  if (package.size() < 0x16 ||
      xe::load_and_swap<uint32_t>(package.data()) != kXuiPackageMagic) {
    return std::nullopt;
  }
  const uint32_t version = xe::load_and_swap<uint32_t>(package.data() + 4);
  if (version != 1 && version != 3) {
    return std::nullopt;
  }
  const size_t data_start =
      0x16 + size_t(xe::load_and_swap<uint32_t>(package.data() + 0x10));
  const uint16_t file_count =
      xe::load_and_swap<uint16_t>(package.data() + 0x14);
  std::vector<XuiPackageFile> files;
  files.reserve(file_count);
  size_t offset = 0x16;
  for (uint16_t i = 0; i < file_count; ++i) {
    if (offset + 9 > package.size()) {
      return std::nullopt;
    }
    const uint32_t size = xe::load_and_swap<uint32_t>(package.data() + offset);
    const uint32_t file_offset =
        xe::load_and_swap<uint32_t>(package.data() + offset + 4);
    const size_t name_length = package[offset + 8];
    offset += 9;
    XuiPackageFile file;
    if (version == 3) {
      if (offset + name_length > package.size()) {
        return std::nullopt;
      }
      file.name.assign(reinterpret_cast<const char*>(package.data() + offset),
                       name_length);
      if (!::utf8::is_valid(file.name)) {
        return std::nullopt;
      }
      offset += name_length;
    } else {
      auto wide_name = ReadXuiString(package, &offset, name_length);
      if (!wide_name) {
        return std::nullopt;
      }
      file.name = xe::to_utf8(*wide_name);
    }
    if (data_start > package.size() ||
        file_offset > package.size() - data_start ||
        size > package.size() - data_start - file_offset) {
      return std::nullopt;
    }
    file.data = package.subspan(data_start + file_offset, size);
    files.push_back(std::move(file));
  }
  return files;
}

std::optional<std::span<const uint8_t>> FindXuiPackageFile(
    std::span<const uint8_t> package, std::string_view name) {
  auto files = ReadXuiPackage(package);
  if (!files) {
    return std::nullopt;
  }
  for (const auto& file : *files) {
    if (xe::utf8::equal_case(file.name, name)) {
      return file.data;
    }
  }
  return std::nullopt;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
