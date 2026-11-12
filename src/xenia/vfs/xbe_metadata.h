/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_XBE_METADATA_H_
#define XENIA_VFS_XBE_METADATA_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "xenia/vfs/gdfx_util.h"

namespace xe {
namespace vfs {

// What an original Xbox game's executable says about itself, from its
// certificate and the title image the dashboard shows.
struct XbeMetadata {
  uint32_t title_id = 0;
  // Raised by each revision of a release.
  uint32_t version = 0;
  uint32_t disc_number = 0;
  std::string title_name;
  // The title image as PNG. Empty when the executable has none in a format
  // this decodes. For a package without one, the package's thumbnail.
  std::vector<uint8_t> icon_png;
};

// An .xbe file, an original Xbox disc image, a ZAR archive of one, or an Xbox
// Original package.
std::optional<XbeMetadata> ExtractXbeMetadata(
    const std::filesystem::path& path);
std::optional<XbeMetadata> ExtractXbeMetadata(const uint8_t* data, size_t size);

// Where a disc image keeps an original Xbox game's default.xbe. Empty for an
// Xbox 360 disc, which keeps a default.xex.
std::optional<GdfxFileLocation> FindXboxOriginalXbe(const uint8_t* data,
                                                    size_t size);

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_XBE_METADATA_H_
