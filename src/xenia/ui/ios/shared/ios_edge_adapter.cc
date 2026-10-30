/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/shared/ios_edge_adapter.h"

#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

namespace xe::ui {

bool DeleteIOSGameConfig(uint32_t title_id) {
  const auto path =
      config::config_folder / "config" /
      (fmt::format("{:08X}", title_id) + config::game_config_suffix);
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    XELOGE("Unable to remove game config {}: {}", xe::strip_pii(xe::path_to_utf8(path)), error.message());
  }
  return !error;
}

uint32_t IOSContentDataFileCount(const std::filesystem::path& path) {
  const auto header = vfs::XContentContainerDevice::ReadContainerHeader(path);
  return header && header->content_header.is_magic_valid()
             ? uint32_t(header->content_metadata.data_file_count)
             : 0;
}

}  // namespace xe::ui
