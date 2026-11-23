/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/sdl/ea_rumble_profile_ios.h"

#include <algorithm>
#include <cctype>

#include "xenia/hid/sdl/gamesir_g8_mfi_rumble_profile_ios.h"

namespace xe {
namespace hid {
namespace sdl {

namespace {

// Register new ExternalAccessory controllers here.
const EARumbleProfile* const kProfiles[] = {
    &kGameSirG8MFiRumbleProfile,
};

}  // namespace

const EARumbleProfile* FindEARumbleProfile(std::string_view controller_name) {
  for (const EARumbleProfile* profile : kProfiles) {
    if (profile->matches_controller_name(controller_name)) {
      return profile;
    }
  }
  return nullptr;
}

bool ContainsIgnoringCase(std::string_view haystack, std::string_view needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(),
                     needle.end(), [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                     }) != haystack.end();
}

}  // namespace sdl
}  // namespace hid
}  // namespace xe
