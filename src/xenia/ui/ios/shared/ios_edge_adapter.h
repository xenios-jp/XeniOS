/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_EDGE_ADAPTER_H_
#define XENIA_UI_IOS_SHARED_IOS_EDGE_ADAPTER_H_

#include <cstdint>
#include <filesystem>

namespace xe::ui {

bool DeleteIOSGameConfig(uint32_t title_id);
uint32_t IOSContentDataFileCount(const std::filesystem::path& path);

}  // namespace xe::ui
#endif  // XENIA_UI_IOS_SHARED_IOS_EDGE_ADAPTER_H_
