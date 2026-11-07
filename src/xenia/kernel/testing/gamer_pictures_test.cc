/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xam/gamer_pictures.h"
#include "xenia/kernel/xam/xui_package.h"

#include "third_party/catch/include/catch.hpp"

namespace {

void PutBE32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(value >> shift));
  }
}

// An XUIZ v3 package holding files (name, contents), in order.
std::vector<uint8_t> MakeXuiPackage(
    const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<uint8_t> table;
  uint32_t data_offset = 0;
  for (const auto& [name, contents] : files) {
    PutBE32(table, uint32_t(contents.size()));
    PutBE32(table, data_offset);
    table.push_back(uint8_t(name.size()));
    table.insert(table.end(), name.begin(), name.end());
    data_offset += uint32_t(contents.size());
  }
  std::vector<uint8_t> out;
  PutBE32(out, 0x5855495A);  // XUIZ
  PutBE32(out, 3);
  PutBE32(out, 0);
  PutBE32(out, 0);
  PutBE32(out, uint32_t(table.size()));
  out.push_back(uint8_t(files.size() >> 8));
  out.push_back(uint8_t(files.size()));
  out.insert(out.end(), table.begin(), table.end());
  for (const auto& file : files) {
    out.insert(out.end(), file.second.begin(), file.second.end());
  }
  return out;
}

std::string AsString(std::span<const uint8_t> data) {
  return std::string(data.begin(), data.end());
}

}  // namespace

TEST_CASE("XUI packages list their files and refuse a short one",
          "[xam][gamer_pictures]") {
  auto package = MakeXuiPackage({{"64_FFFE07D10002000000010000.png", "large"},
                                 {"32_FFFE07D10002000000010000.png", "sm"}});
  auto files = xe::kernel::xam::ReadXuiPackage(package);
  REQUIRE(files);
  REQUIRE(files->size() == 2);
  CHECK((*files)[0].name == "64_FFFE07D10002000000010000.png");
  CHECK(AsString((*files)[0].data) == "large");
  CHECK(AsString((*files)[1].data) == "sm");
  auto found = xe::kernel::xam::FindXuiPackageFile(
      package, "32_fffe07d10002000000010000.png");
  REQUIRE(found);
  CHECK(AsString(*found) == "sm");

  package.resize(package.size() - 3);  // the last file runs past the end
  CHECK_FALSE(xe::kernel::xam::ReadXuiPackage(package));
  CHECK(xe::kernel::xam::ReadXuiPackage(std::span<const uint8_t>()) ==
        std::nullopt);
}

TEST_CASE("Gamer pictures need a real xam.xex", "[xam][gamer_pictures]") {
  std::vector<uint8_t> junk(4096, 0x58);
  CHECK(xe::kernel::xam::ReadBuiltInGamerPictures(junk).empty());
  CHECK(xe::kernel::xam::ReadBuiltInGamerPictures({}).empty());
}

// The system software can't ship with the tests; point XE_TEST_XAM_XEX at an
// installed flash/xam.xex to check the real thing.
TEST_CASE("The system software's xam.xex has 13 gamer pictures",
          "[xam][gamer_pictures]") {
  const char* path = std::getenv("XE_TEST_XAM_XEX");
  if (!path || !*path) {
    WARN("XE_TEST_XAM_XEX not set; skipped");
    return;
  }
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file);
  std::vector<uint8_t> xam((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
  auto pictures = xe::kernel::xam::ReadBuiltInGamerPictures(xam);
  REQUIRE(pictures.size() == 13);
  CHECK(pictures.front().key == "FFFE07D10002000000010000");
  CHECK(pictures.back().key == "FFFE07D10002000C0001000C");
  const std::string_view png("\x89PNG", 4);
  for (const auto& picture : pictures) {
    REQUIRE(picture.large.size() > 8);
    REQUIRE(picture.small.size() > 8);
    CHECK(std::string_view(reinterpret_cast<const char*>(picture.large.data()),
                           4) == png);
    CHECK(std::string_view(reinterpret_cast<const char*>(picture.small.data()),
                           4) == png);
  }
}
