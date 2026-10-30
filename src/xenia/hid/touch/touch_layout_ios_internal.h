/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_TOUCH_LAYOUT_IOS_INTERNAL_H_
#define XENIA_HID_TOUCH_LAYOUT_IOS_INTERNAL_H_

#include <string>

#include "xenia/hid/touch/touch_layout_ios.h"

namespace xe {
namespace hid {
namespace touch {

std::string TrimIOSTouchLabel(std::string value);
std::string DefaultIOSTouchControlLabel(
    const IOSTouchControlDefinition& control);
IOSTouchControlDefinition MakeDefaultIOSTouchControlDefinitionImpl(
    IOSTouchControlType type);

}  // namespace touch
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_TOUCH_LAYOUT_IOS_INTERNAL_H_
