/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/touch/touch_layout_ios_internal.h"

#include <utility>

namespace xe {
namespace hid {
namespace touch {

namespace {

constexpr IOSTouchRect kMoveStickFrame = {0.055f, 0.56f, 0.190f, 0.315f};
constexpr IOSTouchRect kLookZoneFrame = {0.0f, 0.0f, 1.0f, 1.0f};
constexpr IOSTouchRect kPauseFrame = {0.040f, 0.045f, 0.120f, 0.112f};
constexpr IOSTouchRect kBackFrame = {0.390f, 0.045f, 0.080f, 0.112f};
constexpr IOSTouchRect kStartFrame = {0.495f, 0.045f, 0.085f, 0.112f};
constexpr IOSTouchRect kLeftBumperFrame = {0.660f, 0.050f, 0.085f, 0.112f};
constexpr IOSTouchRect kRightBumperFrame = {0.765f, 0.050f, 0.085f, 0.112f};
constexpr IOSTouchRect kAimFrame = {0.095f, 0.405f, 0.120f, 0.110f};
constexpr IOSTouchRect kSwapFrame = {0.760f, 0.455f, 0.065f, 0.115f};
constexpr IOSTouchRect kReloadFrame = {0.700f, 0.585f, 0.065f, 0.115f};
constexpr IOSTouchRect kMeleeFrame = {0.820f, 0.585f, 0.065f, 0.115f};
constexpr IOSTouchRect kJumpFrame = {0.760f, 0.715f, 0.065f, 0.115f};
constexpr IOSTouchRect kFireFrame = {0.860f, 0.405f, 0.120f, 0.110f};

IOSTouchControlDefinition MakeActionButton(const char* identifier,
                                           IOSTouchAction action,
                                           const IOSTouchRect& normalized_frame,
                                           uint8_t capture_priority) {
  IOSTouchControlDefinition control;
  control.identifier = identifier;
  control.type = IOSTouchControlType::kActionButton;
  control.shape = IOSTouchControlShape::kCircle;
  control.normalized_frame = normalized_frame;
  control.activation_radius = 0.5f;
  control.analog_tuning.activation_radius = control.activation_radius;
  control.visual_opacity = 0.92f;
  control.capture_priority = capture_priority;
  ConfigureIOSTouchControlAction(action, &control);
  return control;
}

}  // namespace

IOSTouchControlDefinition MakeDefaultIOSTouchControlDefinitionImpl(
    IOSTouchControlType type) {
  switch (type) {
    case IOSTouchControlType::kMoveStick: {
      IOSTouchControlDefinition control;
      control.identifier = "move_stick";
      control.type = IOSTouchControlType::kMoveStick;
      control.action = IOSTouchAction::kMove;
      control.shape = IOSTouchControlShape::kCircle;
      control.normalized_frame = kMoveStickFrame;
      control.deadzone = 0.14f;
      control.activation_radius = 0.48f;
      control.analog_tuning.deadzone = control.deadzone;
      control.analog_tuning.activation_radius = control.activation_radius;
      control.visual_opacity = 0.80f;
      control.move_with_dpad_ring = true;
      control.secondary_behavior.trigger =
          IOSTouchInteractionTrigger::kDoubleTapForward;
      control.secondary_behavior.action = IOSTouchAction::kLeftThumb;
      control.secondary_behavior.hold_seconds =
          DefaultIOSTouchHoldSecondsForInteractionTrigger(
              control.secondary_behavior.trigger);
      control.capture_priority = 220;
      return control;
    }
    case IOSTouchControlType::kLookSwipeZone: {
      IOSTouchControlDefinition control;
      control.identifier = "look_zone";
      control.type = IOSTouchControlType::kLookSwipeZone;
      control.action = IOSTouchAction::kLook;
      control.shape = IOSTouchControlShape::kRoundedRect;
      control.normalized_frame = kLookZoneFrame;
      control.drag_output = IOSTouchAnalogOutput::kLook;
      control.visual_opacity = 0.0f;
      control.capture_priority = 8;
      SetIOSTouchControlCustomLabel("Look Background", &control);
      return control;
    }
    case IOSTouchControlType::kPauseButton: {
      IOSTouchControlDefinition control;
      control.identifier = "pause_button";
      control.type = IOSTouchControlType::kPauseButton;
      control.action = IOSTouchAction::kPauseMenu;
      control.shape = IOSTouchControlShape::kRoundedRect;
      control.normalized_frame = kPauseFrame;
      control.visual_opacity = 0.92f;
      control.capture_priority = 255;
      return control;
    }
    case IOSTouchControlType::kActionButton:
    default:
      return MakeActionButton("action_button", IOSTouchAction::kButtonA,
                              IOSTouchRect{0.82f, 0.32f, 0.12f, 0.14f}, 232);
  }
}

IOSTouchLayoutModel CreateDefaultIOSFPSLayoutModel() {
  IOSTouchLayoutModel layout;
  layout.layout_id = "fps_compact";
  layout.display_name = "FPS Compact";
  layout.author = "XeniOS";
  layout.base_template = "fps_compact";

  layout.controls.push_back(MakeDefaultIOSTouchControlDefinitionImpl(
      IOSTouchControlType::kMoveStick));
  layout.controls.push_back(MakeDefaultIOSTouchControlDefinitionImpl(
      IOSTouchControlType::kLookSwipeZone));
  layout.controls.push_back(MakeDefaultIOSTouchControlDefinitionImpl(
      IOSTouchControlType::kPauseButton));
  layout.controls.push_back(
      MakeActionButton("back_button", IOSTouchAction::kBack, kBackFrame, 250));
  layout.controls.push_back(MakeActionButton(
      "start_button", IOSTouchAction::kStart, kStartFrame, 250));
  layout.controls.push_back(MakeActionButton("left_bumper_button",
                                             IOSTouchAction::kLeftBumper,
                                             kLeftBumperFrame, 242));
  layout.controls.push_back(MakeActionButton("right_bumper_button",
                                             IOSTouchAction::kRightBumper,
                                             kRightBumperFrame, 243));
  layout.controls.push_back(MakeActionButton(
      "aim_button", IOSTouchAction::kLeftTrigger, kAimFrame, 240));
  layout.controls.push_back(MakeActionButton(
      "swap_button", IOSTouchAction::kButtonY, kSwapFrame, 236));
  layout.controls.push_back(MakeActionButton(
      "reload_button", IOSTouchAction::kButtonX, kReloadFrame, 236));
  IOSTouchControlDefinition melee = MakeActionButton(
      "melee_button", IOSTouchAction::kButtonB, kMeleeFrame, 237);
  melee.secondary_behavior.trigger = IOSTouchInteractionTrigger::kHold;
  melee.secondary_behavior.action = IOSTouchAction::kRightThumb;
  melee.secondary_behavior.hold_seconds =
      DefaultIOSTouchHoldSecondsForInteractionTrigger(
          melee.secondary_behavior.trigger);
  layout.controls.push_back(std::move(melee));
  layout.controls.push_back(MakeActionButton(
      "jump_button", IOSTouchAction::kButtonA, kJumpFrame, 238));
  layout.controls.push_back(MakeActionButton(
      "fire_button", IOSTouchAction::kRightTrigger, kFireFrame, 242));

  return layout;
}

IOSTouchControlDefinition CreateDefaultIOSTouchControlDefinition(
    IOSTouchControlType type) {
  return MakeDefaultIOSTouchControlDefinitionImpl(type);
}

}  // namespace touch
}  // namespace hid
}  // namespace xe
