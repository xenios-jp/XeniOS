/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SDL_ACCESSORY_RUMBLE_H_
#define XENIA_HID_SDL_ACCESSORY_RUMBLE_H_

#include <cstdint>
#include <memory>
#include <string_view>

#include "xenia/base/platform.h"

namespace xe {
namespace hid {
namespace sdl {

// Rumble for a controller whose motors SDL can't reach, driven through a
// vendor side channel instead (ExternalAccessory on iOS).
class AccessoryRumble {
 public:
  // Stops the motors and releases the side channel.
  virtual ~AccessoryRumble() = default;

  // Safe to call from any thread; never blocks on the transport.
  virtual void SetMotors(uint16_t low_frequency, uint16_t high_frequency) = 0;
  void Stop() { SetMotors(0, 0); }
};

// Returns the side channel rumble for the controller SDL reports as
// |controller_name|, or null when SDL rumble should be used for it.
#if XE_PLATFORM_IOS
std::unique_ptr<AccessoryRumble> CreateAccessoryRumble(
    std::string_view controller_name);
#else
inline std::unique_ptr<AccessoryRumble> CreateAccessoryRumble(
    std::string_view) {
  return nullptr;
}
#endif

}  // namespace sdl
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SDL_ACCESSORY_RUMBLE_H_
