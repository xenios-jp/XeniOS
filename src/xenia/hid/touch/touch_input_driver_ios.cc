/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/touch/touch_input_driver_ios.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "xenia/base/logging.h"
#include "xenia/hid/touch/touch_layout_ios.h"
#include "xenia/ui/ios/app/windowed_app_context_ios.h"
#include "xenia/ui/virtual_key.h"

namespace xe {
namespace hid {
namespace touch {

namespace {

constexpr uint8_t kTouchTriggerCapability = 0xFF;
constexpr int16_t kTouchThumbCapability = static_cast<int16_t>(0xFFFFu);
constexpr uint8_t kTouchTriggerThreshold = 0x1F;
constexpr int16_t kTouchThumbThreshold = 0x4E00;

constexpr std::array<ui::VirtualKey, 34> kTouchVirtualKeys = {
    ui::VirtualKey::kXInputPadDpadUp,
    ui::VirtualKey::kXInputPadDpadDown,
    ui::VirtualKey::kXInputPadDpadLeft,
    ui::VirtualKey::kXInputPadDpadRight,
    ui::VirtualKey::kXInputPadStart,
    ui::VirtualKey::kXInputPadBack,
    ui::VirtualKey::kXInputPadLThumbPress,
    ui::VirtualKey::kXInputPadRThumbPress,
    ui::VirtualKey::kXInputPadLShoulder,
    ui::VirtualKey::kXInputPadRShoulder,
    ui::VirtualKey::kXInputPadGuide,
    ui::VirtualKey::kNone,
    ui::VirtualKey::kXInputPadA,
    ui::VirtualKey::kXInputPadB,
    ui::VirtualKey::kXInputPadX,
    ui::VirtualKey::kXInputPadY,
    ui::VirtualKey::kXInputPadLTrigger,
    ui::VirtualKey::kXInputPadRTrigger,
    ui::VirtualKey::kXInputPadLThumbUp,
    ui::VirtualKey::kXInputPadLThumbDown,
    ui::VirtualKey::kXInputPadLThumbRight,
    ui::VirtualKey::kXInputPadLThumbLeft,
    ui::VirtualKey::kXInputPadLThumbUpLeft,
    ui::VirtualKey::kXInputPadLThumbUpRight,
    ui::VirtualKey::kXInputPadLThumbDownRight,
    ui::VirtualKey::kXInputPadLThumbDownLeft,
    ui::VirtualKey::kXInputPadRThumbUp,
    ui::VirtualKey::kXInputPadRThumbDown,
    ui::VirtualKey::kXInputPadRThumbRight,
    ui::VirtualKey::kXInputPadRThumbLeft,
    ui::VirtualKey::kXInputPadRThumbUpLeft,
    ui::VirtualKey::kXInputPadRThumbUpRight,
    ui::VirtualKey::kXInputPadRThumbDownRight,
    ui::VirtualKey::kXInputPadRThumbDownLeft,
};

uint64_t TouchAnalogToKeyfield(const IOSTouchResolvedState& state) {
  uint64_t keyfield = 0;
  keyfield |= uint64_t(state.left_trigger > kTouchTriggerThreshold) << 16;
  keyfield |= uint64_t(state.right_trigger > kTouchTriggerThreshold) << 17;

  auto append_thumb = [&keyfield](int16_t thumb_x, int16_t thumb_y,
                                  size_t bit_base) {
    uint64_t up = thumb_y > kTouchThumbThreshold;
    uint64_t down = thumb_y < -kTouchThumbThreshold;
    uint64_t right = thumb_x > kTouchThumbThreshold;
    uint64_t left = thumb_x < -kTouchThumbThreshold;
    if (up && left) {
      up = 0;
      left = 0;
      keyfield |= uint64_t(1) << (bit_base + 4);
    }
    if (up && right) {
      up = 0;
      right = 0;
      keyfield |= uint64_t(1) << (bit_base + 5);
    }
    if (down && right) {
      down = 0;
      right = 0;
      keyfield |= uint64_t(1) << (bit_base + 6);
    }
    if (down && left) {
      down = 0;
      left = 0;
      keyfield |= uint64_t(1) << (bit_base + 7);
    }
    keyfield |= up << bit_base;
    keyfield |= down << (bit_base + 1);
    keyfield |= right << (bit_base + 2);
    keyfield |= left << (bit_base + 3);
  };

  append_thumb(state.thumb_lx, state.thumb_ly, 18);
  append_thumb(state.thumb_rx, state.thumb_ry, 26);
  return keyfield;
}

uint64_t TouchKeystrokeFieldFromState(const IOSTouchResolvedState& state) {
  return uint64_t(state.buttons) | TouchAnalogToKeyfield(state);
}

void ApplyMappedCapabilities(const IOSTouchControlDefinition& control,
                             X_INPUT_CAPABILITIES* out_caps) {
  if (!out_caps) {
    return;
  }
  out_caps->gamepad.buttons =
      uint16_t(out_caps->gamepad.buttons) | control.mapped_buttons;
  out_caps->gamepad.left_trigger = std::max<uint8_t>(
      out_caps->gamepad.left_trigger,
      control.mapped_left_trigger ? kTouchTriggerCapability : 0);
  out_caps->gamepad.right_trigger = std::max<uint8_t>(
      out_caps->gamepad.right_trigger,
      control.mapped_right_trigger ? kTouchTriggerCapability : 0);
}

void ApplyLookCapabilities(X_INPUT_CAPABILITIES* out_caps) {
  if (!out_caps) {
    return;
  }
  out_caps->gamepad.thumb_rx = kTouchThumbCapability;
  out_caps->gamepad.thumb_ry = kTouchThumbCapability;
}

void ApplyMoveCapabilities(X_INPUT_CAPABILITIES* out_caps) {
  if (!out_caps) {
    return;
  }
  out_caps->gamepad.thumb_lx = kTouchThumbCapability;
  out_caps->gamepad.thumb_ly = kTouchThumbCapability;
}

void ApplyActionCapabilities(IOSTouchAction action,
                             X_INPUT_CAPABILITIES* out_caps) {
  IOSTouchControlDefinition mapped_control;
  if (!ConfigureIOSTouchControlAction(action, &mapped_control)) {
    return;
  }
  ApplyMappedCapabilities(mapped_control, out_caps);
}

void ApplyAnalogOutputCapabilities(IOSTouchAnalogOutput output,
                                   X_INPUT_CAPABILITIES* out_caps) {
  switch (output) {
    case IOSTouchAnalogOutput::kLook:
      ApplyLookCapabilities(out_caps);
      break;
    case IOSTouchAnalogOutput::kMove:
      ApplyMoveCapabilities(out_caps);
      break;
    case IOSTouchAnalogOutput::kNone:
    default:
      break;
  }
}

IOSTouchAnalogOutput EffectiveAnalogOutput(IOSTouchAnalogOutput output,
                                           bool legacy_look_enabled) {
  if (output != IOSTouchAnalogOutput::kNone) {
    return output;
  }
  return legacy_look_enabled ? IOSTouchAnalogOutput::kLook
                             : IOSTouchAnalogOutput::kNone;
}

void ApplyControlCapabilities(const IOSTouchControlDefinition& control,
                              X_INPUT_CAPABILITIES* out_caps) {
  switch (control.type) {
    case IOSTouchControlType::kMoveStick:
      if (control.action == IOSTouchAction::kLook) {
        ApplyLookCapabilities(out_caps);
      } else {
        ApplyMoveCapabilities(out_caps);
      }
      break;
    case IOSTouchControlType::kLookSwipeZone:
      if (control.action == IOSTouchAction::kMove) {
        ApplyMoveCapabilities(out_caps);
      } else {
        ApplyLookCapabilities(out_caps);
      }
      break;
    case IOSTouchControlType::kPauseButton:
      break;
    case IOSTouchControlType::kActionButton:
      ApplyMappedCapabilities(control, out_caps);
      ApplyAnalogOutputCapabilities(
          EffectiveAnalogOutput(control.drag_output,
                                control.enables_relative_look),
          out_caps);
      break;
  }

  if (control.secondary_behavior.trigger != IOSTouchInteractionTrigger::kNone) {
    ApplyActionCapabilities(control.secondary_behavior.action, out_caps);
    ApplyAnalogOutputCapabilities(
        EffectiveAnalogOutput(control.secondary_behavior.analog_output,
                              control.secondary_behavior.enables_relative_look),
        out_caps);
  }
}

}  // namespace

TouchInputDriver::TouchInputDriver(xe::ui::Window* window,
                                   size_t window_z_order)
    : InputDriver(window, window_z_order) {}

TouchInputDriver::~TouchInputDriver() = default;

X_STATUS TouchInputDriver::Setup() {
  RefreshRuntimeModel();
  if (!runtime_model_) {
    XELOGW(
        "iOS touch input driver: runtime model is unavailable during setup;"
        " deferring until first use");
  }
  return X_STATUS_SUCCESS;
}

bool TouchInputDriver::RefreshRuntimeModel() {
  if (runtime_model_) {
    return true;
  }

  auto& app_context =
      static_cast<xe::ui::IOSWindowedAppContext&>(window()->app_context());
  runtime_model_ = app_context.touch_runtime_model();
  return runtime_model_ != nullptr;
}

X_RESULT TouchInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                           X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (!out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!RefreshRuntimeModel() || !IsUserSupported(user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Capabilities reflect what the active touch layout can produce, irrespective
  // of whether the gameplay overlay is currently visible. Games sometimes query
  // capabilities up-front (e.g. at title screen, before the user dismisses any
  // launcher chrome and the overlay activates). Gating capabilities on
  // gameplay_enabled previously made the touch driver appear disconnected for
  // those queries, blocking games from pre-configuring their UI for the
  // detected layout. GetState / SetState / GetKeystroke still gate on
  // gameplay_enabled so input only flows when the user is actually playing.
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->flags = 0;
  // This runs on emulator threads while the layout editor may be mutating
  // layout().controls on the main thread, so read the published snapshot
  // instead of the live vector.
  auto controls = runtime_model_->LoadControlsSnapshot();
  if (controls) {
    for (const auto& control : *controls) {
      ApplyControlCapabilities(control, out_caps);
    }
  }
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetState(uint32_t user_index,
                                    X_INPUT_STATE* out_state) {
  if (!out_state) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!RefreshRuntimeModel() || !IsUserSupported(user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  IOSTouchResolvedState state = runtime_model_->LoadResolvedState();
  if (!state.gameplay_enabled) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::memset(out_state, 0, sizeof(*out_state));
  out_state->packet_number = state.packet_number;
  out_state->gamepad.buttons = state.buttons;
  out_state->gamepad.left_trigger = state.left_trigger;
  out_state->gamepad.right_trigger = state.right_trigger;
  out_state->gamepad.thumb_lx = state.thumb_lx;
  out_state->gamepad.thumb_ly = state.thumb_ly;
  out_state->gamepad.thumb_rx = state.thumb_rx;
  out_state->gamepad.thumb_ry = state.thumb_ry;
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::SetState(uint32_t user_index,
                                    X_INPUT_VIBRATION* vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!RefreshRuntimeModel() || !IsUserSupported(user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  IOSTouchResolvedState state = runtime_model_->LoadResolvedState();
  return state.gameplay_enabled ? X_ERROR_SUCCESS
                                : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT TouchInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                        X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!RefreshRuntimeModel() || !IsUserSupported(user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::memset(out_keystroke, 0, sizeof(*out_keystroke));
  IOSTouchResolvedState state = runtime_model_->LoadResolvedState();
  std::lock_guard<std::mutex> lock(keystroke_mutex_);
  if (!state.gameplay_enabled) {
    pending_keystrokes_.clear();
    has_keystroke_state_ = false;
    last_keystroke_field_ = 0;
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const uint64_t current_field = TouchKeystrokeFieldFromState(state);
  if (!has_keystroke_state_) {
    has_keystroke_state_ = true;
    last_keystroke_field_ = current_field;
  } else if (current_field != last_keystroke_field_) {
    const uint64_t changed_bits = current_field ^ last_keystroke_field_;
    for (size_t bit_index = 0; bit_index < kTouchVirtualKeys.size();
         ++bit_index) {
      const uint64_t bit_mask = uint64_t(1) << bit_index;
      if (!(changed_bits & bit_mask)) {
        continue;
      }
      const ui::VirtualKey virtual_key = kTouchVirtualKeys[bit_index];
      if (virtual_key == ui::VirtualKey::kNone) {
        continue;
      }
      X_INPUT_KEYSTROKE keystroke = {};
      keystroke.virtual_key = uint16_t(virtual_key);
      keystroke.flags = (current_field & bit_mask) ? X_INPUT_KEYSTROKE_KEYDOWN
                                                   : X_INPUT_KEYSTROKE_KEYUP;
      keystroke.user_index =
          user_index == XUserIndexAny ? 0 : uint8_t(user_index);
      pending_keystrokes_.push_back(keystroke);
    }
    last_keystroke_field_ = current_field;
  }

  if (pending_keystrokes_.empty()) {
    return X_ERROR_EMPTY;
  }

  *out_keystroke = pending_keystrokes_.front();
  pending_keystrokes_.pop_front();
  return X_ERROR_SUCCESS;
}

InputType TouchInputDriver::GetInputType() const {
  return InputType::Controller;
}

std::vector<InputDeviceInfo> TouchInputDriver::EnumerateDevices() {
  if (!RefreshRuntimeModel()) {
    return {};
  }

  InputDeviceInfo info = {};
  info.driver_slot = 0;
  info.stable_id = "ios-touch-overlay";
  info.display_name = "Touch Controls";
  info.subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
  info.preferred_slot = 0;
  info.auto_bind = true;
  info.fallback_auto_bind = true;
  return {std::move(info)};
}

bool TouchInputDriver::IsUserSupported(uint32_t user_index) const {
  return user_index == 0 || user_index == XUserIndexAny;
}

}  // namespace touch
}  // namespace hid
}  // namespace xe
