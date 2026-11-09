/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_NETWORK_PATH_H_
#define XENIA_NET_NETWORK_PATH_H_

#include <cstdint>
#include <functional>

namespace xe::net {

// Whether the device has a route to anywhere. Says nothing about the internet
// behind it (a Wi-Fi network with no internet still counts as a route), so
// connections still need their own failure handling.
enum class NetworkPathState : uint8_t {
  kAvailable,
  // Airplane Mode, or Wi-Fi and cellular both off or out of range.
  kNoRoute,
  // A route exists but Settings doesn't let this app use it.
  kNotAllowed,
};

// Available where the platform can't tell. The first call may wait a moment
// for the system's first answer.
NetworkPathState GetNetworkPathState();
inline bool NetworkPathAvailable() {
  return GetNetworkPathState() == NetworkPathState::kAvailable;
}

// |listener| runs on an internal queue whenever the state changes, with the
// listeners' lock held: it should only note the change and wake its owner, and
// must not add or remove listeners. Removing waits for a running call to end.
uint64_t AddNetworkPathListener(std::function<void()> listener);
void RemoveNetworkPathListener(uint64_t id);

}  // namespace xe::net

#endif  // XENIA_NET_NETWORK_PATH_H_
