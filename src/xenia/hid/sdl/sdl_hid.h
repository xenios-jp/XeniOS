/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SDL_SDL_HID_H_
#define XENIA_HID_SDL_SDL_HID_H_

#include <memory>

#include "xenia/hid/input.h"
#include "xenia/hid/input_system.h"

namespace xe {
namespace hid {
namespace sdl {

void SetVirtualControllerState(uint32_t user_index,
                               const X_INPUT_STATE& state);
void ClearVirtualControllerState(uint32_t user_index);
bool AnyPhysicalControllerConnected();

std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order);

}  // namespace sdl
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SDL_SDL_HID_H_
