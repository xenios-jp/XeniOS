/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/touch/touch_layout_ios_internal.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace xe {
namespace hid {
namespace touch {

namespace {

constexpr float kDefaultTouchBehaviorHoldSeconds = 0.30f;
constexpr float kDefaultTouchBehaviorHoldDragSeconds = 0.16f;
constexpr float kDefaultTouchBehaviorDoubleTapSeconds = 0.24f;

float DefaultHoldSecondsForInteractionTriggerImpl(
    IOSTouchInteractionTrigger trigger) {
  switch (trigger) {
    case IOSTouchInteractionTrigger::kHold:
      return kDefaultTouchBehaviorHoldSeconds;
    case IOSTouchInteractionTrigger::kHoldDrag:
      return kDefaultTouchBehaviorHoldDragSeconds;
    case IOSTouchInteractionTrigger::kDoubleTap:
    case IOSTouchInteractionTrigger::kDoubleTapForward:
      return kDefaultTouchBehaviorDoubleTapSeconds;
    case IOSTouchInteractionTrigger::kNone:
    default:
      return kDefaultTouchBehaviorHoldSeconds;
  }
}

}  // namespace

std::string TrimIOSTouchLabel(std::string value) {
  auto not_space = [](char c) {
    return !std::isspace(static_cast<unsigned char>(c));
  };
  auto begin = std::find_if(value.begin(), value.end(), not_space);
  if (begin == value.end()) {
    return {};
  }
  auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
  return std::string(begin, end);
}

std::string DefaultIOSTouchControlLabel(
    const IOSTouchControlDefinition& control) {
  return std::string(IOSTouchActionDisplayName(control.action));
}

bool IsSupportedIOSTouchPrimaryAction(IOSTouchControlType control_type,
                                      IOSTouchAction action) {
  switch (control_type) {
    case IOSTouchControlType::kMoveStick:
      return action == IOSTouchAction::kMove || action == IOSTouchAction::kLook;
    case IOSTouchControlType::kLookSwipeZone:
      return action == IOSTouchAction::kLook || action == IOSTouchAction::kMove;
    case IOSTouchControlType::kPauseButton:
      return action == IOSTouchAction::kPauseMenu;
    case IOSTouchControlType::kActionButton:
      return IsEditableIOSTouchAction(action);
  }
  return false;
}

bool IsEditableIOSTouchAction(IOSTouchAction action) {
  for (IOSTouchAction editable_action : kIOSTouchEditableActions) {
    if (editable_action == action) {
      return true;
    }
  }
  return false;
}

const char* IOSTouchActionDisplayName(IOSTouchAction action) {
  switch (action) {
    case IOSTouchAction::kNone:
      return "Unused";
    case IOSTouchAction::kMove:
      return "Move";
    case IOSTouchAction::kLook:
      return "Look";
    case IOSTouchAction::kPauseMenu:
      return "Pause";
    case IOSTouchAction::kButtonA:
      return "A";
    case IOSTouchAction::kButtonB:
      return "B";
    case IOSTouchAction::kButtonX:
      return "X";
    case IOSTouchAction::kButtonY:
      return "Y";
    case IOSTouchAction::kLeftBumper:
      return "LB";
    case IOSTouchAction::kRightBumper:
      return "RB";
    case IOSTouchAction::kLeftTrigger:
      return "LT";
    case IOSTouchAction::kRightTrigger:
      return "RT";
    case IOSTouchAction::kBack:
      return "Back";
    case IOSTouchAction::kStart:
      return "Start";
    case IOSTouchAction::kLeftThumb:
      return "LS";
    case IOSTouchAction::kRightThumb:
      return "RS";
    case IOSTouchAction::kDpadUp:
      return "D-Pad Up";
    case IOSTouchAction::kDpadDown:
      return "D-Pad Down";
    case IOSTouchAction::kDpadLeft:
      return "D-Pad Left";
    case IOSTouchAction::kDpadRight:
      return "D-Pad Right";
  }
  return "Control";
}

const char* IOSTouchInteractionTriggerDisplayName(
    IOSTouchInteractionTrigger trigger) {
  switch (trigger) {
    case IOSTouchInteractionTrigger::kNone:
      return "Off";
    case IOSTouchInteractionTrigger::kHold:
      return "Hold";
    case IOSTouchInteractionTrigger::kHoldDrag:
      return "Hold + Drag";
    case IOSTouchInteractionTrigger::kDoubleTap:
      return "Double Tap";
    case IOSTouchInteractionTrigger::kDoubleTapForward:
      return "Double Tap Forward";
  }
  return "Off";
}

const char* IOSTouchAnalogOutputDisplayName(IOSTouchAnalogOutput output) {
  switch (output) {
    case IOSTouchAnalogOutput::kNone:
      return "Off";
    case IOSTouchAnalogOutput::kLook:
      return "Look";
    case IOSTouchAnalogOutput::kMove:
      return "Move";
  }
  return "Off";
}

float DefaultIOSTouchHoldSecondsForInteractionTrigger(
    IOSTouchInteractionTrigger trigger) {
  return DefaultHoldSecondsForInteractionTriggerImpl(trigger);
}

bool IOSTouchControlHasCustomLabel(const IOSTouchControlDefinition& control) {
  return !control.label_uses_default && !control.label.empty();
}

std::string IOSTouchConfiguredControlLabel(
    const IOSTouchControlDefinition& control) {
  if (!control.label_uses_default && !control.label.empty()) {
    return control.label;
  }
  return DefaultIOSTouchControlLabel(control);
}

std::string IOSTouchVisibleControlLabel(
    const IOSTouchControlDefinition& control) {
  if (control.label_hidden) {
    return {};
  }
  return IOSTouchConfiguredControlLabel(control);
}

void SetIOSTouchControlCustomLabel(std::string label,
                                   IOSTouchControlDefinition* control) {
  if (!control) {
    return;
  }

  label = TrimIOSTouchLabel(std::move(label));
  if (label.empty()) {
    ResetIOSTouchControlLabel(control);
    return;
  }

  control->label = std::move(label);
  control->label_uses_default = false;
}

void ResetIOSTouchControlLabel(IOSTouchControlDefinition* control) {
  if (!control) {
    return;
  }

  control->label.clear();
  control->label_uses_default = true;
}

bool ConfigureIOSTouchControlAction(IOSTouchAction action,
                                    IOSTouchControlDefinition* control) {
  if (!control) {
    return false;
  }

  IOSTouchControlDefinition updated = *control;
  const bool keep_custom_label = IOSTouchControlHasCustomLabel(updated);
  updated.action = action;
  if (!keep_custom_label) {
    ResetIOSTouchControlLabel(&updated);
  }
  updated.mapped_buttons = 0;
  updated.mapped_left_trigger = 0;
  updated.mapped_right_trigger = 0;
  updated.hold_while_captured = false;
  updated.enables_relative_look = false;
  updated.drag_output = IOSTouchAnalogOutput::kNone;
  updated.analog_tuning.horizontal_scale = 1.0f;
  updated.analog_tuning.vertical_scale = 1.0f;
  updated.relative_look_scale = 1.0f;
  updated.held_look_scale = 1.0f;
  updated.held_move_scale = 1.0f;

  switch (action) {
    case IOSTouchAction::kMove:
    case IOSTouchAction::kLook:
    case IOSTouchAction::kPauseMenu:
    case IOSTouchAction::kNone:
      break;
    case IOSTouchAction::kButtonA:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_A;
      break;
    case IOSTouchAction::kButtonB:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_B;
      break;
    case IOSTouchAction::kButtonX:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_X;
      break;
    case IOSTouchAction::kButtonY:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_Y;
      break;
    case IOSTouchAction::kLeftBumper:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_LEFT_SHOULDER;
      break;
    case IOSTouchAction::kRightBumper:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
      break;
    case IOSTouchAction::kLeftTrigger:
      updated.mapped_left_trigger = 255;
      updated.hold_while_captured = true;
      updated.enables_relative_look = true;
      updated.drag_output = IOSTouchAnalogOutput::kLook;
      updated.relative_look_scale = 0.80f;
      updated.analog_tuning.horizontal_scale = updated.relative_look_scale;
      updated.analog_tuning.vertical_scale = updated.relative_look_scale;
      break;
    case IOSTouchAction::kRightTrigger:
      updated.mapped_right_trigger = 255;
      updated.hold_while_captured = true;
      updated.enables_relative_look = true;
      updated.drag_output = IOSTouchAnalogOutput::kLook;
      updated.relative_look_scale = 0.92f;
      updated.analog_tuning.horizontal_scale = updated.relative_look_scale;
      updated.analog_tuning.vertical_scale = updated.relative_look_scale;
      break;
    case IOSTouchAction::kBack:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_BACK;
      break;
    case IOSTouchAction::kStart:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_START;
      break;
    case IOSTouchAction::kLeftThumb:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_LEFT_THUMB;
      break;
    case IOSTouchAction::kRightThumb:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_RIGHT_THUMB;
      break;
    case IOSTouchAction::kDpadUp:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_DPAD_UP;
      break;
    case IOSTouchAction::kDpadDown:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_DPAD_DOWN;
      break;
    case IOSTouchAction::kDpadLeft:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_DPAD_LEFT;
      break;
    case IOSTouchAction::kDpadRight:
      updated.mapped_buttons = hid::X_INPUT_GAMEPAD_DPAD_RIGHT;
      break;
    default:
      return false;
  }

  *control = std::move(updated);
  return true;
}

IOSTouchAction NextEditableIOSTouchAction(IOSTouchAction action) {
  for (size_t index = 0; index < kIOSTouchEditableActions.size(); ++index) {
    if (kIOSTouchEditableActions[index] == action) {
      return kIOSTouchEditableActions[(index + 1) %
                                      kIOSTouchEditableActions.size()];
    }
  }
  return kIOSTouchEditableActions[0];
}

const char* IOSTouchTintStyleDisplayName(IOSTouchTintStyle tint_style) {
  switch (tint_style) {
    case IOSTouchTintStyle::kAuto:
      return "Auto";
    case IOSTouchTintStyle::kAmber:
      return "Amber";
    case IOSTouchTintStyle::kSky:
      return "Sky";
    case IOSTouchTintStyle::kMint:
      return "Mint";
    case IOSTouchTintStyle::kRose:
      return "Rose";
    case IOSTouchTintStyle::kLime:
      return "Lime";
    case IOSTouchTintStyle::kCoral:
      return "Coral";
    case IOSTouchTintStyle::kSlate:
      return "Slate";
  }
  return "Auto";
}

IOSTouchTintStyle NextIOSTouchTintStyle(IOSTouchTintStyle tint_style) {
  for (size_t index = 0; index < kIOSTouchEditableTintStyles.size(); ++index) {
    if (kIOSTouchEditableTintStyles[index] == tint_style) {
      return kIOSTouchEditableTintStyles[(index + 1) %
                                         kIOSTouchEditableTintStyles.size()];
    }
  }
  return kIOSTouchEditableTintStyles[0];
}

}  // namespace touch
}  // namespace hid
}  // namespace xe
