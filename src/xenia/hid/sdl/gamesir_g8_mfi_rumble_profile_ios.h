/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SDL_GAMESIR_G8_MFI_RUMBLE_PROFILE_IOS_H_
#define XENIA_HID_SDL_GAMESIR_G8_MFI_RUMBLE_PROFILE_IOS_H_

#include "xenia/hid/sdl/ea_rumble_profile_ios.h"

namespace xe {
namespace hid {
namespace sdl {

// GameSir G8+ MFi: iOS's GameController haptics don't reach its motors, but
// the vendor ExternalAccessory protocol does.
extern const EARumbleProfile kGameSirG8MFiRumbleProfile;

}  // namespace sdl
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SDL_GAMESIR_G8_MFI_RUMBLE_PROFILE_IOS_H_
