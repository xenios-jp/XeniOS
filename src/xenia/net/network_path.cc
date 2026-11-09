/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/network_path.h"

#include "xenia/base/platform.h"

#if !XE_PLATFORM_APPLE

namespace xe::net {

NetworkPathState GetNetworkPathState() { return NetworkPathState::kAvailable; }
uint64_t AddNetworkPathListener(std::function<void()>) { return 0; }
void RemoveNetworkPathListener(uint64_t) {}

}  // namespace xe::net

#endif  // !XE_PLATFORM_APPLE
