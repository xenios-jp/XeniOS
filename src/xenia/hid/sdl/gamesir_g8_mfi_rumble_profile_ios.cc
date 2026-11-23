/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/sdl/gamesir_g8_mfi_rumble_profile_ios.h"

namespace xe {
namespace hid {
namespace sdl {

namespace {

constexpr size_t kPacketSize = 9;
static_assert(kPacketSize <= kMaxEARumblePacketSize);

// The pad takes 8-bit motor levels; round XInput's 16-bit ones to nearest.
uint8_t ToMotorLevel(uint16_t speed) {
  return static_cast<uint8_t>((uint32_t(speed) * 255 + 32767) / 65535);
}

bool MatchesControllerName(std::string_view name) {
  return ContainsIgnoringCase(name, "gamesir") &&
         ContainsIgnoringCase(name, "g8+") && ContainsIgnoringCase(name, "mfi");
}

// Report 0x04: left motor, enable, right motor, enable, then padding.
void EncodePacket(uint16_t low_frequency, uint16_t high_frequency,
                  uint8_t* out) {
  out[0] = 0x04;
  out[1] = ToMotorLevel(low_frequency);
  out[2] = 0x01;
  out[3] = ToMotorLevel(high_frequency);
  out[4] = 0x01;
  out[5] = out[6] = out[7] = out[8] = 0x00;
}

}  // namespace

const EARumbleProfile kGameSirG8MFiRumbleProfile = {
    "com.xiaoji.M2boot",
    "GameSir",
    "G8+ MFi",
    MatchesControllerName,
    kPacketSize,
    EncodePacket,
    // The pad stops its motors when a level isn't refreshed for a while.
    std::chrono::microseconds(167253),
};

}  // namespace sdl
}  // namespace hid
}  // namespace xe
