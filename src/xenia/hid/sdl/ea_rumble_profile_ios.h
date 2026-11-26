/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SDL_EA_RUMBLE_PROFILE_IOS_H_
#define XENIA_HID_SDL_EA_RUMBLE_PROFILE_IOS_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace xe {
namespace hid {
namespace sdl {

inline constexpr size_t kMaxEARumblePacketSize = 32;

// How to rumble one controller model over ExternalAccessory. The transport in
// ea_rumble_ios.mm is shared; a new controller only needs a
// <controller>_rumble_profile_ios.cc, an entry in kProfiles
// (ea_rumble_profile_ios.cc) and its protocol in
// UISupportedExternalAccessoryProtocols (Info_ios.plist).
struct EARumbleProfile {
  // EAAccessory protocol string the rumble session is opened on.
  const char* protocol;
  // EAAccessory manufacturer and modelNumber, compared ignoring case.
  const char* manufacturer;
  const char* model;
  // Whether the controller SDL names |controller_name| is this model.
  bool (*matches_controller_name)(std::string_view controller_name);
  // Encodes one motor update into |packet_size| bytes of |out|.
  size_t packet_size;
  void (*encode_packet)(uint16_t low_frequency, uint16_t high_frequency,
                        uint8_t* out);
  // Non-zero rumble is resent at this interval for controllers that time
  // their motors out; zero sends each change once.
  std::chrono::microseconds resend_interval;
};

// The first profile matching |controller_name|, or null.
const EARumbleProfile* FindEARumbleProfile(std::string_view controller_name);

bool ContainsIgnoringCase(std::string_view haystack, std::string_view needle);

}  // namespace sdl
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SDL_EA_RUMBLE_PROFILE_IOS_H_
